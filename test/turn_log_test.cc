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
#include <optional>
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
	const auto sector = (written - 1) / j::framing::sector_bytes * j::framing::sector_bytes;
	directory.zero(segment, sector, written - sector);
	j::TurnLog log(io, identity, recovered, small);
	check(recovered.last_turn == before && !recovered.groups.contains(9) && !recovered.damaged && recovered.groups == model, "a torn last turn is dropped whole");
	check(start < sector, "the torn turn spans more than the lost sector");
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
		log.append(std::vector{record(6, 1, 700)});                  // ends in the damaged block
		at = log.position();
		log.append(std::vector{record(1, 1, 300), record(2, 1, 300)});  // damaged below
		log.append(std::vector{record(3, 1, 300)});
		log.append(std::vector{record(1, 2, 300)});
	}
	directory.flip(1, at + 10);  // inside turn 2's first fragment header
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
		block = log.position() / options.block_bytes;
		for (std::uint64_t s = 2; s < 6; ++s) { log.append(std::vector{record(6, s, 6)}); }
		end = log.position();
		for (auto& r : first) { model[r.group].push_back(r); }
		model[1].push_back(record(1, 2, 3000));
		for (std::uint64_t s = 2; s < 6; ++s) { model[6].push_back(record(6, s, 6)); }
	}
	check(end / options.block_bytes == block + 1, "the last turns end in the block after the damaged one");
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
	check(end > 2 * small.block_bytes, "the last turn crosses block 1");
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
		while (log.position() % small.block_bytes < 80 || log.position() % small.block_bytes > 300) { checkpoint(10); }
		segment = log.segment(); at = log.position();
		log.append(std::vector{record(9, 2, 100)});  // lost to the damage below
		while (*log.data_segments().begin() != segment) { checkpoint(300); }
		check(log.stats().forwarded_records == 1, "only the group's first record moved forward");
	}
	check(at / small.block_bytes + 1 < small.segment_bytes / small.block_bytes, "the next block is in the segment");
	directory.flip(segment, at + 30);  // the second record's fragment
	directory.flip(segment, (at / small.block_bytes + 1) * small.block_bytes + 10);  // the next header's identity
	j::TurnLog log(io, identity, recovered, small);
	check(recovered.damaged, "the double damage is reported");
	check(recovered.lost.contains(9) || recovered.groups[9] == std::vector{record(9, 1, 100), record(9, 2, 100)}, "a forwarded copy does not show a group kept its records");
}

// A turn's fragments in a segment file: where each starts and ends, whether it ends its write,
// whether its header crosses its sector, whether a header of its write starts before it in its
// sector, and where the next fragment of its block starts.
struct Piece { std::uint64_t start, end; bool ends, cut, written; std::optional<std::uint64_t> next; };
std::vector<Piece> pieces(const std::string& bytes, std::uint64_t segment, std::uint32_t block_bytes) {
	namespace f = j::framing; namespace d = j::turn_log_detail;
	std::vector<Piece> out; std::uint8_t kind = 0;
	for (std::uint64_t b = 0; (b + 1) * block_bytes <= bytes.size(); ++b) {
		const auto block = std::string_view(bytes).substr(b * block_bytes, block_bytes);
		const auto header = f::parse_header(block, d::stream(identity, block_bytes, segment, 0), b);
		if (header.check != f::Check::ok) { continue; }
		const auto start = b * block_bytes; std::optional<std::size_t> in_write = 0; std::vector<Piece> here;
		for (std::size_t p = header.size; block_bytes - p >= f::fragment_header + 1;) {
			const auto fragment = f::parse_fragment(block.substr(p), block_bytes - p, segment, header.epoch);
			if (fragment.check != f::Check::ok) { break; }
			if (fragment.type == f::Full || fragment.type == f::First) { kind = static_cast<std::uint8_t>(fragment.payload[0]); }
			const bool ends = (fragment.type == f::Full || fragment.type == f::Last) && (kind == d::end_of_turn || kind == d::open_record);
			const std::uint64_t sector = (start + p) / f::sector_bytes, end = p + f::fragment_header + fragment.payload.size();
			here.push_back({start + p, start + end, ends, (start + p + f::fragment_header - 1) / f::sector_bytes != sector, in_write && (start + *in_write) / f::sector_bytes == sector, std::nullopt});
			in_write = ends ? std::nullopt : std::optional{p}; p = end;
		}
		for (std::size_t i = 0; i + 1 < here.size(); ++i) { here[i].next = here[i + 1].start; }
		out.insert(out.end(), here.begin(), here.end());
	}
	return out;
}
// Which condition of rule 2 (docs/mechanisms/turn-log-damage.md) a byte in sector `s` of `piece`,
// a later sector than its first, leaves unproven; "proven" when none does.
std::string shape(const Piece& piece, std::uint64_t s) {
	const auto sector = j::framing::sector_bytes;
	if ((piece.end - 1) / sector - piece.start / sector > 1) { return "U3"; }
	if (!piece.next || *piece.next / sector != s) { return "U4"; }
	if ((*piece.next + j::framing::fragment_header - 1) / sector != s) { return "U2"; }
	if (piece.cut && !piece.written) { return "U1"; }
	return "proven";
}
// Damage to an acknowledged turn before the newest, in the newest segment: a byte in each later
// sector of each fragment that crosses a sector. A later write that reached the disk began after
// the damaged turn's barrier, whether it starts in the damaged block or a later one, so recovery
// reports the damage, and a group it keeps holds every acknowledged record. The workload and
// seeds are the cut-header probe's, whose layout left these silent (journal-earlier-turn-loss).
void earlier_turn_damage(std::uint32_t block_bytes, std::uint64_t seed, std::map<std::string, unsigned>& shapes) {
	Directory directory("earlier-turn"); j::PosixIO io(directory.path); auto owner = io.acquire_owner(true);
	const j::TurnLogOptions options{8 * std::uint64_t{block_bytes}, block_bytes, 1, 3, 600};
	j::TurnRecovery recovered; Model model; std::mt19937_64 random(seed);
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
			log.append(batch); log.prepare();
			for (const auto& r : batch) { if (r.kind == j::TurnRecordKind::Checkpoint) { model[r.group].clear(); } model[r.group].push_back(r); }
		}
	}
	j::TurnLogGroups known; for (const auto& [group, _] : model) { known.open.push_back(group); }
	std::uint64_t newest = 0; std::string bytes; std::vector<Piece> list;
	for (const auto& entry : std::filesystem::directory_iterator(directory.path)) {
		const auto number = j::turn_log_detail::segment_number(entry.path().filename().string());
		if (!number || *number < newest) { continue; }
		std::ifstream file(entry.path(), std::ios::binary); std::string content(std::filesystem::file_size(entry.path()), '\0');
		file.read(content.data(), static_cast<std::streamsize>(content.size()));
		if (auto found = pieces(content, *number, block_bytes); !found.empty()) { newest = *number; bytes = std::move(content); list = std::move(found); }
	}
	// The newest write starts after the second-to-last write end.
	std::uint64_t newest_write = 0; int ends = 0;
	for (auto it = list.rbegin(); it != list.rend(); ++it) { if (it->ends && ++ends == 2) { newest_write = it->end; break; } }
	const auto sector = j::framing::sector_bytes;
	for (const auto& piece : list) {
		if (piece.start >= newest_write) { break; }
		// Whether a later write starts in a later block: else every later write shares this block.
		bool later_block = false, after_end = false;
		for (const auto& other : list) {
			if (other.start <= piece.start) { continue; }
			if (after_end && other.start / block_bytes > piece.start / block_bytes) { later_block = true; break; }
			after_end = after_end || other.ends;
		}
		for (auto s = piece.start / sector + 1; s <= (piece.end - 1) / sector; ++s) {
			const auto from = s * sector, to = std::min<std::uint64_t>(piece.end, (s + 1) * sector);
			for (const auto offset : {from + (to - from) / 2, to - 1}) {
				++shapes[(later_block ? "later block, " : "same block, ") + shape(piece, s)];
				directory.flip(newest, offset);
				const auto r = j::TurnLog::read(io, identity, options, known);
				directory.flip(newest, offset);
				bool kept = true;
				for (const auto& [group, records] : model) {
					const auto found = r.groups.find(group);
					if (!r.lost.contains(group) && (found == r.groups.end() || found->second != records)) { kept = false; }
				}
				if (!r.damaged || !kept) {
					std::cerr << "  block_bytes " << block_bytes << " seed " << seed << " segment " << newest << " offset " << offset << " (" << shape(piece, s) << (later_block ? ", later block" : ", same block") << ")\n";
				}
				check(r.damaged, "damage to an earlier turn is reported");
				check(kept, "a group recovery keeps holds every acknowledged record");
			}
		}
	}
	check(!bytes.empty() && !list.empty(), "the newest segment holds turns");
}
void earlier_turn_damage() {
	std::map<std::string, unsigned> shapes;
	earlier_turn_damage(4096, 4, shapes); earlier_turn_damage(4096, 6, shapes); earlier_turn_damage(2048, 22, shapes);
	check(shapes["same block, U1"] && shapes["same block, U2"] && shapes["same block, U3"], "the unproven shapes with later writes only in their block occur");
	check(shapes["later block, U3"] + shapes["later block, U4"] + shapes["later block, proven"] > 0, "damage before a later block's write occurs");
}
// A block header larger than its first sector (a manifest of 28 or more groups) leaves no fragment
// header whole there, so damage to it reads as a tear by its sector alone. The turns later in its
// block prove the turn that wrote it durable: recovery reports the damage.
void large_header_damage() {
	Directory directory("large-header"); j::PosixIO io(directory.path); auto owner = io.acquire_owner(true);
	const j::TurnLogOptions options{32 * 2048, 2048, 1};
	j::TurnRecovery recovered; Model model;
	{
		j::TurnLog log(io, identity, recovered, options);
		std::vector<j::TurnRecord> first;
		for (std::uint64_t g = 1; g <= 40; ++g) { first.push_back(record(g, 1, 1, j::TurnRecordKind::Checkpoint)); }
		log.append(first);  // block 0's groups: block 1's header names them
		log.append(std::vector{record(41, 1, 20)}); log.append(std::vector{record(41, 2, 20)});
		check(log.position() / options.block_bytes == 1, "the later turns share block 1");
		for (const auto& r : first) { model[r.group].push_back(r); }
		model[41] = {record(41, 1, 20), record(41, 2, 20)};
	}
	std::string header(64, '\0');
	{ std::ifstream file(directory.segment(1), std::ios::binary); file.seekg(options.block_bytes); file.read(header.data(), 64); }
	const auto size = j::framing::header_bytes(j::framing::get16(std::string_view(header).substr(56)));
	check(size > j::framing::sector_bytes + 100, "block 1's header crosses its first sector");
	directory.flip(1, options.block_bytes + j::framing::sector_bytes + 50);  // a manifest entry in its second sector
	j::TurnLogGroups known; for (const auto& [group, _] : model) { known.open.push_back(group); }
	const auto r = j::TurnLog::read(io, identity, options, known);
	check(r.damaged, "damage to a header before later turns of its block is reported");
	bool kept = true;
	for (const auto& [group, records] : model) { const auto found = r.groups.find(group); if (!r.lost.contains(group) && (found == r.groups.end() || found->second != records)) { kept = false; } }
	check(kept, "a group recovery keeps holds every acknowledged record");
}
// The same header written by a torn newest turn, its second sector left old: the turn's bytes in
// the next sector are its own, so they prove nothing, and the turn is dropped as a tear.
void large_header_tear() {
	Directory directory("large-header-tear"); j::PosixIO io(directory.path); auto owner = io.acquire_owner(true);
	const j::TurnLogOptions options{32 * 2048, 2048, 1};
	j::TurnRecovery recovered; Model model;
	{
		j::TurnLog log(io, identity, recovered, options);
		log.append(std::vector{record(50, 1, 10)}); model[50] = {record(50, 1, 10)};
		std::vector<j::TurnRecord> torn;
		for (std::uint64_t g = 1; g <= 40; ++g) { torn.push_back(record(g, 1, 1, j::TurnRecordKind::Checkpoint)); }
		torn.push_back(record(41, 1, 600));  // from block 1's second sector into its third
		auto write = log.stage(torn);
		const auto old_from = options.block_bytes + j::framing::sector_bytes, old_to = old_from + j::framing::sector_bytes;
		check(write.offset < old_from && write.offset + write.bytes.size() > old_to + 100, "the torn turn spans block 1's second sector and more");
		const auto bytes = std::string_view(write.bytes);
		check(write.file->write_at(write.offset, bytes.substr(0, old_from - write.offset)) == old_from - write.offset, "the sectors before the old one are written");
		check(write.file->write_at(old_to, bytes.substr(old_to - write.offset)) == bytes.size() - (old_to - write.offset), "the sectors after it are written");
		write.file->sync_data();
	}
	const auto r = j::TurnLog::read(io, identity, options);
	check(!r.damaged && r.lost.empty() && r.groups == model, "a torn turn whose own later bytes survive reads as a tear");
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
}  // namespace

int main() {
	try { round_trip(); torn_tail(); torn_turn_in_earlier_epoch(); damage(); double_damage(); damaged_header_last_turn(); double_damage_copy(); earlier_turn_damage(); large_header_damage(); large_header_tear(); checkpoints_and_guards(); stale_bytes(); staged_turns(); for (std::uint64_t seed = 4; seed < 7; ++seed) { reclamation(seed); } }
	catch (const std::exception& error) { check(false, error.what()); }
	std::cout << checks << " turn log checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
