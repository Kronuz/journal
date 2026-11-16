/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// Placement and re-partition (A4): `map` checks each of L loops owns a contiguous run of
// floor or ceil(4,096 / L) slots; `remap` cuts power after every durable step of four
// re-partitions of groups over a damaged old generation, where every group's records come back
// exactly once on its new loop and every lost group stays lost through two more restarts;
// `repartition_lost` and `retention` show the table's mark alone keeps a lost id refused once
// the damage is gone, after a 1->2 re-partition and after retention. `placement_test [case]`.
#include "journal/memory_io.h"
#include "journal/host.h"
#include <iostream>

namespace j = kronuz::journal;
namespace jt = kronuz::journal::testing;
namespace hd = kronuz::journal::host_detail;

namespace {
int checks = 0, failures = 0;
void check(bool value, const std::string& what) {
	++checks;
	if (!value) { ++failures; if (failures < 30) { std::cerr << "FAIL: " << what << '\n'; } }
}
const j::Identity host_identity{'P', 'L', 'A', 'C', 'E'};
// Segments large enough that populating keeps every record where it was written; retention uses
// small ones.
const j::TurnLogOptions turn_options{256 * 1024, 1024, 1, 4, 4096}, small_segments{16 * 1024, 1024, 1, 4, 4096};
using Records = std::map<std::uint64_t, std::vector<j::TurnRecord>>;

std::unique_ptr<j::Host> open(jt::Disk& disk, std::uint32_t loops, const j::TurnLogOptions& options = turn_options) {
	return std::make_unique<j::Host>(std::make_shared<jt::MemoryIO>(disk, "host/"), host_identity, j::HostOptions{loops, options});
}
// Every fiftieth group, unless a later checkpoint supersedes its damaged batch.
bool victim(std::uint64_t group) { return group % 50 == 0 && group % 3 != 0; }
void commit(j::Host& host) { for (std::uint32_t l = 0; l < host.loops(); ++l) { while (!host.driver(l).idle()) { host.driver(l).commit(); } } }
// Flips a byte of a durable record's payload, at `marker`, in whichever segment holds it.
bool damage(jt::Disk& disk, const std::string& marker) {
	for (const auto& [name, inode] : disk.durable_names) {
		if (!name.starts_with("host/l")) { continue; }
		auto& bytes = disk.inodes.at(inode).durable;
		if (auto at = bytes.find(marker); at != std::string::npos) { bytes[at + marker.size() / 2] ^= 0x5a; return true; }
	}
	return false;
}
// A victim's first batch is a run of its own letter spanning whole blocks; the damage lands in
// the middle of the run.
std::string marker(std::uint64_t group) { return std::string(200, static_cast<char>('A' + group / 50 % 26)); }
// Refused by the group's loop's log, which a lost id never reaches.
bool refused(j::Host& host, std::uint64_t group) {
	const j::TurnRecord record{group, 1, j::TurnRecordKind::Batch, "again"};
	try { host.driver_of(group).log().stage(std::span<const j::TurnRecord>(&record, 1)); } catch (const std::invalid_argument&) { return true; }
	return false;
}

// Groups 1..count, each a checkpoint record then batches over four turns, every third with a
// later checkpoint; the first batch of every fiftieth is damaged. Returns what each group wrote
// from its latest checkpoint.
Records populate(jt::Disk& disk, std::uint64_t count, std::set<std::uint64_t>& victims, const j::TurnLogOptions& options = turn_options) {
	Records written;
	{
		auto host = open(disk, 1, options);
		for (std::uint64_t g = 1; g <= count; ++g) {
			host->create_group(g);
			j::TurnRecord record{g, 1, j::TurnRecordKind::Checkpoint, "checkpoint-" + std::to_string(g)};
			host->driver_of(g).stage(record); written[g] = {record};
			if (g % 25 == 0) { commit(*host); }  // a turn fits in a segment
		}
		commit(*host);
		for (int round = 0; round < 4; ++round) {
			for (std::uint64_t g = 1; g <= count; ++g) {
				auto& records = written[g];
				const auto kind = round == 2 && g % 3 == 0 ? j::TurnRecordKind::Checkpoint : j::TurnRecordKind::Batch;
				auto payload = "group-" + std::to_string(g) + "-round-" + std::to_string(round) + std::string(g % 40, '.');
				if (round == 0 && victim(g)) { payload += std::string(2400, marker(g)[0]); }
				j::TurnRecord record{g, records.back().sequence + 1, kind, payload};
				host->driver_of(g).stage(record);
				if (kind == j::TurnRecordKind::Checkpoint) { records.clear(); }
				records.push_back(record);
				if (g % 25 == 0) { commit(*host); }
			}
			commit(*host);
		}
	}
	for (std::uint64_t g = 50; g <= count; g += 50) {
		if (!victim(g)) { continue; }
		check(damage(disk, marker(g)), "the victim's first batch is in the log");
		victims.insert(g);
	}
	return written;
}

// Every group not lost has its records, exactly once, on its own loop; every lost one is
// refused by the table and by its loop's log.
void verify(j::Host& host, const Records& written, const std::set<std::uint64_t>& lost, std::uint32_t loops, const std::string& at) {
	check(host.loops() == loops, "the host runs the requested loops: " + at);
	for (const auto& [g, records] : written) {
		if (lost.contains(g)) {
			check(host.status(g) == j::GroupStatus::lost, "a lost group stays lost: group " + std::to_string(g) + " " + at);
			check(refused(host, g), "its loop refuses its id: group " + std::to_string(g) + " " + at);
			continue;
		}
		for (std::uint32_t l = 0; l < host.loops(); ++l) {
			const bool home = l == host.loop(g);
			check(home ? host.driver(l).records(g) == records : !host.driver(l).has_group(g),
				"group " + std::to_string(g) + " has its records on its own loop only: " + at);
		}
	}
}

void map() {
	for (std::uint32_t loops : {1u, 2u, 3u, 5u, 7u, 16u, 100u, 1000u, 4095u, 4096u}) {
		std::vector<std::uint32_t> owned(loops);
		std::uint32_t previous = 0;
		for (std::uint32_t slot = 0; slot < hd::slots; ++slot) {
			const auto loop = hd::loop(slot, loops);
			check(loop < loops && loop >= previous, "slots map to loops in order");
			previous = loop; ++owned[loop];
		}
		for (auto count : owned) { check(count == hd::slots / loops || count == (hd::slots + loops - 1) / loops, "each loop owns floor or ceil(4096 / L) slots"); }
	}
	std::vector<std::uint32_t> hits(hd::slots);
	for (std::uint64_t g = 1; g <= 409600; ++g) { ++hits[hd::slot(g)]; }
	check(*std::min_element(hits.begin(), hits.end()) > 50 && *std::max_element(hits.begin(), hits.end()) < 150, "sequential ids spread over the slots");
}

void remap() {
	jt::Disk disk;
	std::set<std::uint64_t> victims;
	const auto written = populate(disk, 400, victims);
	std::set<std::uint64_t> lost;
	{
		auto host = open(disk, 1);
		lost = host->driver(0).recovered().lost;
		for (auto g : victims) { check(lost.contains(g), "the damage loses its victim " + std::to_string(g)); }
		check(lost.size() <= 5 * victims.size(), "damage costs the groups of its blocks, not every group");
		verify(*host, written, lost, 1, "first open");
	}
	std::uint64_t points = 0;
	for (std::uint32_t loops : {3u, 2u, 5u, 4u}) {
		const jt::Disk before = disk;
		const auto start = disk.operations;
		{ auto host = open(disk, loops); verify(*host, written, lost, loops, "re-partition to " + std::to_string(loops)); }
		const auto total = disk.operations - start;
		for (std::uint64_t cut = 1; cut <= total; ++cut, ++points) {
			jt::Disk crashed = before;
			crashed.cut = crashed.operations + cut;
			try { open(crashed, loops); } catch (const jt::PowerLoss&) {}
			crashed.power_loss();
			const auto at = "re-partition to " + std::to_string(loops) + " cut at " + std::to_string(cut);
			for (int restart = 0; restart < 3; ++restart) {
				try { auto host = open(crashed, loops); verify(*host, written, lost, loops, at + " restart " + std::to_string(restart)); }
				catch (const std::exception& error) { check(false, at + ": " + error.what()); }
			}
		}
	}
	std::cout << "remap: " << written.size() << " groups, " << lost.size() << " lost, " << points << " crash points\n";
}

// Journal's own damage scenario: a group lost to a damaged block of the only loop's log, then a
// 1->2 re-partition. The new logs hold no damage; only the table's mark refuses the id.
void repartition_lost() {
	jt::Disk disk;
	std::set<std::uint64_t> victims;
	const auto written = populate(disk, 60, victims);
	std::set<std::uint64_t> lost;
	{ auto host = open(disk, 1); lost = host->driver(0).recovered().lost; }
	check(lost.contains(50), "the damaged group is lost");
	auto host = open(disk, 2);
	check(host->generation() == 2, "the re-partition committed a new generation");
	for (std::uint32_t l = 0; l < 2; ++l) { check(!host->driver(l).recovered().damaged, "the new generation's logs hold no damage"); }
	verify(*host, written, lost, 2, "after 1->2");
}

// A lost id stays lost once retention recycled the damaged segment.
void retention() {
	jt::Disk disk;
	std::set<std::uint64_t> victims;
	const auto written = populate(disk, 60, victims, small_segments);
	std::set<std::uint64_t> lost;
	{
		auto host = open(disk, 1, small_segments);
		lost = host->driver(0).recovered().lost;
		check(lost.contains(50) && host->driver(0).recovered().damaged, "the damaged group is lost");
		std::uint64_t sequence = written.at(1).back().sequence;
		for (int turn = 0; turn < 200; ++turn) { host->driver(0).stage({1, ++sequence, j::TurnRecordKind::Batch, std::string(900, 'r')}); commit(*host); }
	}
	auto host = open(disk, 1, small_segments);
	check(!host->driver(0).recovered().damaged, "retention recycled the damaged segment");
	check(host->status(50) == j::GroupStatus::lost, "the table still holds the group lost");
	check(refused(*host, 50), "its id stays refused");
}
}  // namespace

int main(int argc, char** argv) {
	const std::string only = argc > 1 ? argv[1] : "";
	const std::vector<std::pair<std::string, void (*)()>> cases{{"map", map}, {"remap", remap}, {"repartition_lost", repartition_lost}, {"retention", retention}};
	for (const auto& [name, run] : cases) {
		if (!only.empty() && only != name) { continue; }
		try { run(); } catch (const std::exception& error) { check(false, name + ": " + error.what()); }
	}
	std::cout << checks << " placement checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
