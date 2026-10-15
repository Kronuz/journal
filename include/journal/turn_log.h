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

#include "primitives.h"
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
	bool operator==(const TurnRecord&) const = default;
};
// What recovery kept: each group's records from its latest checkpoint, in sequence order.
// A group whose sequence has a gap, or that wrote nothing after damage that may have held
// its records, is lost: it rejoins as a new incarnation (D11, D5). When damaged, a group
// the host knows that is in neither map is lost too.
struct TurnRecovery {
	std::uint64_t epoch = 0;      // the epoch this open started
	std::uint64_t last_turn = 0;  // the last turn kept
	std::map<std::uint64_t, std::vector<TurnRecord>> groups;
	std::set<std::uint64_t> lost;
	bool damaged = false;
};
struct TurnLogStats {
	std::uint64_t turns = 0, barriers = 0, bytes_appended = 0, bytes_written = 0, segments_prepared = 0, prepared_inline = 0;
	std::uint64_t forwarded_records = 0, forwarded_bytes = 0, segments_recycled = 0, segments_removed = 0;
};

namespace turn_log_detail {
inline constexpr std::string_view magic{"RFTW0001", 8};
inline constexpr std::size_t block_header = 48, fragment_header = 24, record_header = 25;
enum Fragment : std::uint8_t { Full = 1, First, Middle, Last };
inline constexpr std::uint8_t end_of_turn = 0x10, open_record = 0x11;

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
inline void put16(std::string& bytes, std::uint16_t value) { bytes.push_back(static_cast<char>(value)); bytes.push_back(static_cast<char>(value >> 8)); }
inline std::uint16_t get16(std::string_view bytes) { return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[0]) | static_cast<unsigned char>(bytes[1]) << 8); }
inline std::uint64_t peek64(std::string_view bytes) { return get64(bytes); }
inline std::uint32_t peek32(std::string_view bytes) { return get32(bytes); }

// Writes records as fragments into blocks from a segment offset, starting a block's header
// as the stream enters it and padding a block's tail too short for a fragment.
class Emitter {
public:
	Emitter(const Identity& identity, std::uint32_t block_bytes, std::uint64_t segment, std::uint64_t epoch, std::uint64_t position)
		: identity_(identity), block_(block_bytes), segment_(segment), epoch_(epoch), position_(position) {}
	// Returns the offset of the record's first fragment.
	std::uint64_t record(std::uint8_t kind, std::uint64_t turn, std::uint64_t group, std::uint64_t sequence, std::string_view payload) {
		std::string logical; logical.reserve(record_header + payload.size());
		logical.push_back(static_cast<char>(kind)); put64(logical, turn); put64(logical, group); put64(logical, sequence); logical.append(payload);
		std::string_view rest(logical); bool first = true; std::uint64_t start = 0;
		while (!rest.empty()) {
			enter_block();
			auto room = block_ - position_ % block_;
			if (room < fragment_header + 1) { pad(room); continue; }
			auto count = std::min<std::size_t>(rest.size(), room - fragment_header);
			bool last = count == rest.size();
			if (first) { start = position_; }
			fragment(first ? (last ? Full : First) : (last ? Last : Middle), rest.substr(0, count));
			rest.remove_prefix(count); first = false;
		}
		return start;
	}
	const std::string& bytes() const noexcept { return bytes_; }
	std::uint64_t position() const noexcept { return position_; }
private:
	void enter_block() {
		if (position_ % block_) { return; }
		auto start = bytes_.size();
		bytes_.append(magic); bytes_.append(identity_.data(), identity_.size());
		put64(bytes_, segment_); put32(bytes_, static_cast<std::uint32_t>(position_ / block_)); put64(bytes_, epoch_);
		put32(bytes_, crc32c(std::string_view(bytes_).substr(start)));
		position_ += block_header;
	}
	void pad(std::uint64_t count) { bytes_.append(count, '\0'); position_ += count; }
	void fragment(Fragment type, std::string_view payload) {
		std::string header; put16(header, static_cast<std::uint16_t>(payload.size())); header.push_back(static_cast<char>(type)); header.push_back('\0');
		put64(header, segment_); put64(header, epoch_);
		Checksum checksum; checksum.update(header); checksum.update(payload);
		put32(bytes_, checksum.value()); bytes_.append(header); bytes_.append(payload);
		position_ += fragment_header + payload.size();
	}
	Identity identity_; std::uint64_t block_, segment_, epoch_, position_; std::string bytes_;
};

// One parsed item of the stream: a whole logical record, or a gap where bytes did not
// read back as current (unwritten, stale, or damaged).
struct Item {
	bool gap = false;
	std::uint64_t epoch = 0;
	std::uint8_t kind = 0;
	std::uint64_t turn = 0, group = 0, sequence = 0;
	std::string payload;
	std::uint64_t segment = 0, offset = 0;  // the record's first fragment
};
inline void scan_segment(File& file, const Identity& identity, const TurnLogOptions& options, std::uint64_t segment, std::vector<Item>& items) {
	auto gap = [&] { if (items.empty() || !items.back().gap) { Item item; item.gap = true; items.push_back(std::move(item)); } };
	std::string block(options.block_bytes, '\0'), partial; bool assembling = false; std::uint64_t partial_epoch = 0, partial_offset = 0;
	const auto size = std::min(file.size(), options.segment_bytes);
	for (std::uint64_t offset = 0, index = 0; offset + options.block_bytes <= size; offset += options.block_bytes, ++index) {
		if (file.read_at(offset, block) != block.size()) { gap(); assembling = false; continue; }
		std::string_view view(block);
		auto header = view.substr(0, block_header);
		if (header.substr(0, 8) != magic || header.substr(8, 16) != std::string_view(identity.data(), 16) || peek64(header.substr(24)) != segment ||
			peek32(header.substr(32)) != index || peek32(header.substr(44)) != crc32c(header.substr(0, 44))) { gap(); assembling = false; continue; }
		const auto epoch = peek64(header.substr(36));
		std::size_t position = block_header;
		while (options.block_bytes - position >= fragment_header + 1) {
			auto fragment = view.substr(position);
			auto length = get16(fragment.substr(4)); auto type = static_cast<std::uint8_t>(fragment[6]);
			bool valid = length > 0 && fragment_header + length <= fragment.size() && type >= Full && type <= Last && fragment[7] == '\0' &&
				peek64(fragment.substr(8)) == segment && peek64(fragment.substr(16)) == epoch;
			if (valid) { Checksum checksum; checksum.update(fragment.substr(4, fragment_header - 4)); checksum.update(fragment.substr(fragment_header, length)); valid = checksum.value() == peek32(fragment); }
			if (!valid) { gap(); assembling = false; break; }
			auto payload = fragment.substr(fragment_header, length);
			position += fragment_header + length;
			if (type == Full || type == First) {
				if (assembling) { gap(); }
				partial.assign(payload); partial_epoch = epoch; partial_offset = offset + position - fragment_header - length; assembling = type == First;
				if (assembling) { continue; }
			} else {
				if (!assembling || partial_epoch != epoch) { gap(); assembling = false; continue; }
				partial.append(payload);
				if (type == Middle) { continue; }
				assembling = false;
			}
			if (partial.size() < record_header) { gap(); continue; }
			std::string_view record(partial);
			Item item; item.epoch = epoch; item.segment = segment; item.offset = partial_offset; item.kind = static_cast<std::uint8_t>(record[0]); record.remove_prefix(1);
			item.turn = get64(record); item.group = get64(record); item.sequence = get64(record); item.payload.assign(record);
			items.push_back(std::move(item));
		}
	}
	if (assembling) { gap(); }
}
// Reads back the logical record whose first fragment starts at offset.
inline std::string read_record(File& file, const TurnLogOptions& options, std::uint64_t offset) {
	std::string logical, header(fragment_header, '\0');
	for (std::uint64_t position = offset;;) {
		if (position % options.block_bytes == 0) { position += block_header; }
		auto room = options.block_bytes - position % options.block_bytes;
		if (room < fragment_header + 1) { position += room; continue; }
		if (file.read_at(position, header) != header.size()) { throw Corruption("short turn log read"); }
		auto length = get16(std::string_view(header).substr(4)); auto type = static_cast<std::uint8_t>(header[6]);
		std::string payload(length, '\0');
		if (!length || length > room - fragment_header || file.read_at(position + fragment_header, payload) != length) { throw Corruption("invalid turn log fragment"); }
		Checksum checksum; checksum.update(std::string_view(header).substr(4)); checksum.update(payload);
		if (checksum.value() != peek32(header) || (logical.empty() ? type != Full && type != First : type != Middle && type != Last)) { throw Corruption("turn log record changed since it was written"); }
		logical.append(payload); position += fragment_header + length;
		if (type == Full || type == Last) { return logical; }
	}
}
}  // namespace turn_log_detail

class TurnLog {
public:
	// Recovers the journal in the IO's directory (empty for a new one), then starts a new
	// epoch whose open record is durable before any turn is admitted (condition 8).
	TurnLog(IO& io, Identity identity, TurnRecovery& recovered, TurnLogOptions options = {}) : io_(io), identity_(identity), options_(options) {
		if (!options_.block_bytes || options_.block_bytes > 65535 + turn_log_detail::fragment_header || options_.block_bytes < 256 ||
			options_.segment_bytes < options_.block_bytes || options_.segment_bytes % options_.block_bytes) {
			throw std::invalid_argument("invalid turn log geometry");
		}
		recovered = recover();
		epoch_ = recovered.epoch; turn_ = recovered.last_turn;
		for (const auto& [group, records] : recovered.groups) { if (!records.empty()) { sequences_[group] = records.back().sequence; } }
		for (auto group : recovered.lost) { lost_.insert(group); }
		next_segment_ = highest_segment_ + 1;
		begin_segment();
		data_segments_.insert(segment_);
		write({[&](turn_log_detail::Emitter& emitter) { emitter.record(turn_log_detail::open_record, turn_, 0, 0, {}); }});
	}
	// Preallocates spare segments; the host calls it off the turn.
	void prepare() { while (spares_.size() < options_.spare_segments) { spares_.push_back(preallocate()); } }
	// Appends one turn and makes it durable. Each group's sequence continues its last one; a
	// new group starts at 1 or with a checkpoint. A lost group rejoins under a new id.
	std::uint64_t append(std::span<const TurnRecord> records) {
		if (failed_) { throw std::logic_error("turn log failed"); }
		std::map<std::uint64_t, std::uint64_t> next;
		for (const auto& record : records) {
			if (!record.group) { throw std::invalid_argument("group 0 is reserved"); }
			if (lost_.contains(record.group)) { throw std::invalid_argument("a lost group rejoins under a new id"); }
			auto [found, fresh] = next.try_emplace(record.group, sequence(record.group));
			auto& last = found->second;
			bool start = fresh && !sequences_.contains(record.group) && record.kind == TurnRecordKind::Checkpoint && record.sequence;
			if (record.sequence != last + 1 && !start) {
				throw std::invalid_argument("turn record out of its group's sequence");
			}
			last = record.sequence;
		}
		const auto turn = turn_ + 1;
		auto copies = forward_slice();
		std::vector<std::uint64_t> offsets;
		auto emit = [&](turn_log_detail::Emitter& emitter) {
			offsets.clear();
			for (const auto& record : records) { offsets.push_back(emitter.record(static_cast<std::uint8_t>(record.kind), turn, record.group, record.sequence, record.payload)); }
			for (const auto& copy : copies) { offsets.push_back(emitter.record(static_cast<std::uint8_t>(copy.kind), turn, copy.group, copy.sequence, copy.payload)); }
			std::string count; put32(count, static_cast<std::uint32_t>(records.size() + copies.size()));
			emitter.record(turn_log_detail::end_of_turn, turn, 0, 0, count);
		};
		// Copies ride along only where the turn still fits in a segment.
		if (!copies.empty() && !fits(emit)) { copies.clear(); }
		write({emit});
		turn_ = turn; ++stats_.turns;
		for (const auto& [group, sequence] : next) { sequences_[group] = sequence; }
		for (std::size_t i = 0; i < records.size(); ++i) { note(records[i].group, records[i].sequence, records[i].kind, offsets[i]); stats_.bytes_appended += records[i].payload.size(); }
		for (std::size_t i = 0; i < copies.size(); ++i) {
			// A copy replaces its original unless a checkpoint in this turn superseded both.
			if (index_[copies[i].group].contains(copies[i].sequence)) { note(copies[i].group, copies[i].sequence, copies[i].kind, offsets[records.size() + i], true); }
			stats_.forwarded_bytes += copies[i].payload.size();
		}
		stats_.forwarded_records += copies.size();
		reclaim();
		return turn;
	}
	std::uint64_t epoch() const noexcept { return epoch_; }
	std::uint64_t last_turn() const noexcept { return turn_; }
	std::uint64_t segment() const noexcept { return segment_; }
	std::uint64_t sequence(std::uint64_t group) const { auto found = sequences_.find(group); return found == sequences_.end() ? 0 : found->second; }
	const TurnLogStats& stats() const noexcept { return stats_; }
	// Segments holding records, the current one included; the oldest goes first.
	const std::set<std::uint64_t>& data_segments() const noexcept { return data_segments_; }

private:
	struct Live { std::uint64_t segment, offset; TurnRecordKind kind; };
	// A checkpoint supersedes the group's earlier records; a copy only replaces its original.
	void note(std::uint64_t group, std::uint64_t sequence, TurnRecordKind kind, std::uint64_t offset, bool copy = false) {
		auto& records = index_[group];
		auto drop = [&](const Live& live) { if (!--live_counts_[live.segment]) { live_counts_.erase(live.segment); } };
		if (kind == TurnRecordKind::Checkpoint && !copy) { for (const auto& [_, live] : records) { drop(live); } records.clear(); }
		if (auto found = records.find(sequence); found != records.end()) { drop(found->second); records.erase(found); }
		records.emplace(sequence, Live{segment_, offset, kind}); ++live_counts_[segment_];
	}
	bool fits(const std::function<void(turn_log_detail::Emitter&)>& emit) const {
		turn_log_detail::Emitter fresh(identity_, options_.block_bytes, segment_, epoch_, 0);
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
		for (const auto& [group, records] : index_) {
			for (const auto& [sequence, live] : records) {
				if (live.segment != oldest) { continue; }
				std::string_view logical;
				auto record = turn_log_detail::read_record(reader(oldest), options_, live.offset);
				logical = record; logical.remove_prefix(turn_log_detail::record_header);
				copies.push_back({group, sequence, live.kind, std::string(logical)});
				if ((bytes += logical.size()) >= std::min<std::uint64_t>(options_.forward_bytes, options_.segment_bytes / 4)) { return copies; }
			}
		}
		return copies;
	}
	// Releases segments from the oldest while no live record remains in them: recycled into
	// the spare pool while it is short, removed otherwise. Only the oldest go, so the turns
	// left stay contiguous.
	void reclaim() {
		bool changed = false;
		while (!data_segments_.empty() && *data_segments_.begin() != segment_ && !live_counts_.contains(*data_segments_.begin())) {
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
	struct Body { std::function<void(turn_log_detail::Emitter&)> emit; };
	std::unique_ptr<File> preallocate() {
		auto file = io_.create_exclusive(turn_log_detail::segment_name(next_segment_++));
		const std::string zeros(std::min<std::uint64_t>(options_.segment_bytes, 1 << 20), '\0');
		for (std::uint64_t offset = 0; offset < options_.segment_bytes; offset += zeros.size()) {
			auto count = std::min<std::uint64_t>(zeros.size(), options_.segment_bytes - offset);
			if (file->write_at(offset, std::string_view(zeros).substr(0, count)) != count) { throw std::runtime_error("short turn log preallocation"); }
		}
		file->sync(); io_.sync_directory(); ++stats_.segments_prepared;
		return file;
	}
	void begin_segment() {
		if (spares_.empty()) { spares_.push_back(preallocate()); ++stats_.prepared_inline; }
		file_ = std::move(spares_.front()); spares_.pop_front();
		segment_ = next_segment_ - 1 - spares_.size(); offset_ = 0;
		data_segments_.insert(segment_);
	}
	// A turn never crosses a segment: one that does not fit starts the next one.
	void write(Body body) {
		turn_log_detail::Emitter emitter(identity_, options_.block_bytes, segment_, epoch_, offset_);
		body.emit(emitter);
		if (emitter.position() > options_.segment_bytes) {
			turn_log_detail::Emitter fresh(identity_, options_.block_bytes, segment_, epoch_, 0);
			body.emit(fresh);
			if (fresh.position() > options_.segment_bytes) { throw std::length_error("turn exceeds a segment"); }
			begin_segment();
			emitter = turn_log_detail::Emitter(identity_, options_.block_bytes, segment_, epoch_, 0);
			body.emit(emitter);
			if (emitter.position() > options_.segment_bytes) { throw std::length_error("turn exceeds a segment"); }
		}
		try {
			if (file_->write_at(offset_, emitter.bytes()) != emitter.bytes().size()) { throw std::runtime_error("short turn log write"); }
			file_->sync_data();
		} catch (...) { failed_ = true; throw; }
		offset_ = emitter.position(); ++stats_.barriers; stats_.bytes_written += emitter.bytes().size();
	}
	TurnRecovery recover();

	IO& io_; Identity identity_; TurnLogOptions options_;
	std::unique_ptr<File> file_; std::deque<std::unique_ptr<File>> spares_;
	std::uint64_t epoch_ = 0, turn_ = 0, segment_ = 0, offset_ = 0, next_segment_ = 1, highest_segment_ = 0;
	std::map<std::uint64_t, std::uint64_t> sequences_; std::set<std::uint64_t> lost_;
	std::map<std::uint64_t, std::map<std::uint64_t, Live>> index_; std::map<std::uint64_t, std::size_t> live_counts_;
	std::set<std::uint64_t> data_segments_; std::map<std::uint64_t, std::unique_ptr<File>> readers_;
	TurnLogStats stats_; bool failed_ = false;
};

// Reads every segment in order. Within the newest epoch the last turn is the torn tail:
// kept only whole. For an earlier epoch, the next epoch's open record names the last turn
// that open kept; later turns are ignored, and damage at or before it is corruption.
inline TurnRecovery TurnLog::recover() {
	using namespace turn_log_detail;
	std::vector<std::uint64_t> segments;
	auto cursor = io_.scan_directory();
	while (auto name = cursor->next()) { if (auto number = segment_number(*name)) { segments.push_back(*number); } }
	std::sort(segments.begin(), segments.end());
	std::vector<Item> items;
	for (auto number : segments) {
		highest_segment_ = std::max(highest_segment_, number); data_segments_.insert(number);
		auto file = io_.open_existing(segment_name(number));
		std::vector<Item> segment; scan_segment(*file, identity_, options_, number, segment);
		while (!segment.empty() && segment.back().gap) { segment.pop_back(); }  // the unwritten tail
		for (auto& item : segment) { items.push_back(std::move(item)); }
	}
	// Opens, in stream order: each names its epoch and the last turn it kept.
	// The oldest segments may have been reclaimed with the first epoch's open record: that
	// epoch then starts at its first turn read.
	struct Open { std::uint64_t epoch, kept; std::size_t at, begin; };
	std::vector<Open> opens; std::uint64_t newest = 0;
	for (std::size_t i = 0; i < items.size(); ++i) {
		if (items[i].gap) { continue; }
		newest = std::max(newest, items[i].epoch);
		if (items[i].kind == open_record) {
			if (!opens.empty() && items[i].epoch <= opens.back().epoch) { throw Corruption("turn log epochs out of order"); }
			opens.push_back({items[i].epoch, items[i].turn, i, i + 1});
		} else if (opens.empty() || items[i].epoch < opens.front().epoch) {
			if (!opens.empty()) { throw Corruption("turn log epochs out of order"); }
			std::uint64_t first = std::numeric_limits<std::uint64_t>::max();
			for (std::size_t k = i; k < items.size() && (items[k].gap || items[k].epoch == items[i].epoch); ++k) {
				if (!items[k].gap && items[k].kind != open_record) { first = std::min(first, items[k].turn); }
			}
			opens.push_back({items[i].epoch, first - 1, i, i});
		}
	}
	TurnRecovery result;
	if (opens.empty()) {
		if (newest) { throw Corruption("turn log has records but no open record"); }
		result.epoch = 1; return result;
	}
	// An epoch whose open record did not read back holds no turn: no turn was admitted
	// before its open record was durable (condition 8).
	for (const auto& item : items) {
		if (!item.gap && item.kind != open_record && std::none_of(opens.begin(), opens.end(), [&](const Open& o) { return o.epoch == item.epoch; })) {
			throw Corruption("turn of an epoch whose open record is unreadable");
		}
	}
	// Turns per epoch: contiguous from the open's kept turn, each complete when its
	// end-of-turn record counts the records before it. Damage before a later complete turn
	// (a gap, an incomplete turn, a missing one) loses only what it held: see below.
	struct Turn { std::vector<std::size_t> records; bool complete = false; std::size_t first = 0, end = 0; };
	std::vector<std::size_t> kept_records; std::size_t damage = items.size();
	auto damaged_at = [&](std::size_t at) { damage = std::min(damage, at); };
	std::uint64_t last_turn = opens.front().kept;
	for (std::size_t e = 0; e < opens.size(); ++e) {
		const auto& open = opens[e];
		const bool newest_epoch = e + 1 == opens.size();
		if (e && open.kept != last_turn) { throw Corruption("open record names a turn its epoch did not keep"); }
		const auto limit = newest_epoch ? std::numeric_limits<std::uint64_t>::max() : opens[e + 1].kept;
		const auto stop = newest_epoch ? items.size() : opens[e + 1].at;
		std::map<std::uint64_t, Turn> turns; std::vector<std::size_t> gaps;
		for (std::size_t i = open.begin; i < stop; ++i) {
			const auto& item = items[i];
			const bool known = (item.kind >= 1 && item.kind <= 3) || item.kind == end_of_turn;
			if (item.gap || item.epoch != open.epoch || !known) { gaps.push_back(i); continue; }
			if (item.turn <= open.kept || item.turn > limit) { continue; }
			auto [found, fresh] = turns.try_emplace(item.turn);
			auto& turn = found->second; if (fresh) { turn.first = i; }
			if (item.kind == end_of_turn) {
				std::string_view count(item.payload);
				turn.complete = !turn.complete && item.payload.size() == 4 && get32(count) == turn.records.size(); turn.end = i;
			} else if (turn.end) { turn.complete = false; } else { turn.records.push_back(i); }
		}
		std::uint64_t expected = open.kept + 1;
		for (auto it = turns.begin(); it != turns.end(); ++it) {
			auto& [number, turn] = *it;
			if (!turn.complete) {
				if (newest_epoch && std::next(it) == turns.end()) { break; }  // the torn tail
				damaged_at(turn.first); continue;
			}
			if (number != expected) { damaged_at(turn.first); }
			for (auto g : gaps) { if (g < turn.end) { damaged_at(g); } }
			for (auto r : turn.records) { kept_records.push_back(r); }
			last_turn = number; expected = number + 1;
		}
		if (!newest_epoch && last_turn != limit) { damaged_at(opens[e + 1].at); last_turn = limit; }
	}
	// Per group: copies agree, sequences are contiguous from 1 or from a checkpoint, and a
	// checkpoint supersedes what precedes it.
	std::map<std::uint64_t, std::map<std::uint64_t, std::size_t>> by_group;
	std::map<std::uint64_t, std::size_t> last_seen;
	for (auto r : kept_records) {
		const auto& item = items[r];
		auto [it, inserted] = by_group[item.group].try_emplace(item.sequence, r);
		if (!inserted) {
			const auto& other = items[it->second];
			if (other.kind != item.kind || other.payload != item.payload) { result.lost.insert(item.group); }
			it->second = r;  // the later copy is the live one
		}
		last_seen[item.group] = std::max(last_seen[item.group], r);
	}
	for (auto& [group, records] : by_group) {
		if (result.lost.contains(group)) { continue; }
		// A group that wrote nothing after the damage cannot show it lost nothing there.
		if (damage < items.size() && last_seen[group] < damage) { result.lost.insert(group); continue; }
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
			index_[group].emplace(item.sequence, Live{item.segment, item.offset, static_cast<TurnRecordKind>(item.kind)});
		}
		if (!result.lost.contains(group)) { result.groups.emplace(group, std::move(kept)); }
		else { index_.erase(group); }
	}
	for (const auto& [group, records] : index_) { for (const auto& [_, live] : records) { ++live_counts_[live.segment]; } }
	result.damaged = damage < items.size();
	result.epoch = opens.back().epoch + 1; result.last_turn = last_turn;
	return result;
}

}  // namespace kronuz::journal
