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

// RFTW0001 (turn_log.h): turns round-trip across blocks, segments, and epochs; the torn
// tail is dropped; damage before a later turn loses only the groups it may have held;
// checkpoints supersede; stale bytes of a recycled file never read as current.
#include "journal/turn_log.h"
#include "completion/posix.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>

namespace j = kronuz::journal;

namespace {
int checks = 0, failures = 0;
void check(bool value, const char* what) {
	++checks;
	if (!value) { ++failures; std::cerr << "FAIL: " << what << '\n'; }
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

using Model = std::map<std::uint64_t, std::vector<j::TurnRecord>>;
j::TurnRecord record(std::uint64_t group, std::uint64_t sequence, std::size_t size, j::TurnRecordKind kind = j::TurnRecordKind::Batch) {
	std::string payload(size, '\0');
	for (std::size_t i = 0; i < size; ++i) { payload[i] = static_cast<char>('a' + (group * 7 + sequence * 3 + i) % 26); }
	return {group, sequence, kind, payload};
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
	std::uint64_t segment = 0, end = 0, before = 0;
	{
		j::TurnLog log(io, identity, recovered, small);
		workload(log, model, random, 5, 100);
		before = log.last_turn();
		auto start = log.stats().bytes_written;
		log.append(std::vector{record(9, 1, 300)});
		segment = log.segment(); end = log.stats().bytes_written - start;
	}
	// The last turn's tail never reached the disk: power was lost before its barrier.
	auto size = std::filesystem::file_size(directory.segment(segment));
	std::uint64_t written = 0;
	{
		std::ifstream file(directory.segment(segment), std::ios::binary); std::string bytes(size, '\0'); file.read(bytes.data(), static_cast<std::streamsize>(size));
		written = bytes.find_last_not_of('\0') + 1;
	}
	directory.zero(segment, written - 20, 20);
	j::TurnLog log(io, identity, recovered, small);
	check(recovered.last_turn == before && !recovered.groups.contains(9) && !recovered.damaged && recovered.groups == model, "a torn last turn is dropped whole");
	check(end > 20, "the torn turn spans more than the cut");
}

void damage() {
	Directory directory("damage"); j::PosixIO io(directory.path); auto owner = io.acquire_owner(true);
	j::TurnRecovery recovered;
	{
		j::TurnLog log(io, identity, recovered, small);
		log.append(std::vector{record(4, 1, 20)});                   // intact, but nothing later
		log.append(std::vector{record(1, 1, 300), record(2, 1, 300)});  // damaged below
		log.append(std::vector{record(3, 1, 300)});
		log.append(std::vector{record(1, 2, 300)});
	}
	// Turn 2 starts after the open record (97 bytes) and turn 1 (122): flip its first fragment.
	directory.flip(1, 219 + 10);
	j::TurnLog log(io, identity, recovered, small);
	check(recovered.damaged && recovered.lost.contains(1) && !recovered.groups.contains(2), "groups whose records the damage held are lost or absent");
	check(recovered.lost.contains(4) && !recovered.groups.contains(4), "a group that wrote nothing after the damage cannot show it is whole");
	check(recovered.groups.contains(3) && recovered.groups[3] == std::vector{record(3, 1, 300)}, "a group whose records follow the damage is kept");
	check(throws([&] { log.append(std::vector{record(1, 3, 10)}); }), "a lost group rejoins under a new id");
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
}  // namespace

int main() {
	try { round_trip(); torn_tail(); damage(); checkpoints_and_guards(); stale_bytes(); }
	catch (const std::exception& error) { check(false, error.what()); }
	std::cout << checks << " turn log checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
