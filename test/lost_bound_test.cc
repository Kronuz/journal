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

// The lost bound (docs/mechanisms/lost-bound.md): one damaged block holding records of several
// groups bounds each group's loss at the highest sequence the block held of it. A reader holding
// a group through its bound continues from the records after it; one that does not loses it; a
// checkpoint kept after the damage loses nothing. The table stores the bound before the host
// serves, so it outlives a crash, a repair of the block and retention, and a second damage only
// raises it. `lost_bound_test [case]`.
#include "faultlab/faultfs.h"
#include "journal/host.h"
#include <algorithm>
#include <iostream>

namespace j = kronuz::journal;
namespace fl = kronuz::faultlab;

namespace {
int checks = 0, failures = 0;
void check(bool value, const std::string& what) {
	++checks;
	if (!value) { ++failures; std::cerr << "FAIL: " << what << '\n'; }
}
const j::Identity host_identity{'B', 'O', 'U', 'N', 'D'};
const j::TurnLogOptions options{256 * 1024, 1024, 1, 4, 4096};
using Records = std::map<std::uint64_t, std::vector<j::TurnRecord>>;

std::shared_ptr<fl::io> host_io(fl::filesystem& disk) {
	const auto names = disk.list();
	if (std::find(names.begin(), names.end(), "host") == names.end()) { disk.make_directory("host"); }
	return std::make_shared<fl::io>(disk, "host");
}
std::unique_ptr<j::Host> open(fl::filesystem& disk) { return std::make_unique<j::Host>(host_io(disk), host_identity, j::HostOptions{1, options}); }
void commit(j::Host& host) { while (!host.driver(0).idle()) { host.driver(0).commit(); } }
// The segment holding `bytes`, and the offset of their first byte.
std::pair<std::string, std::uint64_t> find(fl::filesystem& disk, const std::string& bytes) {
	for (const auto& name : disk.list("host")) {
		if (!name.starts_with("l")) { continue; }
		if (auto at = disk.view("host/" + name).find(bytes); at != std::string_view::npos) { return {name, at}; }
	}
	return {};
}
// The offset just past the last copy of `bytes` in `segment`.
std::uint64_t end_of(fl::filesystem& disk, const std::string& segment, const std::string& bytes) { return disk.view("host/" + segment).rfind(bytes) + bytes.size(); }
void overwrite(fl::filesystem& disk, const std::string& name, std::uint64_t offset, std::string_view bytes) {
	auto file = host_io(disk)->open_existing(name);
	file->write_at(offset, bytes); file->sync();
}
// A run of one letter: a record's payload whose bytes the test finds on disk.
std::string run(char letter, std::size_t size) { return std::string(size, letter); }
std::vector<j::TurnRecord> after(const std::vector<j::TurnRecord>& records, std::uint64_t bound) {
	std::vector<j::TurnRecord> kept;
	for (const auto& record : records) { if (record.sequence > bound) { kept.push_back(record); } }
	return kept;
}
bool refused(j::Host& host, std::uint64_t group) {
	const j::TurnRecord record{group, 1, j::TurnRecordKind::Batch, "again"};
	try { host.driver_of(group).log().stage(std::span<const j::TurnRecord>(&record, 1)); } catch (const std::invalid_argument&) { return true; }
	return false;
}

// Groups 1-6 (1 loop, 1 KiB blocks). Turn A puts a 100-byte batch of groups 1-4, each its
// sequence 2, in one block X, between two batches of group 6 (2,290 and 2,000 bytes) that end in
// X and start in it, so X's manifest names groups 1-4 at 2 and group 6 at 3. After it: group 1 two batches,
// group 3 a checkpoint, group 4 one batch, groups 5 and 6 a checkpoint; group 2 nothing.
struct Layout { Records written; std::string segment; std::uint64_t block = 0, damaged = 0; };
void stage(j::Host& host, Records& written, std::uint64_t group, j::TurnRecordKind kind, std::string payload) {
	auto& records = written[group];
	const std::uint64_t sequence = records.empty() ? 1 : records.back().sequence + 1;
	j::TurnRecord record{group, sequence, kind, std::move(payload), sequence - 1};
	host.driver_of(group).stage(record);
	if (kind == j::TurnRecordKind::Checkpoint) { records.clear(); }
	records.push_back(record);
}
Layout populate(fl::filesystem& disk) {
	Layout layout;
	auto& w = layout.written;
	{
		auto host = open(disk);
		for (std::uint64_t g = 1; g <= 6; ++g) { host->create_group(g); stage(*host, w, g, j::TurnRecordKind::Checkpoint, "checkpoint-" + std::to_string(g)); }
		commit(*host);
		stage(*host, w, 6, j::TurnRecordKind::Batch, run('F', 2290)); commit(*host);
		for (std::uint64_t g = 1; g <= 4; ++g) { stage(*host, w, g, j::TurnRecordKind::Batch, run(static_cast<char>('a' + g), 100)); }
		commit(*host);
		stage(*host, w, 6, j::TurnRecordKind::Batch, run('G', 2000)); commit(*host);
		stage(*host, w, 1, j::TurnRecordKind::Batch, "one-3"); stage(*host, w, 3, j::TurnRecordKind::Checkpoint, "checkpoint-3-again");
		stage(*host, w, 4, j::TurnRecordKind::Batch, "four-3"); stage(*host, w, 5, j::TurnRecordKind::Checkpoint, "checkpoint-5-again");
		stage(*host, w, 6, j::TurnRecordKind::Checkpoint, "checkpoint-6-again"); commit(*host);
		stage(*host, w, 1, j::TurnRecordKind::Batch, "one-4"); commit(*host);
	}
	const auto [segment, at] = find(disk, run('b', 100));
	layout.segment = segment; layout.block = at / options.block_bytes;
	for (std::uint64_t g = 1; g <= 4; ++g) {
		const auto [name, offset] = find(disk, run(static_cast<char>('a' + g), 100));
		check(name == segment && offset / options.block_bytes == layout.block && (offset + 99) / options.block_bytes == layout.block, "turn A's batch of group " + std::to_string(g) + " is in block X");
	}
	const auto f = end_of(disk, segment, run('F', 50)), g = find(disk, run('G', 50)).second;
	check((f - 1) / options.block_bytes == layout.block && g / options.block_bytes == layout.block, "group 6's batches end and start in block X");
	layout.damaged = at + 50;
	return layout;
}
// A payload byte of a record spanning blocks, past its block's header: the middle of a 3,000-byte
// payload starting at `offset` is in a block the record fills.
std::uint64_t middle(std::uint64_t offset) { const auto at = offset + 1500; return at % options.block_bytes < 256 ? at + 300 : at; }
void damage(fl::filesystem& disk, const std::string& segment, std::uint64_t offset) {
	const char flipped = static_cast<char>(disk.view("host/" + segment)[offset] ^ 0x5a);
	overwrite(disk, segment, offset, std::string_view(&flipped, 1));
}

// A1: bounds per group, with values. Readers: group 1 holds through 2, groups 2 and 4 through 1.
void bounds() {
	fl::filesystem disk;
	auto layout = populate(disk);
	auto& w = layout.written;
	damage(disk, layout.segment, layout.damaged);
	auto host = open(disk);
	const auto& recovered = host->driver(0).recovered();
	check(recovered.damaged, "the damage is reported");
	const std::map<std::uint64_t, std::uint64_t> expected{{1, 2}, {2, 2}, {4, 2}};
	std::map<std::uint64_t, std::uint64_t> found;
	for (const auto& [g, b] : recovered.bounded) { found[g] = b.bound; }
	check(found == expected, "recovery bounds groups 1, 2 and 4 at 2, the highest sequence block X held of each");
	for (const auto& [g, b] : expected) {
		check(host->status(g) == j::GroupStatus::bounded && host->bound(g) == b, "group " + std::to_string(g) + " is bounded at 2");
		check(host->driver(0).records(g) == after(w[g], b), "group " + std::to_string(g) + " keeps every record after its bound");
	}
	check(host->driver(0).records(1).size() == 2, "group 1 keeps its batches 3 and 4");
	check(host->status(3) == j::GroupStatus::stored && host->bound(3) == 0 && host->driver(0).records(3) == w[3], "group 3's checkpoint after the damage supersedes it");
	check(host->status(6) == j::GroupStatus::stored && host->driver(0).records(6) == w[6], "group 6's checkpoint after both its damaged batches supersedes them");
	check(host->status(5) == j::GroupStatus::stored && host->driver(0).records(5) == w[5], "group 5, which block X does not hold, is whole");
	// Group 1's reader holds through 2: it continues after its records.
	stage(*host, w, 1, j::TurnRecordKind::Batch, "one-5"); commit(*host);
	check(host->driver(0).sequence(1) == 5, "group 1 continues after its bound and records");
	// Groups 2 and 4: readers holding through 1, below the bound, lose them, loudly and for good.
	for (std::uint64_t g : {2, 4}) {
		host->lose(g);
		check(host->status(g) == j::GroupStatus::lost && refused(*host, g), "group " + std::to_string(g) + "'s reader below the bound loses it");
	}
	host.reset();
	host = open(disk);
	check(host->status(1) == j::GroupStatus::bounded && host->bound(1) == 2 && host->driver(0).records(1) == after(w[1], 2), "a reopen keeps group 1 bounded with its records after the bound");
	for (std::uint64_t g : {2, 4}) { check(host->status(g) == j::GroupStatus::lost, "a lost group stays lost: " + std::to_string(g)); }
	// A checkpoint after the bound supersedes it: the next open clears it.
	stage(*host, w, 1, j::TurnRecordKind::Checkpoint, "checkpoint-1-again"); commit(*host);
	host.reset();
	host = open(disk);
	check(host->status(1) == j::GroupStatus::stored && host->bound(1) == 0 && host->driver(0).records(1) == w[1], "a checkpoint after the bound clears it");
}

// A2: the bound is in the table before the host serves. A power cut at every step of the first
// open after the damage, then a reopen: group 1 is bounded at 2. The block repaired afterwards,
// the log shows no damage, and the table alone keeps the bound and the records through it gone.
void crash() {
	fl::filesystem base;
	auto layout = populate(base);
	damage(base, layout.segment, layout.damaged);
	const std::string original(1, static_cast<char>(base.view("host/" + layout.segment)[layout.damaged] ^ 0x5a));
	std::uint64_t total = 0;
	{
		fl::filesystem probe = base.snapshot();
		const auto start = probe.mutations();
		{ auto host = open(probe); }
		total = probe.mutations() - start;
	}
	check(total > 0, "the open writes");
	for (std::uint64_t cut = 0; cut <= total; ++cut) {
		fl::filesystem disk = base.snapshot();
		const auto at = "cut at " + std::to_string(cut) + " of " + std::to_string(total);
		if (cut < total) {
			disk.power_off_after(cut);
			try { open(disk); } catch (const std::exception&) {}
			disk.crash(cut * 7919 + 1);
		}
		{
			auto host = open(disk);
			check(host->status(1) == j::GroupStatus::bounded && host->bound(1) == 2, "group 1 is bounded at 2 after a cut: " + at);
		}
		overwrite(disk, layout.segment, layout.damaged, original);
		auto host = open(disk);
		check(!host->driver(0).recovered().damaged, "the repaired log shows no damage: " + at);
		check(host->status(1) == j::GroupStatus::bounded && host->bound(1) == 2 && host->driver(0).records(1) == after(layout.written[1], 2), "the table keeps the bound and the records through it gone: " + at);
	}
}

// A2: a second damage. Below the stored bound (group 1's checkpoint record, sequence 1) it
// changes nothing for group 1; above it (a batch at sequence 6) it raises the bound to 6.
void second() {
	fl::filesystem disk;
	auto layout = populate(disk);
	auto& w = layout.written;
	damage(disk, layout.segment, layout.damaged);
	{
		auto host = open(disk);
		check(host->bound(1) == 2, "the first damage bounds group 1 at 2");
		stage(*host, w, 1, j::TurnRecordKind::Batch, "one-5");
		stage(*host, w, 1, j::TurnRecordKind::Batch, run('S', 3000));  // sequence 6, its middle block its own
		commit(*host);
		stage(*host, w, 1, j::TurnRecordKind::Batch, "one-7"); commit(*host);
	}
	const auto [segment, at] = find(disk, "checkpoint-1");
	damage(disk, segment, at + 2);  // a record at or below the bound: superseded
	{
		auto host = open(disk);
		check(host->status(1) == j::GroupStatus::bounded && host->bound(1) == 2 && host->driver(0).records(1) == after(w[1], 2), "damage below the stored bound keeps it");
	}
	const auto [big, offset] = find(disk, run('S', 200));
	damage(disk, big, middle(offset));
	auto host = open(disk);
	check(host->status(1) == j::GroupStatus::bounded && host->bound(1) == 6 && host->driver(0).records(1) == after(w[1], 6), "damage above the stored bound raises it to 6");
}

// A bound outlives retention: once the damaged segment is recycled, the table still bounds the
// group, and a re-partition carries its records after the bound.
void retention() {
	fl::filesystem disk;
	const j::TurnLogOptions small{16 * 1024, 1024, 1, 4, 4096};
	Records w;
	std::string segment; std::uint64_t offset = 0;
	{
		auto host = std::make_unique<j::Host>(host_io(disk), host_identity, j::HostOptions{1, small});
		host->create_group(1); host->create_group(2);
		stage(*host, w, 1, j::TurnRecordKind::Checkpoint, "checkpoint-1"); stage(*host, w, 2, j::TurnRecordKind::Checkpoint, "checkpoint-2"); commit(*host);
		stage(*host, w, 1, j::TurnRecordKind::Batch, run('R', 3000)); commit(*host);
		stage(*host, w, 1, j::TurnRecordKind::Batch, "one-3"); commit(*host);
	}
	std::tie(segment, offset) = find(disk, run('R', 200));
	damage(disk, segment, middle(offset));
	{
		auto host = std::make_unique<j::Host>(host_io(disk), host_identity, j::HostOptions{1, small});
		check(host->status(1) == j::GroupStatus::bounded && host->bound(1) == 2, "the damage bounds group 1 at 2");
		for (int turn = 0; turn < 200; ++turn) { stage(*host, w, 2, j::TurnRecordKind::Batch, run('r', 900)); commit(*host); }
	}
	{
		auto host = std::make_unique<j::Host>(host_io(disk), host_identity, j::HostOptions{1, small});
		check(!host->driver(0).recovered().damaged, "retention recycled the damaged segment");
		check(host->status(1) == j::GroupStatus::bounded && host->bound(1) == 2 && host->driver(0).records(1) == after(w[1], 2), "the table keeps the bound once the damage is gone");
	}
	auto host = std::make_unique<j::Host>(host_io(disk), host_identity, j::HostOptions{3, small});
	check(host->status(1) == j::GroupStatus::bounded && host->bound(1) == 2 && host->driver_of(1).records(1) == after(w[1], 2), "a re-partition carries the records after the bound");
}
}  // namespace

int main(int argc, char** argv) {
	const std::string only = argc > 1 ? argv[1] : "";
	const std::vector<std::pair<std::string, void (*)()>> cases{{"bounds", bounds}, {"crash", crash}, {"second", second}, {"retention", retention}};
	for (const auto& [name, run] : cases) {
		if (!only.empty() && only != name) { continue; }
		try { run(); } catch (const std::exception& error) { check(false, name + ": " + error.what()); }
	}
	std::cout << checks << " lost-bound checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
