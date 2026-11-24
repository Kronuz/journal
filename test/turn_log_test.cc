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

// RFTW0002 (turn_log.h): turns round-trip across blocks, segments, and epochs; the torn
// tail is dropped; damage before a later turn loses only the groups it may have held;
// checkpoints supersede; stale bytes of a recycled file never read as current; each sector
// verdict recovers by value. `turn_log_test [case]`, every case by default.
#include "journal/turn_log.h"
#include "faultlab/faultfs.h"
#include "completion/posix.h"
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>

namespace j = kronuz::journal;
namespace d = j::turn_log_detail;
namespace fl = kronuz::faultlab;

namespace {
int checks = 0, failures = 0;
void check(bool value, const std::string& what) {
	++checks;
	if (!value) { ++failures; if (failures < 100) { std::cerr << "FAIL: " << what << '\n'; } }
}
template <class F> bool throws(F&& f) {
	try { f(); return false; } catch (const std::exception&) { return true; }
}
struct Directory {
	std::filesystem::path path;
	explicit Directory(const std::string& name) : path(std::filesystem::current_path() / ".scratch" / ("turn-log-" + name)) {
		std::filesystem::remove_all(path); std::filesystem::create_directories(path);
		std::filesystem::permissions(path, std::filesystem::perms::owner_all);
	}
	~Directory() { std::filesystem::remove_all(path); }
	std::filesystem::path segment(std::uint64_t number) const { return path / j::turn_log_detail::segment_name(number); }
	void flip(std::uint64_t number, std::uint64_t offset) const {
		std::fstream file(segment(number), std::ios::in | std::ios::out | std::ios::binary);
		file.seekg(static_cast<std::streamoff>(offset)); char c; file.get(c);
		file.seekp(static_cast<std::streamoff>(offset)); file.put(static_cast<char>(c ^ 0x5a));
	}
	void zero(std::uint64_t number, std::uint64_t offset, std::uint64_t count) const {
		std::fstream file(segment(number), std::ios::in | std::ios::out | std::ios::binary);
		file.seekp(static_cast<std::streamoff>(offset)); file << std::string(count, '\0');
	}
};
const j::Identity identity{'T', 'U', 'R', 'N'};
const j::TurnLogOptions small{8 * 512, 512, 1};  // 512-byte blocks, 4 KiB segments
constexpr std::uint64_t sector = 512;
// RFTW0002 positions count sector data bytes. The file also builds against a log without sector
// checks, where positions are file offsets, so its newest-turn cases run there too.
constexpr bool sectors = d::magic[7] == '2';
std::uint64_t file_at(std::uint64_t position) { return sectors ? position / 504 * 512 + position % 504 : position; }
std::uint64_t block_data(std::uint32_t block_bytes) { return sectors ? block_bytes / 512 * 504 : block_bytes; }

using Model = std::map<std::uint64_t, std::vector<j::TurnRecord>>;
// Every group recovery does not call lost holds exactly its acknowledged records.
bool kept(const j::TurnRecovery& r, const Model& model) {
	for (const auto& [group, records] : model) {
		const auto found = r.groups.find(group);
		if (!r.lost.contains(group) && (found == r.groups.end() || found->second != records)) { return false; }
	}
	return true;
}
j::TurnRecord record(std::uint64_t group, std::uint64_t sequence, std::size_t size, j::TurnRecordKind kind = j::TurnRecordKind::Batch) {
	std::string payload(size, '\0');
	for (std::size_t i = 0; i < size; ++i) { payload[i] = static_cast<char>('a' + (group * 7 + sequence * 3 + i) % 26); }
	return {group, sequence, kind, payload, sequence - 1};  // a checkpoint covers the records before it
}
// Appends turns of random groups and sizes, keeping the model of what each group holds.
std::uint64_t workload(j::TurnLog& log, Model& model, std::mt19937_64& random, unsigned turns, std::size_t largest) {
	std::uint64_t last = 0;
	for (unsigned t = 0; t < turns; ++t) {
		std::vector<j::TurnRecord> batch; std::map<std::uint64_t, std::uint64_t> next;
		for (unsigned n = random() % 4 + 1; n; --n) {
			auto group = random() % 5 + 1;
			auto& sequence = next.try_emplace(group, log.sequence(group)).first->second;
			batch.push_back(record(group, ++sequence, random() % largest));
		}
		last = log.append(batch);
		for (auto& r : batch) { model[r.group].push_back(r); }
	}
	return last;
}

void round_trip() {
	Directory directory("round-trip"); j::PosixIO io(directory.path); auto owner = io.acquire_owner(true);
	j::TurnRecovery recovered; Model model; std::mt19937_64 random(1);
	std::uint64_t last = 0;
	{
		j::TurnLog log(io, identity, recovered, small);
		check(recovered.epoch == 1 && recovered.last_turn == 0 && recovered.groups.empty(), "an empty directory opens the first epoch");
		last = workload(log, model, random, 60, 900);  // records cross blocks and turns cross segments
		check(log.segment() > 3 && log.stats().barriers == 61 && log.stats().turns == 60, "one barrier per turn, plus the open record's");
	}
	for (std::uint64_t epoch = 2; epoch <= 3; ++epoch) {
		j::TurnLog log(io, identity, recovered, small);
		check(recovered.epoch == epoch && recovered.last_turn == last && !recovered.damaged && recovered.lost.empty(), "reopening keeps every turn");
		check(recovered.groups == model, "every group's records read back in sequence");
		last = workload(log, model, random, 10, 300);
	}
}

void torn_tail() {
	Directory directory("torn"); j::PosixIO io(directory.path); auto owner = io.acquire_owner(true);
	j::TurnRecovery recovered; Model model; std::mt19937_64 random(2);
	std::uint64_t segment = 0, start = 0, before = 0;
	{
		j::TurnLog log(io, identity, recovered, small);
		workload(log, model, random, 5, 100);
		before = log.last_turn(); start = log.position();
		log.append(std::vector{record(9, 1, 700)});  // crosses a sector
		segment = log.segment();
	}
	// The last turn's last sector never reached the disk: power was lost before its barrier, and
	// the sector reads as before the turn, zeros in a new segment.
	auto size = std::filesystem::file_size(directory.segment(segment));
	std::uint64_t written = 0;
	{
		std::ifstream file(directory.segment(segment), std::ios::binary); std::string bytes(size, '\0'); file.read(bytes.data(), static_cast<std::streamsize>(size));
		written = bytes.find_last_not_of('\0') + 1;
	}
	const auto lost = (written - 1) / sector * sector;
	directory.zero(segment, lost, written - lost);
	j::TurnLog log(io, identity, recovered, small);
	check(recovered.last_turn == before && !recovered.groups.contains(9) && !recovered.damaged && recovered.groups == model, "a torn last turn is dropped whole");
	check(file_at(start) < lost, "the torn turn spans more than the lost sector");
}

void torn_turn_in_earlier_epoch() {
	Directory directory("torn-earlier"); j::PosixIO io(directory.path); auto owner = io.acquire_owner(true);
	j::TurnRecovery recovered; Model model; std::mt19937_64 random(8);
	std::uint64_t kept = 0;
	{
		j::TurnLog log(io, identity, recovered, small);
		for (std::uint64_t s = 1; s <= 3; ++s) { auto r = record(1, s, 40); log.append(std::vector{r}); model[1].push_back(r); }
		kept = log.sequence(1);
		// The turn crosses into the next block; only the bytes from that block on persist.
		auto write = log.stage(std::vector{record(1, kept + 1, 700)});
		const auto boundary = (write.offset / small.block_bytes + 1) * small.block_bytes;
		check(boundary < write.offset + write.bytes.size(), "the torn turn crosses a block boundary");
		auto tail = write.bytes.substr(static_cast<std::size_t>(boundary - write.offset));
		check(write.file->write_at(boundary, tail) == tail.size(), "the torn turn's tail is written");
		write.file->sync_data();
	}
	{
		j::TurnLog log(io, identity, recovered, small);
		check(!recovered.damaged && recovered.lost.empty() && recovered.groups == model, "the torn last turn is dropped whole");
		for (std::uint64_t s = 1; s <= 3; ++s) { auto r = record(2, s, 40); log.append(std::vector{r}); model[2].push_back(r); }
	}
	j::TurnLog log(io, identity, recovered, small);
	check(!recovered.damaged && recovered.lost.empty() && recovered.groups == model, "an earlier epoch's torn turn costs no group later");
}

void damage() {
	Directory directory("damage"); j::PosixIO io(directory.path); auto owner = io.acquire_owner(true);
	j::TurnRecovery recovered; std::uint64_t at = 0;
	{
		j::TurnLog log(io, identity, recovered, small);
		log.append(std::vector{record(4, 1, 20)});                   // intact, but nothing later
		log.append(std::vector{record(6, 1, 760)});                  // ends in the damaged block
		at = log.position();
		log.append(std::vector{record(1, 1, 300), record(2, 1, 300)});  // damaged below
		log.append(std::vector{record(3, 1, 300)});
		log.append(std::vector{record(1, 2, 300)});
	}
	directory.flip(1, file_at(at + 10));  // inside turn 2's first fragment header
	j::TurnLog log(io, identity, recovered, small);
	check(recovered.damaged && recovered.lost.contains(1), "the groups with records in the damaged block are lost");
	check(recovered.lost.contains(2) || recovered.groups[2] == std::vector{record(2, 1, 300)}, "a record of the damaged turn that reads back whole is kept");
	check(recovered.groups.contains(4) && !recovered.lost.contains(4), "a group without records in it is kept: the next block's manifest names the damaged block's groups");
	check(recovered.lost.contains(6), "a group whose record ends in the damaged block is lost with it");
	check(recovered.groups.contains(3) && recovered.groups[3] == std::vector{record(3, 1, 300)}, "a group whose records follow the damage is kept");
	check(throws([&] { log.append(std::vector{record(1, 3, 10)}); }), "a lost group rejoins under a new id");
}
// A damaged block whose next header, its manifest, is damaged too, hiding every later turn: no
// crash leaves that header, so recovery reports the damage instead of reading a torn tail.
void double_damage() {
	Directory directory("double-damage"); j::PosixIO io(directory.path); auto owner = io.acquire_owner(true);
	const j::TurnLogOptions options{16 * 1024, 1024, 1, 4, 4096};
	j::TurnRecovery recovered; Model model; std::uint64_t block = 0, end = 0;
	{
		j::TurnLog log(io, identity, recovered, options);
		std::vector<j::TurnRecord> first;
		for (std::uint64_t g = 1; g <= 6; ++g) { first.push_back(record(g, 1, 2, j::TurnRecordKind::Checkpoint)); }
		log.append(first);
		log.append(std::vector{record(1, 2, 3000)});  // ends in `block`, which later turns share
		block = log.position() / block_data(options.block_bytes);
		for (std::uint64_t s = 2; s < 10; ++s) { log.append(std::vector{record(6, s, 6)}); }
		end = log.position();
		for (auto& r : first) { model[r.group].push_back(r); }
		model[1].push_back(record(1, 2, 3000));
		for (std::uint64_t s = 2; s < 10; ++s) { model[6].push_back(record(6, s, 6)); }
	}
	check(end / block_data(options.block_bytes) == block + 1, "the last turns end in the block after the damaged one");
	directory.flip(1, block * options.block_bytes + 512);  // the 3000-byte record's last fragment
	directory.flip(1, (block + 1) * options.block_bytes + 10);  // the next header's identity
	j::TurnLog log(io, identity, recovered, options);
	check(recovered.damaged, "a damaged block followed by a damaged header reports damage");
	bool kept = true;
	for (const auto& [group, records] : model) { if (!recovered.lost.contains(group) && recovered.groups[group] != records) { kept = false; } }
	check(kept, "a group not lost keeps every acknowledged record");
}
// Only a header damaged, inside the last turn: the turn is dropped, so a group whose record in it
// reads back whole is lost with the groups the next header names for the damaged block.
void damaged_header_last_turn() {
	Directory directory("damaged-header"); j::PosixIO io(directory.path); auto owner = io.acquire_owner(true);
	j::TurnRecovery recovered; std::uint64_t end = 0;
	{
		j::TurnLog log(io, identity, recovered, small);
		log.append(std::vector{record(1, 1, 2, j::TurnRecordKind::Checkpoint), record(2, 1, 2, j::TurnRecordKind::Checkpoint)});
		log.append(std::vector{record(1, 2, 50), record(2, 2, 900)});  // group 1's whole in block 0, group 2's to block 2
		end = log.position();
	}
	check(end > 2 * block_data(small.block_bytes), "the last turn crosses block 1");
	directory.flip(1, small.block_bytes + 10);  // block 1's header
	j::TurnLog log(io, identity, recovered, small);
	check(recovered.damaged, "a damaged header inside the last turn is reported");
	check(recovered.lost.contains(1) || recovered.groups[1] == std::vector{record(1, 1, 2, j::TurnRecordKind::Checkpoint), record(1, 2, 50)}, "a group with a record in the dropped turn keeps it or is lost");
	check(recovered.lost.contains(2), "the group the next header names for the damaged block is lost");
}
// The same double damage before later turns, where a group's only record after it is a
// forwarded copy of an older one: the copy shows nothing of what the group wrote there.
void double_damage_copy() {
	Directory directory("double-damage-copy"); j::PosixIO io(directory.path); auto owner = io.acquire_owner(true);
	j::TurnRecovery recovered; std::uint64_t segment = 0, at = 0, s = 0;
	{
		j::TurnLog log(io, identity, recovered, small);
		log.append(std::vector{record(9, 1, 100)});  // copied forward once its segment is the oldest
		auto checkpoint = [&](std::size_t size) { ++s; log.append(std::vector{record(1, s, size, j::TurnRecordKind::Checkpoint)}); };
		while (log.segment() == 1) { checkpoint(300); }
		while (log.position() % block_data(small.block_bytes) < 80 || log.position() % block_data(small.block_bytes) > 300) { checkpoint(10); }
		segment = log.segment(); at = log.position();
		log.append(std::vector{record(9, 2, 100)});  // lost to the damage below
		while (*log.data_segments().begin() != segment) { checkpoint(300); }
		check(log.stats().forwarded_records == 1, "only the group's first record moved forward");
	}
	check(at / block_data(small.block_bytes) + 1 < small.segment_bytes / small.block_bytes, "the next block is in the segment");
	directory.flip(segment, file_at(at + 30));  // the second record's fragment
	directory.flip(segment, (at / block_data(small.block_bytes) + 1) * small.block_bytes + 10);  // the next header's identity
	j::TurnLog log(io, identity, recovered, small);
	check(recovered.damaged, "the double damage is reported");
	check(recovered.lost.contains(9) || recovered.groups[9] == std::vector{record(9, 1, 100), record(9, 2, 100)}, "a forwarded copy does not show a group kept its records");
}

// Damage to an acknowledged turn before the newest, in the newest segment, on the cut-header
// probe's workload: a byte in each sector written before the newest write's. Recovery reports it,
// and a group it keeps holds every acknowledged record.
void earlier_turn_damage(std::uint32_t block_bytes, std::uint64_t seed) {
	fl::filesystem disk; fl::io io(disk);
	const j::TurnLogOptions options{8 * std::uint64_t{block_bytes}, block_bytes, 1, 3, 600};
	j::TurnRecovery recovered; Model model; std::mt19937_64 random(seed);
	std::uint64_t newest = 0, limit = 0;
	{
		j::TurnLog log(io, identity, recovered, options);
		for (unsigned t = 0; t < 60; ++t) {
			std::vector<j::TurnRecord> batch; std::map<std::uint64_t, std::uint64_t> next;
			for (auto n = random() % 3 + 1; n; --n) {
				const auto group = random() % 6 + 1;
				auto& sequence = next.try_emplace(group, log.sequence(group)).first->second;
				const std::size_t size = random() % 700 + 1;
				batch.push_back(record(group, ++sequence, size, random() % 5 ? j::TurnRecordKind::Batch : j::TurnRecordKind::Checkpoint));
			}
			const auto segment = log.segment(), position = log.position();
			log.append(batch); log.prepare();
			for (const auto& r : batch) { if (r.kind == j::TurnRecordKind::Checkpoint) { model[r.group].clear(); } model[r.group].push_back(r); }
			newest = log.segment(); limit = segment == newest ? file_at(position) / sector * sector : 0;  // the sectors before the newest write's
		}
	}
	j::TurnLogGroups known; for (const auto& [group, _] : model) { known.open.push_back(group); }
	const auto name = j::turn_log_detail::segment_name(newest);
	for (std::uint64_t from = 0; from < limit; from += sector) {
		for (const auto offset : {from + sector / 2, from + sector - 1}) {
			auto damaged = disk.snapshot(); fl::io damaged_io(damaged);
			{
				auto file = damaged_io.open_existing(name); char byte = 0; file->read_at(offset, std::span<char>(&byte, 1));
				byte = static_cast<char>(byte ^ 0x5a); file->write_at(offset, std::string_view(&byte, 1)); file->sync();
			}
			const auto r = j::TurnLog::read(damaged_io, identity, options, known);
			if (!r.damaged || !kept(r, model)) { std::cerr << "  block_bytes " << block_bytes << " seed " << seed << " segment " << newest << " offset " << offset << '\n'; }
			check(r.damaged, "damage to an earlier turn is reported");
			check(kept(r, model), "a group recovery keeps holds every acknowledged record");
		}
	}
	check(limit > 0, "the newest segment holds turns before the newest");
}
void earlier_turn_damage() { earlier_turn_damage(4096, 4); earlier_turn_damage(4096, 6); earlier_turn_damage(2048, 22); }
// A block header larger than its first sector (a manifest of 28 or more groups): damage to it is
// reported, and the turns later in its block are kept or their groups lost.
void large_header_damage() {
	Directory directory("large-header"); j::PosixIO io(directory.path); auto owner = io.acquire_owner(true);
	const j::TurnLogOptions options{32 * 2048, 2048, 1};
	j::TurnRecovery recovered; Model model;
	{
		j::TurnLog log(io, identity, recovered, options);
		std::vector<j::TurnRecord> first;
		for (std::uint64_t g = 1; g <= 40; ++g) { first.push_back(record(g, 1, 20, j::TurnRecordKind::Checkpoint)); }
		log.append(first);  // block 0's groups: block 1's header names them
		log.append(std::vector{record(41, 1, 20)}); log.append(std::vector{record(41, 2, 20)});
		check(log.position() / block_data(options.block_bytes) == 1, "the later turns share block 1");
		for (const auto& r : first) { model[r.group].push_back(r); }
		model[41] = {record(41, 1, 20), record(41, 2, 20)};
	}
	std::string header(64, '\0');
	{ std::ifstream file(directory.segment(1), std::ios::binary); file.seekg(options.block_bytes); file.read(header.data(), 64); }
	const auto size = 58 + 16 * std::size_t{j::framing::get16(std::string_view(header).substr(56))};
	check(size > sector + 100, "block 1's header crosses its first sector");
	directory.flip(1, options.block_bytes + sector + 50);  // a manifest entry in its second sector
	j::TurnLogGroups known; for (const auto& [group, _] : model) { known.open.push_back(group); }
	const auto r = j::TurnLog::read(io, identity, options, known);
	check(r.damaged, "damage to a header before later turns of its block is reported");
	check(kept(r, model), "a group recovery keeps holds every acknowledged record");
}
// The same header written by a torn newest turn, its second sector left old: the turn is dropped
// as a tear.
void large_header_tear() {
	Directory directory("large-header-tear"); j::PosixIO io(directory.path); auto owner = io.acquire_owner(true);
	const j::TurnLogOptions options{32 * 2048, 2048, 1};
	j::TurnRecovery recovered; Model model;
	{
		j::TurnLog log(io, identity, recovered, options);
		log.append(std::vector{record(50, 1, 10)}); model[50] = {record(50, 1, 10)};
		std::vector<j::TurnRecord> torn;
		for (std::uint64_t g = 1; g <= 40; ++g) { torn.push_back(record(g, 1, 20, j::TurnRecordKind::Checkpoint)); }
		torn.push_back(record(41, 1, 600));  // from block 1's second sector into its third
		auto write = log.stage(torn);
		const auto old_from = options.block_bytes + sector, old_to = old_from + sector;
		check(write.offset < old_from && write.offset + write.bytes.size() > old_to + 100, "the torn turn spans block 1's second sector and more");
		const auto bytes = std::string_view(write.bytes);
		check(write.file->write_at(write.offset, bytes.substr(0, old_from - write.offset)) == old_from - write.offset, "the sectors before the old one are written");
		check(write.file->write_at(old_to, bytes.substr(old_to - write.offset)) == bytes.size() - (old_to - write.offset), "the sectors after it are written");
		write.file->sync_data();
	}
	const auto r = j::TurnLog::read(io, identity, options);
	check(!r.damaged && r.lost.empty() && r.groups == model, "a torn turn whose own later bytes survive reads as a tear");
}

// A log's segment files, by name, on a memory disk.
using Files = std::map<std::string, std::string>;
Files files(j::IO& io) {
	Files result; auto cursor = io.scan_directory();
	while (auto name = cursor->next()) {
		if (!d::segment_number(*name)) { continue; }
		auto file = io.open_existing(*name); std::string bytes(file->size(), '\0'); file->read_at(0, bytes); result[*name] = std::move(bytes);
	}
	return result;
}
void put(j::IO& io, const std::string& name, std::uint64_t offset, std::string_view bytes) { auto file = io.open_existing(name); file->write_at(offset, bytes); file->sync(); }
void acknowledge(Model& model, const std::vector<j::TurnRecord>& batch) {
	for (const auto& r : batch) { if (r.kind == j::TurnRecordKind::Checkpoint) { model[r.group].clear(); } model[r.group].push_back(r); }
}
// The cut-header probe's turn: one to three of six groups, records up to 700 bytes, one in five a checkpoint.
std::vector<j::TurnRecord> probe_turn(const j::TurnLog& log, std::mt19937_64& random) {
	std::vector<j::TurnRecord> batch; std::map<std::uint64_t, std::uint64_t> next;
	for (auto n = random() % 3 + 1; n; --n) {
		const auto group = random() % 6 + 1;
		auto& sequence = next.try_emplace(group, log.sequence(group)).first->second;
		const std::size_t size = random() % 700 + 1;
		batch.push_back(record(group, ++sequence, size, random() % 5 ? j::TurnRecordKind::Batch : j::TurnRecordKind::Checkpoint));
	}
	return batch;
}
// The newest turn W of a log opened three times (the cut-header probe's newest tail): a byte flipped
// at three places in each sector W changed is reported, or recovery keeps every acknowledged record,
// W's included; never silent. Every set of W's changed sectors left as they were reads as a tear.
// Without sector checks, damage in a fragment that crosses a sector drops W silently.
void newest_turn_damage(std::uint32_t block_bytes, std::uint64_t seed, std::map<std::string, unsigned>& tally) {
	const j::TurnLogOptions options{8 * std::uint64_t{block_bytes}, block_bytes, 1, 3, 600};
	const auto at = "seed " + std::to_string(seed) + " block_bytes " + std::to_string(block_bytes);
	fl::filesystem disk; fl::io io(disk); Model acknowledged, with; std::mt19937_64 random(seed);
	Files before, after; std::optional<fl::filesystem> pre;
	for (int open = 0; open < 3; ++open) {
		j::TurnRecovery recovered; j::TurnLog log(io, identity, recovered, options);
		check(!recovered.damaged && recovered.groups == acknowledged, "a reopened log holds what was acknowledged: " + at);
		for (unsigned n = open < 2 ? 20 : static_cast<unsigned>(seed % 4) * 7; n; --n) { auto b = probe_turn(log, random); log.append(b); acknowledge(acknowledged, b); log.prepare(); }
		if (open < 2) { continue; }
		before = files(io); pre.emplace(disk.snapshot());
		auto w = probe_turn(log, random); log.append(w);
		after = files(io); with = acknowledged; acknowledge(with, w);
	}
	std::vector<std::pair<std::string, std::uint64_t>> changed;
	for (const auto& [name, bytes] : after) {
		const auto found = before.find(name);
		for (std::uint64_t s = 0; s * sector < bytes.size(); ++s) {
			if (found == before.end() || found->second.compare(s * sector, sector, bytes, s * sector, sector)) { changed.push_back({name, s}); }
		}
	}
	if (changed.empty() || !before.contains(changed.back().first)) { ++tally["runs where W opens a segment"]; return; }  // no old sectors to leave
	j::TurnLogGroups known; for (const auto& [group, _] : with) { known.open.push_back(group); }
	for (const auto& [name, s] : changed) {
		for (int n = 0; n < 3; ++n) {
			auto damaged = disk.snapshot(); fl::io damaged_io(damaged);
			const auto offset = s * sector + random() % sector;
			const char byte = static_cast<char>(after.at(name)[offset] ^ 0x5a); put(damaged_io, name, offset, std::string_view(&byte, 1));
			const auto r = j::TurnLog::read(damaged_io, identity, options, known);
			const bool reported = r.damaged && kept(r, with), harmless = !r.damaged && r.groups == with && r.lost.empty();
			++tally[reported ? "damage reported" : harmless ? "damage harmless" : r.damaged ? "damage reported, a group wrong" : "damage SILENT"];
			check(reported || harmless, "damage to the newest turn is reported, never silent: " + at + " " + name + " offset " + std::to_string(offset));
		}
	}
	const auto k = changed.size();
	std::vector<std::uint64_t> masks;
	if (k <= 5) { for (std::uint64_t m = 0; m + 1 < (std::uint64_t{1} << k); ++m) { masks.push_back(m); } }
	else { for (int i = 0; i < 32; ++i) { masks.push_back(random() & ((k >= 64 ? ~0ull : (1ull << k) - 1) & ~(1ull << (random() % k)))); } }
	for (const auto mask : masks) {  // the sectors in the mask reach the disk; the others stay as before W
		auto torn = pre->snapshot(); fl::io torn_io(torn);
		for (std::size_t i = 0; i < k; ++i) {
			const auto& [name, s] = changed[i];
			if (mask >> i & 1) { put(torn_io, name, s * sector, std::string_view(after.at(name)).substr(s * sector, sector)); }
		}
		const auto r = j::TurnLog::read(torn_io, identity, options);
		const bool tear = !r.damaged && r.lost.empty() && (r.groups == acknowledged || r.groups == with);
		++tally[tear ? "tears read as tears" : "tears read as damage or loss"];
		check(tear, "a tear of the newest turn reads as a tear: " + at + " mask " + std::to_string(mask) + " of " + std::to_string(k));
	}
}
void newest_turn_damage() {
	std::map<std::string, unsigned> tally;
	for (std::uint32_t block : {2048u, 4096u}) { for (std::uint64_t seed = 1; seed <= 24; ++seed) { newest_turn_damage(block, seed, tally); } }
	for (const auto& [what, count] : tally) { std::cout << "newest turn: " << what << ": " << count << '\n'; }
	check(tally["damage reported"] > 100 && tally["tears read as tears"] > 100, "the newest turn's damage and tears occur");
}

// D4's sector verdicts, each recovered by value. The newest turn W spans sectors past the one it
// shares with the turn before: each of W's sectors left as before W (zeros), as all zeros, as a
// recycled file's stale sector, or damaged.
void sector_verdicts() {
	const j::TurnLogOptions options{8 * 2048, 2048, 1, 3, 600};
	fl::filesystem disk; fl::io io(disk); Model acknowledged, with; Files before, after; std::optional<fl::filesystem> pre;
	std::uint64_t segment = 0, kept_turn = 0, w_turn = 0, first = 0, last = 0;
	{
		j::TurnRecovery recovered; j::TurnLog log(io, identity, recovered, options);
		while (log.segment() == 1 || log.position() < 900) { auto b = std::vector{record(1, log.sequence(1) + 1, 300)}; log.append(b); acknowledge(acknowledged, b); }
		segment = log.segment(); kept_turn = log.last_turn(); before = files(io); pre.emplace(disk.snapshot());
		const auto w = std::vector{record(2, 1, 1400), record(3, 1, 20)};
		const auto start = file_at(log.position());
		w_turn = log.append(w); after = files(io); with = acknowledged; acknowledge(with, w);
		first = start / sector; last = (file_at(log.position()) - 1) / sector;
	}
	const auto name = d::segment_name(segment);
	check(last >= first + 3, "W spans four sectors or more");
	// W's sectors on the disk as it was before W, then `change`: what a crash may leave, before the
	// barrier lets the log reclaim anything.
	// The host holds every group open: damage loses those recovery holds nothing of.
	j::TurnLogGroups known; for (const auto& [group, _] : with) { known.open.push_back(group); }
	auto read = [&](const std::function<void(j::IO&)>& change) {
		auto copy = pre->snapshot(); fl::io copy_io(copy);
		for (auto s = first; s <= last; ++s) { put(copy_io, name, s * sector, std::string_view(after.at(name)).substr(s * sector, sector)); }
		change(copy_io);
		return j::TurnLog::read(copy_io, identity, options, known);
	};
	auto tear = [&](const j::TurnRecovery& r) { return !r.damaged && r.lost.empty() && r.groups == acknowledged && r.last_turn == kept_turn; };
	auto old = [&](std::uint64_t s) { return std::string_view(before.at(name)).substr(s * sector, sector); };
	const auto current = read([](j::IO&) {});
	check(!current.damaged && current.lost.empty() && current.groups == with && current.last_turn == w_turn, "current: every sector this segment wrote reads, W included");
	check(tear(read([&](j::IO& io) { put(io, name, first * sector, old(first)); })), "torn: W's first sector, rewritten from the turn before's data, left old");
	check(tear(read([&](j::IO& io) { put(io, name, (first + 2) * sector, old(first + 2)); })), "torn: a later sector of W left unwritten");
	check(tear(read([&](j::IO& io) { put(io, name, last * sector, std::string(sector, '\0')); })), "all-zero: W's last sector reads as never written, not as damage");
	// A recycled file holds the sectors of its earlier use: segment 1's sector at the same index.
	const auto stale = std::string_view(after.at(d::segment_name(1))).substr((first + 1) * sector, sector);
	check(stale.find_first_not_of('\0') != std::string_view::npos, "segment 1 wrote the sector at that index");
	check(tear(read([&](j::IO& io) { put(io, name, (first + 1) * sector, stale); })), "stale: a recycled file's sector of an earlier segment ends the data");
	for (const auto s : {first, first + 1, last}) {
		for (const auto offset : {std::uint64_t{0}, std::uint64_t{300}, std::uint64_t{sector - 1}}) {
			const auto at = s * sector + offset;
			const char byte = static_cast<char>(after.at(name)[at] ^ 0x5a);
			const auto r = read([&](j::IO& io) { put(io, name, at, std::string_view(&byte, 1)); });
			check(r.damaged && r.lost.contains(2) && kept(r, with), "damaged: a flipped byte in W's sector " + std::to_string(s - first) + " at " + std::to_string(offset) + " is reported, W's groups lost");
		}
	}
}
// A3 of the format (D2): a recycled file of segment S that is not zeroed holds, at the index of
// one of W's later sectors, the sector segment S - 2^32 wrote there, its data the same as W's. Its
// stored low 32 bits are S's; only the full 64 bits in its CRC make it stale, so W reads as torn.
void stale_alias() {
	if (!sectors) { return; }  // positions bind no sector without sector checks
	const j::TurnLogOptions options{8 * 2048, 2048, 1, 3, 600};
	const std::uint64_t low = 5, high = low + (std::uint64_t{1} << 32);
	struct Log { fl::filesystem disk; Files files; std::uint64_t first = 0, last = 0, turn = 0; };
	auto write = [&](std::uint64_t segment) {
		Log out; fl::io io(out.disk);
		io.create_exclusive(d::segment_name(segment - 1)); io.sync_directory();  // the log opens `segment` next
		j::TurnRecovery recovered; j::TurnLog log(io, identity, recovered, options);
		check(log.segment() == segment, "the log writes the segment asked");
		log.append(std::vector{record(1, 1, 300)});
		const auto start = file_at(log.position());
		out.turn = log.append(std::vector{record(2, 1, 1400)});
		out.first = start / sector; out.last = (file_at(log.position()) - 1) / sector; out.files = files(io);
		return out;
	};
	auto older = write(low), newer = write(high);
	const std::uint64_t next = newer.first + 1, s = next % 4 ? next : next + 1;  // a later sector of W that holds no block header
	const auto name = d::segment_name(high);
	const auto alias = std::string_view(older.files.at(d::segment_name(low))).substr(s * sector, sector);
	const auto own = std::string_view(newer.files.at(name)).substr(s * sector, sector);
	check(s < newer.last && alias.substr(0, 504) == own.substr(0, 504) && alias.substr(504, 4) == own.substr(504, 4), "the stale sector holds W's data and S's low 32 bits");
	fl::io io(newer.disk); put(io, name, s * sector, alias);
	const auto r = j::TurnLog::read(io, identity, options);
	check(!r.damaged && r.lost.empty() && r.last_turn == newer.turn - 1 && !r.groups.contains(2) && r.groups.at(1) == std::vector{record(1, 1, 300)}, "a sector of segment S - 2^32 reads as stale, never as current: W is torn");
}

void checkpoints_and_guards() {
	Directory directory("checkpoints"); j::PosixIO io(directory.path); auto owner = io.acquire_owner(true);
	j::TurnRecovery recovered;
	{
		j::TurnLog log(io, identity, recovered, small);
		log.append(std::vector{record(1, 1, 10), record(1, 2, 10)});
		log.append(std::vector{record(1, 3, 10, j::TurnRecordKind::Checkpoint), record(1, 4, 10, j::TurnRecordKind::HardState)});
		log.append(std::vector{record(7, 40, 10, j::TurnRecordKind::Checkpoint)});  // a group starting from a checkpoint
		check(throws([&] { log.append(std::vector{record(1, 6, 10)}); }), "a group's sequence is contiguous");
		check(throws([&] { log.append(std::vector{record(0, 1, 10)}); }), "group 0 is reserved");
		check(throws([&] { log.append(std::vector{record(8, 3, 10)}); }), "a new group starts at 1 or with a checkpoint");
		check(throws([&] { log.append(std::vector{record(2, 1, 5000)}); }) && log.last_turn() == 3, "a turn larger than a segment is refused before any write");
	}
	j::TurnLog log(io, identity, recovered, small);
	check(recovered.groups[1] == std::vector{record(1, 3, 10, j::TurnRecordKind::Checkpoint), record(1, 4, 10, j::TurnRecordKind::HardState)},
		"a checkpoint supersedes what precedes it");
	check(recovered.groups[7].size() == 1 && log.sequence(7) == 40, "a group may start from a checkpoint");
	check(throws([] { j::TurnLogOptions bad{1000, 512, 1}; Directory d("bad"); j::PosixIO io(d.path); auto o = io.acquire_owner(true); j::TurnRecovery r; j::TurnLog l(io, identity, r, bad); }),
		"a segment is a whole number of blocks");
}

void stale_bytes() {
	Directory directory("stale"); j::PosixIO io(directory.path); auto owner = io.acquire_owner(true);
	j::TurnRecovery recovered; Model model; std::mt19937_64 random(3);
	{
		j::TurnLog log(io, identity, recovered, small);
		workload(log, model, random, 8, 200);
	}
	// A recycled file keeps its earlier bytes under a later name: its blocks name another segment.
	auto spare = std::filesystem::path(directory.path);
	std::uint64_t highest = 0;
	for (auto& entry : std::filesystem::directory_iterator(directory.path)) {
		if (auto n = j::turn_log_detail::segment_number(entry.path().filename().string())) { highest = std::max(highest, *n); }
	}
	std::filesystem::copy_file(directory.segment(1), directory.segment(highest + 1));
	j::TurnLog log(io, identity, recovered, small);
	check(!recovered.damaged && recovered.groups == model, "a recycled file's earlier bytes are not current");
}

// Groups that checkpoint let their segments go; an idle group's record moves forward
// instead of pinning the oldest segment; reopening after reclamation reads the same state.
void reclamation(std::uint64_t seed) {
	Directory directory("reclaim"); j::PosixIO io(directory.path); auto owner = io.acquire_owner(true);
	j::TurnRecovery recovered; Model model; std::mt19937_64 random(seed);
	j::TurnLogOptions options = small; options.retain_segments = 3; options.forward_bytes = 600;
	auto run = [&](j::TurnLog& log, unsigned turns) {
		for (unsigned t = 0; t < turns; ++t) {
			std::vector<j::TurnRecord> batch;
			auto group = random() % 4 + 1; auto sequence = log.sequence(group) + 1;
			auto kind = random() % 5 == 0 ? j::TurnRecordKind::Checkpoint : j::TurnRecordKind::Batch;
			batch.push_back(record(group, sequence, random() % 400 + 1, kind));
			log.append(batch);
			if (kind == j::TurnRecordKind::Checkpoint) { model[group].clear(); }
			model[group].push_back(batch.back());
		}
	};
	{
		j::TurnLog log(io, identity, recovered, options);
		log.append(std::vector{record(9, 1, 100)}); model[9].push_back(record(9, 1, 100));  // never writes again
		run(log, 150);
		check(log.data_segments().size() <= options.retain_segments + 1, "segments stay within the retention bound");
		check(*log.data_segments().begin() > 1 && log.stats().forwarded_records > 0, "the idle group's record moved forward and the first segment went");
		check(log.stats().segments_recycled > 0, "released segments are recycled into the spare pool");
	}
	for (unsigned round = 0; round < 3; ++round) {
		j::TurnLog log(io, identity, recovered, options);
		check(!recovered.damaged && recovered.lost.empty() && recovered.groups == model, "reopening after reclamation reads every group's live records");
		run(log, 60);
	}
	std::size_t files = 0;
	for (auto& entry : std::filesystem::directory_iterator(directory.path)) { files += j::turn_log_detail::segment_number(entry.path().filename().string()).has_value(); }
	check(files <= options.retain_segments + 1 + options.spare_segments + 1, "segment files stay bounded across reopenings");
}

// The two-step append a completion driver uses: one turn in flight; nothing visible until
// its barrier returns; a failed write fails the log.
void staged_turns() {
	Directory directory("staged"); j::PosixIO io(directory.path); auto owner = io.acquire_owner(true);
	j::TurnRecovery recovered;
	{
		j::TurnLog log(io, identity, recovered, small);
		auto write = log.stage(std::vector{record(1, 1, 100)});
		check(log.sequence(1) == 0 && log.last_turn() == 0 && write.turn == 1, "a staged turn changes nothing visible");
		check(throws([&] { log.stage(std::vector{record(2, 1, 10)}); }), "one turn is in flight at a time (condition 7)");
		check(write.file->write_at(write.offset, write.bytes) == write.bytes.size(), "the host writes the staged bytes");
		write.file->sync_data(); log.completed();
		check(log.sequence(1) == 1 && log.last_turn() == 1 && throws([&] { log.completed(); }), "completion makes the turn visible, once");
		log.stage(std::vector{record(1, 2, 10)}); log.failed();
		check(throws([&] { log.append(std::vector{record(1, 2, 10)}); }), "a failed write fails the log");
	}
	j::TurnLog log(io, identity, recovered, small);
	check(recovered.groups[1] == std::vector{record(1, 1, 100)}, "the completed turn recovers; the failed one does not");
}
// Reads while a turn's write is in flight (Host::park mid-turn, a Store's live()): the write is part
// way through the sector it shares with the turn before, half its new bytes copied, and every
// group still reads back its acknowledged records.
void midwrite_reads() {
	const std::uint64_t unit = sectors ? 504 : 512;
	unsigned cases = 0;
	for (std::uint32_t block : {2048u, 32768u}) {
		for (std::uint64_t seed = 1; seed <= 4; ++seed) {
			fl::filesystem disk; fl::io io(disk); j::TurnRecovery recovered; std::mt19937_64 random(seed); Model model;
			j::TurnLog log(io, identity, recovered, {8 * std::uint64_t{block}, block, 1, 3, 600});
			for (unsigned n = 0; n < 100; ++n) {
				const auto batch = probe_turn(log, random);
				const auto segment = log.segment(), position = log.position();
				const auto write = log.stage(batch);
				std::size_t copied = 0;
				if (log.segment() == segment && position % unit) {
					const auto room = unit - position % unit;  // the turn's new data bytes in the shared sector
					copied = static_cast<std::size_t>((sectors ? position % unit : 0) + std::min<std::uint64_t>(room, write.bytes.size()) / 2);
					check(write.file->write_at(write.offset, write.bytes.substr(0, copied)) == copied, "the half copy lands");
					++cases;
					for (const auto& [group, records] : model) {
						try { check(log.peek(group) == records, "a read mid-write returns the acknowledged records"); }
						catch (const std::exception& error) { check(false, std::string("a read mid-write throws: ") + error.what()); }
					}
				}
				check(write.file->write_at(write.offset + copied, write.bytes.substr(copied)) == write.bytes.size() - copied, "the rest lands");
				write.file->sync_data(); log.completed(); acknowledge(model, batch);
				for (const auto& [group, records] : model) { check(log.peek(group) == records, "after the barrier every group reads back"); }
				log.prepare();
			}
		}
	}
	check(cases > 500, "most turns rewrite a shared sector: " + std::to_string(cases));
}
}  // namespace

// The bytes each turn takes in the log, on the cut-header design's workloads: the data position's
// advance in file bytes (per sector, 512 for each 504 under RFTW0002), the bytes the log wrote, the
// 512-byte sectors and 4 KiB pages that write touches (what a device and the page cache write back),
// and the payload. Not a check: `cost` prints them, at any format, for the A4 comparison.
void cost() {
	struct Load { const char* name; std::uint32_t block; std::size_t bytes; };
	for (const auto& load : {Load{"T9 workload", 2048, 0}, Load{"T9 workload", 4096, 0}, Load{"T9 workload", 32768, 0}, Load{"turn_bench 16 groups 50% 256 B", 32768, 256}, Load{"turn_bench 16 groups 50% 4096 B", 32768, 4096}}) {
		const j::TurnLogOptions options{std::uint64_t{16} << 20, load.block, 1, 3, 600};
		double advance = 0, written = 0, sectors_touched = 0, pages_touched = 0, payload = 0, turns = 0;
		auto touched = [](std::uint64_t offset, std::size_t size, std::uint64_t unit) { return static_cast<double>(((offset + size + unit - 1) / unit - offset / unit) * unit); };
		for (std::uint64_t seed = 1; seed <= 8; ++seed) {
			fl::filesystem disk; fl::io io(disk); j::TurnRecovery recovered; std::mt19937_64 random(seed);
			j::TurnLog log(io, identity, recovered, options);
			for (unsigned turn = 0; turn < 400; ++turn) {
				std::vector<j::TurnRecord> batch;
				if (!load.bytes) { batch = probe_turn(log, random); }
				else {
					for (std::uint64_t g = 1; g <= 16; ++g) {
						if (random() % 100 >= 50) { continue; }
						const auto sequence = log.sequence(g) + 1;
						batch.push_back({g, sequence, sequence % 64 ? j::TurnRecordKind::Batch : j::TurnRecordKind::Checkpoint, std::string(load.bytes, 'x'), sequence - 1});
					}
					if (batch.empty()) { continue; }
				}
				const auto segment = log.segment(), position = log.position(), before = log.stats().bytes_written;
				const auto write = log.stage(batch);
				check(write.file->write_at(write.offset, write.bytes) == write.bytes.size(), "the host writes the staged bytes");
				write.file->sync_data(); log.completed();
				check(log.segment() == segment, "the turns stay in one segment");
				advance += static_cast<double>(log.position() - position); written += static_cast<double>(log.stats().bytes_written - before);
				sectors_touched += touched(write.offset, write.bytes.size(), 512); pages_touched += touched(write.offset, write.bytes.size(), 4096);
				for (const auto& r : batch) { payload += static_cast<double>(r.payload.size()); }
				++turns;
			}
		}
		const double file = sectors ? advance * 512 / 504 : advance;
		std::cout << "cost " << load.name << ", block " << load.block << ": per turn " << payload / turns << " payload bytes, " << file / turns << " log bytes, " << written / turns << " written, " << sectors_touched / turns << " in sectors touched, " << pages_touched / turns << " in pages touched\n";
	}
}

int main(int argc, char** argv) {
	const std::string only = argc > 1 ? argv[1] : "";
	const std::vector<std::pair<std::string, void (*)()>> cases{{"round_trip", round_trip}, {"torn_tail", torn_tail}, {"torn_turn_in_earlier_epoch", torn_turn_in_earlier_epoch},
		{"damage", damage}, {"double_damage", double_damage}, {"damaged_header_last_turn", damaged_header_last_turn}, {"double_damage_copy", double_damage_copy},
		{"earlier_turn_damage", earlier_turn_damage}, {"large_header_damage", large_header_damage}, {"large_header_tear", large_header_tear},
		{"newest_turn_damage", newest_turn_damage}, {"sector_verdicts", sector_verdicts}, {"stale_alias", stale_alias},
		{"checkpoints_and_guards", checkpoints_and_guards}, {"stale_bytes", stale_bytes}, {"staged_turns", staged_turns}, {"midwrite_reads", midwrite_reads},
		{"reclamation", [] { for (std::uint64_t seed = 4; seed < 7; ++seed) { reclamation(seed); } }}};
	bool ran = false;
	if (only == "cost") { cost(); ran = true; }
	for (const auto& [name, run] : cases) {
		if (!only.empty() && name != only) { continue; }
		ran = true;
		try { run(); } catch (const std::exception& error) { check(false, name + ": " + error.what()); }
	}
	check(ran, "a case named " + only);
	std::cout << checks << " turn log checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
