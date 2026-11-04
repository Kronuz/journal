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
	std::uint64_t epoch = 0;
	std::uint8_t kind = 0;
	std::uint64_t turn = 0, group = 0, sequence = 0;
	std::string payload;
	std::uint64_t segment = 0, offset = 0;  // the record's first fragment; a gap's block start
};
// Per described block: its groups' highest sequences, and where the header naming them is.
struct BlockManifest { std::map<std::uint64_t, std::uint64_t> groups; std::uint64_t segment = 0, epoch = 0; std::uint32_t block = 0; };
using Manifests = std::map<std::pair<std::uint64_t, std::uint32_t>, BlockManifest>;
inline void scan_segment(File& file, const Identity& identity, const TurnLogOptions& options, std::uint64_t segment, std::vector<Item>& items, Manifests& manifests) {
	std::uint64_t here = 0;
	auto gap = [&] { if (items.empty() || !items.back().gap) { Item item; item.gap = true; item.segment = segment; item.offset = here; items.push_back(std::move(item)); } };
	std::string block(options.block_bytes, '\0'), partial; bool assembling = false; std::uint64_t partial_epoch = 0, partial_offset = 0;
	const auto size = std::min(file.size(), options.segment_bytes);
	for (std::uint64_t offset = 0, index = 0; offset + options.block_bytes <= size; offset += options.block_bytes, ++index) {
		here = offset;
		if (file.read_at(offset, block) != block.size()) { gap(); assembling = false; continue; }
		std::string_view view(block);
		const auto header = framing::parse_header(view, stream(identity, options.block_bytes, segment, 0), index);
		if (header.check != framing::Check::ok) { gap(); assembling = false; continue; }
		const auto epoch = header.epoch; const auto count = header.count;
		if (count != framing::no_manifest) {
			auto& manifest = manifests[{framing::peek64(view.substr(44)), framing::peek32(view.substr(52))}];
			manifest.segment = segment; manifest.block = static_cast<std::uint32_t>(index); manifest.epoch = epoch;
			const auto entry = framing::header_fixed;
			for (std::size_t i = 0; i < count; ++i) { manifest.groups[framing::peek64(view.substr(entry + 16 * i))] = framing::peek64(view.substr(entry + 16 * i + 8)); }
		}
		std::size_t position = header.size;
		while (options.block_bytes - position >= framing::fragment_header + 1) {
			const auto fragment = framing::parse_fragment(view.substr(position), options.block_bytes - position, segment, epoch);
			if (fragment.check != framing::Check::ok) { gap(); assembling = false; break; }
			const auto type = fragment.type; const auto payload = fragment.payload; const auto length = payload.size();
			position += framing::fragment_header + length;
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
	TurnLog(IO& io, Identity identity, TurnRecovery& recovered, TurnLogOptions options = {}) : io_(io), identity_(identity), options_(options) {
		if (!options_.block_bytes || !framing::valid_block_bytes(options_.block_bytes) ||
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
		write({[&](framing::Emitter& emitter) { emitter.record(turn_log_detail::logical(turn_log_detail::open_record, turn_, 0, 0, {})); }});
	}
	// Preallocates spare segments; the host calls it off the turn.
	void prepare() { while (spares_.size() < options_.spare_segments) { spares_.push_back(preallocate()); } }
	// Appends one turn and makes it durable. Each group's sequence continues its last one; a
	// new group starts at 1 or with a checkpoint. A lost group rejoins under a new id.
	std::uint64_t append(std::span<const TurnRecord> records) {
		auto write = stage(records);
		try {
			if (write.file->write_at(write.offset, write.bytes) != write.bytes.size()) { throw std::runtime_error("short turn log write"); }
			write.file->sync_data();
		} catch (...) { failed(); throw; }
		completed();
		return write.turn;
	}
	// The write a staged turn needs: its bytes at an offset of a segment, then one
	// File::sync_data. A host driving completion I/O issues them and reports back.
	struct TurnWrite { File* file; std::uint64_t offset; std::string_view bytes; std::uint64_t turn; };
	// Encodes a turn; one is in flight at a time (condition 7). Nothing it changes is
	// visible until completed().
	TurnWrite stage(std::span<const TurnRecord> records) {
		if (failed_) { throw std::logic_error("turn log failed"); }
		if (staged_) { throw std::logic_error("a turn is already in flight"); }
		Staged turn; turn.turn = turn_ + 1;
		for (const auto& record : records) {
			if (!record.group) { throw std::invalid_argument("group 0 is reserved"); }
			if (lost_.contains(record.group)) { throw std::invalid_argument("a lost group rejoins under a new id"); }
			auto [found, fresh] = turn.next.try_emplace(record.group, sequence(record.group));
			auto& last = found->second;
			bool start = fresh && !sequences_.contains(record.group) && record.kind == TurnRecordKind::Checkpoint && record.sequence;
			if (record.sequence != last + 1 && !start) {
				throw std::invalid_argument("turn record out of its group's sequence");
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
			if (index_[n.group].contains(n.sequence)) { note(n.group, n.sequence, n.kind, n.offset, true); }
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
	std::uint64_t sequence(std::uint64_t group) const { auto found = sequences_.find(group); return found == sequences_.end() ? 0 : found->second; }
	const TurnLogStats& stats() const noexcept { return stats_; }
	// Segments holding records, the current one included; the oldest goes first.
	const std::set<std::uint64_t>& data_segments() const noexcept { return data_segments_; }

private:
	struct Live { std::uint64_t segment, epoch, offset; TurnRecordKind kind; };
	// A checkpoint supersedes the group's earlier records; a copy only replaces its original.
	void note(std::uint64_t group, std::uint64_t sequence, TurnRecordKind kind, std::uint64_t offset, bool copy = false) {
		auto& records = index_[group];
		auto drop = [&](const Live& live) { if (!--live_counts_[live.segment]) { live_counts_.erase(live.segment); } };
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
		for (const auto& [group, records] : index_) {
			for (const auto& [sequence, live] : records) {
				if (live.segment != oldest) { continue; }
				std::string_view logical;
				auto record = framing::read_record(reader(oldest), turn_log_detail::stream(identity_, options_.block_bytes, oldest, live.epoch), live.offset);
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
	struct Body { std::function<void(framing::Emitter&)> emit; };
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
			file_->sync_data();
		} catch (...) { failed_ = true; throw; }
		offset_ = emitter.position(); filling_ = emitter.filling(); ++stats_.barriers; stats_.bytes_written += emitter.bytes().size();
	}
	TurnRecovery recover();

	IO& io_; Identity identity_; TurnLogOptions options_;
	std::unique_ptr<File> file_; std::deque<std::unique_ptr<File>> spares_;
	std::uint64_t epoch_ = 0, turn_ = 0, segment_ = 0, offset_ = 0, next_segment_ = 1, highest_segment_ = 0;
	std::map<std::uint64_t, std::uint64_t> sequences_; std::set<std::uint64_t> lost_;
	std::map<std::uint64_t, std::map<std::uint64_t, Live>> index_; std::map<std::uint64_t, std::size_t> live_counts_;
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
inline TurnRecovery TurnLog::recover() {
	using namespace turn_log_detail;
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
	// names for the damaged block; where no manifest reads back, it costs every group that
	// wrote nothing after it, which alone cannot show it lost nothing there.
	struct Turn { std::vector<std::size_t> records; bool complete = false; std::size_t first = 0, end = 0; };
	std::vector<std::size_t> kept_records; std::set<std::uint64_t> named; std::set<std::size_t> counted;
	std::size_t unknown = items.size(); bool damaged = false;
	auto gap_damage = [&](std::size_t g) {
		if (!counted.insert(g).second) { return; }
		damaged = true;
		auto found = manifests.find({items[g].segment, static_cast<std::uint32_t>(items[g].offset / options_.block_bytes)});
		if (found == manifests.end()) { unknown = std::min(unknown, g); return; }
		for (const auto& [group, _] : found->second.groups) { named.insert(group); }
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
			if (item.tail) { tails.push_back(i); continue; }
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
			if (!found) { unknown = std::min(unknown, to); }
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
		if (!newest_epoch && last_turn != limit && limit != std::numeric_limits<std::uint64_t>::max()) {
			damaged = true; missing_between(previous_end, stop, previous_end);
			last_turn = limit;
		}
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
		if (named.contains(group) || (unknown < items.size() && last_seen[group] < unknown)) { result.lost.insert(group); continue; }
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
			if (result.lost.contains(group)) { continue; }
			auto found = result.groups.find(group);
			if (found == result.groups.end() || found->second.back().sequence < sequence) {
				damaged = true; result.lost.insert(group);
				if (found != result.groups.end()) { result.groups.erase(found); index_.erase(group); }
			}
		}
	}
	for (auto group : named) { if (!result.groups.contains(group)) { result.lost.insert(group); index_.erase(group); } }
	for (const auto& [group, records] : index_) { for (const auto& [_, live] : records) { ++live_counts_[live.segment]; } }
	result.damaged = damaged;
	result.epoch = opens.back().epoch + 1; result.last_turn = last_turn;
	return result;
}

}  // namespace kronuz::journal
