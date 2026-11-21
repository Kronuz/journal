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

// RFTW0001: one journal of turns shared by every group on a core (strata Track C, D11).
// A turn is the records the core's groups staged, written in one write and made durable
// by one data barrier. Segments are preallocated, so appends never create files or change
// sizes; blocks and fragments are bound to their segment and epoch, so bytes left by a
// recycled segment's earlier use never read as current.

#pragma once

#include "framing.h"
#include <algorithm>
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
#include <vector>

namespace kronuz::journal {

struct TurnLogOptions {
	std::uint64_t segment_bytes = std::uint64_t{64} << 20;  // a multiple of block_bytes
	std::uint32_t block_bytes = 32 << 10;
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
// What recovery kept: each group's records from its latest checkpoint, in sequence order.
// A group whose sequence has a gap, or whose records damage may have held (every group, where
// no manifest names the damaged block's), is lost: it rejoins as a new incarnation (D11, D5).
// When damaged, a group the host holds open (TurnLogGroups::open) that is in neither map is
// lost too.
struct TurnRecovery {
	std::uint64_t epoch = 0;      // the epoch this open started
	std::uint64_t last_turn = 0;  // the last turn kept
	std::map<std::uint64_t, std::vector<TurnRecord>> groups;
	std::set<std::uint64_t> lost;
	bool damaged = false;
};
// What the host's group table knows before a log recovers (one directory per host): a lost id
// stays lost and refused whatever the log holds, a group's records through its base sequence
// are superseded by the table's copy, so recovery ignores them, and the open groups are the
// ones a damaged log loses when it holds nothing of them.
struct TurnLogGroups {
	std::set<std::uint64_t> lost;
	std::map<std::uint64_t, std::uint64_t> base;
	std::vector<std::uint64_t> open;
};
struct TurnLogStats {
	std::uint64_t turns = 0, barriers = 0, bytes_appended = 0, bytes_written = 0, segments_prepared = 0, prepared_inline = 0;
	std::uint64_t forwarded_records = 0, forwarded_bytes = 0, segments_recycled = 0, segments_removed = 0;
	std::uint64_t forward_scanned = 0;  // live records forward_slice visited: grows with the groups in the log
};

namespace turn_log_detail {
inline constexpr std::string_view magic{"RFTW0001", 8};
// A logical record: kind, turn, group and sequence, then the payload.
inline constexpr std::size_t record_header = 25;
inline constexpr std::uint8_t end_of_turn = 0x10, open_record = 0x11;

inline framing::Stream stream(const Identity& identity, std::uint32_t block_bytes, std::uint64_t segment, std::uint64_t epoch) {
	return {magic, identity, block_bytes, segment, epoch, true};
}
inline std::string logical(std::uint8_t kind, std::uint64_t turn, std::uint64_t group, std::uint64_t sequence, std::string_view payload) {
	std::string bytes; bytes.reserve(record_header + payload.size());
	bytes.push_back(static_cast<char>(kind)); put64(bytes, turn); put64(bytes, group); put64(bytes, sequence); bytes.append(payload);
	return bytes;
}
// A bound on a turn's bytes in an empty segment, whatever its manifests: each record costs its
// payload, its header and 40 bytes (a fragment header, a manifest entry); the end-of-turn record
// is one more; each block adds at most 126 and the first block's manifest half a block.
// docs/mechanisms/turn-seal.md derives it.
inline constexpr std::uint64_t record_cost(std::uint64_t payload) { return payload + record_header + 40; }
inline constexpr std::uint64_t block_cost = 126;
inline std::uint64_t turn_bound(std::uint64_t cost, std::uint32_t block_bytes) {
	const std::uint64_t b = block_bytes, fixed = cost + record_cost(4) + block_cost + b / 2;
	return fixed + (fixed * block_cost + (b - block_cost) - 1) / (b - block_cost);
}
// The record cost a turn may carry and still encode within `limit` bytes of an empty segment.
inline std::uint64_t turn_room(std::uint64_t limit, std::uint32_t block_bytes) {
	const std::uint64_t b = block_bytes, scaled = limit / b * (b - block_cost) + limit % b * (b - block_cost) / b;
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
	bool damage = false;  // a gap holding damage no crash leaves: damaged_header, damaged_fragment
	std::uint64_t epoch = 0;
	std::uint8_t kind = 0;
	std::uint64_t turn = 0, group = 0, sequence = 0;
	std::string payload;
	std::uint64_t segment = 0, offset = 0;  // the record's first fragment; a gap's block start
};
// Per described block: its groups' highest sequences, and where the header naming them is.
struct BlockManifest { std::map<std::uint64_t, std::uint64_t> groups; std::uint64_t segment = 0, epoch = 0; std::uint32_t block = 0; };
using Manifests = std::map<std::pair<std::uint64_t, std::uint32_t>, BlockManifest>;
// Damage no crash leaves, found where a block's header or a fragment fails to verify. A crash
// leaves each 512-byte sector old or new, and no earlier use of a recycled file holds bytes
// bound to this segment, so a sector holding them is this segment's: what it holds of the log
// verified, unless a sector boundary cuts it.
inline bool verifies_in_sector(std::string_view block, std::size_t from, std::size_t end, std::uint64_t segment) {
	for (auto p = from; p < end && p + framing::fragment_header <= block.size(); ++p) {
		const auto at = block.substr(p);
		if (framing::parse_fragment(at, block.size() - p, segment, framing::peek64(at.substr(16))).check == framing::Check::ok) { return true; }
	}
	return false;
}
inline std::size_t sector_end(std::string_view block, std::uint64_t offset, std::size_t at) {
	return std::min<std::size_t>(block.size(), at + framing::sector_bytes - (offset + at) % framing::sector_bytes);
}
// A header that fails while a fragment of this segment starts in its sector.
inline bool damaged_header(std::string_view block, std::uint64_t offset, std::uint64_t segment) {
	return verifies_in_sector(block, framing::header_bytes(0), sector_end(block, offset, 0), segment);
}
// A failing fragment is damage when a later fragment of its segment starts in its sector; when its
// header names its segment and epoch with a writer's type and reserved byte, and has a length no
// writer emits or its sector holds it whole; when its header names them and verifies with its length
// (at one its sector holds whole), type or reserved byte corrected; or when its header, or a header
// of its write before it in its sector (`written`), proves that sector and a fragment of the segment
// verifies in the next sector (docs/mechanisms/turn-log-damage.md).
inline bool damaged_fragment(std::string_view block, std::uint64_t offset, std::size_t at, std::uint64_t segment, std::uint64_t epoch, bool written) {
	const auto end = sector_end(block, offset, at);
	if (verifies_in_sector(block, at + 1, end, segment)) { return true; }
	const auto bytes = block.substr(at);
	const bool names = at + framing::fragment_header <= end && framing::peek64(bytes.substr(8)) == segment && framing::peek64(bytes.substr(16)) == epoch;
	const std::size_t length = framing::get16(bytes.substr(4)); const auto type = static_cast<std::uint8_t>(bytes[6]);
	const bool fits = length && framing::fragment_header + length <= block.size() - at;
	// Old bytes can name the segment and epoch, as a stale record's group and sequence do; the
	// type and reserved byte a writer emits before them are what make the header this write's.
	const bool named = names && type >= framing::Full && type <= framing::Last && !bytes[7];
	if (named && (!fits || at + framing::fragment_header + length <= end)) { return true; }
	// Crossing its sector: a fragment verifying in the next sector proves that sector too.
	if ((named || written) && end < block.size() && verifies_in_sector(block, end, sector_end(block, offset, end), segment)) { return true; }
	if (!names) { return false; }
	// Otherwise it reads as a tear unless one damaged field of its header is all that fails.
	std::string header(bytes.substr(4, framing::fragment_header - 4));
	auto verifies = [&](std::size_t payload) {
		Checksum checksum; checksum.update(header); checksum.update(bytes.substr(framing::fragment_header, payload));
		return checksum.value() == framing::peek32(bytes);
	};
	for (std::size_t whole = 1; at + framing::fragment_header + whole <= end; ++whole) {
		header[0] = static_cast<char>(whole); header[1] = static_cast<char>(whole >> 8);
		if (verifies(whole)) { return true; }
	}
	if (named || !fits) { return false; }
	header[0] = bytes[4]; header[1] = bytes[5]; header[3] = '\0';
	for (auto fragment : {framing::Full, framing::First, framing::Middle, framing::Last}) {
		header[2] = static_cast<char>(fragment);
		if (verifies(length)) { return true; }
	}
	return false;
}
inline void scan_segment(File& file, const Identity& identity, const TurnLogOptions& options, std::uint64_t segment, std::vector<Item>& items, Manifests& manifests) {
	std::uint64_t here = 0;
	auto gap = [&] { if (items.empty() || !items.back().gap) { Item item; item.gap = true; item.segment = segment; item.offset = here; items.push_back(std::move(item)); } };
	std::string block(options.block_bytes, '\0'), partial; bool assembling = false; std::uint64_t partial_epoch = 0, partial_offset = 0;
	// Where the last header or fragment that verified starts, while it does not end its write.
	std::optional<std::size_t> in_write;
	const auto size = std::min(file.size(), options.segment_bytes);
	for (std::uint64_t offset = 0, index = 0; offset + options.block_bytes <= size; offset += options.block_bytes, ++index) {
		here = offset;
		if (file.read_at(offset, block) != block.size()) { gap(); assembling = false; continue; }
		std::string_view view(block);
		const auto header = framing::parse_header(view, stream(identity, options.block_bytes, segment, 0), index);
		if (header.check != framing::Check::ok) {
			gap(); assembling = false;
			if (damaged_header(view, offset, segment)) { items.back().damage = true; }
			continue;
		}
		const auto epoch = header.epoch; const auto count = header.count; in_write = 0;
		if (count != framing::no_manifest) {
			auto& manifest = manifests[{framing::peek64(view.substr(44)), framing::peek32(view.substr(52))}];
			manifest.segment = segment; manifest.block = static_cast<std::uint32_t>(index); manifest.epoch = epoch;
			const auto entry = framing::header_fixed;
			for (std::size_t i = 0; i < count; ++i) { manifest.groups[framing::peek64(view.substr(entry + 16 * i))] = framing::peek64(view.substr(entry + 16 * i + 8)); }
		}
		std::size_t position = header.size;
		while (options.block_bytes - position >= framing::fragment_header + 1) {
			const auto fragment = framing::parse_fragment(view.substr(position), options.block_bytes - position, segment, epoch);
			if (fragment.check != framing::Check::ok) {
				gap(); assembling = false;
				const bool written = in_write && (offset + *in_write) / framing::sector_bytes == (offset + position) / framing::sector_bytes;
				if (damaged_fragment(view, offset, position, segment, epoch, written)) { items.back().damage = true; }
				break;
			}
			const auto type = fragment.type; const auto payload = fragment.payload; const auto length = payload.size();
			const auto start = position; position += framing::fragment_header + length;
			in_write = type == framing::First || type == framing::Middle ? std::optional{start} : std::nullopt;
			if (type == framing::Full || type == framing::First) {
				if (assembling) { gap(); }
				partial.assign(payload); partial_epoch = epoch; partial_offset = offset + position - framing::fragment_header - length; assembling = type == framing::First;
				if (assembling) { continue; }
			} else {
				if (!assembling || partial_epoch != epoch) { gap(); assembling = false; continue; }
				partial.append(payload);
				if (type == framing::Middle) { continue; }
				assembling = false;
			}
			if (partial.size() < record_header) { gap(); continue; }
			std::string_view record(partial);
			Item item; item.epoch = epoch; item.segment = segment; item.offset = partial_offset; item.kind = static_cast<std::uint8_t>(record[0]); record.remove_prefix(1);
			item.turn = get64(record); item.group = get64(record); item.sequence = get64(record); item.payload.assign(record);
			if (item.kind != end_of_turn && item.kind != open_record) { in_write = start; }
			items.push_back(std::move(item));
		}
	}
	if (assembling) { gap(); }
}
}  // namespace turn_log_detail

class TurnLog {
public:
	// Recovers the journal in the IO's directory (empty for a new one), then starts a new
	// epoch whose open record is durable before any turn is admitted (condition 8).
	TurnLog(IO& io, Identity identity, TurnRecovery& recovered, TurnLogOptions options = {}, const TurnLogGroups& known = {})
		: io_(io), identity_(identity), options_(valid(options)) {
		recovered = recover(known);
		epoch_ = recovered.epoch; turn_ = recovered.last_turn;
		for (const auto& [group, records] : recovered.groups) { if (!records.empty()) { sequences_[group] = records.back().sequence; } }
		for (auto group : recovered.lost) { lost_.insert(group); }
		next_segment_ = highest_segment_ + 1;
		begin_segment();
		data_segments_.insert(segment_);
		write({[&](framing::Emitter& emitter) { emitter.record(turn_log_detail::logical(turn_log_detail::open_record, turn_, 0, 0, {})); }});
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
	// The write a staged turn needs: its bytes at an offset of a segment, then one
	// File::sync_data. A host driving completion I/O issues them and reports back.
	struct TurnWrite { File* file; std::uint64_t offset; std::string_view bytes; std::uint64_t turn; };
	// The turn's data barrier, through the IO seam; a driver calls it where it would File::sync_data.
	void flush(const TurnWrite& write) { detail::sync_data_file(io_, *write.file); }
	// Encodes a turn; one is in flight at a time (condition 7). Nothing it changes is
	// visible until completed().
	TurnWrite stage(std::span<const TurnRecord> records) {
		if (failed_) { throw std::logic_error("turn log failed"); }
		if (staged_) { throw std::logic_error("a turn is already in flight"); }
		Staged turn; turn.turn = turn_ + 1;
		for (const auto& record : records) {
			if (!record.group) { throw std::invalid_argument("group 0 is reserved"); }
			if (lost_.contains(record.group)) { throw std::invalid_argument("a lost group rejoins under a new id"); }
			if (evicted_.contains(record.group)) { throw std::logic_error("an evicted group stages nothing until it is restored"); }
			auto [found, fresh] = turn.next.try_emplace(record.group, sequence(record.group));
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
		auto copies = forward_slice();
		std::vector<std::uint64_t> offsets;
		auto emit = [&](framing::Emitter& emitter) {
			offsets.clear();
			for (const auto& record : records) { offsets.push_back(emitter.record(turn_log_detail::logical(static_cast<std::uint8_t>(record.kind), turn.turn, record.group, record.sequence, record.payload), record.group, record.sequence)); }
			for (const auto& copy : copies) { offsets.push_back(emitter.record(turn_log_detail::logical(static_cast<std::uint8_t>(copy.kind), turn.turn, copy.group, copy.sequence, copy.payload), copy.group, copy.sequence)); }
			std::string count; put32(count, static_cast<std::uint32_t>(records.size() + copies.size()));
			emitter.record(turn_log_detail::logical(turn_log_detail::end_of_turn, turn.turn, 0, 0, count));
		};
		// Copies ride along only where the turn still fits in a segment.
		if (!copies.empty() && !fits(emit)) { copies.clear(); }
		for (const auto& copy : copies) { turn.notes.push_back({copy.group, copy.sequence, copy.kind, copy.payload.size(), true}); }
		auto emitter = encode(emit);
		for (std::size_t i = 0; i < turn.notes.size(); ++i) { turn.notes[i].offset = offsets[i]; }
		turn.offset = offset_; turn.end = emitter.position(); turn.filling = emitter.filling(); turn.bytes = emitter.bytes();
		staged_ = std::move(turn);
		return {file_.get(), staged_->offset, staged_->bytes, staged_->turn};
	}
	// How many of `records`, from the first, one turn encodes in an empty segment without
	// forwarded copies: what encode() accepts. Sequences are not checked.
	std::size_t fitting(std::span<const TurnRecord> records) const {
		std::size_t n = 0;  // a turn holds at least each record's logical bytes
		for (std::uint64_t bytes = 0; n < records.size() && (bytes += turn_log_detail::record_header + records[n].payload.size()) <= options_.segment_bytes; ++n) {}
		auto fit = [&](std::size_t count) {
			return fits([&](framing::Emitter& emitter) {
				for (const auto& record : records.first(count)) { emitter.record(turn_log_detail::logical(static_cast<std::uint8_t>(record.kind), turn_ + 1, record.group, record.sequence, record.payload), record.group, record.sequence); }
				std::string tally; put32(tally, static_cast<std::uint32_t>(count));
				emitter.record(turn_log_detail::logical(turn_log_detail::end_of_turn, turn_ + 1, 0, 0, tally));
			});
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
		offset_ = turn.end; filling_ = turn.filling; ++stats_.barriers; stats_.bytes_written += turn.bytes.size();
		turn_ = turn.turn; ++stats_.turns;
		for (const auto& [group, sequence] : turn.next) { sequences_[group] = sequence; }
		for (const auto& n : turn.notes) {
			if (!n.copy) { note(n.group, n.sequence, n.kind, n.offset); stats_.bytes_appended += n.bytes; continue; }
			// A copy replaces its original unless a checkpoint in this turn superseded both.
			if (auto evicted = evicted_.find(n.group); evicted != evicted_.end()) {
				auto& live = evicted->second.live;
				if (n.sequence >= evicted->second.first && n.sequence - evicted->second.first < live.size()) {
					auto& original = live[n.sequence - evicted->second.first]; drop(original);
					original = Live{segment_, epoch_, n.offset, n.kind}; ++live_counts_[segment_];
				}
			} else if (auto found = index_.find(n.group); found != index_.end() && found->second.contains(n.sequence)) { note(n.group, n.sequence, n.kind, n.offset, true); }
			stats_.forwarded_bytes += n.bytes; ++stats_.forwarded_records;
		}
		reclaim();
	}
	// The staged turn's write or barrier failed: what it reached is unknown, so the log fails.
	void failed() noexcept { staged_.reset(); failed_ = true; }
	std::uint64_t epoch() const noexcept { return epoch_; }
	std::uint64_t last_turn() const noexcept { return turn_; }
	std::uint64_t segment() const noexcept { return segment_; }
	std::uint64_t position() const noexcept { return offset_; }  // in the current segment
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
		if (!options.block_bytes || !framing::valid_block_bytes(options.block_bytes) ||
			options.segment_bytes < options.block_bytes || options.segment_bytes % options.block_bytes) {
			throw std::invalid_argument("invalid turn log geometry");
		}
		return options;
	}
	struct Live { std::uint64_t segment, epoch, offset; TurnRecordKind kind; };
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
			const auto bytes = framing::read_record(reader(live.segment), turn_log_detail::stream(identity_, options_.block_bytes, live.segment, live.epoch), live.offset);
			std::string_view logical(bytes);
			if (logical.size() < turn_log_detail::record_header || static_cast<std::uint8_t>(logical[0]) != static_cast<std::uint8_t>(live.kind)) { throw Corruption("a live record does not read back as itself"); }
			logical.remove_prefix(1); get64(logical);  // the kind, then the turn
			if (get64(logical) != group || get64(logical) != sequence) { throw Corruption("a live record does not read back as itself"); }
			records.push_back({group, sequence, live.kind, std::string(logical)});
		}
		return records;
	}
	// A checkpoint supersedes the group's earlier records; a copy only replaces its original.
	void note(std::uint64_t group, std::uint64_t sequence, TurnRecordKind kind, std::uint64_t offset, bool copy = false) {
		auto& records = index_[group];
		if (kind == TurnRecordKind::Checkpoint && !copy) { for (const auto& [_, live] : records) { drop(live); } records.clear(); }
		if (auto found = records.find(sequence); found != records.end()) { drop(found->second); records.erase(found); }
		records.emplace(sequence, Live{segment_, epoch_, offset, kind}); ++live_counts_[segment_];
	}
	bool fits(const std::function<void(framing::Emitter&)>& emit) const {
		framing::Emitter fresh(turn_log_detail::stream(identity_, options_.block_bytes, segment_, epoch_), 0, filling_);
		emit(fresh); return fresh.position() <= options_.segment_bytes;
	}
	// Past retain_segments, the oldest segment's live records move forward, a bounded slice
	// per turn (at most a quarter segment), keeping their group and sequence (condition 9).
	std::vector<TurnRecord> forward_slice() {
		std::vector<TurnRecord> copies;
		if (data_segments_.size() <= options_.retain_segments) { return copies; }
		const auto oldest = *data_segments_.begin();
		if (oldest == segment_ || !live_counts_.contains(oldest)) { return copies; }
		std::size_t bytes = 0;
		// Copies one live record of the oldest segment; true once the slice is full.
		auto forward = [&](std::uint64_t group, std::uint64_t sequence, const Live& live) {
			++stats_.forward_scanned;
			if (live.segment != oldest) { return false; }
			auto record = framing::read_record(reader(oldest), turn_log_detail::stream(identity_, options_.block_bytes, oldest, live.epoch), live.offset);
			std::string_view logical = record; logical.remove_prefix(turn_log_detail::record_header);
			copies.push_back({group, sequence, live.kind, std::string(logical)});
			return (bytes += logical.size()) >= std::min<std::uint64_t>(options_.forward_bytes, options_.segment_bytes / 4);
		};
		for (const auto& [group, records] : index_) {
			for (const auto& [sequence, live] : records) { if (forward(group, sequence, live)) { return copies; } }
		}
		for (const auto& [group, evicted] : evicted_) {
			for (std::size_t i = 0; i < evicted.live.size(); ++i) { if (forward(group, evicted.first + i, evicted.live[i])) { return copies; } }
		}
		return copies;
	}
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
	File& reader(std::uint64_t segment) {
		auto& file = readers_[segment];
		if (!file) { file = io_.open_existing(turn_log_detail::segment_name(segment)); }
		return *file;
	}
	struct Body { std::function<void(framing::Emitter&)> emit; };
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
		segment_ = next_segment_ - 1 - spares_.size(); offset_ = 0;
		data_segments_.insert(segment_);
	}
	// A turn never crosses a segment: one that does not fit starts the next one.
	framing::Emitter encode(const std::function<void(framing::Emitter&)>& emit) {
		framing::Emitter emitter(turn_log_detail::stream(identity_, options_.block_bytes, segment_, epoch_), offset_, filling_);
		emit(emitter);
		if (emitter.position() > options_.segment_bytes) {
			framing::Emitter fresh(turn_log_detail::stream(identity_, options_.block_bytes, segment_, epoch_), 0, filling_);
			emit(fresh);
			if (fresh.position() > options_.segment_bytes) { throw std::length_error("turn exceeds a segment"); }
			begin_segment();
			emitter = framing::Emitter(turn_log_detail::stream(identity_, options_.block_bytes, segment_, epoch_), 0, filling_);
			emit(emitter);
		}
		return emitter;
	}
	void write(Body body) {
		auto emitter = encode(body.emit);
		try {
			if (file_->write_at(offset_, emitter.bytes()) != emitter.bytes().size()) { throw std::runtime_error("short turn log write"); }
			detail::sync_data_file(io_, *file_);
		} catch (...) { failed_ = true; throw; }
		offset_ = emitter.position(); filling_ = emitter.filling(); ++stats_.barriers; stats_.bytes_written += emitter.bytes().size();
	}
	TurnRecovery recover(const TurnLogGroups& known);

	IO& io_; Identity identity_; TurnLogOptions options_;
	std::unique_ptr<File> file_; std::deque<std::unique_ptr<File>> spares_;
	std::uint64_t epoch_ = 0, turn_ = 0, segment_ = 0, offset_ = 0, next_segment_ = 1, highest_segment_ = 0;
	std::map<std::uint64_t, std::uint64_t> sequences_; std::set<std::uint64_t> lost_;
	std::map<std::uint64_t, std::map<std::uint64_t, Live>> index_; std::map<std::uint64_t, std::size_t> live_counts_;
	std::map<std::uint64_t, Evicted> evicted_;  // groups no writer holds: evict()
	std::set<std::uint64_t> data_segments_; std::map<std::uint64_t, std::unique_ptr<File>> readers_;
	framing::Manifest filling_;  // unknown until the first block this open enters
	struct Note { std::uint64_t group, sequence; TurnRecordKind kind; std::size_t bytes; bool copy; std::uint64_t offset = 0; };
	struct Staged { std::uint64_t turn = 0, offset = 0, end = 0; std::map<std::uint64_t, std::uint64_t> next; std::vector<Note> notes; framing::Manifest filling; std::string bytes; };
	std::optional<Staged> staged_;
	TurnLogStats stats_; bool failed_ = false;
};

// Reads every segment in order. Within the newest epoch the last turn is the torn tail:
// kept only whole. For an earlier epoch, the next epoch's open record names the last turn
// that open kept; later turns are ignored, and damage at or before it is corruption.
inline TurnRecovery TurnLog::recover(const TurnLogGroups& known) {
	using namespace turn_log_detail;
	auto superseded = [&](std::uint64_t group, std::uint64_t sequence) { auto found = known.base.find(group); return found != known.base.end() && sequence <= found->second; };
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
	std::vector<std::size_t> kept_records; std::set<std::uint64_t> named; std::set<std::size_t> counted;
	bool unknown = false, damaged = false;  // unknown: damage whose groups no manifest names
	auto gap_damage = [&](std::size_t g) {
		if (!counted.insert(g).second) { return; }
		damaged = true;
		auto found = manifests.find({items[g].segment, static_cast<std::uint32_t>(items[g].offset / options_.block_bytes)});
		if (found == manifests.end()) { unknown = true; return; }
		for (const auto& [group, sequence] : found->second.groups) { if (!superseded(group, sequence)) { named.insert(group); } }
	};
	std::uint64_t last_turn = 0;
	// Per epoch, the block where its last kept turn ends: a manifest is acknowledged only if
	// the turn that wrote its header was kept, which in its epoch means at or before there.
	std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> kept_end;
	auto keep_end = [&](std::uint64_t epoch, std::size_t i) { kept_end[epoch] = {items[i].segment, items[i].offset / options_.block_bytes}; };
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
			auto written = [&](std::size_t g) { return manifests.contains({items[g].segment, static_cast<std::uint32_t>(items[g].offset / options_.block_bytes)}); };
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
			bool hidden = false;
			for (std::size_t i = previous_end; i < stop; ++i) { if (items[i].damage) { gap_damage(i); hidden = true; } }
			for (std::size_t i = previous_end; hidden && i < stop; ++i) {
				const auto& item = items[i];
				if (!item.gap && item.group && item.epoch == open.epoch && item.turn > last_turn && !superseded(item.group, item.sequence)) { named.insert(item.group); }
			}
		}
		if (!newest_epoch && last_turn != limit && limit != std::numeric_limits<std::uint64_t>::max()) {
			damaged = true; missing_between(previous_end, stop, previous_end);
			last_turn = limit;
		}
	}
	// Per group: copies agree, sequences are contiguous from 1 or from a checkpoint, and a
	// checkpoint supersedes what precedes it.
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
	for (auto& [group, records] : by_group) {
		if (result.lost.contains(group)) { continue; }
		if (named.contains(group) || unknown) { result.lost.insert(group); continue; }
		auto start = records.end();
		for (auto it = records.begin(); it != records.end(); ++it) {
			if (items[it->second].kind == static_cast<std::uint8_t>(TurnRecordKind::Checkpoint)) { start = it; }
		}
		if (start == records.end()) {
			if (records.begin()->first != 1) { result.lost.insert(group); continue; }
			start = records.begin();
		}
		std::vector<TurnRecord> kept; auto expected = start->first;
		for (auto it = start; it != records.end(); ++it, ++expected) {
			if (it->first != expected) { result.lost.insert(group); break; }
			const auto& item = items[it->second];
			kept.push_back({group, item.sequence, static_cast<TurnRecordKind>(item.kind), item.payload});
			index_[group].emplace(item.sequence, Live{item.segment, item.epoch, item.offset, static_cast<TurnRecordKind>(item.kind)});
		}
		if (!result.lost.contains(group)) { result.groups.emplace(group, std::move(kept)); }
		else { index_.erase(group); }
	}
	// A manifest written by a kept turn names acknowledged records: a group holding less than
	// it names lost its tail, even where the damage read as a segment's unwritten end.
	// Manifests a lost turn wrote (the newest epoch's torn tail, or an earlier epoch's turns
	// past what its next open kept) name nothing acknowledged.
	for (const auto& [described, manifest] : manifests) {
		auto end = kept_end.find(manifest.epoch);
		if (end == kept_end.end() || std::pair<std::uint64_t, std::uint64_t>{manifest.segment, manifest.block} > end->second) { continue; }
		for (const auto& [group, sequence] : manifest.groups) {
			if (result.lost.contains(group) || superseded(group, sequence)) { continue; }
			auto found = result.groups.find(group);
			if (found == result.groups.end() || found->second.back().sequence < sequence) {
				damaged = true; result.lost.insert(group);
				if (found != result.groups.end()) { result.groups.erase(found); index_.erase(group); }
			}
		}
	}
	for (auto group : named) { if (!result.groups.contains(group)) { result.lost.insert(group); index_.erase(group); } }
	if (damaged) { for (auto group : known.open) { if (!result.groups.contains(group)) { result.lost.insert(group); } } }
	for (auto group : known.lost) { result.lost.insert(group); result.groups.erase(group); index_.erase(group); }
	for (const auto& [group, records] : index_) { for (const auto& [_, live] : records) { ++live_counts_[live.segment]; } }
	result.damaged = damaged;
	result.epoch = opens.back().epoch + 1; result.last_turn = last_turn;
	return result;
}

}  // namespace kronuz::journal
