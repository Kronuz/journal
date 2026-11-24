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

// T9 (strata Track C): RFTW0002 under power loss. A randomized multi-group workload over
// faultlab's faultfs, whose unsynced 512-byte sectors each persist or not, whose unsynced size
// may end at any sector boundary (size_pick::sector), and whose namespace changes persist only
// after a directory sync. Power is cut after every mutating I/O operation of each run, and at
// every third point again during the recovery that follows. Segments recycle, so stale blocks
// from an earlier use are present (condition 10). The sweep runs in either flush mode, with the
// calls macOS makes on any host: full loses nothing acknowledged; plain survives a process crash
// whole, and after a power loss recovers, per group, only a state the group was in.
#include "faultlab/faultfs.h"
#include "journal/turn_log.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <random>

namespace j = kronuz::journal;
namespace d = j::turn_log_detail;

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
	std::map<std::uint64_t, std::vector<std::vector<j::TurnRecord>>> history;  // each group's every acknowledged state
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
		for (const auto& r : batch) { history[r.group].push_back(acknowledged[r.group]); }
	}
	// Plain mode's power loss: every group recovery keeps is in a state it was in, the in-flight
	// turn's included; nothing recovered was never written.
	bool consistent(const j::TurnRecovery& recovered) const {
		auto with = acknowledged; for (const auto& r : in_flight) { if (r.kind == j::TurnRecordKind::Checkpoint) { with[r.group].clear(); } with[r.group].push_back(r); }
		for (const auto& [group, records] : recovered.groups) {
			if (recovered.lost.contains(group)) { continue; }
			const auto found = history.find(group);
			const bool known = found != history.end() && std::find(found->second.begin(), found->second.end(), records) != found->second.end();
			if (!known && !(with.contains(group) && with.at(group) == records)) { return false; }
		}
		return true;
	}
	// Recovery holds every acknowledged record, and the in-flight turn whole or not at all.
	bool matches(const j::TurnRecovery& recovered) {
		if (recovered.groups == acknowledged) { return true; }
		auto with = acknowledged; for (const auto& r : in_flight) { if (r.kind == j::TurnRecordKind::Checkpoint) { with[r.group].clear(); } with[r.group].push_back(r); }
		if (recovered.groups == with) { acknowledge(in_flight); return true; }
		return false;
	}
};
// The geometry under test: main runs sector-sized and multi-sector blocks.
j::TurnLogOptions options{8 * 512, 512, 1, 3, 600};
// The sweep's flush mode, and how each run's loss ends: a power loss (crash) or, in plain mode, a
// process crash (power_on), which keeps what a plain sync left in the drive's cache.
enum class Mode { full, plain };
enum class Loss { power, process };
// The disk makes the calls completion's PosixIO makes on macOS in `mode`, on every host.
void open_mode(fl::filesystem& disk, Mode mode) {
	disk.set_flush_mode(mode == Mode::full ? fl::flush_mode::full : fl::flush_mode::plain, fl::sync_platform::macos);
}
// Plain mode's tally: crash points whose power loss lost an acknowledged record, or reported damage.
struct PlainLoss { std::uint64_t lost = 0, damaged = 0; };
PlainLoss plain_loss;
std::string listing(const fl::filesystem& disk) { std::string names; for (const auto& name : disk.list()) { names += ' ' + name; } return names; }

// Runs `turns` turns, cutting power at mutation `cut` (and at `second` during the recovery
// that follows), then reopens and continues for `after` more turns. Only the power cut fails a
// run: any other error is a failure.
void run(std::uint64_t seed, std::uint64_t cut, std::uint64_t second, unsigned turns, unsigned after, Mode mode, Loss loss) {
	fl::filesystem disk; open_mode(disk, mode); fl::io io(disk); Workload work(seed);
	j::TurnRecovery recovered;
	const auto at = std::string(mode == Mode::full ? "full" : "plain") + (loss == Loss::power ? " power" : " process") + " seed " + std::to_string(seed) + " cut " + std::to_string(cut) + " second " + std::to_string(second);
	auto lose = [&](std::uint64_t crash_seed) { if (loss == Loss::power) { disk.crash(crash_seed, sector_cut); } else { disk.power_on(); } };
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
	lose(seed * 7919 + cut);
	names += "; after" + listing(disk);
	if (second) {
		disk.power_off_after(second - 1);
		try { j::TurnLog log(io, identity, recovered, options); }
		catch (const std::exception& error) { check(disk.powered_off(), "only the power cut fails recovery: " + at + ": " + error.what()); }
		names += "; before second crash" + listing(disk);
		lose(seed * 7919 + cut + (second << 32));
		names += "; after" + listing(disk);
	}
	try {
		j::TurnLog log(io, identity, recovered, options);
		if (mode == Mode::plain && loss == Loss::power) {
			check(work.consistent(recovered), "plain mode's power loss recovers each group in a state it was in: " + at + names);
			plain_loss.damaged += recovered.damaged || !recovered.lost.empty();
			plain_loss.lost += !work.matches(recovered);
			return;
		}
		check(!recovered.damaged && recovered.lost.empty(), "power loss alone damages nothing and loses no group: " + at + names);
		check(work.matches(recovered), "every acknowledged record survives, and the in-flight turn whole or not at all: " + at);
		work.in_flight.clear();
		for (unsigned t = 0; t < after; ++t) { auto batch = work.turn(log); log.append(batch); work.acknowledge(batch); }
	} catch (const std::exception& error) {
		check(false, at + ": " + error.what());
	}
}

constexpr std::uint64_t sector = d::sector_bytes;
// A clean log's written blocks, read from their sectors: each block's header size, and each
// fragment's data positions [start, end) in its segment with the record it belongs to.
struct Fragment { std::uint64_t start = 0, end = 0; std::uint8_t kind = 0; std::uint64_t turn = 0, group = 0; bool first = false, last = false, known = false, ends = false; };
struct Written { std::uint64_t header = 0; std::vector<Fragment> fragments; };
using Layout = std::map<std::pair<std::uint64_t, std::uint64_t>, Written>;  // by segment and block
Layout layout(j::IO& io) {
	Layout result;
	const std::uint64_t block = d::data_bytes(options.block_bytes), per_block = options.block_bytes / sector;
	auto cursor = io.scan_directory();
	while (auto name = cursor->next()) {
		const auto number = d::segment_number(*name); if (!number) { continue; }
		auto file = io.open_existing(*name); std::string raw(options.block_bytes, '\0'), data(block, '\0');
		std::string head;  // the record header the fragments so far assembled
		for (std::uint64_t b = 0; (b + 1) * options.block_bytes <= file->size(); ++b) {
			file->read_at(b * options.block_bytes, raw);
			std::uint64_t current = 0;  // the data bytes of the block's leading current sectors
			while (current < block && d::verdict(raw.data() + current / d::sector_data * sector, *number, b * per_block + current / d::sector_data) == d::Sector::current) {
				std::copy_n(raw.data() + current / d::sector_data * sector, d::sector_data, data.data() + current); current += d::sector_data;
			}
			const std::string_view view(data);
			if (current < d::header_fixed || view.substr(0, 8) != d::magic || j::framing::peek64(view.substr(24)) != *number || j::framing::peek32(view.substr(32)) != b) { continue; }
			Written written{d::header_bytes(j::framing::get16(view.substr(56))), {}};
			for (auto p = written.header; block - p >= d::fragment_header + 1 && p + d::fragment_header <= current;) {
				const std::uint64_t length = j::framing::get16(view.substr(p)); const auto type = static_cast<std::uint8_t>(view[p + 2]);
				if (!length || p + d::fragment_header + length > current) { break; }
				if (type == j::framing::Full || type == j::framing::First) { head.clear(); }
				head.append(view.substr(p + d::fragment_header, std::min<std::size_t>(length, d::record_header - std::min(head.size(), d::record_header))));
				Fragment fragment{b * block + p, b * block + p + d::fragment_header + length};
				fragment.first = type == j::framing::Full || type == j::framing::First;
				fragment.last = type == j::framing::Full || type == j::framing::Last;
				if (head.size() >= d::record_header) {
					fragment.known = true;
					fragment.kind = static_cast<std::uint8_t>(head[0]); fragment.turn = j::framing::peek64(std::string_view(head).substr(1)); fragment.group = j::framing::peek64(std::string_view(head).substr(9));
				}
				written.fragments.push_back(fragment);
				p += d::fragment_header + length;
			}
			result[{*number, b}] = std::move(written);
		}
	}
	// A record whose header a block's end cuts: its earlier fragments take what its header names.
	std::vector<Fragment*> record;
	for (auto& [where, written] : result) {
		for (auto& f : written.fragments) {
			if (f.first) { record.clear(); }
			record.push_back(&f);
			if (f.known) { for (auto* earlier : record) { earlier->known = true; earlier->kind = f.kind; earlier->turn = f.turn; earlier->group = f.group; } }
			f.ends = f.last && (f.kind == d::end_of_turn || f.kind == d::open_record);
		}
	}
	return result;
}
// The sectors a written block's data reaches: from its first through the one its last fragment ends in.
std::pair<std::uint64_t, std::uint64_t> written_sectors(std::uint64_t block, const Written& written) {
	const auto first = d::file_offset(block * d::data_bytes(options.block_bytes)) / sector;
	return {first, written.fragments.empty() ? first : d::file_offset(written.fragments.back().end - 1) / sector};
}
void flip(j::IO& io, std::uint64_t segment, std::uint64_t offset) {
	auto file = io.open_existing(d::segment_name(segment)); char byte = 0;
	check(file->read_at(offset, std::span<char>(&byte, 1)) == 1, "the damaged byte is in the segment");
	byte = static_cast<char>(byte ^ 0x5a); file->write_at(offset, std::string_view(&byte, 1)); file->sync();
}
bool kept(const j::TurnRecovery& recovered, const Model& model) {
	for (const auto& [group, records] : model) {
		const auto found = recovered.groups.find(group);
		if (!recovered.lost.contains(group) && (found == recovered.groups.end() || found->second != records)) { return false; }
	}
	return true;
}
void clean_log(fl::filesystem& clean, Workload& work) {
	fl::io io(clean); j::TurnRecovery recovered;
	{ j::TurnLog log(io, identity, recovered, options); for (unsigned t = 0; t < 60; ++t) { auto b = work.turn(log); log.append(b); work.acknowledge(b); log.prepare(); } }
	clean.crash(0, sector_cut);  // everything acknowledged is durable
}

// One corrupted block before the newest turn costs only the groups with records in it: every
// other group's acknowledged records survive.
void corrupted_blocks(std::uint64_t seed, std::uint64_t& blocks) {
	fl::filesystem clean; Workload work(seed); clean_log(clean, work);
	fl::io clean_io(clean); const auto written = layout(clean_io);
	std::map<std::pair<std::uint64_t, std::uint64_t>, std::set<std::uint64_t>> groups;  // a record's every block holds its group
	std::uint64_t newest = 0; std::pair<std::uint64_t, std::uint64_t> last{std::numeric_limits<std::uint64_t>::max(), 0};
	for (const auto& [where, block] : written) {
		for (const auto& f : block.fragments) { if (f.group) { groups[where].insert(f.group); } if (f.kind != d::open_record) { newest = std::max(newest, f.turn); } }
	}
	for (const auto& [where, block] : written) { for (const auto& f : block.fragments) { if (f.turn == newest && f.kind != d::open_record) { last = std::min(last, where); } } }
	for (const auto& [where, block] : written) {
		if (where >= last) { continue; }
		fl::filesystem disk = clean.snapshot(); fl::io io(disk); ++blocks;
		std::mt19937_64 random(seed * 131 + where.second);
		const auto [first, end] = written_sectors(where.second, block);
		flip(io, where.first, first * sector + random() % ((end + 1 - first) * sector));
		const auto at = "seed " + std::to_string(seed) + " segment " + std::to_string(where.first) + " block " + std::to_string(where.second);
		try {
			j::TurnRecovery recovered; j::TurnLog log(io, identity, recovered, options);
			const auto& named = groups[where];
			check(recovered.damaged, "a corrupted block is reported: " + at);
			check(std::all_of(recovered.lost.begin(), recovered.lost.end(), [&](std::uint64_t g) { return named.contains(g); }), "a corrupted block loses only groups with records in it: " + at);
			check(kept(recovered, work.acknowledged), "every other group keeps its acknowledged records: " + at);
		} catch (const std::exception& error) { check(false, at + ": " + error.what()); }
	}
}

// Damage at every block boundary of a written log, the newest turn's blocks included: a byte past a
// block's header and one in the next block's header, which names the first block's groups, or the
// header byte alone; and a byte of a fragment of the last written block. The damage is reported,
// and every group recovery does not call lost keeps its acknowledged records.
enum class Damage { both, header, last };
void boundary_damage(std::uint64_t seed, Damage damage, std::uint64_t& cases) {
	fl::filesystem clean; Workload work(seed); clean_log(clean, work);
	fl::io clean_io(clean); const auto written = layout(clean_io);
	if (written.empty()) { return; }
	const auto newest = written.rbegin()->first;
	const std::uint64_t block = d::data_bytes(options.block_bytes);
	for (const auto& [where, here] : written) {
		const auto next = written.find({where.first, where.second + 1});
		if (damage == Damage::last ? where != newest : next == written.end()) { continue; }
		if (here.fragments.empty()) { continue; }
		std::mt19937_64 random(seed * 137 + where.first * 4099 + where.second);
		const auto& f = here.fragments[random() % here.fragments.size()];
		const auto payload = d::file_offset(f.start + random() % (f.end - f.start));
		fl::filesystem disk = clean.snapshot(); fl::io io(disk); ++cases;
		const auto header = damage == Damage::last ? payload : d::file_offset(next->first.second * block + random() % next->second.header);
		if (damage != Damage::header) { flip(io, where.first, payload); }
		if (damage != Damage::last) { flip(io, where.first, header); }
		const auto at = std::string(damage == Damage::both ? "both" : damage == Damage::header ? "header" : "last") + " block_bytes " + std::to_string(options.block_bytes) + " seed " + std::to_string(seed) + " segment " + std::to_string(where.first) + " block " + std::to_string(where.second);
		try {
			j::TurnLogGroups known;  // the host holds every group open: a damaged log loses those it holds nothing of
			for (const auto& [group, _] : work.acknowledged) { known.open.push_back(group); }
			j::TurnRecovery recovered; j::TurnLog log(io, identity, recovered, options, known);
			check(recovered.damaged, "the damage is reported: " + at);
			check(kept(recovered, work.acknowledged), "a group not lost keeps every acknowledged record: " + at);
		} catch (const std::exception& error) { check(false, at + ": " + error.what()); }
	}
}

// A byte in every sector the log wrote, the newest turn's included, two places each: each is
// reported, and every group not lost keeps its acknowledged records.
void sector_damage(std::uint64_t seed, std::uint64_t& cases) {
	fl::filesystem clean; Workload work(seed); clean_log(clean, work);
	fl::io clean_io(clean);
	j::TurnLogGroups known; for (const auto& [group, _] : work.acknowledged) { known.open.push_back(group); }
	std::mt19937_64 random(seed * 139);
	for (const auto& [where, block] : layout(clean_io)) {
		const auto [first, last] = written_sectors(where.second, block);
		for (auto s = first; s <= last; ++s) {
			fl::filesystem disk = clean.snapshot(); fl::io io(disk); ++cases;
			const auto offset = s * sector + random() % sector;
			flip(io, where.first, offset);
			const auto at = "sector block_bytes " + std::to_string(options.block_bytes) + " seed " + std::to_string(seed) + " segment " + std::to_string(where.first) + " offset " + std::to_string(offset);
			try {
				const auto recovered = j::TurnLog::read(io, identity, options, known);
				if (offset % sector >= d::sector_data && offset % sector < d::sector_data + 4) {
					// The stored low bits only find a stale sector's segment: the sector verifies under its own.
					check(!recovered.damaged && recovered.lost.empty() && recovered.groups == work.acknowledged, "a sector's damaged low bits cost nothing: " + at);
					continue;
				}
				check(recovered.damaged, "the damage is reported: " + at);
				check(kept(recovered, work.acknowledged), "a group not lost keeps every acknowledged record: " + at);
			} catch (const std::exception& error) { check(false, at + ": " + error.what()); }
		}
	}
}

// The newest write W of a clean log torn every way: each set of the sectors it changed reaches the
// disk, the others stay as before W (the turn before's data, zeros, or a recycled file's stale
// sectors). None is damage, and recovery holds the acknowledged records, with W whole or without it.
void torn_newest(std::uint64_t seed, std::uint64_t& tears) {
	fl::filesystem disk; fl::io io(disk); Workload work(seed); j::TurnRecovery recovered;
	j::TurnLog log(io, identity, recovered, options);
	for (unsigned t = 0; t < 60; ++t) { auto b = work.turn(log); log.append(b); work.acknowledge(b); log.prepare(); }
	fl::filesystem pre = disk.snapshot(); pre.crash(0, sector_cut);
	const auto w = work.turn(log); log.append(w);
	fl::filesystem post = disk.snapshot(); post.crash(0, sector_cut);
	auto read = [](fl::filesystem& fs) {
		fl::io fio(fs); std::map<std::string, std::string> out;
		for (const auto& name : fs.list()) { if (d::segment_number(name)) { auto f = fio.open_existing(name); std::string bytes(f->size(), '\0'); f->read_at(0, bytes); out[name] = bytes; } }
		return out;
	};
	const auto before = read(pre), after = read(post);
	std::vector<std::pair<std::string, std::uint64_t>> changed;
	for (const auto& [name, bytes] : after) {
		const auto found = before.find(name);
		if (found == before.end()) { continue; }  // a segment W's barrier let the log recycle
		for (std::uint64_t s = 0; s * sector < bytes.size(); ++s) { if (found->second.compare(s * sector, sector, bytes, s * sector, sector)) { changed.push_back({name, s}); } }
	}
	std::mt19937_64 random(seed * 151);
	const auto k = changed.size();
	std::vector<std::uint64_t> masks;
	if (k <= 6) { for (std::uint64_t m = 0; m + 1 < (std::uint64_t{1} << k); ++m) { masks.push_back(m); } }
	else { for (int i = 0; i < 64; ++i) { masks.push_back(random() & ((k >= 64 ? ~0ull : (1ull << k) - 1) & ~(1ull << (random() % k)))); } }
	for (const auto mask : masks) {
		fl::filesystem torn = pre.snapshot(); fl::io torn_io(torn); ++tears;
		for (std::size_t i = 0; i < k; ++i) {
			if (!(mask >> i & 1)) { continue; }
			const auto& [name, s] = changed[i];
			auto file = torn_io.open_existing(name); file->write_at(s * sector, std::string_view(after.at(name)).substr(s * sector, sector)); file->sync();
		}
		const auto at = "torn newest: block_bytes " + std::to_string(options.block_bytes) + " seed " + std::to_string(seed) + " mask " + std::to_string(mask) + " of " + std::to_string(k);
		try {
			const auto r = j::TurnLog::read(torn_io, identity, options);
			auto model = work; model.in_flight = w;
			check(!r.damaged && r.lost.empty() && model.matches(r), "a tear of the newest write reads as a tear: " + at);
		} catch (const std::exception& error) { check(false, at + ": " + error.what()); }
	}
}

// A sector the device cannot read: one an acknowledged turn before the newest wrote is damage; one
// past the newest write's data is never read, so it costs nothing.
void unreadable() {
	const auto saved = options;
	options = j::TurnLogOptions{8 * 2048, 2048, 1, 3, 600};
	// The first seed whose newest block has a sector past its data.
	std::uint64_t seed = 1;
	for (;; ++seed) {
		fl::filesystem disk; Workload work(seed); clean_log(disk, work);
		fl::io io(disk); const auto found = layout(io); const auto& last = found.rbegin()->second;
		if ((d::file_offset(last.fragments.back().end) / sector + 1) % (options.block_bytes / sector) != 0) { break; }
	}
	fl::filesystem clean; Workload work(seed); clean_log(clean, work);
	fl::io clean_io(clean); const auto written = layout(clean_io);
	const auto& [newest, last] = *written.rbegin();
	const auto end = d::file_offset(last.fragments.back().end) / sector;
	const auto name = d::segment_name(newest.first);
	{
		fl::filesystem disk = clean.snapshot(); fl::io io(disk);
		const auto first = d::file_offset(newest.second * d::data_bytes(options.block_bytes)) / sector;
		disk.add_media_fault(name, fl::media::read_error, first * sector + 100, first * sector + 101);
		j::TurnLogGroups known; for (const auto& [group, _] : work.acknowledged) { known.open.push_back(group); }
		const auto r = j::TurnLog::read(io, identity, options, known);
		check(r.damaged && kept(r, work.acknowledged) && !r.lost.empty(), "an unreadable sector the log wrote is damage, and costs the groups it may hold");
		check(disk.media_hits() > 0, "recovery read the unreadable sector");
	}
	const auto past = end + 1;
	{
		fl::filesystem disk = clean.snapshot(); fl::io io(disk);
		disk.add_media_fault(name, fl::media::read_error, past * sector, past * sector + sector);
		const auto r = j::TurnLog::read(io, identity, options);
		check(!r.damaged && r.lost.empty() && r.groups == work.acknowledged, "an unreadable sector past the data costs nothing");
	}
	options = saved;
}

// The cut-header probe's sweeps (journal:b335b69, test/cut_header_probe.cc) on this format, the
// same workload, seeds and opens. newest: the newest turn W of a log reopened twice, damage at
// three places in each later sector of each W fragment crossing a sector, then in every sector W
// changed; blocks: the same damage in every crossing fragment of a clean 60-turn log, its newest
// turn tallied apart. Each case is reported, harmless, or SILENT.
using Tally = std::map<std::string, std::uint64_t>;
struct Probe { Tally newest, changed, blocks, blocks_last; std::uint64_t new_segment = 0; };
std::map<std::string, std::string> segment_files(fl::filesystem& disk) {
	fl::io io(disk); std::map<std::string, std::string> result;
	for (const auto& name : disk.list()) {
		if (!d::segment_number(name)) { continue; }
		auto file = io.open_existing(name); std::string bytes(file->size(), '\0'); file->read_at(0, bytes); result[name] = bytes;
	}
	return result;
}
void probe_damage(Tally& tally, const fl::filesystem& post, const std::string& name, const std::string& after, std::uint64_t offset, const Model& expected) {
	fl::filesystem damaged = post.snapshot(); fl::io io(damaged);
	const char byte = static_cast<char>(after[offset] ^ 0x5a);
	{ auto file = io.open_existing(name); file->write_at(offset, std::string_view(&byte, 1)); file->sync(); }
	try {
		j::TurnLogGroups known; for (const auto& [g, _] : expected) { known.open.push_back(g); }
		const auto r = j::TurnLog::read(io, identity, options, known);
		const auto outcome = r.damaged ? (kept(r, expected) ? "reported" : "reported, a group wrong") : (r.groups == expected && r.lost.empty() ? "harmless" : "SILENT");
		++tally[outcome];
		check(r.damaged ? kept(r, expected) : r.groups == expected && r.lost.empty(), "probe damage is reported or harmless: " + name + " offset " + std::to_string(offset));
	} catch (const std::exception& error) { check(false, "probe damage threw: " + name + ": " + error.what()); }
}
// The probe's case selection: the fragments of `segment` crossing a sector, by file offsets.
std::vector<std::pair<std::uint64_t, std::uint64_t>> crossing(const Layout& written, std::uint64_t segment, std::uint64_t from = 0) {
	std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
	for (const auto& [where, block] : written) {
		if (where.first != segment) { continue; }
		for (const auto& f : block.fragments) {
			const auto start = d::file_offset(f.start), end = d::file_offset(f.end - 1) + 1;
			if (start >= from && start / sector != (end - 1) / sector) { out.push_back({start, end}); }
		}
	}
	return out;
}
void probe_newest(std::uint64_t seed, Probe& probe) {
	fl::filesystem disk; Workload work(seed); j::TurnRecovery recovered;
	std::mt19937_64 random(seed * 7907);
	const auto at = "seed " + std::to_string(seed) + " block " + std::to_string(options.block_bytes);
	for (int open = 0; open < 3; ++open) {
		fl::io io(disk);
		try {
			j::TurnLog log(io, identity, recovered, options);
			check(!recovered.damaged && recovered.lost.empty(), "reopen reports no damage: " + at);
			if (work.matches(recovered)) { work.in_flight.clear(); } else { check(false, "reopen keeps what was acknowledged: " + at); }
			const unsigned turns = open < 2 ? 20 : static_cast<unsigned>(seed % 4) * 7;
			for (unsigned t = 0; t < turns; ++t) { auto b = work.turn(log); log.append(b); work.acknowledge(b); log.prepare(); }
			if (open == 0) { work.in_flight = work.turn(log); disk.power_off_after(1); log.append(work.in_flight); }
			if (open < 2) { continue; }
			fl::filesystem pre = disk.snapshot(); pre.crash(seed, sector_cut);
			auto w = work.turn(log); log.append(w);
			fl::filesystem post = disk.snapshot(); post.crash(seed, sector_cut);
			auto with_w = work.acknowledged; for (const auto& r : w) { if (r.kind == j::TurnRecordKind::Checkpoint) { with_w[r.group].clear(); } with_w[r.group].push_back(r); }
			const auto before = segment_files(pre), after = segment_files(post);
			std::vector<std::pair<std::string, std::uint64_t>> sectors;
			for (const auto& [name, bytes] : after) {
				auto found = before.find(name);
				for (std::uint64_t s = 0; s * sector < bytes.size(); ++s) {
					if (found == before.end() || found->second.compare(s * sector, sector, bytes, s * sector, sector)) { sectors.push_back({name, s}); }
				}
			}
			if (sectors.empty() || !before.contains(sectors.back().first)) { ++probe.new_segment; return; }
			const auto& name = sectors.back().first; const auto number = *d::segment_number(name);
			std::set<std::uint64_t> changed; for (const auto& [n, s] : sectors) { if (n == name) { changed.insert(s); } }
			fl::io post_io(post);
			for (const auto& [start, end] : crossing(layout(post_io), number)) {
				const auto first = start / sector, last = (end - 1) / sector;
				if (!changed.contains(first)) { continue; }
				for (auto s = first + 1; s <= last; ++s) {
					const auto from = s * sector, to = std::min<std::uint64_t>(end, (s + 1) * sector);
					for (int n = 0; n < 3; ++n) { probe_damage(probe.newest, post, name, after.at(name), from + random() % (to - from), with_w); }
				}
			}
			for (const auto s : changed) { for (int n = 0; n < 3; ++n) { probe_damage(probe.changed, post, name, after.at(name), s * sector + random() % sector, with_w); } }
		} catch (const std::exception& error) {
			if (open == 0 && disk.powered_off()) { disk.crash(seed * 31, sector_cut); continue; }
			check(false, at + ": " + error.what());
		}
	}
}
void probe_blocks(std::uint64_t seed, Probe& probe) {
	fl::filesystem clean; fl::io clean_io(clean); Workload work(seed); j::TurnRecovery recovered;
	{ j::TurnLog log(clean_io, identity, recovered, options); for (unsigned t = 0; t < 60; ++t) { auto b = work.turn(log); log.append(b); work.acknowledge(b); log.prepare(); } }
	clean.crash(seed, sector_cut);
	std::mt19937_64 random(seed * 139);
	const auto all = segment_files(clean); const auto written = layout(clean_io);
	std::uint64_t newest_segment = 0, newest_turn = 0;
	for (const auto& [where, block] : written) { for (const auto& f : block.fragments) { newest_segment = std::max(newest_segment, where.first); if (f.kind != d::open_record) { newest_turn = std::max(newest_turn, f.turn); } } }
	std::uint64_t last_from = ~0ull;  // where the newest turn's first fragment starts, in the newest segment
	for (const auto& [where, block] : written) { for (const auto& f : block.fragments) { if (where.first == newest_segment && f.turn == newest_turn && f.kind != d::open_record) { last_from = std::min(last_from, d::file_offset(f.start)); } } }
	for (const auto& [name, bytes] : all) {
		const auto number = *d::segment_number(name);
		for (const auto& [start, end] : crossing(written, number)) {
			auto& tally = number == newest_segment && start >= last_from ? probe.blocks_last : probe.blocks;
			for (auto s = start / sector + 1; s <= (end - 1) / sector; ++s) {
				const auto from = s * sector, to = std::min<std::uint64_t>(end, (s + 1) * sector);
				probe_damage(tally, clean, name, bytes, from + random() % (to - from), work.acknowledged);
			}
		}
	}
}
void probe(std::uint64_t seeds, std::uint64_t block_seeds) {
	Probe probe;
	for (std::uint32_t block : {2048u, 4096u}) {
		options = j::TurnLogOptions{block * 8, block, 1, 3, 600};
		for (std::uint64_t seed = 1; seed <= seeds; ++seed) { probe_newest(seed, probe); }
		for (std::uint64_t seed = 1; seed <= block_seeds; ++seed) { probe_blocks(seed, probe); }
	}
	std::cout << "newest tail: " << probe.new_segment << " runs where W opens a new segment, not in the shape\n";
	for (const auto& [key, count] : probe.newest) { std::cout << "newest crossing " << key << ": " << count << '\n'; }
	for (const auto& [key, count] : probe.changed) { std::cout << "newest every changed sector " << key << ": " << count << '\n'; }
	for (const auto& [key, count] : probe.blocks) { std::cout << "blocks " << key << ": " << count << '\n'; }
	for (const auto& [key, count] : probe.blocks_last) { std::cout << "blocks-newest-turn " << key << ": " << count << '\n'; }
}
}  // namespace

int main(int argc, char** argv) {
	// turn_log_power_test [plain] [SEEDS [FIRST_SEED [BLOCK_BYTES]]]: SEEDS seeds from FIRST_SEED,
	// at one geometry or all, in full mode or plain. turn_log_power_test probe [SEEDS [BLOCK_SEEDS]]:
	// the cut-header probe's sweeps.
	if (argc > 1 && std::string_view(argv[1]) == "probe") {
		probe(argc > 2 ? std::stoull(argv[2]) : 24, argc > 3 ? std::stoull(argv[3]) : 40);
		std::cout << checks << " checks, " << failures << " failures\n";
		return failures ? 1 : 0;
	}
	const Mode mode = argc > 1 && std::string_view(argv[1]) == "plain" ? Mode::plain : Mode::full;
	if (mode == Mode::plain) { --argc; ++argv; }
	const std::uint64_t seeds = argc > 1 ? std::stoull(argv[1]) : 8, first = argc > 2 ? std::stoull(argv[2]) : 1;
	const std::uint32_t only = argc > 3 ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 0;
	const std::pair<std::uint32_t, std::uint64_t> geometries[] = {{512, 8}, {4096, 4}, {2048, 8}, {1024, 8}};
	if (mode == Mode::full) { unreadable(); }
	for (const auto& [block, blocks] : geometries) {
		if (only && block != only) { continue; }
		options = j::TurnLogOptions{block * blocks, block, 1, 3, 600};
		const auto before = failures;
		const auto loss_before = plain_loss;
		std::uint64_t points = 0, seconds = 0;
		for (std::uint64_t seed = first; seed < first + seeds; ++seed) {
			// How many mutations an uninterrupted run makes.
			fl::filesystem disk; open_mode(disk, mode); fl::io io(disk); Workload work(seed); j::TurnRecovery recovered;
			{
				j::TurnLog log(io, identity, recovered, options);
				for (unsigned t = 0; t < 60; ++t) { auto b = work.turn(log); log.append(b); work.acknowledge(b); log.prepare(); }
				if (block == 512) { check(log.stats().segments_recycled > 0 && log.stats().forwarded_records > 0, "the run recycles segments and moves records forward (condition 10)"); }
			}
			for (std::uint64_t cut = 1; cut <= disk.mutations(); ++cut, ++points) {
				const std::uint64_t second = cut % 3 == 0 ? cut % 7 + 1 : 0;
				seconds += second != 0;
				run(seed, cut, second, 60, 5, mode, Loss::power);
				if (mode == Mode::plain) { run(seed, cut, second, 60, 5, mode, Loss::process); }
			}
		}
		if (mode == Mode::plain) {
			std::cout << "block " << block << ", plain: " << points << " crash points (" << seconds << " with a second loss in recovery), each a power loss and a process crash; "
				<< plain_loss.lost - loss_before.lost << " power losses lost an acknowledged record, " << plain_loss.damaged - loss_before.damaged << " reported damage, " << failures - before << " failures\n";
			continue;
		}
		std::uint64_t corrupted = 0, both = 0, header = 0, last = 0, sectors = 0, tears = 0;
		for (std::uint64_t seed = first; seed < first + seeds; ++seed) {
			corrupted_blocks(seed, corrupted);
			boundary_damage(seed, Damage::both, both); boundary_damage(seed, Damage::header, header); boundary_damage(seed, Damage::last, last);
			sector_damage(seed, sectors); torn_newest(seed, tears);
		}
		std::cout << "block " << block << ": " << points << " crash points (" << seconds << " with a second loss in recovery), " << corrupted << " corrupted blocks, " << both << " doubly damaged boundaries, "
			<< header << " damaged headers, " << last << " damaged last blocks, " << sectors << " damaged sectors, " << tears << " tears of the newest write, " << failures - before << " failures\n";
	}
	// Plain mode is not full mode: its power losses lose acknowledged records the sweep finds.
	if (mode == Mode::plain) { check(plain_loss.lost > 0, "a plain-mode power loss loses an acknowledged record at some crash point"); }
	std::cout << checks << " checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
