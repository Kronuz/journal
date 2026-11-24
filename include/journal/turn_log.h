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

// RFTW0002: one journal of turns shared by every group on a core (strata Track C, D11).
// A turn is the records the core's groups staged, written in one write and made durable
// by one data barrier. Segments are preallocated, so appends never create files or change
// sizes. Every 512-byte sector carries its segment and a CRC-32C, so recovery tells a sector
// this segment wrote from a stale one of a recycled file's earlier use, an unwritten one, and
// a damaged one (docs/mechanisms/turn-log-damage.md).

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

class TurnLog {
public:
	// Recovers the journal in the IO's directory (empty for a new one), then starts a new
	// epoch whose open record is durable before any turn is admitted (condition 8).
	TurnLog(IO& io, Identity identity, TurnRecovery& recovered, TurnLogOptions options = {}, const TurnLogGroups& known = {})
		: io_(io), identity_(identity), options_(valid(options)) {
		recovered = recover(known);
		if (known.settle) { known.settle(recovered); }
		epoch_ = recovered.epoch; turn_ = recovered.last_turn;
		for (const auto& [group, records] : recovered.groups) { if (!records.empty()) { sequences_[group] = records.back().sequence; } }
		for (const auto& [group, bound] : known.bound) {
			if (!recovered.lost.contains(group)) { auto& last = sequences_[group]; last = std::max(last, bound); }
		}
		for (auto group : recovered.lost) { lost_.insert(group); }
		next_segment_ = highest_segment_ + 1;
		begin_segment();
		data_segments_.insert(segment_);
		write([&](turn_log_detail::Emitter& emitter) { emitter.record(turn_log_detail::view(turn_log_detail::header(turn_log_detail::open_record, turn_, 0, 0)), {}, 0, 0); });
	}
	// What a log holds, read without writing to it: a re-partition reads the stored generation.
	static TurnRecovery read(IO& io, Identity identity, TurnLogOptions options = {}, const TurnLogGroups& known = {}) {
		TurnLog log(io, identity, valid(options));
		return log.recover(known);
	}
	// Drops a group the host closed: its records stop being live, so retention reclaims them,
	// and it starts again with a checkpoint record, as a new group does.
	void forget(std::uint64_t group) {
		if (staged_ && staged_->next.contains(group)) { throw std::logic_error("the group has a turn in flight"); }
		if (auto found = index_.find(group); found != index_.end()) {
			for (const auto& [_, live] : found->second) { drop(live); }
			index_.erase(found);
		}
		if (auto found = evicted_.find(group); found != evicted_.end()) {
			for (const auto& live : found->second.live) { drop(live); }
			evicted_.erase(found);
		}
		sequences_.erase(group);
	}
	// Evicts a group no writer holds: its live records stay live, so retention keeps and forwards
	// them, but the log keeps only their positions, and no sequence or index entry. Nothing of
	// the group may be in the turn in flight, and it stages nothing until live() restores it.
	// Parking (Host::park) is the durable step after it: the table takes the group.
	void evict(std::uint64_t group) {
		if (staged_ && staged_->next.contains(group)) { throw std::logic_error("the group has a turn in flight"); }
		auto found = index_.find(group);
		if (found == index_.end() || found->second.empty()) { return; }
		Evicted evicted; evicted.first = found->second.begin()->first; evicted.live.reserve(found->second.size());
		for (const auto& [sequence, live] : found->second) {
			if (sequence != evicted.first + evicted.live.size()) { throw std::logic_error("a group's live records are not consecutive"); }
			evicted.live.push_back(live);
		}
		if (evicted.first + evicted.live.size() - 1 != sequence(group)) { throw std::logic_error("a group's live records end before its sequence"); }
		evicted_.emplace(group, std::move(evicted));
		index_.erase(found); sequences_.erase(group);
	}
	bool evicted(std::uint64_t group) const { return evicted_.contains(group); }
	// Refuses the id from now on, as recovery refuses a lost one: a group its host lost after
	// recovery (Host::lose), once forgotten.
	void refuse(std::uint64_t group) { lost_.insert(group); }
	// Keeps a group whose caller stores `bound` for it durably (TurnLogGroups::bound): a bounded
	// lost group (TurnRecovery::bounded) is no longer lost, its records after the bound are live
	// again, and it continues after them and the bound, as a re-partition's new log continues it.
	void keep(std::uint64_t group, std::uint64_t bound) {
		if (!bound) { throw std::invalid_argument("a kept group needs a bound"); }
		std::uint64_t last = bound;
		if (auto found = bounded_.find(group); found != bounded_.end()) {
			if (found->second.bound != bound) { throw std::logic_error("the group's bound is another"); }
			if (!found->second.live.empty()) {
				auto& index = index_[group];
				for (const auto& [sequence, live] : found->second.live) { index.emplace(sequence, live); ++live_counts_[live.segment]; }
				last = std::max(last, found->second.live.rbegin()->first);
			}
			bounded_.erase(found);
		}
		auto& sequence = sequences_[group]; sequence = std::max(sequence, last);
		lost_.erase(group);
	}
	// A group's live records from its latest checkpoint record, read back by position; an evicted
	// group is restored. A record that does not read back as itself throws Corruption and
	// changes nothing.
	std::vector<TurnRecord> live(std::uint64_t group) {
		const auto positions = live_positions(group);
		auto records = read_live(group, positions);
		if (auto evicted = evicted_.find(group); evicted != evicted_.end()) {
			auto& index = index_[group];
			for (const auto& [sequence, live] : positions) { index.emplace(sequence, live); }
			sequences_[group] = positions.back().first;
			evicted_.erase(evicted);
		}
		return records;
	}
	// As live(), leaving an evicted group evicted.
	std::vector<TurnRecord> peek(std::uint64_t group) { return read_live(group, live_positions(group)); }
	// Groups whose only live record is a checkpoint, held or not: what Host::park may take into
	// its table.
	std::vector<std::uint64_t> idle() const {
		std::vector<std::uint64_t> groups;
		for (const auto& [group, records] : index_) { if (records.size() == 1 && records.begin()->second.kind == TurnRecordKind::Checkpoint) { groups.push_back(group); } }
		for (const auto& [group, evicted] : evicted_) { if (evicted.live.size() == 1 && evicted.live[0].kind == TurnRecordKind::Checkpoint) { groups.push_back(group); } }
		return groups;
	}
	// Preallocates spare segments; the host calls it off the turn.
	void prepare() { while (spares_.size() < options_.spare_segments) { spares_.push_back(preallocate()); } }
	// Appends one turn and makes it durable. Each group's sequence continues its last one; a
	// new group starts at 1 or with a checkpoint. A lost group rejoins under a new id.
	std::uint64_t append(std::span<const TurnRecord> records) {
		auto write = stage(records);
		try {
			if (write.file->write_at(write.offset, write.bytes) != write.bytes.size()) { throw std::runtime_error("short turn log write"); }
			flush(write);
		} catch (...) { failed(); throw; }
		completed();
		return write.turn;
	}
	// The write a staged turn needs: its bytes, whole sectors, at a file offset of a segment, then
	// one File::sync_data. A host driving completion I/O issues them and reports back.
	struct TurnWrite { File* file; std::uint64_t offset; std::string_view bytes; std::uint64_t turn; };
	// The turn's data barrier, through the IO seam; a driver calls it where it would File::sync_data.
	void flush(const TurnWrite& write) { detail::sync_data_file(io_, *write.file); }
	// Encodes a turn; one is in flight at a time (condition 7). Nothing it changes is
	// visible until completed().
	TurnWrite stage(std::span<const TurnRecord> records) {
		if (failed_) { throw std::logic_error("turn log failed"); }
		if (staged_) { throw std::logic_error("a turn is already in flight"); }
		Staged turn = std::move(spare_); turn.turn = turn_ + 1; turn.next.clear(); turn.notes.clear();
		for (const auto& record : records) {
			if (!record.group) { throw std::invalid_argument("group 0 is reserved"); }
			if (lost_.contains(record.group)) { throw std::invalid_argument("a lost group rejoins under a new id"); }
			if (evicted_.contains(record.group)) { throw std::logic_error("an evicted group stages nothing until it is restored"); }
			auto found = turn.next.find(record.group); const bool fresh = found == turn.next.end();
			if (fresh) { found = next_nodes_.put(turn.next, turn.next.end(), record.group, sequence(record.group)); }
			auto& last = found->second;
			bool start = fresh && !sequences_.contains(record.group) && record.kind == TurnRecordKind::Checkpoint && record.sequence;
			if (record.sequence != last + 1 && !start) {
				throw std::invalid_argument("turn record out of its group's sequence");
			}
			if (record.kind == TurnRecordKind::Checkpoint && record.covers < last) {
				throw std::invalid_argument("a checkpoint behind a record it would drop");
			}
			last = record.sequence;
		}
		for (const auto& record : records) { turn.notes.push_back({record.group, record.sequence, record.kind, record.payload.size(), false}); }
		forward_slice();
		// Each record's header and payload, and each copy read back from its segment, framed
		// straight into the turn's buffer.
		auto emit = [&](turn_log_detail::Emitter& emitter) {
			std::size_t i = 0;
			for (const auto& record : records) {
				const auto head = turn_log_detail::header(static_cast<std::uint8_t>(record.kind), turn.turn, record.group, record.sequence);
				const auto offset = emitter.record(turn_log_detail::view(head), record.payload, record.group, record.sequence);
				if (!emitter.measuring()) { turn.notes[i].offset = offset; }
				++i;
			}
			for (const auto& copy : copies_) {
				const auto head = turn_log_detail::header(static_cast<std::uint8_t>(copy.live.kind), turn.turn, copy.group, copy.sequence);
				Forward original{*this, copy, std::nullopt};
				const auto offset = emitter.record(turn_log_detail::view(head), copy.live.bytes, original, copy.group, copy.sequence);
				if (!emitter.measuring()) { original.finish(); turn.notes[i].offset = offset; }
				++i;
			}
			char count[4]; framing::store(count, records.size() + copies_.size(), 4);
			emitter.record(turn_log_detail::view(turn_log_detail::header(turn_log_detail::end_of_turn, turn.turn, 0, 0)), std::string_view(count, 4), 0, 0);
		};
		// Copies ride along only where the turn still fits in a segment.
		if (!copies_.empty() && !fits(emit, turn.filling)) { copies_.clear(); }
		for (const auto& copy : copies_) { turn.notes.push_back({copy.group, copy.sequence, copy.live.kind, copy.live.bytes, true}); }
		turn.end = encode(emit, turn.bytes, turn.filling);
		turn.offset = turn_log_detail::file_offset(offset_ - offset_ % turn_log_detail::sector_data);
		staged_ = std::move(turn);
		return {file_.get(), staged_->offset, staged_->bytes, staged_->turn};
	}
	// How many of `records`, from the first, one turn encodes in an empty segment without
	// forwarded copies: what encode() accepts. Sequences are not checked.
	std::size_t fitting(std::span<const TurnRecord> records) {
		std::size_t n = 0;  // a turn holds at least each record's logical bytes
		for (std::uint64_t bytes = 0; n < records.size() && (bytes += turn_log_detail::record_header + records[n].payload.size()) <= options_.segment_bytes; ++n) {}
		auto fit = [&](std::size_t count) {
			return fits([&](turn_log_detail::Emitter& emitter) {
				for (const auto& record : records.first(count)) { emitter.record(turn_log_detail::view(turn_log_detail::header(static_cast<std::uint8_t>(record.kind), turn_ + 1, record.group, record.sequence)), record.payload, record.group, record.sequence); }
				char tally[4]; framing::store(tally, count, 4);
				emitter.record(turn_log_detail::view(turn_log_detail::header(turn_log_detail::end_of_turn, turn_ + 1, 0, 0)), std::string_view(tally, 4), 0, 0);
			}, spare_.filling);
		};
		if (fit(n)) { return n; }
		std::size_t low = 0;  // fits; n does not
		while (n - low > 1) { const auto mid = low + (n - low) / 2; (fit(mid) ? low : n) = mid; }
		return low;
	}
	// The staged turn's barrier returned: its records are durable.
	void completed() {
		if (!staged_) { throw std::logic_error("no turn in flight"); }
		auto turn = std::move(*staged_); staged_.reset();
		offset_ = turn.end; keep_partial(turn.bytes); std::swap(filling_, turn.filling); ++stats_.barriers; stats_.bytes_written += turn.bytes.size();
		turn_ = turn.turn; ++stats_.turns;
		for (const auto& [group, sequence] : turn.next) { sequences_[group] = sequence; }
		for (const auto& n : turn.notes) {
			if (!n.copy) { note(n.group, n.sequence, n.kind, n.offset, n.bytes); stats_.bytes_appended += n.bytes; continue; }
			// A copy replaces its original unless a checkpoint in this turn superseded both.
			if (auto evicted = evicted_.find(n.group); evicted != evicted_.end()) {
				auto& live = evicted->second.live;
				if (n.sequence >= evicted->second.first && n.sequence - evicted->second.first < live.size()) {
					auto& original = live[n.sequence - evicted->second.first]; drop(original);
					original = Live{segment_, epoch_, n.offset, n.kind, n.bytes}; ++live_counts_[segment_];
				}
			} else if (auto found = index_.find(n.group); found != index_.end() && found->second.contains(n.sequence)) { note(n.group, n.sequence, n.kind, n.offset, n.bytes, true); }
			stats_.forwarded_bytes += n.bytes; ++stats_.forwarded_records;
		}
		// The next turn reuses this one's buffers and map nodes.
		while (!turn.next.empty()) { next_nodes_.give(turn.next, turn.next.begin()); }
		turn.notes.clear(); spare_ = std::move(turn);
		reclaim();
	}
	// The staged turn's write or barrier failed: what it reached is unknown, so the log fails.
	void failed() noexcept { staged_.reset(); failed_ = true; }
	std::uint64_t epoch() const noexcept { return epoch_; }
	std::uint64_t last_turn() const noexcept { return turn_; }
	std::uint64_t segment() const noexcept { return segment_; }
	std::uint64_t position() const noexcept { return offset_; }  // a data position in the current segment
	std::uint64_t sequence(std::uint64_t group) const {
		if (auto found = sequences_.find(group); found != sequences_.end()) { return found->second; }
		auto found = evicted_.find(group); return found == evicted_.end() ? 0 : found->second.first + found->second.live.size() - 1;
	}
	const TurnLogStats& stats() const noexcept { return stats_; }
	const TurnLogOptions& options() const noexcept { return options_; }
	// Segments holding records, the current one included; the oldest goes first.
	const std::set<std::uint64_t>& data_segments() const noexcept { return data_segments_; }

private:
	TurnLog(IO& io, Identity identity, TurnLogOptions options) : io_(io), identity_(identity), options_(options) {}
	static TurnLogOptions valid(TurnLogOptions options) {
		if (!turn_log_detail::valid_block_bytes(options.block_bytes) ||
			options.segment_bytes < options.block_bytes || options.segment_bytes % options.block_bytes) {
			throw std::invalid_argument("invalid turn log geometry");
		}
		return options;
	}
	// A live record's position, kind and payload size, in 32 bytes.
	// Every field is the constructor's: a Live built without its payload size does not compile.
	struct Live {
		Live(std::uint64_t segment, std::uint64_t epoch, std::uint64_t offset, TurnRecordKind kind, std::uint64_t bytes) : segment(segment), epoch(epoch), offset(offset), kind(kind), bytes(bytes) {}
		std::uint64_t segment, epoch, offset; TurnRecordKind kind; std::uint64_t bytes : 56;
	};
	static_assert(sizeof(Live) == 32);
	// Map nodes the turn path frees, reused by later inserts: a warm turn allocates none. They
	// never outnumber what the map they came from held at its largest.
	template <class Map> struct Nodes {
		std::vector<typename Map::node_type> spare;
		typename Map::iterator put(Map& map, typename Map::const_iterator hint, const typename Map::key_type& key, const typename Map::mapped_type& value) {
			if (spare.empty()) { return map.emplace_hint(hint, key, value); }
			auto node = std::move(spare.back()); spare.pop_back();
			node.key() = key; node.mapped() = value;
			return map.insert(hint, std::move(node));
		}
		void give(Map& map, typename Map::const_iterator at) { spare.push_back(map.extract(at)); }
	};
	// An evicted group's live records, consecutive from `first`.
	struct Evicted { std::uint64_t first = 0; std::vector<Live> live; };
	void drop(const Live& live) { if (!--live_counts_[live.segment]) { live_counts_.erase(live.segment); } }
	std::vector<std::pair<std::uint64_t, Live>> live_positions(std::uint64_t group) const {
		std::vector<std::pair<std::uint64_t, Live>> positions;
		if (auto evicted = evicted_.find(group); evicted != evicted_.end()) {
			for (std::size_t i = 0; i < evicted->second.live.size(); ++i) { positions.emplace_back(evicted->second.first + i, evicted->second.live[i]); }
		} else if (auto found = index_.find(group); found != index_.end()) {
			positions.assign(found->second.begin(), found->second.end());
		}
		return positions;
	}
	// Reads records back by position; one that does not read back as itself throws Corruption.
	std::vector<TurnRecord> read_live(std::uint64_t group, const std::vector<std::pair<std::uint64_t, Live>>& positions) {
		std::vector<TurnRecord> records; records.reserve(positions.size());
		for (const auto& [sequence, live] : positions) {
			const auto bytes = turn_log_detail::read_record(reader(live.segment), turn_log_detail::stream(identity_, options_.block_bytes, live.segment, live.epoch), live.offset, held(live.segment));
			std::string_view logical(bytes);
			if (logical.size() < turn_log_detail::record_header || static_cast<std::uint8_t>(logical[0]) != static_cast<std::uint8_t>(live.kind)) { throw Corruption("a live record does not read back as itself"); }
			logical.remove_prefix(1); get64(logical);  // the kind, then the turn
			if (get64(logical) != group || get64(logical) != sequence) { throw Corruption("a live record does not read back as itself"); }
			records.push_back({group, sequence, live.kind, std::string(logical)});
		}
		return records;
	}
	// A checkpoint supersedes the group's earlier records; a copy only replaces its original.
	// The new record counts first, so the current segment's count never passes through zero.
	void note(std::uint64_t group, std::uint64_t sequence, TurnRecordKind kind, std::uint64_t offset, std::uint64_t bytes, bool copy = false) {
		auto& records = index_[group]; ++live_counts_[segment_];
		if (kind == TurnRecordKind::Checkpoint && !copy) { while (!records.empty()) { drop(records.begin()->second); index_nodes_.give(records, records.begin()); } }
		if (auto found = records.find(sequence); found != records.end()) { drop(found->second); index_nodes_.give(records, found); }
		index_nodes_.put(records, records.end(), sequence, Live{segment_, epoch_, offset, kind, bytes});
	}
	// Whether a turn fits an empty segment, laid out without writing it in `manifest`, a turn
	// manifest not in use.
	template <class Emit> bool fits(Emit&& emit, framing::Manifest& manifest) const {
		manifest.groups.reserve(turn_log_detail::block_groups(options_.block_bytes)); manifest = filling_;
		auto empty = turn_log_detail::Emitter::measure(turn_log_detail::stream(identity_, options_.block_bytes, segment_, epoch_), 0, std::move(manifest));
		emit(empty);
		manifest = std::move(empty).filling();
		return empty.position() <= turn_log_detail::data_bytes(options_.segment_bytes);
	}
	// A live record forward_slice chose; stage() reads it straight into the turn's buffer.
	struct Copy { std::uint64_t group, sequence; Live live; };
	// Past retain_segments, the oldest segment's live records move forward, a bounded slice
	// per turn (at most a quarter segment), keeping their group and sequence (condition 9).
	void forward_slice() {
		copies_.clear();
		if (data_segments_.size() <= options_.retain_segments) { return; }
		const auto oldest = *data_segments_.begin();
		if (oldest == segment_ || !live_counts_.contains(oldest)) { return; }
		std::size_t bytes = 0;
		// Chooses one live record of the oldest segment; true once the slice is full.
		auto forward = [&](std::uint64_t group, std::uint64_t sequence, const Live& live) {
			++stats_.forward_scanned;
			if (live.segment != oldest) { return false; }
			copies_.push_back({group, sequence, live});
			return (bytes += live.bytes) >= std::min<std::uint64_t>(options_.forward_bytes, options_.segment_bytes / 4);
		};
		for (const auto& [group, records] : index_) {
			for (const auto& [sequence, live] : records) { if (forward(group, sequence, live)) { return; } }
		}
		for (const auto& [group, evicted] : evicted_) {
			for (std::size_t i = 0; i < evicted.live.size(); ++i) { if (forward(group, evicted.first + i, evicted.live[i])) { return; } }
		}
	}
	// A copy's payload, read from its original into the emitter's buffer: the original's header
	// must name the same record, and its payload end where the copy's does.
	struct Forward {
		TurnLog& log; const Copy& copy; std::optional<turn_log_detail::RecordReader> original;
		void operator()(std::string& out, std::size_t count) {
			open();
			const auto at = out.size(); out.resize(at + count); original->read(out.data() + at, count);
		}
		void finish() { open(); if (!original->done()) { throw Corruption("a live record does not read back as itself"); } }
		void open() {
			if (original) { return; }
			const auto& live = copy.live;
			original.emplace(log.reader(live.segment), turn_log_detail::stream(log.identity_, log.options_.block_bytes, live.segment, live.epoch), live.offset);
			turn_log_detail::RecordHeader head; original->read(head.data(), head.size());
			if (head != turn_log_detail::header(static_cast<std::uint8_t>(live.kind), framing::peek64(std::string_view(head.data() + 1, 8)), copy.group, copy.sequence)) {
				throw Corruption("a live record does not read back as itself");
			}
		}
	};
	// Releases segments from the oldest while no live record remains in them: recycled into
	// the spare pool while it is short, removed otherwise. Only the oldest go, each ordered
	// ahead of the next, so a power cut undoes only the newest of them and the turns left stay contiguous.
	void reclaim() {
		bool changed = false;
		while (!data_segments_.empty() && *data_segments_.begin() != segment_ && !live_counts_.contains(*data_segments_.begin())) {
			if (changed) { io_.order_files_and_directory({}); }
			const auto oldest = *data_segments_.begin();
			data_segments_.erase(data_segments_.begin()); readers_.erase(oldest);
			if (spares_.size() < options_.spare_segments) {
				const auto name = turn_log_detail::segment_name(next_segment_);
				io_.replace(turn_log_detail::segment_name(oldest), name); ++next_segment_;
				spares_.push_back(io_.open_existing(name)); ++stats_.segments_recycled;
			} else { io_.remove(turn_log_detail::segment_name(oldest)); ++stats_.segments_removed; }
			changed = true;
		}
		if (changed) { io_.sync_directory(); }
	}
	// While a turn is in flight, the sector its write rewrites reads from partial_.
	turn_log_detail::Held held(std::uint64_t segment) const {
		if (!staged_ || segment != segment_ || partial_.empty()) { return {}; }
		return {offset_ / turn_log_detail::sector_data, partial_};
	}
	File& reader(std::uint64_t segment) {
		auto& file = readers_[segment];
		if (!file) { file = io_.open_existing(turn_log_detail::segment_name(segment)); }
		return *file;
	}
	std::unique_ptr<File> preallocate() {
		auto file = io_.create_exclusive(turn_log_detail::segment_name(next_segment_++));
		const std::string zeros(std::min<std::uint64_t>(options_.segment_bytes, 1 << 20), '\0');
		for (std::uint64_t offset = 0; offset < options_.segment_bytes; offset += zeros.size()) {
			auto count = std::min<std::uint64_t>(zeros.size(), options_.segment_bytes - offset);
			if (file->write_at(offset, std::string_view(zeros).substr(0, count)) != count) { throw std::runtime_error("short turn log preallocation"); }
		}
		detail::sync_file(io_, *file); io_.sync_directory(); ++stats_.segments_prepared;
		return file;
	}
	void begin_segment() {
		if (spares_.empty()) { spares_.push_back(preallocate()); ++stats_.prepared_inline; }
		file_ = std::move(spares_.front()); spares_.pop_front();
		segment_ = next_segment_ - 1 - spares_.size(); offset_ = 0; partial_.clear();
		data_segments_.insert(segment_);
	}
	// Frames a turn into `bytes`, whole sectors from the one holding the current position, and
	// `filling` the manifest after it. A turn never crosses a segment: one that does not fit starts
	// the next one. Returns its end.
	// The manifest reserves what a block can name, so no turn grows it.
	template <class Emit> std::uint64_t encode(Emit&& emit, std::string& bytes, framing::Manifest& filling) {
		filling.groups.reserve(turn_log_detail::block_groups(options_.block_bytes)); filling = filling_;
		turn_log_detail::Emitter emitter(turn_log_detail::stream(identity_, options_.block_bytes, segment_, epoch_), offset_, std::move(filling), std::move(bytes), partial_);
		emit(emitter);
		if (emitter.position() > turn_log_detail::data_bytes(options_.segment_bytes)) {
			filling = std::move(emitter).filling(); bytes = std::move(emitter).bytes();
			if (!fits(emit, filling)) { throw std::length_error("turn exceeds a segment"); }
			begin_segment();
			filling = filling_;
			emitter = turn_log_detail::Emitter(turn_log_detail::stream(identity_, options_.block_bytes, segment_, epoch_), 0, std::move(filling), std::move(bytes));
			emit(emitter);
		}
		emitter.seal();
		const auto end = emitter.position();
		filling = std::move(emitter).filling(); bytes = std::move(emitter).bytes();
		return end;
	}
	template <class Emit> void write(Emit&& emit) {
		const auto end = encode(emit, spare_.bytes, spare_.filling);
		try {
			if (file_->write_at(turn_log_detail::file_offset(offset_ - offset_ % turn_log_detail::sector_data), spare_.bytes) != spare_.bytes.size()) { throw std::runtime_error("short turn log write"); }
			detail::sync_data_file(io_, *file_);
		} catch (...) { failed_ = true; throw; }
		offset_ = end; keep_partial(spare_.bytes); std::swap(filling_, spare_.filling); ++stats_.barriers; stats_.bytes_written += spare_.bytes.size();
	}
	// Keeps the data a write left in its last sector, where the next write starts.
	void keep_partial(const std::string& bytes) {
		const auto keep = offset_ % turn_log_detail::sector_data;
		partial_.assign(keep ? bytes.data() + bytes.size() - turn_log_detail::sector_bytes : bytes.data(), keep);
	}
	TurnRecovery recover(const TurnLogGroups& known);

	IO& io_; Identity identity_; TurnLogOptions options_;
	std::unique_ptr<File> file_; std::deque<std::unique_ptr<File>> spares_;
	std::uint64_t epoch_ = 0, turn_ = 0, segment_ = 0, offset_ = 0, next_segment_ = 1, highest_segment_ = 0;
	std::map<std::uint64_t, std::uint64_t> sequences_; std::set<std::uint64_t> lost_;
	std::map<std::uint64_t, std::map<std::uint64_t, Live>> index_; std::map<std::uint64_t, std::size_t> live_counts_;
	std::map<std::uint64_t, Evicted> evicted_;  // groups no writer holds: evict()
	struct Bounded { std::uint64_t bound = 0; std::map<std::uint64_t, Live> live; };
	std::map<std::uint64_t, Bounded> bounded_;  // bounded lost groups, until keep()
	std::set<std::uint64_t> data_segments_; std::map<std::uint64_t, std::unique_ptr<File>> readers_;
	framing::Manifest filling_;  // unknown until the first block this open enters
	std::string partial_;  // the data before offset_ in its sector, which the next write rewrites
	struct Note { std::uint64_t group, sequence; TurnRecordKind kind; std::size_t bytes; bool copy; std::uint64_t offset = 0; };
	struct Staged { std::uint64_t turn = 0, offset = 0, end = 0; std::map<std::uint64_t, std::uint64_t> next; std::vector<Note> notes; framing::Manifest filling; std::string bytes; };
	std::optional<Staged> staged_;
	Staged spare_;  // the last turn's buffers, emptied: the next stage() frames into their capacity
	Nodes<std::map<std::uint64_t, std::uint64_t>> next_nodes_; Nodes<std::map<std::uint64_t, Live>> index_nodes_;
	std::vector<Copy> copies_;  // forward copies
	TurnLogStats stats_; bool failed_ = false;
};

// Reads every segment in order. Within the newest epoch the last turn is the torn tail:
// kept only whole. For an earlier epoch, the next epoch's open record names the last turn
// that open kept; later turns are ignored, and damage at or before it is corruption.
inline TurnRecovery TurnLog::recover(const TurnLogGroups& known) {
	using namespace turn_log_detail;
	const auto block_data = data_bytes(options_.block_bytes);
	// The table's floor for a group: its records through its closed base or its stored bound.
	auto held = [](const std::map<std::uint64_t, std::uint64_t>& floors, std::uint64_t group) { auto found = floors.find(group); return found == floors.end() ? 0 : found->second; };
	auto superseded = [&](std::uint64_t group, std::uint64_t sequence) { return sequence <= std::max(held(known.base, group), held(known.bound, group)); };
	std::vector<std::uint64_t> segments;
	auto cursor = io_.scan_directory();
	while (auto name = cursor->next()) { if (auto number = segment_number(*name)) { segments.push_back(*number); } }
	std::sort(segments.begin(), segments.end());
	std::vector<Item> items; Manifests manifests;
	for (auto number : segments) {
		highest_segment_ = std::max(highest_segment_, number); data_segments_.insert(number);
		auto file = io_.open_existing(segment_name(number));
		std::vector<Item> segment; scan_segment(*file, identity_, options_, number, segment, manifests);
		for (auto it = segment.rbegin(); it != segment.rend() && it->gap; ++it) { it->tail = true; }  // unwritten, or damage
		for (auto& item : segment) { items.push_back(std::move(item)); }
	}
	// Opens, in stream order: each names its epoch and the last turn it kept. An epoch whose
	// open record did not read back is inferred: the oldest segments may have been
	// reclaimed with the first epoch's, which then starts at its first turn read; a later
	// one kept its previous epoch's last complete turn, which read back whole then as now
	// (condition 8). Its block's damage costs what any block's does.
	struct Open { std::uint64_t epoch, kept; std::size_t at, begin; bool inferred = false; };
	std::vector<Open> opens;
	for (std::size_t i = 0; i < items.size(); ++i) {
		if (items[i].gap) { continue; }
		const auto epoch = items[i].epoch;
		if (!opens.empty() && epoch < opens.back().epoch) { throw Corruption("turn log epochs out of order"); }
		if (items[i].kind == open_record) {
			if (!opens.empty() && epoch == opens.back().epoch) {
				if (!opens.back().inferred) { throw Corruption("turn log epoch opened twice"); }
				opens.back() = {epoch, items[i].turn, i, i + 1};
			} else { opens.push_back({epoch, items[i].turn, i, i + 1}); }
		} else if (opens.empty() || epoch != opens.back().epoch) {
			std::uint64_t first = std::numeric_limits<std::uint64_t>::max();
			for (std::size_t k = i; k < items.size() && (items[k].gap || items[k].epoch == epoch); ++k) {
				if (!items[k].gap && items[k].kind != open_record) { first = std::min(first, items[k].turn); }
			}
			// Reclamation removes whole segments, so the first epoch's leading gaps are damage.
			opens.push_back({epoch, first - 1, i, opens.empty() ? 0 : i, true});
		}
	}
	TurnRecovery result;
	if (opens.empty()) { result.epoch = 1; return result; }
	// Turns per epoch: contiguous from the open's kept turn, each complete when its
	// end-of-turn record counts the records before it. Damage before a later complete turn
	// (a gap, an incomplete turn, a missing one) costs the groups the next block's manifest
	// names for the damaged block; where no manifest reads back, it costs every group: a record
	// after it may be a forwarded copy, which shows nothing of what its group wrote there.
	struct Turn { std::vector<std::size_t> records; bool complete = false; std::size_t first = 0, end = 0; };
	// named: each group damage may have cost, with the highest sequence it may have held.
	std::vector<std::size_t> kept_records; std::map<std::uint64_t, std::uint64_t> named; std::set<std::size_t> counted;
	auto name = [&](std::uint64_t group, std::uint64_t sequence) { auto& bound = named[group]; bound = std::max(bound, sequence); };
	bool unknown = false, damaged = false;  // unknown: damage whose groups no manifest names
	// A gap costs what every block it covers names, through its last that may begin a lost record.
	auto gap_damage = [&](std::size_t g) {
		if (!counted.insert(g).second) { return; }
		damaged = true;
		const auto& gap = items[g];
		for (auto block = gap.offset / block_data; block <= std::max(gap.offset, gap.last) / block_data; ++block) {
			auto found = manifests.find({gap.segment, static_cast<std::uint32_t>(block)});
			if (found == manifests.end()) { unknown = true; return; }
			for (const auto& [group, sequence] : found->second.groups) { if (!superseded(group, sequence)) { name(group, sequence); } }
		}
	};
	std::uint64_t last_turn = 0;
	// Per epoch, the block where its last kept turn ends: a manifest is acknowledged only if
	// the turn that wrote its header was kept, which in its epoch means at or before there.
	std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> kept_end;
	auto keep_end = [&](std::uint64_t epoch, std::size_t i) { kept_end[epoch] = {items[i].segment, items[i].offset / block_data}; };
	for (std::size_t e = 0; e < opens.size(); ++e) {
		auto& open = opens[e];
		const bool newest_epoch = e + 1 == opens.size();
		if (e && open.inferred) { open.kept = last_turn; }
		if (e && open.kept != last_turn) { throw Corruption("open record names a turn its epoch did not keep"); }
		if (!e) { last_turn = open.kept; }
		const auto limit = newest_epoch || opens[e + 1].inferred ? std::numeric_limits<std::uint64_t>::max() : opens[e + 1].kept;
		const auto stop = newest_epoch ? items.size() : opens[e + 1].at;
		std::map<std::uint64_t, Turn> turns; std::vector<std::size_t> gaps, tails;
		for (std::size_t i = open.begin; i < stop; ++i) {
			const auto& item = items[i];
			if (item.tail && !item.damage) { tails.push_back(i); continue; }  // damage no crash leaves is no unwritten end
			const bool recognized = (item.kind >= 1 && item.kind <= 3) || item.kind == end_of_turn;
			if (item.gap || item.epoch != open.epoch || !recognized) { gaps.push_back(i); continue; }
			if (item.turn <= open.kept || item.turn > limit) { continue; }
			auto [found, fresh] = turns.try_emplace(item.turn);
			auto& turn = found->second; if (fresh) { turn.first = i; }
			if (item.kind == end_of_turn) {
				std::string_view count(item.payload);
				turn.complete = !turn.complete && item.payload.size() == 4 && get32(count) == turn.records.size(); turn.end = i;
			} else if (turn.end) { turn.complete = false; } else { turn.records.push_back(i); }
		}
		// Records missing between two items: the gaps there, segment tails included, say which
		// blocks were damaged; with none, which groups is unknown.
		// A segment's tail is damage only where a manifest names its block as written; tails
		// after `prefer` are tried before those before it.
		// `from` is the first index after the previous kept turn.
		auto missing_between = [&](std::size_t from, std::size_t to, std::size_t prefer) {
			bool found = false;
			for (auto g : gaps) { if (g >= from && g < to) { gap_damage(g); found = true; } }
			auto written = [&](std::size_t g) { return manifests.contains({items[g].segment, static_cast<std::uint32_t>(items[g].offset / block_data)}); };
			for (auto [low, high] : {std::pair{prefer, to}, std::pair{from, prefer}}) {
				for (auto g : tails) { if (!found && g >= low && g < high && written(g)) { gap_damage(g); found = true; } }
			}
			if (!found) { unknown = true; }
		};
		std::uint64_t expected = open.kept + 1; std::size_t previous_end = open.begin;  // first index after the previous turn
		for (auto it = turns.begin(); it != turns.end(); ++it) {
			auto& [number, turn] = *it;
			if (!turn.complete) {
				if ((newest_epoch || opens[e + 1].inferred) && std::next(it) == turns.end()) { break; }  // the torn tail
				// Acknowledged, since a complete turn follows: its readable records are its own, and
				// the damaged block's manifest names the groups of the rest.
				damaged = true;
				const auto next = std::next(it) == turns.end() ? stop : std::next(it)->second.first;
				missing_between(std::min(previous_end, turn.first), next, turn.first + 1);
				for (auto r : turn.records) { kept_records.push_back(r); }
				last_turn = number; expected = number + 1; previous_end = std::max(turn.first, turn.end) + 1;
				keep_end(open.epoch, std::max({turn.first, turn.end, turn.records.empty() ? turn.first : turn.records.back()}));
				continue;
			}
			if (number != expected) { damaged = true; missing_between(previous_end, turn.first, previous_end); }
			for (auto g : gaps) { if (g < turn.end) { gap_damage(g); } }
			for (auto r : turn.records) { kept_records.push_back(r); }
			last_turn = number; expected = number + 1; previous_end = turn.end + 1; keep_end(open.epoch, turn.end);
		}
		// Past the newest epoch's last kept turn, damage no crash leaves may hide acknowledged
		// turns that nothing after it shows: it costs what any gap costs, and the turns dropped
		// there cost every group with a record in them. In an earlier epoch, the next open names
		// the turns it kept.
		if (newest_epoch) {
			// A write starts only after every earlier write's barrier returned, so a record of a turn
			// later than a failure's makes the failure damage (rule 6). A failure's turn is at least
			// the first not kept, and each record's before it (the next, past a turn's end).
			std::vector<std::uint64_t> at_least;  // per gap, in order
			std::uint64_t least = last_turn + 1;
			for (std::size_t i = previous_end; i < stop; ++i) {
				const auto& item = items[i];
				if (item.gap) { at_least.push_back(least); }
				else if (item.epoch == open.epoch) { least = std::max<std::uint64_t>(least, item.turn + (item.kind == end_of_turn)); }
			}
			std::uint64_t later = 0;
			for (auto i = stop; i-- > previous_end;) {
				auto& item = items[i];
				if (!item.gap && item.epoch == open.epoch) { later = std::max(later, item.turn); }
				if (item.gap) { if (later > at_least.back()) { item.damage = true; } at_least.pop_back(); }
			}
			bool hidden = false;
			for (std::size_t i = previous_end; i < stop; ++i) { if (items[i].damage) { gap_damage(i); hidden = true; } }
			for (std::size_t i = previous_end; hidden && i < stop; ++i) {
				const auto& item = items[i];
				if (!item.gap && item.group && item.epoch == open.epoch && item.turn > last_turn && !superseded(item.group, item.sequence)) { name(item.group, item.sequence); }
			}
		} else {
			// A sector that failed past an earlier epoch's last kept turn is damage too: the next
			// open's record, when it did not read back, or a turn its open did not keep.
			for (std::size_t i = previous_end; i < stop; ++i) { if (items[i].damage) { gap_damage(i); } }
		}
		if (!newest_epoch && last_turn != limit && limit != std::numeric_limits<std::uint64_t>::max()) {
			damaged = true; missing_between(previous_end, stop, previous_end);
			last_turn = limit;
		}
	}
	// Per group: copies agree, and sequences are contiguous from where its records start: its
	// latest checkpoint, else 1 or the table's bound + 1. A checkpoint supersedes what precedes it,
	// so damage below one the group kept costs it nothing. A group damage named, with no checkpoint
	// after the damage's bound, is lost and bounded: its records after the bound must start at
	// bound + 1, since every record of it a damaged block held is at or below the bound.
	std::map<std::uint64_t, std::map<std::uint64_t, std::size_t>> by_group;
	for (auto r : kept_records) {
		const auto& item = items[r];
		if (superseded(item.group, item.sequence)) { continue; }
		auto [it, inserted] = by_group[item.group].try_emplace(item.sequence, r);
		if (!inserted) {
			const auto& other = items[it->second];
			if (other.kind != item.kind || other.payload != item.payload) { result.lost.insert(item.group); }
			it->second = r;  // the later copy is the live one
		}
	}
	auto record_of = [&](std::uint64_t group, std::size_t r) { const auto& item = items[r]; return TurnRecord{group, item.sequence, static_cast<TurnRecordKind>(item.kind), item.payload}; };
	auto live_of = [&](std::size_t r) { const auto& item = items[r]; return Live{item.segment, item.epoch, item.offset, static_cast<TurnRecordKind>(item.kind), item.payload.size()}; };
	// bound: a lost group bounded at `bound`, holding its records after it.
	auto bound = [&](std::uint64_t group, std::uint64_t at, std::vector<TurnRecord> records, std::map<std::uint64_t, Live> live) {
		result.lost.insert(group);
		result.bounded[group] = {at, std::move(records)};
		bounded_[group] = {at, std::move(live)};
	};
	for (auto& [group, records] : by_group) {
		if (result.lost.contains(group)) { continue; }
		if (unknown) { result.lost.insert(group); continue; }
		const auto damage = named.find(group);
		const std::uint64_t floor = damage != named.end() ? damage->second : 0;  // named sequences pass the table's floor
		const auto after = records.upper_bound(floor);
		auto start = records.end();
		for (auto it = after; it != records.end(); ++it) {
			if (items[it->second].kind == static_cast<std::uint8_t>(TurnRecordKind::Checkpoint)) { start = it; }
		}
		const bool checkpoint = start != records.end();
		if (!checkpoint) {
			start = after;
			const auto first = damage != named.end() ? floor + 1 : held(known.bound, group) + 1;
			if (start == records.end()) { bound(group, floor, {}, {}); continue; }  // only damage leaves none after the floor
			if (start->first != first) { result.lost.insert(group); continue; }
		}
		std::vector<TurnRecord> kept; std::map<std::uint64_t, Live> live; auto expected = start->first;
		bool contiguous = true;
		for (auto it = start; it != records.end(); ++it, ++expected) {
			if (it->first != expected) { contiguous = false; break; }
			kept.push_back(record_of(group, it->second)); live.emplace(it->first, live_of(it->second));
		}
		if (!contiguous) { result.lost.insert(group); continue; }
		if (damage != named.end() && !checkpoint) { bound(group, floor, std::move(kept), std::move(live)); continue; }
		index_[group] = std::move(live);
		result.groups.emplace(group, std::move(kept));
	}
	// A group damage named that holds nothing after the floor: bounded, its records all gone.
	for (const auto& [group, at] : named) {
		if (result.groups.contains(group) || result.lost.contains(group)) { continue; }
		if (unknown) { result.lost.insert(group); continue; }
		bound(group, at, {}, {});
	}
	// A manifest written by a kept turn names acknowledged records: a group holding less than
	// it names lost its tail, even where the damage read as a segment's unwritten end, so it is
	// bounded there. Manifests a lost turn wrote (the newest epoch's torn tail, or an earlier
	// epoch's turns past what its next open kept) name nothing acknowledged.
	for (const auto& [described, manifest] : manifests) {
		auto end = kept_end.find(manifest.epoch);
		if (end == kept_end.end() || std::pair<std::uint64_t, std::uint64_t>{manifest.segment, manifest.block} > end->second) { continue; }
		for (const auto& [group, sequence] : manifest.groups) {
			if (superseded(group, sequence)) { continue; }
			const auto bounded = result.bounded.find(group);
			if (result.lost.contains(group) && bounded == result.bounded.end()) { continue; }
			std::optional<std::uint64_t> through;  // the group's last sequence recovery holds
			if (bounded != result.bounded.end()) { through = bounded->second.records.empty() ? bounded->second.bound : bounded->second.records.back().sequence; }
			else if (auto found = result.groups.find(group); found != result.groups.end()) { through = found->second.back().sequence; }
			if (through && *through >= sequence) { continue; }
			damaged = true;
			result.groups.erase(group); index_.erase(group);
			if (unknown) { result.lost.insert(group); result.bounded.erase(group); bounded_.erase(group); continue; }
			bound(group, std::max(sequence, bounded == result.bounded.end() ? 0 : bounded->second.bound), {}, {});
		}
	}
	if (damaged) {
		for (auto group : known.open) {
			if (!result.groups.contains(group) && !result.bounded.contains(group) && (unknown || !known.bound.contains(group))) { result.lost.insert(group); }
		}
	}
	for (auto group : known.lost) { result.lost.insert(group); result.groups.erase(group); result.bounded.erase(group); bounded_.erase(group); index_.erase(group); }
	for (const auto& [group, records] : index_) { for (const auto& [_, live] : records) { ++live_counts_[live.segment]; } }
	result.damaged = damaged;
	result.epoch = opens.back().epoch + 1; result.last_turn = last_turn;
	return result;
}

}  // namespace kronuz::journal
