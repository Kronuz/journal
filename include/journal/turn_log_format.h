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

// The turn log's types and its on-disk format (RFTW0002): options, records, recovery results, and
// turn_log_detail's sectors, blocks, fragments and the scan that reads a segment back.
#pragma once

#include "framing.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace kronuz::journal {

struct TurnLogOptions {
	std::uint64_t segment_bytes = std::uint64_t{64} << 20;  // a multiple of block_bytes
	std::uint32_t block_bytes = 32 << 10;  // a multiple of 512
	std::size_t spare_segments = 1;  // preallocated ahead of need
	std::size_t retain_segments = 4;  // past this many, live records move forward
	std::size_t forward_bytes = 1 << 20;  // moved forward per turn
};
enum class TurnRecordKind : std::uint8_t { Batch = 1, HardState = 2, Checkpoint = 3 };
struct TurnRecord {
	std::uint64_t group = 0, sequence = 0;
	TurnRecordKind kind = TurnRecordKind::Batch;
	std::string payload;
	// A checkpoint's covered sequence: the group's last record its state includes. The log
	// refuses a checkpoint below the record before it, which it would drop; 0 covers nothing, so
	// only a group's first record may leave it unset. Not stored, so not compared.
	std::uint64_t covers = 0;
	bool operator==(const TurnRecord& other) const {
		return group == other.group && sequence == other.sequence && kind == other.kind && payload == other.payload;
	}
};
// A lost group whose loss the damage bounds (docs/mechanisms/lost-bound.md): the highest
// sequence the damage could hold, and the group's records after it, in sequence order.
struct TurnBound {
	std::uint64_t bound = 0;
	std::vector<TurnRecord> records;
	bool operator==(const TurnBound&) const = default;
};
// What recovery kept: each group's records from its latest checkpoint, or from 1, or from its
// table bound + 1 (TurnLogGroups::bound), in sequence order. A group whose sequence has a gap, or
// whose records damage may have held (every group, where no manifest names the damaged block's),
// is lost: it rejoins as a new incarnation (D11, D5), unless a checkpoint it kept after the damage
// supersedes every record there. A lost group the damaged blocks' manifests name is also bounded:
// a caller that stores the bound keeps it (TurnLog::keep), and a reader holding the group through
// the bound continues from the records after it. When damaged, a group the host holds open
// (TurnLogGroups::open) that is in neither map, nor bounded by the table, is lost too.
struct TurnRecovery {
	std::uint64_t epoch = 0;      // the epoch this open started
	std::uint64_t last_turn = 0;  // the last turn kept
	std::map<std::uint64_t, std::vector<TurnRecord>> groups;
	std::set<std::uint64_t> lost;
	std::map<std::uint64_t, TurnBound> bounded;  // lost groups whose loss is bounded
	bool damaged = false;
};
// What the host's group table knows before a log recovers (one directory per host): a lost id
// stays lost and refused whatever the log holds, a group's records through its base sequence
// are superseded by the table's copy, so recovery ignores them, and the open groups are the
// ones a damaged log loses when it holds nothing of them. A bound the table stored for a group
// (TurnRecovery::bounded) supersedes its records through it too: the group continues after it.
// `settle`, when set, gets the recovery before the log writes its open record, which makes the
// recovered epoch an earlier one: what the caller stores there, a verdict, outlives that record.
struct TurnLogGroups {
	std::set<std::uint64_t> lost;
	std::map<std::uint64_t, std::uint64_t> base;
	std::vector<std::uint64_t> open;
	std::map<std::uint64_t, std::uint64_t> bound;
	std::function<void(const TurnRecovery&)> settle;
};
struct TurnLogStats {
	std::uint64_t turns = 0, barriers = 0, bytes_appended = 0, bytes_written = 0, segments_prepared = 0, prepared_inline = 0;
	std::uint64_t forwarded_records = 0, forwarded_bytes = 0, segments_recycled = 0, segments_removed = 0;
	std::uint64_t forward_scanned = 0;  // live records forward_slice visited: grows with the groups in the log
};

namespace turn_log_detail {
inline constexpr std::string_view magic{"RFTW0002", 8};
// A sector is 504 data bytes, then the segment's low 32 bits and a CRC-32C over the data, all
// 64 bits of the segment and the sector's 32-bit index in it. Positions are data bytes.
inline constexpr std::size_t sector_bytes = 512, sector_data = 504;
// A block header: magic (8), identity (16), segment (8), block (4), epoch (8), the manifest's
// segment (8), block (4) and count (2), then 16 bytes per manifest entry. A fragment header:
// length (2), type (1), reserved (1); a zero length ends the segment's data.
inline constexpr std::size_t header_fixed = 58, fragment_header = 4;
inline constexpr std::uint16_t no_manifest = 0xffff;
inline constexpr std::size_t header_bytes(std::uint16_t count) { return header_fixed + (count == no_manifest ? 0 : 16 * std::size_t{count}); }
// The data bytes `bytes` of whole sectors hold, and the file offset of a data position.
inline constexpr std::uint64_t data_bytes(std::uint64_t bytes) { return bytes / sector_bytes * sector_data; }
inline constexpr std::uint64_t file_offset(std::uint64_t position) { return position / sector_data * sector_bytes + position % sector_data; }
// Blocks are whole sectors; a fragment's length is 16 bits.
inline constexpr bool valid_block_bytes(std::uint32_t bytes) { return bytes >= sector_bytes && bytes % sector_bytes == 0 && data_bytes(bytes) <= 65535 + fragment_header; }
// The most groups a block's fragments can name: one per fragment, each at least 5 bytes.
inline constexpr std::size_t block_groups(std::uint32_t block_bytes) { return (data_bytes(block_bytes) - header_fixed) / (fragment_header + 1); }

inline std::uint32_t sector_check(const char* data, std::uint64_t segment, std::uint64_t index) {
	char bound[12]; framing::store(bound, segment, 8); framing::store(bound + 8, index, 4);
	Checksum checksum; checksum.update(data, sector_data); checksum.update(bound, sizeof bound);
	return checksum.value();
}
inline void seal(char* sector, std::uint64_t segment, std::uint64_t index) {
	framing::store(sector + sector_data, segment, 4); framing::store(sector + sector_data + 4, sector_check(sector, segment, index), 4);
}
// What a sector holds: a version this segment wrote, a stale sector of the file's earlier use
// (the nearest lower segment with its stored low bits, 2^32 below when they are this one's), never
// written, or damage, which no crash leaves.
enum class Sector : std::uint8_t { current, stale, zero, damaged };
inline Sector verdict(const char* sector, std::uint64_t segment, std::uint64_t index) {
	const auto stored = framing::peek32(std::string_view(sector + sector_data + 4, 4));
	if (sector_check(sector, segment, index) == stored) { return Sector::current; }
	const std::uint64_t below = segment - ((segment - framing::peek32(std::string_view(sector + sector_data, 4))) & 0xffff'ffffu);
	const std::uint64_t older = below != segment ? below : segment - (std::uint64_t{1} << 32);  // wraps past 0: none
	if (older < segment && sector_check(sector, older, index) == stored) { return Sector::stale; }
	return std::all_of(sector, sector + sector_bytes, [](char c) { return c == '\0'; }) ? Sector::zero : Sector::damaged;
}

// What every block header of a segment is bound to.
struct Stream { Identity identity{}; std::uint32_t block_bytes = 0; std::uint64_t segment = 0, epoch = 0; };
inline Stream stream(const Identity& identity, std::uint32_t block_bytes, std::uint64_t segment, std::uint64_t epoch) { return {identity, block_bytes, segment, epoch}; }

// Writes records as fragments into blocks from a data position, starting a block's header as the
// stream enters it and padding a block's tail too short for a fragment. Its bytes are whole sectors
// from the one holding the start, which begins with `partial`, the data the log wrote there
// before; seal() zero-fills the last sector's data and writes each sector's trailer. A measuring
// emitter lays records out, positions and manifests, without writing a byte.
class Emitter {
public:
	Emitter(const Stream& stream, std::uint64_t position, framing::Manifest filling = {}, std::string buffer = {}, std::string_view partial = {})
		: Emitter(stream, position, std::move(filling), false) {
		bytes_ = std::move(buffer); bytes_.clear();
		if (partial.size() != position % sector_data) { throw std::logic_error("turn log: the kept sector is not the position's"); }
		bytes_.append(partial);
	}
	static Emitter measure(const Stream& stream, std::uint64_t position, framing::Manifest filling = {}) { return Emitter(stream, position, std::move(filling), true); }
	// Returns the position of the record's first fragment. A nonzero group is noted in the
	// manifest of each block the record touches.
	std::uint64_t record(std::string_view head, std::string_view body, std::uint64_t group, std::uint64_t sequence) {
		return record(head, body.size(), [body](std::string& out, std::size_t count) mutable { out.append(body.substr(0, count)); body.remove_prefix(count); }, group, sequence);
	}
	// A record of `head` then `size` bytes that `fill(out, count)` appends to `out` in order, each
	// piece straight into its sector. A measuring emitter never calls it.
	template <class Fill> std::uint64_t record(std::string_view head, std::uint64_t size, Fill&& fill, std::uint64_t group, std::uint64_t sequence) {
		const std::uint64_t total = head.size() + size;
		std::uint64_t done = 0, start = 0;
		while (done < total) {
			enter_block();
			const auto room = block_ - position_ % block_;
			if (room < fragment_header + 1) { pad(room); continue; }
			const auto count = std::min<std::uint64_t>(total - done, room - fragment_header);
			const bool first = done == 0, last = done + count == total;
			if (first) { start = position_; }
			if (group) { filling_.groups.raise(group, sequence); }
			char header[fragment_header]; framing::store(header, count, 2);
			header[2] = static_cast<char>(first ? (last ? framing::Full : framing::First) : (last ? framing::Last : framing::Middle)); header[3] = '\0';
			put(std::string_view(header, fragment_header));
			if (done < head.size()) { put(head.substr(done, std::min<std::uint64_t>(count, head.size() - done))); }
			for (std::uint64_t left = done + count > head.size() ? done + count - std::max<std::uint64_t>(done, head.size()) : 0; left;) {
				const auto piece = std::min<std::uint64_t>(left, sector_data - position_ % sector_data);
				if (!measuring_) {
					const auto at = bytes_.size(); fill(bytes_, static_cast<std::size_t>(piece));
					if (bytes_.size() != at + piece) { throw std::logic_error("turn log: a fill wrote other than it was asked"); }
				}
				advance(piece); left -= piece;
			}
			done += count;
		}
		return start;
	}
	// Zero-fills the last sector's data and writes each sector's trailer; the bytes are then the write.
	void seal() {
		if (measuring_) { return; }
		if (const auto used = position_ % sector_data) { bytes_.append(sector_data - used + 8, '\0'); }
		for (std::size_t at = 0; at < bytes_.size(); at += sector_bytes) { turn_log_detail::seal(bytes_.data() + at, stream_.segment, first_ + at / sector_bytes); }
	}
	const std::string& bytes() const& noexcept { return bytes_; }
	std::string bytes() && noexcept { return std::move(bytes_); }
	std::uint64_t position() const noexcept { return position_; }
	const framing::Manifest& filling() const& noexcept { return filling_; }
	framing::Manifest filling() && noexcept { return std::move(filling_); }
	bool measuring() const noexcept { return measuring_; }
private:
	Emitter(const Stream& stream, std::uint64_t position, framing::Manifest filling, bool measuring)
		: stream_(stream), block_(data_bytes(stream.block_bytes)), position_(position), first_(position / sector_data), filling_(std::move(filling)), measuring_(measuring) {}
	// Advances the position past `count` data bytes appended, leaving a trailer's room at a sector's end.
	void advance(std::uint64_t count) {
		position_ += count;
		if (!measuring_ && position_ % sector_data == 0) { bytes_.append(8, '\0'); }
	}
	void put(std::string_view bytes) {
		while (!bytes.empty()) {
			const auto piece = std::min<std::uint64_t>(bytes.size(), sector_data - position_ % sector_data);
			if (!measuring_) { bytes_.append(bytes.substr(0, piece)); }
			advance(piece); bytes.remove_prefix(piece);
		}
	}
	void enter_block() {
		if (position_ % block_) { return; }
		// A manifest larger than half a block is not kept: damage there costs conservatively.
		const auto groups = filling_.groups.size();
		const auto count = filling_.known && header_bytes(static_cast<std::uint16_t>(std::min<std::size_t>(groups, 4096))) <= block_ / 2 ? static_cast<std::uint16_t>(groups) : no_manifest;
		if (!measuring_) {
			char fixed[header_fixed];
			std::copy_n(magic.data(), 8, fixed); std::copy_n(stream_.identity.data(), 16, fixed + 8);
			framing::store(fixed + 24, stream_.segment, 8); framing::store(fixed + 32, position_ / block_, 4); framing::store(fixed + 36, stream_.epoch, 8);
			framing::store(fixed + 44, count == no_manifest ? 0 : filling_.segment, 8); framing::store(fixed + 52, count == no_manifest ? 0 : filling_.block, 4); framing::store(fixed + 56, count, 2);
			put(std::string_view(fixed, header_fixed));
			if (count != no_manifest) {
				for (const auto& [group, sequence] : filling_.groups) { char entry[16]; framing::store(entry, group, 8); framing::store(entry + 8, sequence, 8); put(std::string_view(entry, 16)); }
			}
		} else { position_ += header_bytes(count); }
		filling_.known = true; filling_.segment = stream_.segment; filling_.block = static_cast<std::uint32_t>(position_ / block_); filling_.groups.clear();
	}
	void pad(std::uint64_t count) { static constexpr char zeros[fragment_header] = {}; put(std::string_view(zeros, count)); }
	Stream stream_; std::uint64_t block_, position_, first_; framing::Manifest filling_; std::string bytes_; bool measuring_ = false;
};

// The sector a turn in flight rewrites and the data its durable version holds, served from memory:
// the file's copy may be part way through the write.
struct Held { std::uint64_t sector = ~std::uint64_t{0}; std::string_view data; };
// A segment's sectors, read a run at a time and checked as they are used: only this segment's
// pass, so a stale, unwritten or damaged one is corruption to a reader of acknowledged records.
class Sectors {
public:
	Sectors(File& file, std::uint64_t segment, Held held = {}) : file_(file), segment_(segment), held_(held) {}
	// The data bytes at [position, position + count), within one block of `block` data bytes.
	void read(std::uint64_t position, char* to, std::size_t count, std::uint64_t block) {
		while (count) {
			const auto sector = position / sector_data;
			if (sector == held_.sector) {
				const auto at = position % sector_data, n = std::min<std::uint64_t>(count, sector_data - at);
				if (at + n > held_.data.size()) { throw Corruption("log record past the data written"); }
				std::copy_n(held_.data.data() + at, n, to);
				position += n; to += n; count -= n;
				continue;
			}
			if (sector < first_ || sector >= first_ + loaded_) { load(sector, block); }
			auto& checked = checked_[sector - first_];
			const char* bytes = buffer_.data() + (sector - first_) * sector_bytes;
			if (!checked) {
				if (verdict(bytes, segment_, sector) != Sector::current) { throw Corruption("log record changed since it was written"); }
				checked = true;
			}
			const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(count, sector_data - position % sector_data));
			std::copy_n(bytes + position % sector_data, n, to);
			position += n; to += n; count -= n;
		}
	}
private:
	static constexpr std::size_t run = 8;  // sectors read at once, held in the reader
	// Up to `run` sectors from `sector` to its block's end.
	void load(std::uint64_t sector, std::uint64_t block) {
		const auto per_block = block / sector_data, end = (sector / per_block + 1) * per_block;
		loaded_ = static_cast<std::size_t>(std::min<std::uint64_t>(run, end - sector)); first_ = sector; checked_ = {};
		if (file_.read_at(sector * sector_bytes, std::span<char>(buffer_.data(), loaded_ * sector_bytes)) != loaded_ * sector_bytes) { loaded_ = 0; throw Corruption("short log read"); }
	}
	File& file_; std::uint64_t segment_; Held held_; std::uint64_t first_ = 0; std::size_t loaded_ = 0; std::array<char, run * sector_bytes> buffer_; std::array<bool, run> checked_{};
};

// Reads back, in order, the record whose first fragment starts at `offset`, checking each block
// header, fragment and sector on the way, so a record of another segment or epoch is corruption.
class RecordReader {
public:
	RecordReader(File& file, const Stream& stream, std::uint64_t offset, Held held = {}) : sectors_(file, stream.segment, held), stream_(stream), block_(data_bytes(stream.block_bytes)), position_(offset) {}
	// Reads the record's next `count` bytes to `to`; a record that ends first is corruption.
	void read(char* to, std::size_t count) {
		while (count) {
			if (!left_) { next(); }
			const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(count, left_));
			sectors_.read(position_, to, n, block_);
			position_ += n; left_ -= n; to += n; count -= n;
		}
	}
	// The bytes its current fragment has left, entering the next one when none: 0 once done.
	std::uint64_t piece() { if (!left_ && !done()) { next(); } return left_; }
	bool done() const noexcept { return !left_ && (type_ == framing::Full || type_ == framing::Last); }
private:
	void next() {
		if (done()) { throw Corruption("log record shorter than read"); }
		for (;;) {
			if (position_ % block_ == 0) { enter_block(); }
			if (block_ - position_ % block_ >= fragment_header + 1) { break; }
			position_ += block_ - position_ % block_;
		}
		char header[fragment_header]; sectors_.read(position_, header, fragment_header, block_);
		const std::size_t length = framing::get16(std::string_view(header, 2)); const auto type = static_cast<std::uint8_t>(header[2]);
		const bool follows = type_ ? type == framing::Middle || type == framing::Last : type == framing::Full || type == framing::First;
		if (!length || fragment_header + length > block_ - position_ % block_ || !follows || header[3]) { throw Corruption("invalid log fragment"); }
		type_ = type; left_ = length; position_ += fragment_header;
	}
	void enter_block() {
		char fixed[header_fixed]; sectors_.read(position_, fixed, header_fixed, block_);
		const std::string_view view(fixed, header_fixed);
		const auto count = framing::get16(view.substr(56));
		if (view.substr(0, 8) != magic || view.substr(8, 16) != std::string_view(stream_.identity.data(), 16) || framing::peek64(view.substr(24)) != stream_.segment ||
			framing::peek32(view.substr(32)) != position_ / block_ || framing::peek64(view.substr(36)) != stream_.epoch || header_bytes(count) + fragment_header >= block_) {
			throw Corruption("invalid log block header");
		}
		position_ += header_bytes(count);
	}
	Sectors sectors_; Stream stream_; std::uint64_t block_, position_; std::uint64_t left_ = 0; std::uint8_t type_ = 0;
};
// Reads back the record whose first fragment starts at `offset` (RecordReader).
inline std::string read_record(File& file, const Stream& stream, std::uint64_t offset, Held held = {}) {
	std::string logical;
	RecordReader reader(file, stream, offset, held);
	while (const auto count = reader.piece()) { const auto at = logical.size(); logical.resize(at + count); reader.read(logical.data() + at, count); }
	return logical;
}

// A logical record: kind, turn, group and sequence, then the payload.
inline constexpr std::size_t record_header = 25;
inline constexpr std::uint8_t end_of_turn = 0x10, open_record = 0x11;

using RecordHeader = std::array<char, record_header>;
inline RecordHeader header(std::uint8_t kind, std::uint64_t turn, std::uint64_t group, std::uint64_t sequence) {
	RecordHeader bytes; bytes[0] = static_cast<char>(kind);
	framing::store(bytes.data() + 1, turn, 8); framing::store(bytes.data() + 9, group, 8); framing::store(bytes.data() + 17, sequence, 8);
	return bytes;
}
inline std::string_view view(const RecordHeader& header) { return {header.data(), header.size()}; }
// A record's bytes as one string; the turn path frames the header and payload in place instead.
inline std::string logical(std::uint8_t kind, std::uint64_t turn, std::uint64_t group, std::uint64_t sequence, std::string_view payload) {
	std::string bytes(view(header(kind, turn, group, sequence))); bytes.append(payload);
	return bytes;
}
// A bound on a turn's bytes in an empty segment, whatever its manifests: each record costs at most
// its payload, its header and 40 bytes (a fragment header, a manifest entry); the end-of-turn record
// is one more; each block adds at most 126 data bytes and the first block's manifest half a
// block's; whole sectors hold the data. docs/mechanisms/turn-seal.md derives it.
inline constexpr std::uint64_t record_cost(std::uint64_t payload) { return payload + record_header + 40; }
inline constexpr std::uint64_t block_cost = 126;
inline std::uint64_t turn_bound(std::uint64_t cost, std::uint32_t block_bytes) {
	const std::uint64_t b = data_bytes(block_bytes), fixed = cost + record_cost(4) + block_cost + b / 2;
	const auto data = fixed + (fixed * block_cost + (b - block_cost) - 1) / (b - block_cost);
	return (data + sector_data - 1) / sector_data * sector_bytes;
}
// The record cost a turn may carry and still encode within `limit` bytes of an empty segment.
inline std::uint64_t turn_room(std::uint64_t limit, std::uint32_t block_bytes) {
	const std::uint64_t b = data_bytes(block_bytes), data = data_bytes(limit), scaled = data / b * (b - block_cost) + data % b * (b - block_cost) / b;
	const std::uint64_t fixed = record_cost(4) + block_cost + b / 2;
	return scaled > fixed ? scaled - fixed : 0;
}

inline std::string segment_name(std::uint64_t number) {
	char name[32]; std::snprintf(name, sizeof name, "turns-%016llx", static_cast<unsigned long long>(number)); return name;
}
inline std::optional<std::uint64_t> segment_number(std::string_view name) {
	if (name.size() != 22 || !name.starts_with("turns-")) { return std::nullopt; }
	std::uint64_t value = 0;
	for (char c : name.substr(6)) {
		int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
		if (digit < 0) { return std::nullopt; }
		value = value << 4 | static_cast<unsigned>(digit);
	}
	return value;
}

// One parsed item of the stream: a whole logical record, or a gap where bytes did not
// read back as current (unwritten, stale, or damaged).
struct Item {
	bool gap = false, tail = false;  // a tail: a segment's gap after its last record
	bool damage = false;  // a gap holding a sector that failed, or that the device could not read
	std::uint64_t epoch = 0;
	std::uint8_t kind = 0;
	std::uint64_t turn = 0, group = 0, sequence = 0;
	std::string payload;
	std::uint64_t segment = 0, offset = 0;  // the record's first fragment; a gap's block start
	std::uint64_t last = 0;  // a gap's last block whose bytes may begin a lost record
};
// Per described block: its groups' highest sequences, and where the header naming them is.
struct BlockManifest { std::map<std::uint64_t, std::uint64_t> groups; std::uint64_t segment = 0, epoch = 0; std::uint32_t block = 0; };
using Manifests = std::map<std::pair<std::uint64_t, std::uint32_t>, BlockManifest>;
// Reads a segment block by block. Each sector the parse needs decides alone (D4): this segment's
// is read; a stale or unwritten one, or data that does not parse, ends the block's data (a tear,
// or the segment's end); a damaged or unreadable one is damage. A gap merges every unreadable run
// up to the next record.
inline void scan_segment(File& file, const Identity& identity, const TurnLogOptions& options, std::uint64_t segment, std::vector<Item>& items, Manifests& manifests) {
	const std::uint64_t block = data_bytes(options.block_bytes), per_block = options.block_bytes / sector_bytes;
	std::uint64_t here = 0;
	// `holds`: this block's bytes may begin a lost record (a continuation's record began in a block
	// the gap covers already).
	auto gap = [&](bool holds, bool damage = false) {
		if (items.empty() || !items.back().gap) { Item item; item.gap = true; item.segment = segment; item.offset = item.last = here; items.push_back(std::move(item)); }
		else if (holds) { items.back().last = here; }
		if (damage) { items.back().damage = true; }
	};
	std::string raw(options.block_bytes, '\0'), data(block, '\0'), partial; std::vector<std::optional<Sector>> verdicts; std::vector<bool> unreadable(per_block);
	bool assembling = false; std::uint64_t partial_epoch = 0, partial_offset = 0;
	// The verdict of the sectors holding the block's data [from, from + count): the first that is not current.
	auto need = [&](std::uint64_t first_sector, std::size_t from, std::size_t count) {
		for (auto s = from / sector_data; count && s <= (from + count - 1) / sector_data; ++s) {
			if (!verdicts[s]) {
				const char* bytes = raw.data() + s * sector_bytes;
				verdicts[s] = unreadable[s] ? Sector::damaged : verdict(bytes, segment, first_sector + s);
				if (verdicts[s] == Sector::current) { std::copy_n(bytes, sector_data, data.data() + s * sector_data); }
			}
			if (verdicts[s] != Sector::current) { return *verdicts[s]; }
		}
		return Sector::current;
	};
	const auto size = std::min(file.size(), options.segment_bytes);
	for (std::uint64_t offset = 0, index = 0; offset + options.block_bytes <= size; offset += options.block_bytes, ++index) {
		here = index * block;
		// A block that does not read whole is read a sector at a time: only the sectors the device
		// cannot read are damage, where the parse needs them.
		auto read = [&](std::uint64_t at, std::span<char> to) {
			try { return file.read_at(at, to) == to.size(); } catch (const std::system_error&) { return false; }
		};
		const bool whole = read(offset, raw);
		for (std::uint64_t s = 0; s < per_block; ++s) { unreadable[s] = !whole && !read(offset + s * sector_bytes, std::span<char>(raw.data() + s * sector_bytes, sector_bytes)); }
		verdicts.assign(per_block, std::nullopt);
		const auto first_sector = index * per_block;
		const std::string_view view(data);
		auto fixed = need(first_sector, 0, header_fixed);
		const auto count = fixed == Sector::current ? framing::get16(view.substr(56)) : 0;
		const bool shaped = fixed == Sector::current && view.substr(0, 8) == magic && view.substr(8, 16) == std::string_view(identity.data(), 16) &&
			framing::peek64(view.substr(24)) == segment && framing::peek32(view.substr(32)) == index && header_bytes(count) + fragment_header < block &&
			(count != no_manifest || (!framing::peek64(view.substr(44)) && !framing::peek32(view.substr(52))));
		if (shaped && count != no_manifest) { fixed = need(first_sector, header_fixed, header_bytes(count) - header_fixed); }
		if (!shaped || fixed != Sector::current) {
			gap(fixed == Sector::damaged, fixed == Sector::damaged); assembling = false;  // else unwritten, stale, torn or not this segment's
			continue;
		}
		const auto epoch = framing::peek64(view.substr(36));
		if (count != no_manifest) {
			auto& manifest = manifests[{framing::peek64(view.substr(44)), framing::peek32(view.substr(52))}];
			manifest.segment = segment; manifest.block = static_cast<std::uint32_t>(index); manifest.epoch = epoch;
			for (std::size_t i = 0; i < count; ++i) { manifest.groups[framing::peek64(view.substr(header_fixed + 16 * i))] = framing::peek64(view.substr(header_fixed + 16 * i + 8)); }
		}
		std::size_t position = header_bytes(count);
		while (block - position >= fragment_header + 1) {
			auto check = need(first_sector, position, fragment_header);
			const std::size_t length = check == Sector::current ? framing::get16(view.substr(position)) : 0;
			const auto type = check == Sector::current ? static_cast<std::uint8_t>(view[position + 2]) : 0;
			const bool parses = length && fragment_header + length <= block - position && type >= framing::Full && type <= framing::Last && !view[position + 3];
			if (parses) { check = need(first_sector, position + fragment_header, length); }
			if (!parses || check != Sector::current) { gap(true, check == Sector::damaged); assembling = false; break; }  // a zero length: the data's end
			const auto payload = view.substr(position + fragment_header, length);
			position += fragment_header + length;
			if (type == framing::Full || type == framing::First) {
				if (assembling) { gap(false); }
				partial.assign(payload); partial_epoch = epoch; partial_offset = here + position - fragment_header - length; assembling = type == framing::First;
				if (assembling) { continue; }
			} else {
				if (!assembling || partial_epoch != epoch) { gap(false); assembling = false; continue; }
				partial.append(payload);
				if (type == framing::Middle) { continue; }
				assembling = false;
			}
			if (partial.size() < record_header) { gap(true); continue; }
			std::string_view record(partial);
			Item item; item.epoch = epoch; item.segment = segment; item.offset = partial_offset; item.kind = static_cast<std::uint8_t>(record[0]); record.remove_prefix(1);
			item.turn = get64(record); item.group = get64(record); item.sequence = get64(record); item.payload.assign(record);
			items.push_back(std::move(item));
		}
	}
	if (assembling) { gap(false); }
}
}  // namespace turn_log_detail

}  // namespace kronuz::journal
