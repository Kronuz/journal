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

// T9 (strata Track C): RFTW0001 under power loss. A randomized multi-group workload over
// faultlab's faultfs, whose unsynced 512-byte sectors each persist or not, whose unsynced size
// may end at any sector boundary (size_pick::sector), and whose namespace changes persist only
// after a directory sync. Power is cut after every mutating I/O operation of each run, and at
// every third point again during the recovery that follows. Segments recycle, so stale blocks
// from an earlier use are present (condition 10).
#include "faultlab/faultfs.h"
#include "journal/turn_log.h"
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <random>

namespace j = kronuz::journal;

namespace {
int checks = 0, failures = 0;
void check(bool value, const std::string& what) {
	++checks;
	if (!value) { ++failures; if (failures < 200) { std::cerr << "FAIL: " << what << '\n'; } }
}
namespace fl = kronuz::faultlab;
// A crash keeps any sector-boundary prefix of an unsynced append, as a real device may.
const fl::crash_policy sector_cut{.size = fl::size_pick::sector};

const j::Identity identity{'P', 'O', 'W', 'E', 'R'};
using Model = std::map<std::uint64_t, std::vector<j::TurnRecord>>;
j::TurnRecord record(std::uint64_t group, std::uint64_t sequence, std::size_t size, j::TurnRecordKind kind) {
	std::string payload(size, '\0');
	for (std::size_t i = 0; i < size; ++i) { payload[i] = static_cast<char>('a' + (group * 7 + sequence * 3 + i) % 26); }
	return {group, sequence, kind, payload, sequence - 1};  // a checkpoint covers the records before it
}
struct Workload {
	std::mt19937_64 random; Model acknowledged; std::vector<j::TurnRecord> in_flight;
	explicit Workload(std::uint64_t seed) : random(seed) {}
	// One turn: one to three groups of six, records up to 700 bytes, one in five a checkpoint;
	// group 9 writes once and then idles, so its record must move forward.
	std::vector<j::TurnRecord> turn(const j::TurnLog& log) {
		std::vector<j::TurnRecord> batch; std::map<std::uint64_t, std::uint64_t> next;
		if (!log.sequence(9) && !acknowledged.contains(9)) { batch.push_back(record(9, 1, 100, j::TurnRecordKind::Batch)); return batch; }
		for (auto n = random() % 3 + 1; n; --n) {
			auto group = random() % 6 + 1;
			auto& sequence = next.try_emplace(group, log.sequence(group)).first->second;
			// Drawn in order: an argument list's evaluation order differs between compilers.
			const std::size_t size = random() % 700 + 1;
			const auto kind = random() % 5 ? j::TurnRecordKind::Batch : j::TurnRecordKind::Checkpoint;
			batch.push_back(record(group, ++sequence, size, kind));
		}
		return batch;
	}
	void acknowledge(const std::vector<j::TurnRecord>& batch) {
		for (const auto& r : batch) { if (r.kind == j::TurnRecordKind::Checkpoint) { acknowledged[r.group].clear(); } acknowledged[r.group].push_back(r); }
	}
	// Recovery holds every acknowledged record, and the in-flight turn whole or not at all.
	bool matches(const j::TurnRecovery& recovered) {
		if (recovered.groups == acknowledged) { return true; }
		auto with = acknowledged; for (const auto& r : in_flight) { if (r.kind == j::TurnRecordKind::Checkpoint) { with[r.group].clear(); } with[r.group].push_back(r); }
		if (recovered.groups == with) { acknowledge(in_flight); return true; }
		return false;
	}
};
// The geometry under test: main runs each of sector-sized, multi-sector, unaligned and
// sub-sector blocks.
j::TurnLogOptions options{8 * 512, 512, 1, 3, 600};
std::string listing(const fl::filesystem& disk) { std::string names; for (const auto& name : disk.list()) { names += ' ' + name; } return names; }

// Runs `turns` turns, cutting power at mutation `cut` (and at `second` during the recovery
// that follows), then reopens and continues for `after` more turns. Only the power cut fails a
// run: any other error is a failure.
void run(std::uint64_t seed, std::uint64_t cut, std::uint64_t second, unsigned turns, unsigned after) {
	fl::filesystem disk; fl::io io(disk); Workload work(seed);
	j::TurnRecovery recovered;
	const auto at = "seed " + std::to_string(seed) + " cut " + std::to_string(cut) + " second " + std::to_string(second);
	try {
		disk.power_off_after(cut - 1);
		j::TurnLog log(io, identity, recovered, options);
		for (unsigned t = 0; t < turns; ++t) {
			work.in_flight = work.turn(log); log.append(work.in_flight);
			work.acknowledge(work.in_flight); work.in_flight.clear();
			log.prepare();
		}
	} catch (const std::exception& error) { check(disk.powered_off(), "only the power cut fails the run: " + at + ": " + error.what()); }
	// The namespace at each crash, so a failure names the segments it left.
	auto names = "; before crash" + listing(disk);
	disk.crash(seed * 7919 + cut, sector_cut);
	names += "; after" + listing(disk);
	if (second) {
		disk.power_off_after(second - 1);
		try { j::TurnLog log(io, identity, recovered, options); }
		catch (const std::exception& error) { check(disk.powered_off(), "only the power cut fails recovery: " + at + ": " + error.what()); }
		names += "; before second crash" + listing(disk);
		disk.crash(seed * 7919 + cut + (second << 32), sector_cut);
		names += "; after" + listing(disk);
	}
	try {
		j::TurnLog log(io, identity, recovered, options);
		check(!recovered.damaged && recovered.lost.empty(), "power loss alone damages nothing and loses no group: " + at + names);
		check(work.matches(recovered), "every acknowledged record survives, and the in-flight turn whole or not at all: " + at);
		work.in_flight.clear();
		for (unsigned t = 0; t < after; ++t) { auto batch = work.turn(log); log.append(batch); work.acknowledge(batch); }
	} catch (const std::exception& error) {
		check(false, at + ": " + error.what());
	}
}

// The blocks each record's fragments occupy, on a clean disk: what a damaged block may cost.
std::map<std::pair<std::uint64_t, std::uint64_t>, std::set<std::uint64_t>> groups_by_block(j::IO& io) {
	namespace d = j::turn_log_detail; namespace f = j::framing;
	std::map<std::pair<std::uint64_t, std::uint64_t>, std::set<std::uint64_t>> result;
	auto cursor = io.scan_directory();
	while (auto name = cursor->next()) {
		auto number = d::segment_number(*name); if (!number) { continue; }
		auto file = io.open_existing(*name);
		std::vector<d::Item> items; d::Manifests manifests; d::scan_segment(*file, identity, options, *number, items, manifests);
		for (const auto& item : items) {
			if (item.gap || !item.group) { continue; }
			std::string header(f::fragment_header, '\0');
			for (std::uint64_t position = item.offset;;) {
				if (position % options.block_bytes == 0) { std::string c(2, '\0'); file->read_at(position + 56, c); position += f::header_bytes(f::get16(c)); }
				auto room = options.block_bytes - position % options.block_bytes;
				if (room < f::fragment_header + 1) { position += room; continue; }
				file->read_at(position, header);
				result[{*number, position / options.block_bytes}].insert(item.group);
				auto type = static_cast<std::uint8_t>(header[6]); position += f::fragment_header + f::get16(std::string_view(header).substr(4));
				if (type == f::Full || type == f::Last) { break; }
			}
		}
	}
	return result;
}

// One corrupted block costs only the groups with records in it: every other group's
// acknowledged records survive, and the damage is reported.
void corrupted_blocks(std::uint64_t seed, std::uint64_t& blocks) {
	fl::filesystem clean; fl::io clean_io(clean); Workload work(seed); j::TurnRecovery recovered;
	{ j::TurnLog log(clean_io, identity, recovered, options); for (unsigned t = 0; t < 60; ++t) { auto b = work.turn(log); log.append(b); work.acknowledge(b); log.prepare(); } }
	clean.crash(seed, sector_cut);  // everything acknowledged is durable
	auto truth = groups_by_block(clean_io);
	if (truth.empty()) { return; }
	// Damage to the last turn reads as a torn tail: recovery cannot tell it from a turn
	// never acknowledged, so only blocks before the last turn's first are corrupted.
	std::pair<std::uint64_t, std::uint64_t> last{std::numeric_limits<std::uint64_t>::max(), 0};
	{
		namespace d = j::turn_log_detail;
		std::uint64_t newest = 0; std::vector<std::pair<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t>> firsts;
		auto cursor = clean_io.scan_directory();
		while (auto name = cursor->next()) {
			auto number = d::segment_number(*name); if (!number) { continue; }
			auto file = clean_io.open_existing(*name);
			std::vector<d::Item> items; d::Manifests manifests; d::scan_segment(*file, identity, options, *number, items, manifests);
			for (const auto& item : items) {
				if (item.gap || item.kind == d::open_record) { continue; }
				newest = std::max(newest, item.turn); firsts.push_back({{*number, item.offset / options.block_bytes}, item.turn});
			}
		}
		for (const auto& [where, turn] : firsts) { if (turn == newest) { last = std::min(last, where); } }
	}
	for (const auto& [where, groups] : truth) {
		if (where >= last) { continue; }
		fl::filesystem disk = clean.snapshot(); fl::io io(disk); ++blocks;
		auto name = j::turn_log_detail::segment_name(where.first);
		std::mt19937_64 random(seed * 131 + where.second);
		const auto offset = where.second * options.block_bytes + random() % options.block_bytes;
		{
			auto file = io.open_existing(name); char byte = 0;
			check(file->read_at(offset, std::span<char>(&byte, 1)) == 1, "the damaged byte is in the segment");
			byte = static_cast<char>(byte ^ 0x5a); file->write_at(offset, std::string_view(&byte, 1)); file->sync();
		}
		const auto at = "seed " + std::to_string(seed) + " segment " + std::to_string(where.first) + " block " + std::to_string(where.second);
		try {
			j::TurnLog log(io, identity, recovered, options);
			bool only = std::all_of(recovered.lost.begin(), recovered.lost.end(), [&](std::uint64_t g) { return groups.contains(g); });
			bool kept = true;
			for (const auto& [group, records] : work.acknowledged) { if (!recovered.lost.contains(group) && recovered.groups[group] != records) { kept = false; } }
			check(only, "a corrupted block loses only groups with records in it: " + at);
			check(kept, "every other group keeps its acknowledged records: " + at);
			if (!kept && std::getenv("T9_DEBUG")) {
				std::cerr << "  damaged " << recovered.damaged << " lost:"; for (auto g : recovered.lost) std::cerr << ' ' << g;
				std::cerr << " truth:"; for (auto g : groups) std::cerr << ' ' << g; std::cerr << '\n';
				for (const auto& [group, records] : work.acknowledged) {
					if (recovered.lost.contains(group) || recovered.groups[group] == records) { continue; }
					auto& got = recovered.groups[group];
					std::cerr << "  group " << group << " acked " << records.size() << " [" << records.front().sequence << ".." << records.back().sequence << "] got " << got.size();
					if (!got.empty()) { std::cerr << " [" << got.front().sequence << ".." << got.back().sequence << "]"; }
					std::cerr << '\n';
				}
			}
		} catch (const std::exception& error) { check(false, at + ": " + error.what()); }
	}
}

// Damage at every block boundary of a written log, the last turn's blocks included: a byte past a
// block's header and one in the next block's header, which names the first block's groups, or the
// header byte alone; and a byte past the last written block's header. Every group recovery does
// not call lost keeps its acknowledged records, and the damage is reported where `reported` says.
enum class Damage { both, header, last };
void boundary_damage(std::uint64_t seed, Damage damage, std::uint64_t& cases) {
	namespace d = j::turn_log_detail; namespace f = j::framing;
	fl::filesystem clean; fl::io clean_io(clean); Workload work(seed); j::TurnRecovery recovered;
	{ j::TurnLog log(clean_io, identity, recovered, options); for (unsigned t = 0; t < 60; ++t) { auto b = work.turn(log); log.append(b); work.acknowledge(b); log.prepare(); } }
	clean.crash(seed, sector_cut);  // everything acknowledged is durable
	// Each written block's header size, by segment and block.
	std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> headers;
	{
		auto cursor = clean_io.scan_directory();
		while (auto name = cursor->next()) {
			auto number = d::segment_number(*name); if (!number) { continue; }
			auto file = clean_io.open_existing(*name); std::string block(options.block_bytes, '\0');
			for (std::uint64_t b = 0; b * options.block_bytes < options.segment_bytes; ++b) {
				if (file->read_at(b * options.block_bytes, block) != block.size()) { break; }
				auto header = f::parse_header(block, d::stream(identity, options.block_bytes, *number, 0), b);
				if (header.check == f::Check::ok) { headers[{*number, b}] = header.size; }
			}
		}
	}
	if (headers.empty()) { return; }
	const auto last = headers.rbegin()->first;
	for (const auto& [where, size] : headers) {
		const auto next = headers.find({where.first, where.second + 1});
		if (damage == Damage::last ? where != last : next == headers.end()) { continue; }
		std::mt19937_64 random(seed * 137 + where.first * 4099 + where.second);
		const std::uint64_t start = where.second * options.block_bytes;
		auto payload = start + size + random() % (options.block_bytes - size);
		if (damage == Damage::last) {
			// A byte of a fragment its sector holds whole, past the segment and epoch it names: what
			// recovery can tell from a tear (the journal's R7).
			std::vector<std::pair<std::uint64_t, std::uint64_t>> whole;
			auto file = clean_io.open_existing(d::segment_name(where.first)); std::string block(options.block_bytes, '\0'); file->read_at(start, block);
			for (std::size_t p = size; options.block_bytes - p >= f::fragment_header + 1;) {
				const auto fragment = f::parse_fragment(std::string_view(block).substr(p), options.block_bytes - p, where.first, f::peek64(std::string_view(block).substr(p + 16)));
				if (fragment.check != f::Check::ok) { break; }
				const auto end = p + f::fragment_header + fragment.payload.size();
				if ((start + p) / f::sector_bytes == (start + end - 1) / f::sector_bytes) { whole.emplace_back(start + p, start + end); }
				p = end;
			}
			if (whole.empty()) { continue; }
			const auto [low, high] = whole[random() % whole.size()];
			payload = low + random() % (high - low - 16); if (payload >= low + 8) { payload += 16; }
		}
		fl::filesystem disk = clean.snapshot(); fl::io io(disk); ++cases;
		const auto header = damage == Damage::last ? payload : next->first.second * options.block_bytes + random() % next->second;
		std::vector<std::uint64_t> offsets{damage == Damage::header ? header : payload};
		if (damage == Damage::both) { offsets.push_back(header); }
		{
			auto file = io.open_existing(d::segment_name(where.first));
			for (auto offset : offsets) {
				char byte = 0; file->read_at(offset, std::span<char>(&byte, 1));
				byte = static_cast<char>(byte ^ 0x5a); file->write_at(offset, std::string_view(&byte, 1));
			}
			file->sync();
		}
		const auto at = std::string(damage == Damage::both ? "both" : damage == Damage::header ? "header" : "last") + " block_bytes " + std::to_string(options.block_bytes) + " seed " + std::to_string(seed) + " segment " + std::to_string(where.first) + " block " + std::to_string(where.second);
		try {
			j::TurnLogGroups known;  // the host holds every group open: a damaged log loses those it holds nothing of
			for (const auto& [group, _] : work.acknowledged) { known.open.push_back(group); }
			j::TurnLog log(io, identity, recovered, options, known);
			bool kept = true;
			for (const auto& [group, records] : work.acknowledged) { if (!recovered.lost.contains(group) && recovered.groups[group] != records) { kept = false; } }
			check(recovered.damaged, "the damage is reported: " + at);
			check(kept, "a group not lost keeps every acknowledged record: " + at);
			if ((!kept || !recovered.damaged) && std::getenv("T9_DEBUG")) {
				std::cerr << "  block start " << start << " header " << size << " damaged:"; for (auto o : offsets) std::cerr << ' ' << o - start; std::cerr << " last_turn " << recovered.last_turn << '\n';
				std::cerr << "  payload " << payload << " header " << header << " lost:"; for (auto g : recovered.lost) std::cerr << ' ' << g; std::cerr << '\n';
				for (const auto& [group, records] : work.acknowledged) {
					if (recovered.lost.contains(group) || recovered.groups[group] == records) { continue; }
					auto& got = recovered.groups[group];
					std::cerr << "  group " << group << " acked [" << records.front().sequence << ".." << records.back().sequence << "] got " << got.size();
					if (!got.empty()) { std::cerr << " [" << got.front().sequence << ".." << got.back().sequence << "]"; }
					std::cerr << '\n';
				}
			}
		} catch (const std::exception& error) { check(false, at + ": " + error.what()); }
	}
}

std::uint64_t sector_of(std::uint64_t offset) { return offset / j::framing::sector_bytes; }
// A clean log's written blocks: header size, epoch, bytes, and each fragment's [start, end) and
// whether a header of its write starts before it in its sector, as scan_segment's `written`.
struct Fragment { std::size_t start = 0, end = 0; bool written = false; };
struct Written { std::size_t header = 0; std::uint64_t epoch = 0; std::string bytes; std::vector<Fragment> fragments; };
std::map<std::pair<std::uint64_t, std::uint64_t>, Written> written_blocks(j::IO& io) {
	namespace d = j::turn_log_detail; namespace f = j::framing;
	std::map<std::pair<std::uint64_t, std::uint64_t>, Written> result;
	auto cursor = io.scan_directory();
	while (auto name = cursor->next()) {
		auto number = d::segment_number(*name); if (!number) { continue; }
		auto file = io.open_existing(*name); std::string bytes(options.block_bytes, '\0');
		std::uint8_t kind = 0;  // the kind of the record the last First fragment began
		for (std::uint64_t b = 0; (b + 1) * options.block_bytes <= options.segment_bytes; ++b) {
			if (file->read_at(b * options.block_bytes, bytes) != bytes.size()) { break; }
			const auto header = f::parse_header(bytes, d::stream(identity, options.block_bytes, *number, 0), b);
			if (header.check != f::Check::ok) { continue; }
			Written block{header.size, header.epoch, bytes, {}};
			std::optional<std::size_t> in_write = 0;  // a block's header is written with its first fragment
			for (std::size_t p = header.size; options.block_bytes - p >= f::fragment_header + 1;) {
				const auto fragment = f::parse_fragment(std::string_view(bytes).substr(p), options.block_bytes - p, *number, header.epoch);
				if (fragment.check != f::Check::ok) { break; }
				const auto end = p + f::fragment_header + fragment.payload.size();
				const std::uint64_t start = b * options.block_bytes;
				block.fragments.push_back({p, end, in_write && sector_of(start + *in_write) == sector_of(start + p)});
				if (fragment.type == f::Full || fragment.type == f::First) { kind = static_cast<std::uint8_t>(fragment.payload[0]); }
				const bool ends = (fragment.type == f::Full || fragment.type == f::Last) && (kind == d::end_of_turn || kind == d::open_record);
				in_write = ends ? std::nullopt : std::optional{p}; p = end;
			}
			result[{*number, b}] = std::move(block);
		}
	}
	return result;
}

// A byte in the second sector of a fragment that spans two, where the next fragment starts and
// verifies (the review's P1), in every block, the newest tail included. Where a whole header of
// its write proves its first sector, the damage is reported and every group not lost keeps its
// records; the others, whose header the boundary cuts, are only counted (`unproven`).
void crossing_damage(std::uint64_t seed, std::uint64_t& cases, std::uint64_t& unproven) {
	namespace d = j::turn_log_detail;
	fl::filesystem clean; fl::io clean_io(clean); Workload work(seed); j::TurnRecovery recovered;
	{ j::TurnLog log(clean_io, identity, recovered, options); for (unsigned t = 0; t < 60; ++t) { auto b = work.turn(log); log.append(b); work.acknowledge(b); log.prepare(); } }
	clean.crash(seed, sector_cut);  // everything acknowledged is durable
	for (const auto& [where, block] : written_blocks(clean_io)) {
		const std::uint64_t start = where.second * options.block_bytes;
		std::mt19937_64 random(seed * 139 + where.first * 4099 + where.second);
		for (std::size_t i = 0; i + 1 < block.fragments.size(); ++i) {
			const auto [p, end, written] = block.fragments[i];
			const auto second = sector_of(start + end - 1);
			if (sector_of(start + p) + 1 != second || sector_of(start + block.fragments[i + 1].start) != second) { continue; }
			if (!written && sector_of(start + p + j::framing::fragment_header - 1) == second) { ++unproven; continue; }
			const auto boundary = second * j::framing::sector_bytes - start;
			const auto offset = start + boundary + random() % (end - boundary);
			fl::filesystem disk = clean.snapshot(); fl::io io(disk); ++cases;
			{
				auto file = io.open_existing(d::segment_name(where.first)); char byte = 0;
				file->read_at(offset, std::span<char>(&byte, 1));
				byte = static_cast<char>(byte ^ 0x5a); file->write_at(offset, std::string_view(&byte, 1)); file->sync();
			}
			const auto at = "crossing block_bytes " + std::to_string(options.block_bytes) + " seed " + std::to_string(seed) + " segment " + std::to_string(where.first) + " block " + std::to_string(where.second) + " offset " + std::to_string(offset - start);
			try {
				j::TurnLogGroups known; for (const auto& [group, _] : work.acknowledged) { known.open.push_back(group); }
				j::TurnLog log(io, identity, recovered, options, known);
				bool kept = true;
				for (const auto& [group, records] : work.acknowledged) { if (!recovered.lost.contains(group) && recovered.groups[group] != records) { kept = false; } }
				check(recovered.damaged, "the damage is reported: " + at);
				check(kept, "a group not lost keeps every acknowledged record: " + at);
			} catch (const std::exception& error) { check(false, at + ": " + error.what()); }
		}
	}
}

// Real tears of every fragment that crosses its sector (the review's P6), read with its scan's
// `written` and, where the tear is past its first sector, with its first sector proven too: the
// sectors after its first read zeros, random bytes or another segment's; only the sectors it fills
// read so, the later ones as written; or, with no header of its write before it, its first sector
// reads so. None is damage.
void simulated_tears(std::uint64_t seed, std::uint64_t& tears) {
	namespace d = j::turn_log_detail; namespace f = j::framing;
	fl::filesystem clean; fl::io clean_io(clean); Workload work(seed); j::TurnRecovery recovered;
	{ j::TurnLog log(clean_io, identity, recovered, options); for (unsigned t = 0; t < 60; ++t) { auto b = work.turn(log); log.append(b); work.acknowledge(b); log.prepare(); } }
	clean.crash(seed, sector_cut);
	const auto blocks = written_blocks(clean_io);
	for (const auto& [where, block] : blocks) {
		const std::uint64_t start = where.second * options.block_bytes;
		const Written* stale = nullptr;  // a block of another segment: bytes no fragment of this one verifies in
		for (const auto& [other, candidate] : blocks) { if (other.first != where.first) { stale = &candidate; break; } }
		std::mt19937_64 random(seed * 149 + where.first * 4099 + where.second);
		for (const auto& [p, end, written] : block.fragments) {
			const auto first = sector_of(start + p), last = sector_of(start + end - 1);
			if (first == last) { continue; }
			const std::size_t next = (first + 1) * f::sector_bytes - start;
			// Old bytes over [from, to): after the first sector, over the filled sectors, or over the first.
			const std::pair<std::size_t, std::size_t> spans[] = {{next, block.bytes.size()}, {next, last * f::sector_bytes - start}, {p, next}};
			for (std::size_t span = 0; span < 3; ++span) {
				const auto [from, to] = spans[span];
				if (to <= from || (span == 2 && written)) { continue; }
				for (int fill = 0; fill < 3; ++fill) {
					if (fill == 2 && !stale) { continue; }
					std::string torn = block.bytes;
					for (auto k = from; k < to; ++k) { torn[k] = fill == 0 ? '\0' : fill == 1 ? static_cast<char>(random()) : stale->bytes[k]; }
					const auto at = "seed " + std::to_string(seed) + " block_bytes " + std::to_string(options.block_bytes) + " segment " + std::to_string(where.first) + " block " + std::to_string(where.second) + " fragment " + std::to_string(p) + " span " + std::to_string(span) + " fill " + std::to_string(fill);
					++tears; check(!d::damaged_fragment(torn, start, p, where.first, block.epoch, written), "a torn fragment reads as a tear: " + at);
					if (!written && span != 2) { ++tears; check(!d::damaged_fragment(torn, start, p, where.first, block.epoch, true), "a torn fragment reads as a tear with its first sector proven: " + at); }
				}
			}
		}
	}
}

// Simulated tears whose old first sector holds another segment's record where the fragment
// starts: its turn reads as the checksum, length, type and reserved byte, and its group 2 and
// sequence 1 as segment 2 and epoch 1. Stale bytes naming the segment and epoch are no proof.
struct StaleRecordTear { std::uint64_t seed; std::uint32_t block_bytes; std::uint64_t segment, block; std::size_t fragment; };
constexpr StaleRecordTear stale_record_tears[] = {{29, 2048, 2, 2, 338}, {136, 2048, 2, 1, 750}, {189, 4096, 2, 3, 1627}};
void stale_record_tear(const StaleRecordTear& tear) {
	namespace d = j::turn_log_detail; namespace f = j::framing;
	const auto at = "stale record tear: seed " + std::to_string(tear.seed) + " block_bytes " + std::to_string(tear.block_bytes) + " segment " + std::to_string(tear.segment) + " block " + std::to_string(tear.block) + " fragment " + std::to_string(tear.fragment);
	const auto saved = options;
	options = j::TurnLogOptions{tear.block_bytes * (tear.block_bytes == 4096 ? 4u : 8u), tear.block_bytes, 1, 3, 600};
	fl::filesystem clean; fl::io clean_io(clean); Workload work(tear.seed); j::TurnRecovery recovered;
	{ j::TurnLog log(clean_io, identity, recovered, options); for (unsigned t = 0; t < 60; ++t) { auto b = work.turn(log); log.append(b); work.acknowledge(b); log.prepare(); } }
	clean.crash(tear.seed, sector_cut);
	const auto blocks = written_blocks(clean_io);
	const auto found = blocks.find({tear.segment, tear.block});
	const Written* stale = nullptr;
	for (const auto& [other, candidate] : blocks) { if (other.first != tear.segment) { stale = &candidate; break; } }
	const Fragment* fragment = nullptr;
	if (found != blocks.end()) { for (const auto& candidate : found->second.fragments) { if (candidate.start == tear.fragment) { fragment = &candidate; } } }
	check(stale && fragment && !fragment->written, "the case's fragment and stale block exist: " + at);
	if (stale && fragment && !fragment->written) {
		const std::uint64_t start = tear.block * options.block_bytes;
		const std::size_t next = (sector_of(start + fragment->start) + 1) * f::sector_bytes - start;
		std::string torn = found->second.bytes;
		for (auto k = fragment->start; k < next; ++k) { torn[k] = stale->bytes[k]; }
		const auto bytes = std::string_view(torn).substr(fragment->start);
		check(f::peek64(bytes.substr(8)) == tear.segment && f::peek64(bytes.substr(16)) == found->second.epoch, "the stale bytes name the segment and epoch: " + at);
		check(!d::damaged_fragment(torn, start, fragment->start, tear.segment, found->second.epoch, false), "a torn fragment reads as a tear: " + at);
	}
	options = saved;
}
}  // namespace

int main(int argc, char** argv) {
	// turn_log_power_test [SEEDS [FIRST_SEED [BLOCK_BYTES]]]: SEEDS seeds from FIRST_SEED, at one
	// geometry or all.
	const std::uint64_t seeds = argc > 1 ? std::stoull(argv[1]) : 8, first = argc > 2 ? std::stoull(argv[2]) : 1;
	const std::uint32_t only = argc > 3 ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 0;
	// Block bytes and blocks per segment. Damage runs where block starts are sector-aligned: an
	// unaligned header shares its sector with the block before (the journal's R8).
	const std::pair<std::uint32_t, std::uint64_t> geometries[] = {{512, 8}, {4096, 4}, {2048, 8}, {1000, 8}, {768, 8}, {256, 16}};
	for (const auto& tear : stale_record_tears) { stale_record_tear(tear); }
	for (const auto& [block, blocks] : geometries) {
		if (only && block != only) { continue; }
		options = j::TurnLogOptions{block * blocks, block, 1, 3, 600};
		const auto before = failures;
		std::uint64_t points = 0, seconds = 0;
		for (std::uint64_t seed = first; seed < first + seeds; ++seed) {
			// How many mutations an uninterrupted run makes.
			fl::filesystem disk; fl::io io(disk); Workload work(seed); j::TurnRecovery recovered;
			{
				j::TurnLog log(io, identity, recovered, options);
				for (unsigned t = 0; t < 60; ++t) { auto b = work.turn(log); log.append(b); work.acknowledge(b); log.prepare(); }
				if (block == 512) { check(log.stats().segments_recycled > 0 && log.stats().forwarded_records > 0, "the run recycles segments and moves records forward (condition 10)"); }
			}
			for (std::uint64_t cut = 1; cut <= disk.mutations(); ++cut, ++points) {
				const std::uint64_t second = cut % 3 == 0 ? cut % 7 + 1 : 0;
				seconds += second != 0;
				run(seed, cut, second, 60, 5);
			}
		}
		std::cout << "block " << block << ": " << points << " crash points (" << seconds << " with a second loss in recovery)";
		if (block % j::framing::sector_bytes == 0 || j::framing::sector_bytes % block == 0) {
			std::uint64_t corrupted = 0, both = 0, header = 0, last = 0, crossing = 0, unproven = 0, tears = 0;
			for (std::uint64_t seed = first; seed < first + seeds; ++seed) {
				corrupted_blocks(seed, corrupted);
				boundary_damage(seed, Damage::both, both); boundary_damage(seed, Damage::header, header); boundary_damage(seed, Damage::last, last);
				crossing_damage(seed, crossing, unproven); simulated_tears(seed, tears);
			}
			std::cout << ", " << corrupted << " corrupted blocks, " << both << " doubly damaged boundaries, " << header << " damaged headers, " << last << " damaged last blocks";
			std::cout << ", " << crossing << " damaged crossing fragments (" << unproven << " more with a cut header), " << tears << " simulated tears";
		}
		std::cout << ", " << failures - before << " failures\n";
	}
	std::cout << checks << " checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
