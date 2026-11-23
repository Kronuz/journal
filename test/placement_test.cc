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
// exactly once on its new loop (a bounded one's after its bound, docs/mechanisms/lost-bound.md)
// and every lost or bounded group stays so through two more restarts; `repartition_lost`,
// `repartition_damaged` and `retention` show the table alone keeps a damaged group's bound once
// the damage is gone: after a 1->2 re-partition, after a re-partition that first sees the
// damage, and after retention; `damaged_absent`, a group whose only records the damage took, is
// lost or bounded rather than offered for create. `placement_test [case [shard shards]]`.
#include "faultlab/faultfs.h"
#include "journal/host.h"
#include <algorithm>
#include <iostream>

namespace j = kronuz::journal;
namespace fl = kronuz::faultlab;
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

// The host's directory on `disk`, made on first use.
std::shared_ptr<fl::io> host_io(fl::filesystem& disk) {
	const auto names = disk.list();
	if (std::find(names.begin(), names.end(), "host") == names.end()) { disk.make_directory("host"); }
	return std::make_shared<fl::io>(disk, "host");
}
std::unique_ptr<j::Host> open(fl::filesystem& disk, std::uint32_t loops, const j::TurnLogOptions& options = turn_options) {
	return std::make_unique<j::Host>(host_io(disk), host_identity, j::HostOptions{loops, options});
}
// A crash keeps any sector-boundary prefix of an unsynced append, as a real device may.
const fl::crash_policy sector_cut{.size = fl::size_pick::sector};
// The host's turn-log segment holding `bytes`, or empty.
std::string holder(fl::filesystem& disk, const std::string& bytes) {
	for (const auto& name : disk.list("host")) {
		if (name.starts_with("l") && disk.view("host/" + name).find(bytes) != std::string_view::npos) { return name; }
	}
	return {};
}
// Overwrites the host file `name` at `offset` with `bytes`, durably.
void overwrite(fl::filesystem& disk, const std::string& name, std::uint64_t offset, std::string_view bytes) {
	auto file = host_io(disk)->open_existing(name);
	file->write_at(offset, bytes); file->sync();
}
// Every fiftieth group, unless a later checkpoint supersedes its damaged batch.
bool victim(std::uint64_t group) { return group % 50 == 0 && group % 3 != 0; }
void commit(j::Host& host) { for (std::uint32_t l = 0; l < host.loops(); ++l) { while (!host.driver(l).idle()) { host.driver(l).commit(); } } }
// Flips a byte of a durable record's payload, at `marker`, in whichever segment holds it.
bool damage(fl::filesystem& disk, const std::string& marker) {
	const auto name = holder(disk, marker);
	if (name.empty()) { return false; }
	const auto at = disk.view("host/" + name).find(marker) + marker.size() / 2;
	const char flipped = static_cast<char>(disk.view("host/" + name)[at] ^ 0x5a);
	overwrite(disk, name, at, std::string_view(&flipped, 1));
	return true;
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
Records populate(fl::filesystem& disk, std::uint64_t count, std::set<std::uint64_t>& victims, const j::TurnLogOptions& options = turn_options) {
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
				j::TurnRecord record{g, records.back().sequence + 1, kind, payload, records.back().sequence};
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

using Bounds = std::map<std::uint64_t, std::uint64_t>;
// The groups an open bounded, at their bounds (TurnRecovery::bounded, kept by the host).
Bounds bounds(j::Host& host) {
	Bounds found;
	for (std::uint32_t l = 0; l < host.loops(); ++l) { for (const auto& [g, b] : host.driver(l).recovered().bounded) { if (host.bound(g)) { found[g] = b.bound; } } }
	return found;
}
// Every group not lost has its records, exactly once, on its own loop: a bounded one, at its
// bound, those after it; every lost one is refused by the table and by its loop's log.
void verify(j::Host& host, const Records& written, const std::set<std::uint64_t>& lost, const Bounds& bounded, std::uint32_t loops, const std::string& at) {
	check(host.loops() == loops, "the host runs the requested loops: " + at);
	for (const auto& [g, records] : written) {
		if (lost.contains(g)) {
			check(host.status(g) == j::GroupStatus::lost, "a lost group stays lost: group " + std::to_string(g) + " " + at);
			check(refused(host, g), "its loop refuses its id: group " + std::to_string(g) + " " + at);
			continue;
		}
		auto expected = records;
		if (auto found = bounded.find(g); found != bounded.end()) {
			check(host.status(g) == j::GroupStatus::bounded && host.bound(g) == found->second, "a bounded group stays bounded at its bound: group " + std::to_string(g) + " " + at);
			std::erase_if(expected, [&](const j::TurnRecord& record) { return record.sequence <= found->second; });
		}
		for (std::uint32_t l = 0; l < host.loops(); ++l) {
			const bool home = l == host.loop(g);
			check(home ? host.driver(l).records(g) == expected : !host.driver(l).has_group(g),
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

// `remap` alone may run one shard of its crash points: those whose index modulo `shards` is
// `shard`; every shard repeats the uncut re-partitions, so the shards together cut every point.
std::uint64_t shard = 0, shards = 1;
void remap() {
	fl::filesystem disk;
	std::set<std::uint64_t> victims;
	const auto written = populate(disk, 400, victims);
	std::set<std::uint64_t> lost; Bounds bounded;
	{
		auto host = open(disk, 1);
		lost = host->driver(0).recovered().lost; bounded = bounds(*host);
		for (auto g : victims) { check(lost.contains(g) || bounded.contains(g), "the damage costs its victim " + std::to_string(g)); }
		check(lost.size() + bounded.size() <= 5 * victims.size(), "damage costs the groups of its blocks, not every group");
		verify(*host, written, lost, bounded, 1, "first open");
	}
	std::uint64_t points = 0;
	for (std::uint32_t loops : {3u, 2u, 5u, 4u}) {
		const fl::filesystem before = disk.snapshot();
		const auto start = disk.mutations();
		{ auto host = open(disk, loops); verify(*host, written, lost, bounded, loops, "re-partition to " + std::to_string(loops)); }
		const auto total = disk.mutations() - start;
		for (std::uint64_t cut = 1; cut <= total; ++cut) {
			if ((cut - 1) % shards != shard) { continue; }
			++points;
			fl::filesystem crashed = before.snapshot();
			const auto at = "re-partition to " + std::to_string(loops) + " cut at " + std::to_string(cut);
			crashed.power_off_after(cut - 1);
			try { open(crashed, loops); } catch (const std::exception& error) { check(crashed.powered_off(), "only the power cut fails the re-partition: " + at + ": " + error.what()); }
			crashed.crash(loops * 7919 + cut, sector_cut);
			for (int restart = 0; restart < 3; ++restart) {
				try { auto host = open(crashed, loops); verify(*host, written, lost, bounded, loops, at + " restart " + std::to_string(restart)); }
				catch (const std::exception& error) { check(false, at + ": " + error.what()); }
			}
		}
	}
	std::cout << "remap: " << written.size() << " groups, " << lost.size() << " lost, " << points << " crash points (shard " << shard << " of " << shards << ")\n";
}

// Journal's own damage scenario: a group bounded by a damaged block of the only loop's log, then
// a 1->2 re-partition. The new logs hold no damage; only the table keeps the bound.
void repartition_lost() {
	fl::filesystem disk;
	std::set<std::uint64_t> victims;
	const auto written = populate(disk, 60, victims);
	std::set<std::uint64_t> lost; Bounds bounded;
	{ auto host = open(disk, 1); lost = host->driver(0).recovered().lost; bounded = bounds(*host); }
	check(bounded.contains(50), "the damaged group is bounded");
	auto host = open(disk, 2);
	check(host->generation() == 2, "the re-partition committed a new generation");
	for (std::uint32_t l = 0; l < 2; ++l) { check(!host->driver(l).recovered().damaged, "the new generation's logs hold no damage"); }
	verify(*host, written, lost, bounded, 2, "after 1->2");
}

// Damage first seen by a re-partition: step 1 stores the bounds and marks the lost ids before the
// new generation, which holds no damage, is committed.
void repartition_damaged() {
	fl::filesystem disk;
	std::set<std::uint64_t> victims;
	const auto written = populate(disk, 120, victims);
	std::set<std::uint64_t> lost; Bounds bounded;
	{
		fl::filesystem probe = disk.snapshot();  // what the stored generation lost, read on a copy
		auto host = open(probe, 1); lost = host->driver(0).recovered().lost; bounded = bounds(*host);
	}
	check(bounded.contains(50) && bounded.contains(100), "the damage bounds its victims");
	auto host = open(disk, 3);
	verify(*host, written, lost, bounded, 3, "a re-partition over fresh damage");
}

// A group whose only records a damaged log no longer holds is never offered for create: bounded
// at its one record's sequence when a manifest names it (its checkpoint record's bytes flipped),
// lost when none does (its segment zeroed past the first block, or the segment gone). It stays
// so through another restart.
void damaged_absent() {
	const j::TurnLogOptions options{64 * 1024, 1024, 1, 4, 4096};
	const std::uint64_t group = 7;
	for (int mode = 0; mode < 3; ++mode) {
		const auto at = " (mode " + std::to_string(mode) + ")";
		fl::filesystem disk;
		{
			auto host = open(disk, 1, options);
			host->create_group(group);
			host->driver(0).stage({group, 1, j::TurnRecordKind::Checkpoint, std::string(3000, 'Q')});
			commit(*host);
			for (std::uint64_t g = 1000; g < 1400; ++g) {
				host->create_group(g);
				host->driver_of(g).stage({g, 1, j::TurnRecordKind::Checkpoint, "filler-" + std::to_string(g) + std::string(900, 'f')});
				if (g % 10 == 9) { commit(*host); }
			}
			commit(*host);
		}
		const auto segment = holder(disk, std::string(200, 'Q'));
		check(!segment.empty(), "the group's record is on disk" + at);
		const std::string bytes(disk.view("host/" + segment));
		const auto record = bytes.find(std::string(200, 'Q'));
		if (mode == 0) { const char flipped = static_cast<char>(bytes[record + 100] ^ 0x5a); overwrite(disk, segment, record + 100, std::string_view(&flipped, 1)); }
		else if (mode == 1) { overwrite(disk, segment, 1024, std::string(bytes.size() - 1024, '\0')); }
		else { auto io = host_io(disk); io->remove(segment); io->sync_directory(); }
		for (int restart = 0; restart < 2; ++restart) {
			auto host = open(disk, 1, options);
			const bool bounded = mode == 0 && host->status(group) == j::GroupStatus::bounded && host->bound(group) == 1 && host->driver(0).records(group).empty();
			check((bounded || host->status(group) == j::GroupStatus::lost) && refused(*host, group), "the group is lost, or bounded with nothing after its bound" + at);
			bool created = false;
			try { host->create_group(group); created = true; } catch (const std::logic_error&) {}
			check(!created, "create refuses it" + at);
		}
	}
}

// A bound stays once retention recycled the damaged segment.
void retention() {
	fl::filesystem disk;
	std::set<std::uint64_t> victims;
	const auto written = populate(disk, 60, victims, small_segments);
	std::uint64_t bound = 0;
	{
		auto host = open(disk, 1, small_segments);
		bound = host->bound(50);
		check(bound && host->driver(0).recovered().damaged, "the damaged group is bounded");
		std::uint64_t sequence = written.at(1).back().sequence;
		for (int turn = 0; turn < 200; ++turn) { host->driver(0).stage({1, ++sequence, j::TurnRecordKind::Batch, std::string(900, 'r')}); commit(*host); }
	}
	auto host = open(disk, 1, small_segments);
	check(!host->driver(0).recovered().damaged, "retention recycled the damaged segment");
	check(host->status(50) == j::GroupStatus::bounded && host->bound(50) == bound, "the table still holds the group bounded");
}
}  // namespace

int main(int argc, char** argv) {
	const std::string only = argc > 1 ? argv[1] : "";
	if (argc > 3) { shard = std::stoull(argv[2]); shards = std::stoull(argv[3]); }  // `remap SHARD SHARDS`
	if (!shards || shard >= shards) { std::cerr << "invalid remap shard\n"; return 2; }
	const std::vector<std::pair<std::string, void (*)()>> cases{{"map", map}, {"remap", remap}, {"repartition_lost", repartition_lost}, {"repartition_damaged", repartition_damaged}, {"retention", retention}, {"damaged_absent", damaged_absent}};
	for (const auto& [name, run] : cases) {
		if (!only.empty() && only != name) { continue; }
		try { run(); } catch (const std::exception& error) { check(false, name + ": " + error.what()); }
	}
	std::cout << checks << " placement checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
