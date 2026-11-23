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

// The turn path owns its buffers (docs/mechanisms/turn-seal.md, "The turn's buffers"). One
// CTest entry per case:
//   bytes    framing a record's header and body in place writes what main's logical() path wrote
//   turns    a log's turns, rolling into new segments, write main's bytes at main's offsets
//   warm     a warm turn of 32 records from 8 groups allocates nothing in stage() or completed()
//   seal     a sealed turn through the driver allocates far less than one body: no copy at seal
//   recycle  the driver hands each sealed body back once its turn completes; warm turns allocate nothing
//   forward  a forward copy reads into the turn's buffer, allocating nothing, byte for byte
//   power    a power cut at every write of such turns recovers exactly the acknowledged records
#include "faultlab/faultfs.h"
#include "journal/turn_driver.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <new>
#include <optional>
#include <random>
#include <span>
#include <tuple>

// TSan's runtime defines operator new, so under TSan nothing is counted and the allocation
// checks are skipped, saying so.
#if defined(__SANITIZE_THREAD__)
#define JOURNAL_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define JOURNAL_TEST_TSAN 1
#endif
#endif
namespace {
#ifdef JOURNAL_TEST_TSAN
constexpr bool heap_counted = false;
#else
constexpr bool heap_counted = true;
#endif
std::atomic<bool> counting{false};
std::atomic<std::uint64_t> allocations{0}, allocated{0};
}  // namespace

#ifndef JOURNAL_TEST_TSAN
// Counts every allocation made while `counting` is set.
void* operator new(std::size_t size) {
	if (counting.load(std::memory_order_relaxed)) { allocations.fetch_add(1, std::memory_order_relaxed); allocated.fetch_add(size, std::memory_order_relaxed); }
	if (void* p = std::malloc(size ? size : 1)) { return p; }
	throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif

namespace j = kronuz::journal;
namespace d = j::turn_log_detail;
namespace f = j::framing;
namespace fl = kronuz::faultlab;

namespace {
int checks = 0, failures = 0;
void check(bool value, const std::string& what) {
	++checks;
	if (!value) { ++failures; if (failures < 100) { std::cerr << "FAIL: " << what << '\n'; } }
}
// An allocation check: skipped under TSan, where nothing is counted.
void check_heap(bool value, const std::string& what) { if (heap_counted) { check(value, what); } }

struct Count { std::uint64_t calls = 0, bytes = 0; };
template <class F> Count counted(F&& fn) {
	allocations = 0; allocated = 0; counting = true;
	try { fn(); } catch (...) { counting = false; throw; }
	counting = false;
	return {allocations.load(), allocated.load()};
}

// main's framing at journal:178030b, verbatim but for names: what the bytes on disk must stay.
namespace reference {
struct Manifest { bool known = false; std::uint64_t segment = 0; std::uint32_t block = 0; std::map<std::uint64_t, std::uint64_t> groups; };
class Emitter {
public:
	Emitter(const f::Stream& stream, std::uint64_t position, Manifest filling = {})
		: stream_(stream), position_(position), filling_(std::move(filling)) {}
	std::uint64_t record(std::string_view logical, std::uint64_t group = 0, std::uint64_t sequence = 0) {
		std::string_view rest(logical); bool first = true; std::uint64_t start = 0;
		const std::uint64_t block = stream_.block_bytes;
		while (!rest.empty()) {
			enter_block();
			auto room = block - position_ % block;
			if (room < f::fragment_header + 1) { pad(room); continue; }
			auto count = std::min<std::size_t>(rest.size(), room - f::fragment_header);
			bool last = count == rest.size();
			if (first) { start = position_; }
			if (group) { auto& highest = filling_.groups[group]; highest = std::max(highest, sequence); }
			fragment(first ? (last ? f::Full : f::First) : (last ? f::Last : f::Middle), rest.substr(0, count));
			rest.remove_prefix(count); first = false;
		}
		return start;
	}
	const std::string& bytes() const noexcept { return bytes_; }
	std::uint64_t position() const noexcept { return position_; }
	const Manifest& filling() const noexcept { return filling_; }
private:
	void enter_block() {
		const std::uint64_t block = stream_.block_bytes;
		if (position_ % block) { return; }
		auto start = bytes_.size();
		bytes_.append(stream_.magic); bytes_.append(stream_.identity.data(), stream_.identity.size());
		j::put64(bytes_, stream_.segment); j::put32(bytes_, static_cast<std::uint32_t>(position_ / block)); j::put64(bytes_, stream_.epoch);
		auto count = stream_.manifests && filling_.known && f::header_bytes(static_cast<std::uint16_t>(std::min<std::size_t>(filling_.groups.size(), 4096))) <= block / 2 ?
			static_cast<std::uint16_t>(filling_.groups.size()) : f::no_manifest;
		j::put64(bytes_, count == f::no_manifest ? 0 : filling_.segment); j::put32(bytes_, count == f::no_manifest ? 0 : filling_.block); f::put16(bytes_, count);
		if (count != f::no_manifest) { for (const auto& [group, sequence] : filling_.groups) { j::put64(bytes_, group); j::put64(bytes_, sequence); } }
		j::put32(bytes_, j::crc32c(std::string_view(bytes_).substr(start)));
		position_ += f::header_bytes(count);
		filling_ = Manifest{true, stream_.segment, static_cast<std::uint32_t>(position_ / block), {}};
	}
	void pad(std::uint64_t count) { bytes_.append(count, '\0'); position_ += count; }
	void fragment(f::Fragment type, std::string_view payload) {
		std::string header; f::put16(header, static_cast<std::uint16_t>(payload.size())); header.push_back(static_cast<char>(type)); header.push_back('\0');
		j::put64(header, stream_.segment); j::put64(header, stream_.epoch);
		j::Checksum checksum; checksum.update(header); checksum.update(payload);
		j::put32(bytes_, checksum.value()); bytes_.append(header); bytes_.append(payload);
		position_ += f::fragment_header + payload.size();
	}
	f::Stream stream_; std::uint64_t position_; std::string bytes_; Manifest filling_;
};
inline std::string logical(std::uint8_t kind, std::uint64_t turn, std::uint64_t group, std::uint64_t sequence, std::string_view payload) {
	std::string bytes; bytes.reserve(d::record_header + payload.size());
	bytes.push_back(static_cast<char>(kind)); j::put64(bytes, turn); j::put64(bytes, group); j::put64(bytes, sequence); bytes.append(payload);
	return bytes;
}
// The turn main's TurnLog::stage framed: its records, then the end-of-turn record.
inline void turn(Emitter& emitter, std::uint64_t number, std::span<const j::TurnRecord> records) {
	for (const auto& r : records) { emitter.record(logical(static_cast<std::uint8_t>(r.kind), number, r.group, r.sequence, r.payload), r.group, r.sequence); }
	std::string count; j::put32(count, static_cast<std::uint32_t>(records.size()));
	emitter.record(logical(d::end_of_turn, number, 0, 0, count));
}
}  // namespace reference

f::Manifest converted(const reference::Manifest& from) {
	f::Manifest to{from.known, from.segment, from.block, {}};
	for (const auto& [group, sequence] : from.groups) { to.groups[group] = sequence; }
	return to;
}
bool same(const f::Manifest& a, const reference::Manifest& b) {
	if (a.known != b.known || a.segment != b.segment || a.block != b.block || a.groups.size() != b.groups.size()) { return false; }
	return std::equal(a.groups.begin(), a.groups.end(), b.groups.begin(), [](const auto& x, const auto& y) { return x.first == y.first && x.second == y.second; });
}

std::string payload(std::uint64_t group, std::uint64_t sequence, std::size_t size) {
	std::string bytes(size, '\0');
	for (std::size_t i = 0; i < size; ++i) { bytes[i] = static_cast<char>('a' + (group * 7 + sequence * 3 + i) % 26); }
	return bytes;
}
j::TurnRecord record(std::uint64_t group, std::uint64_t sequence, std::size_t size, j::TurnRecordKind kind = j::TurnRecordKind::Batch) {
	return {group, sequence, kind, payload(group, sequence, size), sequence - 1};  // a checkpoint covers the records before it
}
const j::Identity identity{'B', 'U', 'F', 'F', 'E', 'R'};

// A1. Every record kind, size class and start position, from manifests that are empty, small,
// past half a block and past 4096 groups: the head-and-body path, the fill path in uneven
// pieces and the one-piece path write main's bytes, offsets and manifests; a measuring emitter
// lays them out at the same positions.
void bytes() {
	std::mt19937_64 random(7);
	std::uint64_t records = 0;
	const std::uint8_t kinds[] = {1, 2, 3, d::end_of_turn, d::open_record};
	for (std::uint32_t block : {256u, 512u, 4096u, 32768u}) {
		const std::uint64_t room = block - f::header_bytes(f::no_manifest) - f::fragment_header;
		const std::uint64_t sizes[] = {0, 1, 2, 23, 24, 25, 26, 100, room - 25, room - 24, room, room + 1, block, 2 * block + 7, 3 * block - 1};
		for (bool manifests : {true, false}) {
			for (std::size_t prior : {std::size_t{0}, std::size_t{3}, std::size_t{block / 32}, std::size_t{4100}}) {
				for (std::uint64_t start : {std::uint64_t{0}, std::uint64_t{f::header_bytes(f::no_manifest)}, std::uint64_t{block - 25}, std::uint64_t{block - 24}, std::uint64_t{block - 1}, std::uint64_t{block + 100}}) {
					const f::Stream stream{d::magic, identity, block, 5, 3, manifests};
					reference::Manifest filling{true, 5, static_cast<std::uint32_t>(start / block), {}};
					for (std::size_t g = 0; g < prior; ++g) { filling.groups[random() % 9000 + 1] = random() % 50; }
					reference::Emitter old(stream, start, filling);
					f::Emitter pieces(stream, start, converted(filling)), fills(stream, start, converted(filling)), whole(stream, start, converted(filling));
					auto measure = f::Emitter::measure(stream, start, converted(filling));
					for (int n = 0; n < 24; ++n) {
						const auto kind = kinds[random() % 5];
						const auto size = sizes[random() % std::size(sizes)];
						const std::uint64_t group = random() % 4 ? random() % 40 : 0, sequence = random() % 1000;
						const auto body = payload(group, sequence, size);
						const auto head = d::header(kind, 9, group, sequence);
						const auto expected = old.record(reference::logical(kind, 9, group, sequence, body), group, sequence);
						const auto a = pieces.record(d::view(head), body, group, sequence);
						std::string_view rest(body);
						const auto b = fills.record(d::view(head), body.size(), [&](std::string& out, std::size_t count) {
							while (count) { const auto piece = std::min<std::size_t>(count, random() % 7 + 1); out.append(rest.substr(0, piece)); rest.remove_prefix(piece); count -= piece; }
						}, group, sequence);
						const auto c = whole.record(d::logical(kind, 9, group, sequence, body), group, sequence);
						const auto m = measure.record(d::view(head), body.size(), [&](std::string&, std::size_t) { check(false, "a measuring emitter calls no fill"); }, group, sequence);
						++records;
						const auto at = "block " + std::to_string(block) + " manifests " + std::to_string(manifests) + " prior " + std::to_string(prior) + " start " + std::to_string(start) + " record " + std::to_string(n);
						check(a == expected && b == expected && c == expected && m == expected, "each path starts the record where main did: " + at);
						check(pieces.position() == old.position() && fills.position() == old.position() && whole.position() == old.position() && measure.position() == old.position(), "each path ends it where main did: " + at);
						check(same(pieces.filling(), old.filling()) && same(measure.filling(), old.filling()), "each path notes main's manifest: " + at);
					}
					check(pieces.bytes() == old.bytes() && fills.bytes() == old.bytes() && whole.bytes() == old.bytes(), "the bytes are main's: block " + std::to_string(block) + " start " + std::to_string(start));
					check(measure.bytes().empty(), "a measuring emitter writes nothing");
				}
			}
		}
	}
	std::cout << "bytes: " << records << " records against main's framing\n";
}

// Mirrors a log with main's framing: where each turn starts and the bytes it writes.
struct Shadow {
	f::Stream stream; std::uint64_t segment_bytes = 0, end = 0; reference::Manifest filling;
	Shadow(const j::TurnLog& log, const j::TurnLogOptions& options)
		: stream(d::stream(identity, options.block_bytes, log.segment(), log.epoch())), segment_bytes(options.segment_bytes) {
		reference::Emitter open(stream, 0, {});
		open.record(reference::logical(d::open_record, log.last_turn(), 0, 0, {}));
		end = open.position(); filling = open.filling();
	}
	// main's bytes for a turn staged at `segment` and `offset`; `rolled` when main would start
	// it in the next segment, which the log must then have done.
	std::string turn(std::uint64_t number, std::span<const j::TurnRecord> records, std::uint64_t segment, std::uint64_t offset, bool& rolled, const std::string& at) {
		reference::Emitter here(stream, end, filling); reference::turn(here, number, records);
		rolled = here.position() > segment_bytes;
		if (!rolled) {
			check(segment == stream.segment && offset == end, "the turn follows the last one: " + at);
			end = here.position(); filling = here.filling(); return here.bytes();
		}
		check(segment > stream.segment && offset == 0, "a turn past the segment's rest starts the next segment: " + at);
		stream.segment = segment;
		reference::Emitter fresh(stream, 0, filling); reference::turn(fresh, number, records);
		end = fresh.position(); filling = fresh.filling(); return fresh.bytes();
	}
};
// Stages, checks against the shadow, writes, syncs and completes one turn.
std::uint64_t commit(j::TurnLog& log, Shadow& shadow, std::span<const j::TurnRecord> records, const std::string& at, bool* rolled = nullptr) {
	const auto write = log.stage(records);
	bool moved = false;
	const auto expected = shadow.turn(write.turn, records, log.segment(), write.offset, moved, at);
	check(write.bytes == expected, "the turn's bytes are main's: " + at);
	check(write.file->write_at(write.offset, write.bytes) == write.bytes.size(), "the turn is written");
	log.flush(write); log.completed();
	if (rolled) { *rolled = moved; }
	return write.turn;
}

// A1. Turns of one to four records, empty to four blocks, from five groups, on 16 KiB segments:
// many turns start the next segment. Each turn's bytes and offset are main's, and the log
// recovers every record.
void turns() {
	const j::TurnLogOptions options{32 * 512, 512, 1, 1000, 600};
	fl::filesystem disk; fl::io io(disk);
	std::map<std::uint64_t, std::vector<j::TurnRecord>> model;
	std::mt19937_64 random(11);
	unsigned rolls = 0, crossing = 0;
	{
		j::TurnRecovery recovered; j::TurnLog log(io, identity, recovered, options);
		Shadow shadow(log, options);
		for (unsigned t = 0; t < 400; ++t) {
			std::vector<j::TurnRecord> batch; std::map<std::uint64_t, std::uint64_t> next;
			for (auto n = random() % 4 + 1; n; --n) {
				const auto group = random() % 5 + 1;
				auto& sequence = next.try_emplace(group, log.sequence(group)).first->second;
				const std::size_t size = random() % 3 ? random() % 600 : random() % 2000;
				crossing += size > 512;
				batch.push_back(record(group, ++sequence, size, random() % 6 ? j::TurnRecordKind::Batch : j::TurnRecordKind::Checkpoint));
			}
			bool rolled = false;
			commit(log, shadow, batch, "turn " + std::to_string(t), &rolled);
			rolls += rolled;
			for (const auto& r : batch) { if (r.kind == j::TurnRecordKind::Checkpoint) { model[r.group].clear(); } model[r.group].push_back(r); }
			log.prepare();
		}
	}
	check(rolls >= 20 && crossing >= 50, "turns start new segments and records cross blocks");
	j::TurnRecovery recovered; j::TurnLog log(io, identity, recovered, options);
	check(recovered.groups == model && !recovered.damaged && recovered.lost.empty(), "the log recovers every record");
	std::cout << "turns: 400 turns, " << rolls << " into a new segment, " << crossing << " records across blocks\n";
}

// A2. Rounds of a batch turn (8 groups, 4 records each, 100 B to 40 KiB) and a checkpoint turn;
// after four warm rounds, stage() and completed() of each turn allocate nothing, the turn's
// bytes are main's, and its payload bytes are the bodies' bytes.
void warm() {
	const j::TurnLogOptions options{std::uint64_t{8} << 20, 32 << 10, 1, 100, 1 << 20};
	fl::filesystem disk; fl::io io(disk);
	j::TurnRecovery recovered; j::TurnLog log(io, identity, recovered, options);
	Shadow shadow(log, options);
	const std::size_t sizes[] = {100, 1000, 40000, 7000};
	std::map<std::uint64_t, std::vector<j::TurnRecord>> model;
	unsigned measured = 0;
	for (unsigned round = 0; round < 8; ++round) {
		std::vector<j::TurnRecord> batch, checkpoints;
		std::uint64_t body = 0;
		for (std::uint64_t group = 1; group <= 8; ++group) {
			for (std::uint64_t i = 0; i < 4; ++i) { batch.push_back(record(group, log.sequence(group) + 1 + i, sizes[i])); body += sizes[i]; }
		}
		for (std::uint64_t group = 1; group <= 8; ++group) { checkpoints.push_back(record(group, log.sequence(group) + 5, 5000, j::TurnRecordKind::Checkpoint)); }
		for (auto* turn : {&batch, &checkpoints}) {
			const auto at = "round " + std::to_string(round) + (turn == &batch ? " batch" : " checkpoint");
			const auto segment = log.segment(); const auto appended = log.stats().bytes_appended;
			std::optional<j::TurnLog::TurnWrite> write;
			const auto staging = counted([&] { write = log.stage(*turn); });
			const auto expected_bytes = [&] { bool rolled = false; return shadow.turn(write->turn, *turn, log.segment(), write->offset, rolled, at); }();
			check(write->bytes == expected_bytes, "the turn's bytes are main's: " + at);
			check(write->file->write_at(write->offset, write->bytes) == write->bytes.size(), "the turn is written");
			log.flush(*write);
			const auto completing = counted([&] { log.completed(); });
			check(log.segment() == segment, "the turn stays in its segment: " + at);
			std::uint64_t bodies = 0; for (const auto& r : *turn) { bodies += r.payload.size(); }
			check(log.stats().bytes_appended - appended == bodies, "the turn appends its bodies' bytes: " + at);
			if (round >= 4) {
				++measured;
				check_heap(staging.calls == 0 && staging.bytes == 0, "a warm stage() allocates nothing: " + at + ": " + std::to_string(staging.calls) + " allocations, " + std::to_string(staging.bytes) + " bytes");
				check_heap(completing.calls == 0 && completing.bytes == 0, "a warm completed() allocates nothing: " + at + ": " + std::to_string(completing.calls) + " allocations, " + std::to_string(completing.bytes) + " bytes");
			}
			if (turn == &batch) { std::cout << "warm: " << at << ": " << staging.calls << " allocations (" << staging.bytes << " bytes) staging " << turn->size() << " records of " << body << " body bytes, " << completing.calls << " completing\n"; }
			for (const auto& r : *turn) { if (r.kind == j::TurnRecordKind::Checkpoint) { model[r.group].clear(); } model[r.group].push_back(r); }
		}
	}
	check(measured == 8, "eight warm turns measured");
	j::TurnRecovery after; j::TurnLog reopened(io, identity, after, options);
	check(after.groups == model, "every record recovers");
}

// A2 through the driver. Four sealers hand over pre-encoded 128 KiB bodies by move; once warm,
// the turn's begin() (sealing and staging) allocates less than a quarter of one body, so no
// body is copied at seal; every record recovers.
void seal() {
	const j::TurnLogOptions options{std::uint64_t{16} << 20, 32 << 10, 1, 100, 1 << 20};
	constexpr std::size_t body = 128 << 10;
	fl::filesystem disk; fl::io io(disk);
	std::map<std::uint64_t, std::optional<j::TurnRecord>> open;
	std::map<std::uint64_t, std::vector<j::TurnRecord>> model;
	{
		j::TurnDriver driver(io, identity, options);
		for (std::uint64_t group = 1; group <= 4; ++group) {
			driver.seal(group, [&open, group](std::uint64_t room, bool) -> std::optional<j::TurnRecord> {
				auto& o = open[group];
				if (!o || d::record_cost(o->payload.size()) > room) { return std::nullopt; }
				auto taken = std::move(*o); o.reset();
				return taken;
			});
		}
		for (unsigned t = 0; t < 8; ++t) {
			for (std::uint64_t group = 1; group <= 4; ++group) { open[group] = record(group, t + 1, body); model[group].push_back(*open[group]); }
			const j::TurnLog::TurnWrite* write = nullptr;
			const auto sealing = counted([&] { write = &driver.begin(); });
			check(write->file->write_at(write->offset, write->bytes) == write->bytes.size(), "the turn is written");
			driver.log().flush(*write); driver.completed();
			check(std::all_of(open.begin(), open.end(), [](const auto& o) { return !o.second; }), "every body sealed in its turn");
			if (t >= 2) {
				check_heap(sealing.bytes < body / 4, "a warm sealed turn copies no body: turn " + std::to_string(t) + ": " + std::to_string(sealing.calls) + " allocations, " + std::to_string(sealing.bytes) + " bytes for 4 bodies of " + std::to_string(body));
			}
			if (t == 7) { std::cout << "seal: " << sealing.calls << " allocations, " << sealing.bytes << " bytes sealing and staging 4 bodies of " << body << " bytes\n"; }
		}
	}
	j::TurnDriver after(io, identity, options);
	check(after.recovered().groups == model, "every sealed record recovers");
}

// A6. Four sealers encode each turn's 128 KiB body into one buffer per group and take it back
// through their recycler; group 5's sealer has none and hands over a body built before the turn.
// Every second record is a checkpoint, so the log's live index stays bounded and reuses its nodes.
// Each buffer comes back only once its turn completes (its sequence durable), cleared with its
// capacity, and the next body is encoded into it: once warm, begin() (sealing, encoding and
// staging) and completed() allocate nothing. A group unsealed while its turn is in flight gets
// nothing back; a recycler's throw fails the driver after its turn is acknowledged.
void recycle() {
	const j::TurnLogOptions options{std::uint64_t{16} << 20, 32 << 10, 1, 100, 1 << 20};
	constexpr std::size_t body = 128 << 10;
	constexpr std::uint64_t groups = 4, plain = 5;
	fl::filesystem disk; fl::io io(disk);
	// A body is zeros with a mark every 4 KiB, so encoding into a cleared buffer writes it whole.
	auto encode = [](std::string& into, std::uint64_t group, std::uint64_t sequence) {
		into.resize(body);
		for (std::size_t i = 0; i < body; i += 4096) { into[i] = static_cast<char>('a' + (group + sequence + i / 4096) % 26); }
	};
	auto kind = [](std::uint64_t sequence) { return sequence % 2 ? j::TurnRecordKind::Batch : j::TurnRecordKind::Checkpoint; };
	std::map<std::uint64_t, std::vector<j::TurnRecord>> model;
	auto modeled = [&](j::TurnRecord r) { auto& records = model[r.group]; if (r.kind == j::TurnRecordKind::Checkpoint) { records.clear(); } records.push_back(std::move(r)); };
	std::vector<std::string> buffers(groups + 1);
	std::vector<const char*> sealed(groups + 1), returned(groups + 1);
	std::vector<std::uint64_t> sequences(groups + 1), handed(groups + 1);
	std::optional<j::TurnRecord> open_plain;
	std::uint64_t turn = 0;
	bool in_turn = false, throw_next = false;
	unsigned reused = 0, early = 0, cleared = 0, durable = 0;
	{
		j::TurnDriver driver(io, identity, options);
		const int owner = 0;
		auto sealer = [&](std::uint64_t group) {
			return [&, group](std::uint64_t room, bool) -> std::optional<j::TurnRecord> {
				if (sequences[group] > turn || d::record_cost(body) > room) { return std::nullopt; }
				auto& buffer = buffers[group];
				encode(buffer, group, turn + 1);
				if (returned[group]) { reused += buffer.data() == returned[group]; }
				sealed[group] = buffer.data(); sequences[group] = turn + 1;
				return j::TurnRecord{group, turn + 1, kind(turn + 1), std::move(buffer), turn};
			};
		};
		auto recycler = [&](std::uint64_t group) {
			return [&, group](std::string&& payload) {
				if (throw_next) { throw_next = false; throw std::runtime_error("recycler failed"); }
				early += in_turn; cleared += payload.empty() && payload.capacity() >= body;
				durable += driver.sequence(group) >= sequences[group] && !driver.in_flight();
				if (payload.data() != sealed[group]) { check(false, "the payload handed back is the buffer group " + std::to_string(group) + " sealed"); }  // no string unless it fails
				returned[group] = payload.data(); ++handed[group];
				buffers[group] = std::move(payload);
			};
		};
		for (std::uint64_t group = 1; group <= groups; ++group) { driver.seal(group, sealer(group), &owner, recycler(group)); }
		driver.seal(plain, [&](std::uint64_t room, bool) -> std::optional<j::TurnRecord> {
			if (!open_plain || d::record_cost(open_plain->payload.size()) > room) { return std::nullopt; }
			auto taken = std::move(*open_plain); open_plain.reset();
			return taken;
		});
		// One turn: the plain body is built before it, the recycled ones inside begin().
		auto run_turn = [&](std::uint64_t sealing_groups, const std::function<void()>& in_flight) {
			open_plain = record(plain, turn + 1, 1000, kind(turn + 1)); modeled(*open_plain);
			for (std::uint64_t group = 1; group <= sealing_groups; ++group) {
				std::string bytes; encode(bytes, group, turn + 1);
				modeled({group, turn + 1, kind(turn + 1), std::move(bytes), turn});
			}
			const j::TurnLog::TurnWrite* write = nullptr;
			in_turn = true;
			const auto sealing = counted([&] { write = &driver.begin(); });
			in_flight();
			check(write->file->write_at(write->offset, write->bytes) == write->bytes.size(), "the turn is written: " + std::to_string(turn));
			driver.log().flush(*write);
			in_turn = false;
			std::pair<Count, Count> counts{sealing, {}};
			try { counts.second = counted([&] { driver.completed(); }); } catch (...) { ++turn; throw; }
			++turn;
			return counts;
		};
		for (unsigned t = 0; t < 8; ++t) {
			const auto at = "turn " + std::to_string(t);
			const auto [sealing, completing] = run_turn(groups, [] {});
			check(!open_plain, "the plain sealer's record sealed: " + at);
			for (std::uint64_t group = 1; group <= groups; ++group) { check(handed[group] == t + 1, "group " + std::to_string(group) + "'s buffer came back once: " + at); }
			if (t >= 4) {  // the log's live index has its spare nodes by then
				check_heap(sealing.calls == 0 && sealing.bytes == 0, "a warm begin() allocates nothing: " + at + ": " + std::to_string(sealing.calls) + " allocations, " + std::to_string(sealing.bytes) + " bytes");
				check_heap(completing.calls == 0 && completing.bytes == 0, "a warm completed() allocates nothing: " + at + ": " + std::to_string(completing.calls) + " allocations, " + std::to_string(completing.bytes) + " bytes");
			}
			if (t == 7) { std::cout << "recycle: " << sealing.calls << " allocations (" << sealing.bytes << " bytes) in begin() sealing and encoding 4 recycled bodies of " << body << " bytes and a plain one, " << completing.calls << " in completed()\n"; }
		}
		check(reused == groups * 7, "every body after the first is encoded into the buffer handed back: " + std::to_string(reused) + " of " + std::to_string(groups * 7));
		check(cleared == groups * 8, "each buffer comes back cleared, with its capacity: " + std::to_string(cleared));
		check(early == 0 && durable == groups * 8, "each buffer comes back only after its turn is durable: " + std::to_string(early) + " early, " + std::to_string(durable) + " durable");
		// Unsealed in flight: its record is durable, its buffer freed, not handed back.
		run_turn(groups, [&] { driver.unseal(groups, &owner); });
		check(handed[groups] == 8 && handed[1] == 9, "a group unsealed in flight gets nothing back; the others do");
		check(driver.sequence(groups) == turn, "the unsealed group's record is durable");
		// A throwing recycler: its turn is acknowledged, then the driver fails with its error.
		throw_next = true;
		bool threw = false;
		try { run_turn(groups - 1, [] {}); } catch (const std::runtime_error& error) { threw = std::string(error.what()) == "recycler failed"; }
		check(threw && driver.failed(), "a recycler's throw fails the driver");
		check(driver.sequence(1) == turn && driver.sequence(plain) == turn, "the throwing turn's records are durable");
	}
	j::TurnDriver after(io, identity, options);
	check(after.recovered().groups == model, "every record recovers");
}

// A3. Four idle groups' checkpoints sit in the oldest segment while group 9 checkpoints every
// turn; one record moves forward a turn. Each forward copy reads back as its original's bytes,
// and once warm (its segment's reader open, the turn in its segment) stage() allocates nothing.
void forward() {
	const j::TurnLogOptions options{16 * 4096, 4096, 1, 2, 1};
	fl::filesystem disk; fl::io io(disk);
	j::TurnRecovery recovered; j::TurnLog log(io, identity, recovered, options);
	Shadow shadow(log, options);
	std::map<std::uint64_t, j::TurnRecord> idle;
	for (std::uint64_t group = 1; group <= 4; ++group) {
		idle[group] = record(group, 1, 5000, j::TurnRecordKind::Checkpoint);
		const std::vector<j::TurnRecord> batch{idle[group]};
		commit(log, shadow, batch, "idle " + std::to_string(group));
	}
	unsigned forwards = 0, measured = 0; std::uint64_t last_oldest = 0;
	for (unsigned t = 0; t < 120; ++t) {
		const std::vector<j::TurnRecord> batch{record(9, log.sequence(9) + 1, 6000, j::TurnRecordKind::Checkpoint)};
		const auto oldest = *log.data_segments().begin(); const auto segment = log.segment();
		const auto copied = log.stats().forwarded_records; const auto moved = log.stats().forwarded_bytes;
		std::optional<j::TurnLog::TurnWrite> write;
		const auto staging = counted([&] { write = log.stage(batch); });
		const bool same_segment = log.segment() == segment;
		check(write->file->write_at(write->offset, write->bytes) == write->bytes.size(), "the turn is written");
		log.flush(*write); log.completed(); log.prepare();
		if (log.stats().forwarded_records == copied) { continue; }
		++forwards;
		check(log.stats().forwarded_records == copied + 1 && log.stats().forwarded_bytes - moved == 5000, "one 5000-byte record moves forward a turn");
		if (forwards > 4 && same_segment && oldest == last_oldest) {
			++measured;
			check_heap(staging.calls == 0 && staging.bytes == 0, "a warm forward copy allocates nothing: turn " + std::to_string(t) + ": " + std::to_string(staging.calls) + " allocations, " + std::to_string(staging.bytes) + " bytes");
		}
		last_oldest = oldest;
		for (const auto& [group, original] : idle) {
			const auto live = log.live(group);
			check(live.size() == 1 && live[0] == original, "a forwarded record reads back as its original: group " + std::to_string(group) + " turn " + std::to_string(t));
		}
	}
	check(forwards >= 8 && measured >= 3, "records move forward, warm turns among them: " + std::to_string(forwards) + " forwards, " + std::to_string(measured) + " measured");
	check(log.stats().segments_recycled + log.stats().segments_removed > 0, "forwarding frees segments");
	std::cout << "forward: " << forwards << " forward copies, " << measured << " warm ones measured\n";
}

// A4. Power is cut at every mutating operation of runs whose turns frame records across blocks,
// roll into new segments and forward an idle group's record; recovery holds every acknowledged
// record and the in-flight turn whole or not at all, and the log continues.
struct Workload {
	std::mt19937_64 random; std::size_t largest; std::map<std::uint64_t, std::vector<j::TurnRecord>> acknowledged; std::vector<j::TurnRecord> in_flight;
	Workload(std::uint64_t seed, std::size_t largest) : random(seed), largest(largest) {}
	std::vector<j::TurnRecord> turn(const j::TurnLog& log) {
		std::vector<j::TurnRecord> batch; std::map<std::uint64_t, std::uint64_t> next;
		if (!log.sequence(9) && !acknowledged.contains(9)) { batch.push_back(record(9, 1, largest / 2, j::TurnRecordKind::Checkpoint)); return batch; }
		for (auto n = random() % 2 + 1; n; --n) {
			const auto group = random() % 5 + 1;
			auto& sequence = next.try_emplace(group, log.sequence(group)).first->second;
			batch.push_back(record(group, ++sequence, random() % largest + 1, random() % 5 ? j::TurnRecordKind::Batch : j::TurnRecordKind::Checkpoint));
		}
		return batch;
	}
	void acknowledge(const std::vector<j::TurnRecord>& batch) {
		for (const auto& r : batch) { if (r.kind == j::TurnRecordKind::Checkpoint) { acknowledged[r.group].clear(); } acknowledged[r.group].push_back(r); }
	}
	bool matches(const j::TurnRecovery& recovered) {
		if (recovered.groups == acknowledged) { return true; }
		auto with = acknowledged; for (const auto& r : in_flight) { if (r.kind == j::TurnRecordKind::Checkpoint) { with[r.group].clear(); } with[r.group].push_back(r); }
		if (recovered.groups == with) { acknowledge(in_flight); return true; }
		return false;
	}
};
void power() {
	const fl::crash_policy sector_cut{.size = fl::size_pick::sector};
	std::uint64_t points = 0;
	for (const auto& [block, blocks, largest] : {std::tuple{512u, 16u, std::size_t{1200}}, std::tuple{4096u, 8u, std::size_t{6000}}}) {
		const j::TurnLogOptions options{std::uint64_t{block} * blocks, block, 1, 3, 600};
		for (std::uint64_t seed = 1; seed <= 3; ++seed) {
			std::uint64_t mutations = 0;
			{
				fl::filesystem disk; fl::io io(disk); Workload work(seed, largest); j::TurnRecovery recovered;
				j::TurnLog log(io, identity, recovered, options);
				for (unsigned t = 0; t < 40; ++t) { auto b = work.turn(log); log.append(b); work.acknowledge(b); log.prepare(); }
				check(log.stats().forwarded_records > 0 && log.stats().segments_prepared > 3, "the run forwards records and rolls into new segments: block " + std::to_string(block));
				mutations = disk.mutations();
			}
			for (std::uint64_t cut = 1; cut <= mutations; ++cut, ++points) {
				fl::filesystem disk; fl::io io(disk); Workload work(seed, largest); j::TurnRecovery recovered;
				const auto at = "block " + std::to_string(block) + " seed " + std::to_string(seed) + " cut " + std::to_string(cut);
				try {
					disk.power_off_after(cut - 1);
					j::TurnLog log(io, identity, recovered, options);
					for (unsigned t = 0; t < 40; ++t) {
						work.in_flight = work.turn(log); log.append(work.in_flight);
						work.acknowledge(work.in_flight); work.in_flight.clear();
						log.prepare();
					}
				} catch (const std::exception& error) { check(disk.powered_off(), "only the power cut fails the run: " + at + ": " + error.what()); }
				disk.crash(seed * 7919 + cut, sector_cut);
				try {
					j::TurnLog log(io, identity, recovered, options);
					check(!recovered.damaged && recovered.lost.empty(), "power loss alone damages nothing: " + at);
					check(work.matches(recovered), "recovery holds exactly the acknowledged records, the in-flight turn whole or not at all: " + at);
					work.in_flight.clear();
					for (unsigned t = 0; t < 5; ++t) { auto b = work.turn(log); log.append(b); work.acknowledge(b); }
				} catch (const std::exception& error) { check(false, at + ": " + error.what()); }
			}
		}
	}
	std::cout << "power: " << points << " crash points\n";
}
}  // namespace

int main(int argc, char** argv) {
	const std::vector<std::pair<std::string, std::function<void()>>> cases{
		{"bytes", bytes}, {"turns", turns}, {"warm", warm}, {"seal", seal}, {"recycle", recycle}, {"forward", forward}, {"power", power},
	};
	const std::string only = argc > 1 ? argv[1] : "";
	if (!heap_counted) { std::cout << "allocation checks skipped: TSan's runtime defines operator new, so the heap is not counted\n"; }
	bool ran = false;
	for (const auto& [name, run] : cases) {
		if (!only.empty() && name != only) { continue; }
		ran = true;
		try { run(); } catch (const std::exception& error) { check(false, name + ": " + error.what()); }
	}
	if (!ran) { std::cerr << "unknown case: " << only << '\n'; return 2; }
	std::cout << checks << " checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
