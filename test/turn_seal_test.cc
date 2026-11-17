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

// TurnDriver::seal (docs/mechanisms/turn-seal.md): sealed records under power loss, the
// sequence rule, the turn budget, forget, and the waiter that asks for a turn.
// `turn_seal_test [case]`, every case by default.
#include "journal/blocking.h"
#include "journal/memory_io.h"
#include "completion/posix.h"
#include <filesystem>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <thread>

namespace j = kronuz::journal;
namespace jt = kronuz::journal::testing;
namespace d = kronuz::journal::turn_log_detail;

namespace {
int checks = 0, failures = 0;
void check(bool value, const std::string& what) {
	++checks;
	if (!value) { ++failures; if (failures < 30) { std::cerr << "FAIL: " << what << '\n'; } }
}
const j::Identity identity{'S', 'E', 'A', 'L'};
const j::TurnLogOptions options{16 * 1024, 1024, 1, 4, 4096};
using Model = std::map<std::uint64_t, std::vector<j::TurnRecord>>;

j::TurnRecord record(std::uint64_t group, std::uint64_t sequence, std::size_t size, j::TurnRecordKind kind = j::TurnRecordKind::Batch) {
	std::string payload(size, '\0');
	for (std::size_t i = 0; i < size; ++i) { payload[i] = static_cast<char>('a' + (group * 7 + sequence * 3 + i) % 26); }
	return {group, sequence, kind, payload, sequence - 1};  // a checkpoint covers the records before it
}
void fold(Model& model, const std::vector<j::TurnRecord>& records) {
	for (const auto& r : records) { if (r.kind == j::TurnRecordKind::Checkpoint) { model[r.group].clear(); } model[r.group].push_back(r); }
}
// The refusal's message, or empty when `run` did not throw std::invalid_argument.
std::string refusal(const std::function<void()>& run) {
	try { run(); } catch (const std::invalid_argument& error) { return error.what(); } catch (...) { return "(another exception)"; }
	return {};
}
// A consumer's open records, one per group: appends join it, and its sealer takes it whole when
// the budget admits it. `next` is the group's last staged sequence + 1, taken at first append.
struct Consumer {
	std::map<std::uint64_t, std::optional<j::TurnRecord>> open;
	std::map<std::uint64_t, std::uint64_t> staged;
	std::vector<j::TurnRecord>* sealed = nullptr;  // where taken records go, if anywhere
	std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;  // (group, room)
	std::vector<bool> firsts;
	void append(std::uint64_t group, std::size_t size) {
		auto& o = open[group];
		if (!o) { o = record(group, staged[group] + 1, size); return; }
		o->payload += record(group, o->sequence, size).payload;
	}
	j::TurnDriver::Sealer sealer(std::uint64_t group) {
		return [this, group](std::uint64_t room, bool first) -> std::optional<j::TurnRecord> {
			calls.emplace_back(group, room); firsts.push_back(first);
			auto& o = open[group];
			if (!o || d::record_cost(o->payload.size()) > room) { return std::nullopt; }
			auto taken = std::move(*o); o.reset();
			staged[group] = taken.sequence;
			if (sealed) { sealed->push_back(taken); }
			return taken;
		};
	}
	// A record staged directly, as a checkpoint is: the open record's next one follows it.
	j::TurnRecord direct(std::uint64_t group, std::size_t size, j::TurnRecordKind kind) {
		auto r = record(group, ++staged[group], size, kind);
		return r;
	}
};

// A1. Power is cut at every I/O operation of a run whose turns mix staged and sealed records,
// committed inline or asked for by a waiter through the blocking entry; recovery holds every
// acknowledged record and the in-flight turn whole or not at all, so a sealed record is durable
// exactly when its turn's staged records are.
bool crash_run(std::uint64_t seed, std::uint64_t cut, bool& reached) {
	jt::Disk disk; disk.random.seed(seed * 7919 + cut); jt::MemoryIO io(disk);
	std::mt19937_64 random(seed);
	Model acknowledged; std::vector<j::TurnRecord> in_flight;
	Consumer consumer; consumer.sealed = &in_flight;
	reached = false;
	const auto at = "seed " + std::to_string(seed) + " cut " + std::to_string(cut);
	try {
		disk.cut = cut;
		j::TurnDriver driver(io, identity, options);
		for (std::uint64_t g = 1; g <= 3; ++g) { driver.seal(g, consumer.sealer(g)); }
		for (unsigned turn = 0; turn < 14; ++turn) {
			in_flight.clear();
			for (std::uint64_t g = 1; g <= 3; ++g) { if (random() % 3) { consumer.append(g, random() % 400 + 1); } }
			std::uint64_t waited = 0;
			for (std::uint64_t g = 1; g <= 3; ++g) { if (consumer.open[g]) { waited = g; } }
			if (turn % 3 != 2) {
				for (std::uint64_t g = 4; g <= 5; ++g) {
					if (random() % 2) { continue; }
					auto kind = random() % 4 ? j::TurnRecordKind::Batch : j::TurnRecordKind::Checkpoint;
					if (!consumer.staged[g]) { kind = j::TurnRecordKind::Batch; }
					auto r = consumer.direct(g, random() % 400 + 1, kind);
					driver.stage(r); in_flight.push_back(r);
				}
			}
			if (!driver.idle()) { driver.commit(); }
			else if (waited) { j::run(driver, driver.durable(waited, consumer.open[waited]->sequence)); }
			else { continue; }
			check(in_flight.empty() || std::all_of(consumer.open.begin(), consumer.open.end(), [](const auto& o) { return !o.second; }),
				"every open record seals in its turn under an unspent budget: " + at);
			fold(acknowledged, in_flight); in_flight.clear();
			driver.log().prepare();
		}
		reached = true;
	} catch (const jt::PowerLoss&) {
	} catch (const std::exception& error) {
		check(false, "run threw " + std::string(error.what()) + ": " + at);
	}
	disk.power_loss();
	try {
		j::TurnDriver after(io, identity, options);
		const auto& recovered = after.recovered();
		check(!recovered.damaged && recovered.lost.empty(), "power loss alone damages nothing: " + at);
		auto with = acknowledged; fold(with, in_flight);
		const bool none = recovered.groups == acknowledged, whole = recovered.groups == with;
		check(none || whole, "every acknowledged record survives and the in-flight turn, sealed records included, is whole or absent: " + at);
	} catch (const std::exception& error) {
		check(false, "recovery threw " + std::string(error.what()) + ": " + at);
	}
	return reached;
}
void crash() {
	unsigned runs = 0;
	for (std::uint64_t seed = 1; seed <= 12; ++seed) {
		bool reached = false;
		for (std::uint64_t cut = 1; ; ++cut) {
			crash_run(seed, cut, reached); ++runs;
			if (reached) { break; }  // the run finished before the cut: every point is covered
		}
	}
	std::cout << "crash: " << runs << " cuts\n";
}

// A2. A sealed record continues its group's last staged sequence: one behind, one ahead, one
// of another group, a group's start, and a record whose sequence a checkpoint staged since
// took are refused before any byte is written, and the turn fails with them.
void sequence() {
	auto refused = [](const j::TurnDriver::Sealer& fn, const std::string& what, bool with_start = true) {
		jt::Disk disk; jt::MemoryIO io(disk);
		j::TurnDriver driver(io, identity, options);
		if (with_start) { driver.stage(record(1, 1, 10, j::TurnRecordKind::Checkpoint)); driver.commit(); }
		const auto turns = driver.log().stats().turns;
		driver.seal(1, fn);
		driver.stage(record(2, 1, 10));
		const auto message = refusal([&] { driver.commit(); });
		check(message == "sealed record out of its group's sequence" || message == "a sealer returned another group's record", what + " is refused by the driver: " + message);
		check(driver.failed() && driver.log().stats().turns == turns, what + ": the turn fails unwritten");
	};
	refused([](std::uint64_t, bool) { return std::optional(record(1, 1, 10)); }, "a sealed record behind its group");
	refused([](std::uint64_t, bool) { return std::optional(record(1, 3, 10)); }, "a sealed record ahead of its group");
	refused([](std::uint64_t, bool) { return std::optional(record(3, 1, 10)); }, "another group's record");
	// The log alone admits a checkpoint as a group's first record at any sequence; a sealer
	// continues a group, it never starts one.
	{
		jt::Disk disk; jt::MemoryIO io(disk);
		j::TurnDriver driver(io, identity, options);
		driver.stage(record(9, 5, 10, j::TurnRecordKind::Checkpoint));
		check(refusal([&] { driver.commit(); }).empty() && driver.sequence(9) == 5, "the log admits a group's start at sequence 5");
	}
	refused([](std::uint64_t, bool) { return std::optional(record(1, 5, 10, j::TurnRecordKind::Checkpoint)); }, "a sealed checkpoint that would start its group", false);

	// The review's F1/P1 shape: 1 (checkpoint) and 2 are acknowledged; the open record took 3;
	// a checkpoint naming record 1 is then staged at 3. Admitting both would make that checkpoint
	// durable behind acknowledged record 2 and drop it; the driver refuses the sealed 3 instead,
	// the waiter on 3 fails, and recovery keeps 1 and 2.
	{
		jt::Disk disk; jt::MemoryIO io(disk);
		Consumer consumer;
		{
			j::TurnDriver driver(io, identity, options);
			driver.stage(consumer.direct(7, 8, j::TurnRecordKind::Checkpoint)); driver.commit();
			driver.seal(7, consumer.sealer(7));
			consumer.append(7, 20);
			j::run(driver, driver.durable(7, 2));
			check(driver.sequence(7) == 2, "record 2 is acknowledged");
			consumer.append(7, 30);
			check(consumer.open[7]->sequence == 3, "the open record took sequence 3");
			driver.stage(record(7, 3, 8, j::TurnRecordKind::Checkpoint));  // the racing checkpoint
			const auto message = refusal([&] { j::run(driver, driver.durable(7, 3)); });
			check(message == "sealed record out of its group's sequence", "the stale sealed record is refused by the driver: " + message);
			check(driver.failed() && driver.sequence(7) == 2, "nothing of the turn is durable");
			bool failed = false;
			try { j::run(driver, driver.durable(7, 3)); } catch (...) { failed = true; }
			check(failed, "the waiter on 3 is never acknowledged");
		}
		j::TurnDriver after(io, identity, options);
		const auto& records = after.records(7);
		check(records.size() == 2 && records[0].kind == j::TurnRecordKind::Checkpoint && records[1].sequence == 2 && records[1].payload == record(7, 2, 20).payload,
			"recovery keeps acknowledged record 2 and no checkpoint behind it");
	}
	// The right order: seal 3 first, then the checkpoint at 4, staged before the next open
	// record seals at 5 in the same turn; the checkpoint keeps its lower sequence.
	{
		jt::Disk disk; jt::MemoryIO io(disk);
		Consumer consumer;
		{
			j::TurnDriver driver(io, identity, options);
			driver.stage(consumer.direct(7, 8, j::TurnRecordKind::Checkpoint)); driver.commit();
			driver.seal(7, consumer.sealer(7));
			consumer.append(7, 20); j::run(driver, driver.durable(7, 2));
			consumer.append(7, 30); j::run(driver, driver.durable(7, 3));
			driver.stage(consumer.direct(7, 8, j::TurnRecordKind::Checkpoint));
			consumer.append(7, 40);
			driver.commit();
			check(!driver.failed() && driver.sequence(7) == 5, "the checkpoint at 4 and the sealed 5 share a turn");
		}
		j::TurnDriver after(io, identity, options);
		const auto& records = after.records(7);
		check(records.size() == 2 && records[0].sequence == 4 && records[0].kind == j::TurnRecordKind::Checkpoint && records[1].sequence == 5,
			"recovery holds the checkpoint and the record sealed after it");
	}
}

// A3. The budget: the first sealed record of a turn is offered what an empty segment has left
// (R3: a full-size record seals); later ones share segment_bytes / 2; a sealer not called, or
// declining a share, is called first next turn; no record is ever dropped.
void budget() {
	const auto whole = d::turn_room(options.segment_bytes, options.block_bytes);
	const auto half = d::turn_room(options.segment_bytes / 2, options.block_bytes);
	const auto cost = d::record_cost(0);
	jt::Disk disk; jt::MemoryIO io(disk);
	Consumer consumer;
	Model expected;
	{
		j::TurnDriver driver(io, identity, options);
		for (std::uint64_t g = 1; g <= 4; ++g) { driver.seal(g, consumer.sealer(g)); }
		// Group 1 takes the share whole: the others are not called this turn.
		consumer.append(1, half - 2 * cost + 1);
		for (std::uint64_t g = 2; g <= 4; ++g) { consumer.append(g, 2500); }
		std::vector<j::TurnRecord> sealed; consumer.sealed = &sealed;
		driver.commit();
		check(consumer.calls.size() == 1 && consumer.calls[0] == std::pair<std::uint64_t, std::uint64_t>{1, whole} && consumer.firsts[0], "the first sealer is offered the whole segment, as the turn's first");
		check(sealed.size() == 1 && driver.sequence(2) == 0, "a spent budget calls no other sealer");
		fold(expected, sealed); sealed.clear(); consumer.calls.clear();
		// Next turn starts with group 2: it takes the whole, 3 takes a share, 4's 2500 bytes do not fit.
		consumer.append(1, 10);
		driver.commit();
		check(consumer.calls.size() == 4 && consumer.calls[0].first == 2 && consumer.calls[0].second == whole, "the sealer left waiting is called first, with the whole segment");
		check(consumer.calls[1] == std::pair<std::uint64_t, std::uint64_t>{3, half - d::record_cost(2500)} && !consumer.firsts[1], "the next one gets the share's rest, not as the first");
		check(consumer.calls[2].first == 4 && consumer.calls[2].second < d::record_cost(2500) && consumer.open[4], "group 4 declines a share too small for its record");
		check(sealed.size() == 3, "groups 2, 3 and 1 seal");
		fold(expected, sealed); sealed.clear(); consumer.calls.clear();
		// Group 4 declined a share: it is called first next turn and seals.
		driver.commit();
		check(!consumer.calls.empty() && consumer.calls[0].first == 4 && consumer.calls[0].second == whole, "the sealer that declined a share is called first");
		check(sealed.size() == 1 && sealed[0].group == 4 && !consumer.open[4], "and its record seals");
		fold(expected, sealed); sealed.clear(); consumer.calls.clear();
		// Records past the budget break the contract: the driver refuses them.
		jt::Disk other_disk; jt::MemoryIO other_io(other_disk);
		j::TurnDriver other(other_io, identity, options);
		other.seal(1, [](std::uint64_t room, bool) { return std::optional(record(1, 1, room - d::record_cost(0) + 1)); });
		other.stage(record(2, 1, 1));
		check(refusal([&] { other.commit(); }) == "sealed record past its room" && other.failed(), "a record past its room is refused");
	}
	j::TurnDriver after(io, identity, options);
	check(after.recovered().groups == expected, "every sealed record is durable once and in order: none is dropped");
}

// R3. A record past segment_bytes / 2 seals as its turn's first record, behind another turn's.
void full_size() {
	const auto whole = d::turn_room(options.segment_bytes, options.block_bytes);
	const auto big = whole - d::record_cost(0);
	check(d::turn_bound(d::record_cost(big), options.block_bytes) <= options.segment_bytes, "the bound fits the largest record in an empty segment");
	check(d::record_cost(big) > options.segment_bytes / 2, "the largest record is past half a segment");
	jt::Disk disk; jt::MemoryIO io(disk);
	Consumer consumer; Model expected;
	{
		j::TurnDriver driver(io, identity, options);
		driver.seal(1, consumer.sealer(1)); driver.seal(2, consumer.sealer(2));
		std::vector<j::TurnRecord> sealed; consumer.sealed = &sealed;
		consumer.append(1, 100); consumer.append(2, big);
		driver.commit();
		check(sealed.size() == 1 && sealed[0].group == 1 && consumer.open[2], "behind another record, the full-size record waits");
		fold(expected, sealed); sealed.clear();
		driver.commit();
		check(sealed.size() == 1 && sealed[0].group == 2 && sealed[0].payload.size() == big && !driver.failed(), "first in its turn, it seals");
		fold(expected, sealed);
	}
	j::TurnDriver after(io, identity, options);
	check(after.recovered().groups == expected, "the full-size record is durable");
}

// The bound against the framing itself: random turns of many groups, small records and big
// ones, behind a first block whose manifest is as large as a header keeps.
void bound() {
	std::mt19937_64 random(42);
	unsigned turns = 0;
	for (std::uint32_t block : {256u, 512u, 1024u, 4096u, 32768u}) {
		for (int round = 0; round < 100; ++round) {
			j::framing::Manifest filling{true, 1, 0, {}};
			const auto room = (block / 2 - j::framing::header_bytes(0)) / 16;
			for (std::uint64_t g = 0; g < room; ++g) { filling.groups[g + 1] = 1; }
			j::framing::Emitter emitter(d::stream(identity, block, 2, 1), 0, filling);
			std::uint64_t cost = 0;
			const bool tiny = random() % 2;
			const auto records = tiny ? random() % 600 + 1 : random() % 30 + 1;
			for (std::uint64_t i = 0; i < records; ++i) {
				const auto group = random() % 3000 + 1;
				const std::size_t size = tiny ? random() % 8 : random() % (3 * block);
				emitter.record(d::logical(1, 1, group, 1, std::string(size, 'x')), group, 1);
				cost += d::record_cost(size);
			}
			std::string count; j::put32(count, static_cast<std::uint32_t>(records));
			emitter.record(d::logical(d::end_of_turn, 1, 0, 0, count));
			check(emitter.position() <= d::turn_bound(cost, block), "the bound holds: block " + std::to_string(block) + " records " + std::to_string(records));
			const auto room = d::turn_room(emitter.position(), block);
			check(!room || d::turn_bound(room, block) <= emitter.position(), "the room inverts the bound");
			++turns;
		}
	}
	std::cout << "bound: " << turns << " turns\n";
}

// A4. forget() unregisters the sealer while a turn is in flight, and from inside a sealer:
// no sealer of the group runs once it returns. A group whose record is in flight is refused.
void forget_in_flight() {
	jt::Disk disk; jt::MemoryIO io(disk);
	j::TurnDriver driver(io, identity, options);
	Consumer consumer;
	driver.seal(1, consumer.sealer(1)); driver.seal(2, consumer.sealer(2));
	driver.stage(record(3, 1, 4));
	consumer.append(2, 10);
	const auto& write = driver.begin();
	check(consumer.calls.size() == 2, "both sealers ran for the turn");
	driver.forget(1);
	check(!driver.sealed(1), "a group with nothing in flight is forgotten while the turn is in flight");
	bool refused = false;
	try { driver.forget(2); } catch (const std::logic_error&) { refused = true; }
	check(refused && driver.sealed(2), "a group with its sealed record in flight is refused and keeps its sealer");
	write.file->write_at(write.offset, write.bytes); write.file->sync_data(); driver.completed();
	consumer.calls.clear();
	consumer.append(1, 10); consumer.append(2, 10);
	driver.stage(record(3, 2, 4));
	driver.commit();
	check(consumer.calls.size() == 1 && consumer.calls[0].first == 2, "the forgotten sealer never runs again");
	driver.forget(2);
	consumer.calls.clear();
	driver.stage(record(3, 3, 4));
	driver.commit();
	check(consumer.calls.empty() && !driver.sealed(2), "once its record is durable the group is forgotten too");
}
void forget_from_sealer() {
	jt::Disk disk; jt::MemoryIO io(disk);
	j::TurnDriver driver(io, identity, options);
	std::vector<std::uint64_t> calls;
	auto alive = std::make_shared<bool>(true);
	struct Sentinel { std::shared_ptr<bool> alive; explicit Sentinel(std::shared_ptr<bool> flag) : alive(std::move(flag)) {} ~Sentinel() { *alive = false; } };
	bool alive_after = false, stage_refused = false;
	driver.seal(1, [&, sentinel = std::make_shared<Sentinel>(alive)](std::uint64_t, bool) -> std::optional<j::TurnRecord> {
		calls.push_back(1);
		driver.forget(2); driver.forget(1);
		alive_after = *alive && sentinel->alive;
		try { driver.stage(record(3, 2, 4)); } catch (const std::logic_error&) { stage_refused = true; }
		return std::nullopt;
	});
	driver.seal(2, [&](std::uint64_t, bool) -> std::optional<j::TurnRecord> { calls.push_back(2); return std::nullopt; });
	driver.seal(4, [&](std::uint64_t, bool) -> std::optional<j::TurnRecord> { calls.push_back(4); return std::nullopt; });
	driver.stage(record(3, 1, 4));
	driver.commit();
	check(calls == std::vector<std::uint64_t>{1, 4}, "a sealer forgotten by an earlier one does not run in the same turn");
	check(alive_after && !*alive, "a sealer that forgets its own group finishes its call, then is released");
	check(stage_refused && !driver.failed(), "a sealer cannot stage records");
	check(!driver.sealed(1) && !driver.sealed(2) && driver.sealed(4), "both groups are unregistered");
	// A sealer that forgets its group and still returns a record breaks the contract.
	jt::Disk other_disk; jt::MemoryIO other_io(other_disk);
	j::TurnDriver other(other_io, identity, options);
	other.seal(1, [&](std::uint64_t, bool) { other.forget(1); return std::optional(record(1, 1, 4)); });
	other.stage(record(3, 1, 4));
	bool refused = false;
	try { other.commit(); } catch (const std::logic_error&) { refused = true; }
	check(refused && other.failed(), "a record for a group its sealer forgot is refused");
}
// The consumer's shape under TSan: a writer thread appends to two open records under their
// mutex while the loop seals them; the loop forgets group 1 with a turn in flight, and no
// call of its sealer happens after forget returns.
void forget_threads() {
	jt::Disk disk; jt::MemoryIO io(disk);
	j::TurnDriver driver(io, identity, {256 * 1024, 4096, 1, 4, 64 * 1024});
	struct Open { std::mutex mutex; std::string payload; std::uint64_t staged = 0, sequence = 0; };
	Open opens[3];
	std::atomic<bool> stop{false}, forgotten{false}, quiet{false};
	std::atomic<int> late{0};
	for (std::uint64_t g : {1u, 2u}) {
		driver.seal(g, [&, g](std::uint64_t room, bool) -> std::optional<j::TurnRecord> {
			if (g == 1 && forgotten.load(std::memory_order_acquire)) { late.fetch_add(1); }
			auto& o = opens[g];
			std::lock_guard hold(o.mutex);
			if (o.payload.empty() || d::record_cost(o.payload.size()) > room) { return std::nullopt; }
			j::TurnRecord taken{g, o.sequence, j::TurnRecordKind::Batch, std::move(o.payload)};
			o.payload.clear(); o.staged = o.sequence;
			return taken;
		});
	}
	std::thread writer([&] {
		for (std::uint64_t n = 0; !stop.load(std::memory_order_acquire); ++n) {
			auto& o = opens[n % 2 + 1];
			std::lock_guard hold(o.mutex);
			if (n % 2 == 0 && quiet.load(std::memory_order_acquire)) { continue; }  // group 1 stops writing, still locking
			if (o.payload.empty()) { o.sequence = o.staged + 1; }  // the position, taken with the append
			if (o.payload.size() < 500) { o.payload += static_cast<char>('a' + n % 26); }
		}
	});
	std::uint64_t sequence = 0;
	bool done = false;
	for (int turn = 0; turn < 400 && !done; ++turn) {
		driver.stage(record(3, ++sequence, 4));
		const auto& write = driver.begin();
		if (turn == 200) { quiet.store(true, std::memory_order_release); }
		if (turn >= 200 && !forgotten.load()) {
			try { driver.forget(1); forgotten.store(true, std::memory_order_release); } catch (const std::logic_error&) {}
		}
		write.file->write_at(write.offset, write.bytes); write.file->sync_data(); driver.completed();
	}
	stop.store(true, std::memory_order_release);
	writer.join();
	check(forgotten.load(), "group 1 is forgotten while a turn is in flight");
	check(late.load() == 0, "no sealer of group 1 runs once forget returns (" + std::to_string(late.load()) + " late calls)");
	check(driver.sequence(2) > 0 && !driver.failed(), "group 2 keeps sealing");
}

// A waiter on a sealed group above its staged sequence asks for a turn: run() wakes and
// seals; a sealer with nothing under the whole budget goes quiet until a new waiter links,
// so run() sleeps instead of spinning, and the blocking entry calls that a lost wake.
struct Outcome { bool done = false; std::exception_ptr error; };
void trigger() {
	jt::Disk disk; jt::MemoryIO io(disk);
	j::TurnDriver driver(io, identity, options);
	Consumer consumer;
	driver.seal(1, consumer.sealer(1));
	task::inline_scheduler loop;
	auto ref = task::scheduler_ref::of(loop);
	j::HeldCompletion held;
	task::cancel_source stop;
	Outcome ran, first, second, third;
	auto wait = [&](Outcome& outcome, std::uint64_t group, std::uint64_t sequence) -> task::task<void> {
		try { co_await driver.durable(group, sequence); } catch (...) { outcome.error = std::current_exception(); }
		outcome.done = true;
	};
	auto run = [&]() -> task::task<void> {
		try { co_await driver.run(j::completion_ref::of(held), stop.token()); } catch (...) { ran.error = std::current_exception(); }
		ran.done = true;
	};
	auto drain = [&] { loop.run(); while (held.held()) { held.complete(held.tickets().front()); loop.run(); } };
	task::spawn(ref, run());
	loop.run();
	check(held.held() == 0 && consumer.calls.empty(), "run() sleeps with nothing owed");
	consumer.append(1, 12);
	task::spawn(ref, wait(first, 1, 1));
	drain();
	check(first.done && !first.error && driver.sequence(1) == 1 && consumer.calls.size() == 1, "a waiter on the open record makes run() seal it");
	// Nothing open: the sealer answers nothing once and run() goes back to sleep.
	consumer.calls.clear();
	task::spawn(ref, wait(second, 1, 2));
	drain();
	check(!second.done && consumer.calls.size() == 1 && driver.idle(), "a quiet sealer is asked once, then run() sleeps");
	consumer.append(1, 7);
	task::spawn(ref, wait(third, 1, 2));
	drain();
	check(second.done && third.done && driver.sequence(1) == 2, "a new waiter after the append asks again and both resume");
	// A sealer registered under a waiter that already waits for it.
	Outcome fourth;
	task::spawn(ref, wait(fourth, 2, 1));
	loop.run();
	check(!fourth.done && held.held() == 0, "a waiter on a group without a sealer asks for nothing");
	consumer.append(2, 5);
	driver.seal(2, consumer.sealer(2));
	drain();
	check(fourth.done && !fourth.error && driver.sequence(2) == 1, "registering the sealer answers the waiter");
	stop.request(); loop.run();
	check(ran.done && !ran.error, "run() ends");
	// The blocking entry: a waiter whose sealer has nothing to seal is a lost wake.
	bool stalled = false;
	try { j::run(driver, driver.durable(1, 5)); } catch (const j::stalled& error) { stalled = error.groups == std::vector<std::uint64_t>{1}; }
	check(stalled, "the entry throws stalled for a sealer with nothing to seal");
}

// A7. The document-gate design review's supersede probe (P1): 1 is durable through X, 2 is
// acknowledged after it, and a checkpoint at 3 covering only 1 would drop 2 at its barrier. The
// log refuses it, through append() and through a driver's turn, and recovery keeps 2.
void supersede() {
	const auto dir = std::filesystem::current_path() / ".scratch" / "turn-seal-supersede";
	std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);
	const j::TurnLogOptions small{8 * 512, 512, 1};
	{
		j::PosixIO io(dir); auto owner = io.acquire_owner(true);
		{
			j::TurnRecovery rec; j::TurnLog log(io, identity, rec, small);
			log.append(std::vector<j::TurnRecord>{{1, 1, j::TurnRecordKind::Batch, "tx@X (covered by the head)"}});
			log.append(std::vector<j::TurnRecord>{{1, 2, j::TurnRecordKind::Batch, "tx after X (acknowledged)"}});
			check(!refusal([&] { log.append(std::vector<j::TurnRecord>{{1, 2, j::TurnRecordKind::Checkpoint, "ckpt X=seq1", 1}}); }).empty(),
				"a checkpoint at X's sequence is refused");
			const auto message = refusal([&] { log.append(std::vector<j::TurnRecord>{{1, 3, j::TurnRecordKind::Checkpoint, "ckpt X=seq1", 1}}); });
			check(message == "a checkpoint behind a record it would drop", "a checkpoint at 3 covering only 1 is refused: " + message);
			check(log.sequence(1) == 2, "the refused checkpoint stages nothing");
		}
		j::TurnRecovery rec; j::TurnLog log(io, identity, rec, small);
		const auto& kept = rec.groups[1];
		check(kept.size() == 2 && kept[1].sequence == 2 && kept[1].payload == "tx after X (acknowledged)", "the acknowledged record above X survives recovery");
		// The checkpoint that covers 2 supersedes both, as before.
		log.append(std::vector<j::TurnRecord>{{1, 3, j::TurnRecordKind::Checkpoint, "ckpt X=seq2", 2}});
		check(log.sequence(1) == 3, "a checkpoint covering the record before it is admitted");
	}
	std::filesystem::remove_all(dir);
	// The driver's turns go through the same guard: a staged checkpoint behind its group's
	// acknowledged record fails the turn unwritten.
	jt::Disk disk; jt::MemoryIO io(disk);
	j::TurnDriver driver(io, identity, options);
	driver.stage(record(1, 1, 8)); driver.commit();
	driver.stage(record(1, 2, 8)); driver.commit();
	auto checkpoint = record(1, 3, 8, j::TurnRecordKind::Checkpoint); checkpoint.covers = 1;
	driver.stage(checkpoint);
	check(refusal([&] { driver.commit(); }) == "a checkpoint behind a record it would drop" && driver.failed() && driver.sequence(1) == 2,
		"a driver's turn with the checkpoint fails unwritten");
	// The guard is unconditional: a checkpoint that does not say what it covers covers nothing,
	// which only a group's first record may.
	jt::Disk other_disk; jt::MemoryIO other_io(other_disk);
	j::TurnDriver other(other_io, identity, options);
	other.stage({1, 1, j::TurnRecordKind::Checkpoint, "start"}); other.commit();
	check(!other.failed() && other.sequence(1) == 1, "a group's first checkpoint needs no covered sequence");
	other.stage(record(1, 2, 8)); other.commit();
	other.stage({1, 3, j::TurnRecordKind::Checkpoint, "unstated"});
	check(refusal([&] { other.commit(); }) == "a checkpoint behind a record it would drop" && other.failed(), "a later checkpoint that states no covered sequence is refused");
}

// A8. unseal() is keyed by its owner: a reopened index registers again, replacing the old
// sealer, and the old index's unseal leaves the new one in place.
void unseal_owner() {
	jt::Disk disk; jt::MemoryIO io(disk);
	j::TurnDriver driver(io, identity, options);
	driver.stage(record(1, 1, 4)); driver.commit();
	int old_calls = 0, new_calls = 0, old_owner = 0, new_owner = 0;
	check(driver.seal(1, [&](std::uint64_t, bool) -> std::optional<j::TurnRecord> { ++old_calls; return std::nullopt; }, &old_owner) == 1,
		"seal returns the group's last staged sequence");
	driver.stage(record(1, 2, 4));
	check(driver.seal(1, [&](std::uint64_t, bool) -> std::optional<j::TurnRecord> { ++new_calls; return std::nullopt; }, &new_owner) == 2,
		"a second registration replaces the first and counts what is pending");
	driver.unseal(1, &old_owner);
	driver.commit();
	check(old_calls == 0 && new_calls == 1 && driver.sealed(1), "the old owner's unseal leaves the new sealer");
	driver.unseal(1, &new_owner);
	driver.stage(record(1, 3, 4)); driver.commit();
	check(new_calls == 1 && !driver.sealed(1), "its own owner's unseal removes it");
}
// A8. The lock order is the turn, then the group's record mutex: a sealer runs under no lock
// of the driver's, so it may hold its record mutex and call wake(), which asks for one more turn.
void lock_order() {
	jt::Disk disk; jt::MemoryIO io(disk);
	j::TurnDriver driver(io, identity, options);
	std::mutex record_mutex;
	int calls = 0;
	driver.seal(1, [&](std::uint64_t, bool) -> std::optional<j::TurnRecord> {
		std::lock_guard hold(record_mutex);
		++calls;
		if (calls == 1) { driver.wake(); }  // would deadlock if the driver held its wake lock here
		return std::nullopt;
	});
	driver.wake();
	check(!driver.idle(), "a wake makes the driver busy");
	driver.commit();
	check(calls == 1 && !driver.idle(), "a wake from inside a sealer asks for the next turn");
	driver.commit();
	check(calls == 2 && driver.idle(), "and is answered by it");
}
// A8, under TSan. A writer thread opens records under its mutex and calls wake() holding it,
// the first transaction of each; run() on another thread's loop seals them with no waiter and
// no staged record. Every record becomes durable, and no lock-order inversion is reported.
void wake_threads() {
	jt::Disk disk; jt::MemoryIO io(disk);
	j::TurnDriver driver(io, identity, {256 * 1024, 4096, 1, 4, 64 * 1024});
	struct Open { std::mutex mutex; std::string payload; std::uint64_t staged = 0, sequence = 0, opened = 0; } open;
	driver.seal(1, [&](std::uint64_t room, bool) -> std::optional<j::TurnRecord> {
		std::lock_guard hold(open.mutex);
		if (open.payload.empty() || d::record_cost(open.payload.size()) > room) { return std::nullopt; }
		j::TurnRecord taken{1, open.sequence, j::TurnRecordKind::Batch, std::move(open.payload)};
		open.payload.clear(); open.staged = open.sequence;
		return taken;
	});
	task::cancel_source stop;
	std::atomic<bool> loop_done{false};
	std::exception_ptr loop_error;
	std::thread loop([&] {
		task::blocking_scheduler scheduler;
		j::InlineCompletion inline_io;
		auto runner = [&]() -> task::task<void> {
			try { co_await driver.run(j::completion_ref::of(inline_io), stop.token()); } catch (...) { loop_error = std::current_exception(); }
			loop_done.store(true, std::memory_order_release);
		};
		task::spawn(task::scheduler_ref::of(scheduler), runner());
		scheduler.drive([&] { return loop_done.load(std::memory_order_acquire); }, [] { return true; });
	});
	constexpr std::uint64_t records = 300;
	std::thread writer([&] {
		for (int n = 0;; ++n) {
			std::lock_guard hold(open.mutex);
			if (open.payload.empty()) {
				if (open.opened == records) { break; }
				open.sequence = open.staged + 1; ++open.opened;  // the position, taken with the first append
				open.payload += 'a';
				driver.wake();  // under the record mutex, as the consumer calls it
				continue;
			}
			if (open.payload.size() < 64) { open.payload += static_cast<char>('a' + n % 26); }
		}
	});
	writer.join();
	for (;;) {
		{ std::lock_guard hold(open.mutex); if (open.payload.empty() && open.staged == open.opened) { break; } }
		std::this_thread::yield();
	}
	stop.request();
	loop.join();
	check(!loop_error, "run() ends cleanly");
	check(driver.sequence(1) == open.opened && open.opened == records, "every woken record is durable (" + std::to_string(driver.sequence(1)) + " of " + std::to_string(open.opened) + ")");
}
// R9, under TSan. unseal() from another thread while a turn calls the sealer returns only once
// that call has finished; a record the call sealed is the group's; no call starts afterwards.
void unseal_threads() {
	jt::Disk disk; jt::MemoryIO io(disk);
	j::TurnDriver driver(io, identity, options);
	int owner = 0;
	std::atomic<bool> entered{false}, unsealing{false}, finished{false};
	std::atomic<int> calls{0};
	driver.seal(1, [&](std::uint64_t, bool) -> std::optional<j::TurnRecord> {
		if (calls.fetch_add(1) == 0) {
			entered.store(true, std::memory_order_release);
			while (!unsealing.load(std::memory_order_acquire)) { std::this_thread::yield(); }
			std::this_thread::sleep_for(std::chrono::milliseconds(20));  // unseal waits meanwhile
			finished.store(true, std::memory_order_release);
			return record(1, 1, 8);
		}
		return std::nullopt;
	}, &owner);
	bool returned_after = false;
	std::thread other([&] {
		while (!entered.load(std::memory_order_acquire)) { std::this_thread::yield(); }
		unsealing.store(true, std::memory_order_release);
		driver.unseal(1, &owner);
		returned_after = finished.load(std::memory_order_acquire);
	});
	std::uint64_t sequence = 0;
	driver.stage(record(2, ++sequence, 4));
	driver.commit();
	other.join();
	check(returned_after, "unseal returns only once the running call has finished");
	check(driver.sequence(1) == 1 && !driver.failed(), "the record sealed before unseal returned is durable");
	driver.stage(record(2, ++sequence, 4)); driver.commit();
	check(calls.load() == 1 && !driver.sealed(1), "the unsealed sealer is never called again");
	// Rounds of turns on this thread while another thread unseals at a random moment: no call
	// of a sealer starts once its unseal has returned.
	std::atomic<int> late{0};
	std::mt19937_64 random(7);
	for (int round = 0; round < 200; ++round) {
		int key = 0;
		std::atomic<bool> gone{false};
		driver.seal(1, [&](std::uint64_t, bool) -> std::optional<j::TurnRecord> {
			if (gone.load(std::memory_order_acquire)) { late.fetch_add(1); }
			std::this_thread::yield();
			return std::nullopt;
		}, &key);
		const auto delay = std::chrono::microseconds(random() % 200);
		std::thread unsealer([&] { std::this_thread::sleep_for(delay); driver.unseal(1, &key); gone.store(true, std::memory_order_release); });
		for (int turn = 0; turn < 20; ++turn) { driver.stage(record(2, ++sequence, 4)); driver.commit(); }
		unsealer.join();
	}
	check(late.load() == 0 && !driver.failed(), "no sealer call starts after its unseal returns (" + std::to_string(late.load()) + " late)");
}
}  // namespace

int main(int argc, char** argv) {
	const std::vector<std::pair<std::string, std::function<void()>>> cases{
		{"crash", crash}, {"sequence", sequence}, {"budget", budget}, {"full_size", full_size}, {"bound", bound},
		{"forget_in_flight", forget_in_flight}, {"forget_from_sealer", forget_from_sealer}, {"forget_threads", forget_threads}, {"trigger", trigger}, {"supersede", supersede},
{"unseal_owner", unseal_owner}, {"lock_order", lock_order}, {"wake_threads", wake_threads}, {"unseal_threads", unseal_threads},
	};
	const std::string only = argc > 1 ? argv[1] : "";
	for (const auto& [name, run] : cases) {
		if (!only.empty() && only != name) { continue; }
		const auto before = failures;
		try { run(); } catch (const std::exception& error) { check(false, name + " threw: " + error.what()); }
		std::cout << name << ": " << (failures == before ? "ok" : "FAILED") << '\n';
	}
	std::cout << checks << " checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
