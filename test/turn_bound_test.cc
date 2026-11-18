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

// A turn never exceeds a segment: a wave of records larger than one segment commits across
// several turns, in order; a record no segment holds fails only its group; power cut at every
// I/O operation of a wave keeps every acknowledged record and the in-flight turn whole or absent.
// `turn_bound_test [case]`, every case by default.
#include "journal/blocking.h"
#include "journal/memory_io.h"
#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace j = kronuz::journal;
namespace jt = kronuz::journal::testing;
namespace d = kronuz::journal::turn_log_detail;

namespace {
int checks = 0, failures = 0;
void check(bool value, const std::string& what) {
	++checks;
	if (!value) { ++failures; if (failures < 30) { std::cerr << "FAIL: " << what << '\n'; } }
}
// The footprint consensus's wave probe and strata's tests use: 16 KiB segments of 1 KiB blocks.
const j::Identity identity{'B', 'O', 'U', 'N', 'D'};
const j::TurnLogOptions options{16 * 1024, 1024, 1, 4, 4096};
const std::uint64_t room = d::turn_room(options.segment_bytes, options.block_bytes);
using Model = std::map<std::uint64_t, std::vector<j::TurnRecord>>;

j::TurnRecord record(std::uint64_t group, std::uint64_t sequence, std::size_t size, j::TurnRecordKind kind = j::TurnRecordKind::Batch) {
	std::string payload(size, '\0');
	for (std::size_t i = 0; i < size; ++i) { payload[i] = static_cast<char>('a' + (group * 7 + sequence * 3 + i) % 26); }
	return {group, sequence, kind, payload, sequence - 1};  // a checkpoint covers the records before it
}
void fold(Model& model, const j::TurnRecord& r) {
	if (r.kind == j::TurnRecordKind::Checkpoint) { model[r.group].clear(); }
	model[r.group].push_back(r);
}
std::string what(const std::exception_ptr& error) {
	if (!error) { return {}; }
	try { std::rethrow_exception(error); } catch (const std::exception& e) { return e.what(); } catch (...) { return "(not a std::exception)"; }
}
template <class E> bool is(const std::exception_ptr& error) {
	if (!error) { return false; }
	try { std::rethrow_exception(error); } catch (const E&) { return true; } catch (...) { return false; }
}
// Main's one-turn limit: TurnLog::append encodes one turn in a new log, as main's driver did with
// every pending record, and throws "turn exceeds a segment" past it.
bool main_encodes(std::size_t count, std::size_t payload) {
	jt::Disk disk; jt::MemoryIO io(disk);
	j::TurnDriver driver(io, identity, options);
	std::vector<j::TurnRecord> records;
	for (std::size_t i = 0; i < count; ++i) { records.push_back(record(i + 1, 1, payload, j::TurnRecordKind::Checkpoint)); }
	try { driver.log().append(records); return true; } catch (const std::length_error&) { return false; }
}
// The largest payload main takes as one record alone.
std::size_t main_single() {
	static const std::size_t largest = [] {
		std::size_t low = 0, high = options.segment_bytes;  // low encodes, high does not
		while (high - low > 1) { const auto mid = low + (high - low) / 2; (main_encodes(1, mid) ? low : high) = mid; }
		return low;
	}();
	return largest;
}
// One byte more than main takes: a record no segment holds.
std::size_t over() { return main_single() + 1; }
// The most records of `payload` bytes main takes in one turn.
std::size_t main_count(std::size_t payload) {
	std::size_t low = 0, high = options.segment_bytes / payload + 1;
	while (high - low > 1) { const auto mid = low + (high - low) / 2; (main_encodes(mid, payload) ? low : high) = mid; }
	return low;
}

// A staged record's operation: records whether the disk had power when its turn submitted it,
// and how it completed.
struct Op final : j::IOOperation {
	Op(j::IO& io, const jt::Disk& disk) : backend(io), power(disk) {}
	const j::MutationRequest& request() const override { return request_; }
	void submitted() override {
		if (busy || finished) { throw std::logic_error("submitted twice"); }
		busy = true; live = !power.dead();
	}
	bool complete(j::MutationCompletion completion) noexcept override {
		if (!busy) { return false; }
		busy = false; finished = true; error = completion.error;
		return true;
	}
	bool done() const noexcept override { return finished; }
	bool in_flight() const noexcept override { return busy; }
	j::IO& io() const noexcept override { return backend; }
	bool acknowledged() const noexcept { return finished && !error; }

	j::IO& backend;
	const jt::Disk& power;
	j::MutationRequest request_;
	std::exception_ptr error;
	bool busy = false, finished = false, live = false;
	unsigned turn = 0;  // the commit that acknowledged it, from 1
};

struct Staged { j::TurnRecord record; std::shared_ptr<Op> op; };

// A1. 64 groups each stage a create checkpoint and a first batch at once, plus a record of
// exactly a turn's room between two small ones: more than one segment holds. Every operation
// completes without error across several turns, each turn takes the oldest records left, and
// recovery holds every record in its group's order.
void wave() {
	for (std::uint64_t seed = 1; seed <= 16; ++seed) {
		const auto at = "seed " + std::to_string(seed);
		jt::Disk disk; jt::MemoryIO io(disk);
		std::mt19937_64 random(seed);
		Model model; std::vector<Staged> staged;
		std::uint64_t cost = 0;
		{
			j::TurnDriver driver(io, identity, options);
			auto stage = [&](j::TurnRecord r) {
				auto op = std::make_shared<Op>(io, disk);
				cost += d::record_cost(r.payload.size());
				fold(model, r); staged.push_back({r, op});
				driver.stage(std::move(r), op);
			};
			for (std::uint64_t g = 1; g <= 64; ++g) {
				stage(record(g, 1, seed == 1 ? 120 : random() % 300 + 1, j::TurnRecordKind::Checkpoint));
				stage(record(g, 2, seed == 1 ? 60 : random() % 300 + 1));
			}
			stage(record(65, 1, 8, j::TurnRecordKind::Checkpoint));
			stage(record(66, 1, room - d::record_cost(0), j::TurnRecordKind::Checkpoint));  // a full turn alone
			stage(record(65, 2, 8));
			check(cost > room, "the wave exceeds one turn's room: " + at);
			unsigned commits = 0;
			const auto turns = driver.log().stats().turns;
			std::string error;
			try {
				while (!driver.idle()) {
					driver.commit(); ++commits;
					for (auto& s : staged) { if (s.op->acknowledged() && !s.op->turn) { s.op->turn = commits; } }
				}
			} catch (const std::exception& e) { error = e.what(); }
			check(error.empty(), "the wave commits: " + error + ": " + at);
			check(!driver.failed() && commits >= 2 && driver.log().stats().turns - turns == commits, "the wave takes several turns and the driver stays healthy: " + at);
			unsigned last = 1, unacknowledged = 0;
			bool ordered = true;
			for (const auto& s : staged) {
				if (!s.op->acknowledged()) { ++unacknowledged; continue; }
				ordered = ordered && s.op->turn >= last; last = s.op->turn;
			}
			check(!unacknowledged, std::to_string(unacknowledged) + " operations unacknowledged: " + at);
			check(ordered, "each turn takes the oldest records left: " + at);
			for (std::uint64_t g = 1; g <= 64; ++g) { check(driver.sequence(g) == 2, "group " + std::to_string(g) + " is durable through 2: " + at); }
			check(driver.sequence(65) == 2 && driver.sequence(66) == 1, "the full-size record commits between two small ones: " + at);
		}
		j::TurnDriver after(io, identity, options);
		check(!after.recovered().damaged && after.recovered().groups == model, "recovery holds every record of the wave, in order: " + at);
	}
}

// A1 through run(): a wave staged at once while run() sleeps, every group waiting on its last
// record; run() writes turn after turn until every waiter resumes.
void wave_run() {
	jt::Disk disk; jt::MemoryIO io(disk);
	j::TurnDriver driver(io, identity, options);
	task::inline_scheduler loop; auto ref = task::scheduler_ref::of(loop);
	j::InlineCompletion inline_io; task::cancel_source stop;
	bool ended = false; std::exception_ptr ran;
	auto runner = [&]() -> task::task<void> {
		try { co_await driver.run(j::completion_ref::of(inline_io), stop.token()); } catch (...) { ran = std::current_exception(); }
		ended = true;
	};
	std::map<std::uint64_t, std::exception_ptr> errors; unsigned resumed = 0;
	auto waiter = [&](std::uint64_t group) -> task::task<void> {
		try { co_await driver.durable(group, 2); } catch (...) { errors[group] = std::current_exception(); }
		++resumed;
	};
	task::spawn(ref, runner()); loop.run();
	for (std::uint64_t g = 1; g <= 64; ++g) {
		driver.stage(record(g, 1, 120, j::TurnRecordKind::Checkpoint));
		driver.stage(record(g, 2, 60));
		task::spawn(ref, waiter(g));
	}
	loop.run();
	check(resumed == 64 && errors.empty(), std::to_string(resumed) + " of 64 waiters resumed, " + std::to_string(errors.size()) + " failed" + (errors.empty() ? "" : ": " + what(errors.begin()->second)));
	check(!driver.failed() && driver.log().stats().turns >= 2 && !ended, "run() writes the wave as several turns and keeps serving: " + what(ran));
	stop.request(); loop.run();
	check(ended && !ran, "run() ends cleanly: " + what(ran));
}

// A2. A record no segment holds fails its operation with std::length_error, and so do its
// group's later records and the durable() waits on them, whether linked before the refusal or
// after; its group's sealer is no longer called. The driver stays healthy: the records staged
// around it, and later ones, commit, and recovery holds them.
void oversize() {
	jt::Disk disk; jt::MemoryIO io(disk);
	Model model;
	{
		j::TurnDriver driver(io, identity, options);
		auto stage = [&](j::TurnRecord r, bool kept) {
			auto op = std::make_shared<Op>(io, disk);
			if (kept) { fold(model, r); }
			driver.stage(std::move(r), op);
			return op;
		};
		stage(record(1, 1, 10, j::TurnRecordKind::Checkpoint), true);
		stage(record(2, 1, 10, j::TurnRecordKind::Checkpoint), true);
		while (!driver.idle()) { driver.commit(); }
		unsigned sealer_calls = 0;
		driver.seal(2, [&](std::uint64_t, bool) -> std::optional<j::TurnRecord> { ++sealer_calls; return std::nullopt; });
		auto before = stage(record(1, 2, 10), true);
		auto big = stage(record(2, 2, over()), false);  // one byte past what a segment holds
		auto later = stage(record(2, 3, 10), false);
		auto other = stage(record(3, 1, 10, j::TurnRecordKind::Checkpoint), true);
		std::exception_ptr linked;
		auto wait = [&](std::uint64_t group, std::uint64_t sequence, std::exception_ptr& error) -> task::task<void> {
			try { co_await driver.durable(group, sequence); } catch (...) { error = std::current_exception(); }
		};
		const auto calls = sealer_calls;
		std::exception_ptr entry;
		try { j::run(driver, wait(2, 2, linked)); } catch (...) { entry = std::current_exception(); }
		check(!entry, "the blocking entry ends cleanly: " + what(entry));
		check(is<std::length_error>(linked) && what(linked) == "turn record exceeds a segment", "a waiter linked before the refusal fails with it: " + what(linked));
		check(!driver.failed(), "the driver stays healthy");
		check(is<std::length_error>(big->error) && big->done(), "the oversized record's operation fails: " + what(big->error));
		check(is<std::length_error>(later->error) && later->done(), "its group's later record fails with it: " + what(later->error));
		check(before->acknowledged() && other->acknowledged(), "the records staged around it commit: " + what(before->error) + what(other->error));
		check(driver.sequence(1) == 2 && driver.sequence(2) == 1 && driver.sequence(3) == 1, "the log holds the other records and its group's earlier ones");
		check(!driver.staged(2), "nothing of the refused group stays staged");
		std::exception_ptr ready;
		try { j::run(driver, wait(2, 3, ready)); } catch (...) { entry = std::current_exception(); }
		check(is<std::length_error>(ready), "a wait on a refused sequence fails at once: " + what(ready));
		std::exception_ptr fine;
		j::run(driver, wait(2, 1, fine));
		check(!fine, "a wait below the refused record resolves: " + what(fine));
		// Later records: the refused group's next one fails too; another group's commits.
		auto after = stage(record(2, 4, 10), false);
		auto next = stage(record(4, 1, 10, j::TurnRecordKind::Checkpoint), true);
		while (!driver.idle()) { driver.commit(); }
		check(is<std::length_error>(after->error) && next->acknowledged() && !driver.failed(), "later records: the refused group's fails, another group's commits");
		check(sealer_calls == calls, "the refused group's sealer is not called: " + std::to_string(sealer_calls - calls) + " calls");
		driver.unseal(2, nullptr);
		driver.forget(2);
		check(driver.retired(2) == 5, "forgetting the refused group retires its refused sequences and the open record past them: " + std::to_string(driver.retired(2)));
		model.erase(2);
	}
	j::TurnDriver recovered(io, identity, options);
	auto groups = recovered.recovered().groups; groups.erase(2);
	check(groups == model, "recovery holds every record the driver acknowledged");
}

// A1 and A2 at main's limit, in a new log as main_encodes measures it. One record of the largest
// payload main takes commits and one byte more is refused; the most records main takes in one
// turn commit in one, and one more record takes a second turn. Both limits lie above the room.
void bound() {
	struct Run { unsigned commits = 0; std::vector<unsigned> turn; std::vector<std::exception_ptr> errors; bool failed = false; };
	auto drive = [](const std::vector<j::TurnRecord>& records) {
		jt::Disk disk; jt::MemoryIO io(disk);
		j::TurnDriver driver(io, identity, options);
		std::vector<std::shared_ptr<Op>> ops;
		for (const auto& r : records) { ops.push_back(std::make_shared<Op>(io, disk)); driver.stage(r, ops.back()); }
		Run run;
		try {
			while (!driver.idle() && run.commits < 8) {
				driver.commit(); ++run.commits;
				for (auto& op : ops) { if (op->acknowledged() && !op->turn) { op->turn = run.commits; } }
			}
		} catch (...) { run.failed = true; }
		run.failed = run.failed || driver.failed();
		for (auto& op : ops) { run.turn.push_back(op->turn); run.errors.push_back(op->done() ? op->error : std::make_exception_ptr(std::logic_error("not done"))); }
		return run;
	};
	const auto single = main_single();
	check(d::record_cost(single) > room, "main takes a single record above the room: cost " + std::to_string(d::record_cost(single)) + " of " + std::to_string(room));
	auto at = drive({record(1, 1, single, j::TurnRecordKind::Checkpoint), record(2, 1, 10, j::TurnRecordKind::Checkpoint)});
	check(!at.failed && !at.errors[0] && !at.errors[1] && at.turn[0] == 1, "a record of main's largest payload commits in the first turn: " + what(at.errors[0]));
	auto past = drive({record(1, 1, single + 1, j::TurnRecordKind::Checkpoint), record(2, 1, 10, j::TurnRecordKind::Checkpoint)});
	check(!past.failed && is<std::length_error>(past.errors[0]) && !past.errors[1], "one byte more is refused alone: " + what(past.errors[0]) + what(past.errors[1]));
	for (const std::size_t payload : {10, 100, 1000}) {
		const std::uint64_t most = main_count(payload), cost = most * d::record_cost(payload);
		const auto load = "payload " + std::to_string(payload) + ", " + std::to_string(most) + " records";
		check(cost > room, "main takes more than the room in one turn: " + std::to_string(cost) + " of " + std::to_string(room) + ": " + load);
		std::vector<j::TurnRecord> records;
		for (std::size_t i = 0; i <= most; ++i) { records.push_back(record(i + 1, 1, payload, j::TurnRecordKind::Checkpoint)); }
		auto last = records.back(); records.pop_back();
		auto full = drive(records);
		check(!full.failed && full.commits == 1 && std::all_of(full.errors.begin(), full.errors.end(), [](const auto& e) { return !e; }), "main's most records commit in one turn: " + std::to_string(full.commits) + " turns: " + load);
		records.push_back(last);
		auto more = drive(records);
		const auto first = static_cast<std::size_t>(std::count(more.turn.begin(), more.turn.end(), 1u));
		check(!more.failed && more.commits == 2 && first == most && more.turn.back() == 2, "one record more takes a second turn after the first takes main's most: " + std::to_string(first) + " in the first of " + std::to_string(more.commits) + ": " + load);
	}
}

// A2, a sealed group. Its sealer owes a turn to a waiter on the open record (staged + 1) when
// its staged record is refused; after the refusal the sealer owes nothing, through a waiter
// linked below the hole or a new registration, so the driver goes idle and run() and the
// blocking entry sleep instead of spinning.
void owed() {
	jt::Disk disk; jt::MemoryIO io(disk);
	j::TurnDriver driver(io, identity, options);
	task::inline_scheduler loop; auto ref = task::scheduler_ref::of(loop);
	struct Outcome { bool done = false; std::exception_ptr error; };
	auto wait = [&](Outcome& o, std::uint64_t group, std::uint64_t sequence) -> task::task<void> {
		try { co_await driver.durable(group, sequence); } catch (...) { o.error = std::current_exception(); }
		o.done = true;
	};
	auto nothing = [](std::uint64_t, bool) -> std::optional<j::TurnRecord> { return std::nullopt; };
	auto settle = [&] { for (unsigned i = 0; i < 4 && !driver.idle(); ++i) { driver.commit(); loop.run(); } return driver.idle(); };
	driver.stage(record(1, 1, 10, j::TurnRecordKind::Checkpoint));
	driver.stage(record(2, 1, 10, j::TurnRecordKind::Checkpoint));
	while (!driver.idle()) { driver.commit(); }
	driver.seal(1, nothing); driver.seal(2, nothing);
	// Group 1, through commit(): the open-record waiter fails with the refusal.
	driver.stage(record(1, 2, over()));
	Outcome open1; task::spawn(ref, wait(open1, 1, 3)); loop.run();
	driver.commit(); loop.run();
	check(open1.done && is<std::length_error>(open1.error), "the open-record waiter fails with the refusal: " + what(open1.error));
	check(settle(), "after the refusal the driver goes idle");
	// Group 2, by hand: its record 2 is in the turn the refusal of 3 leaves, and a waiter on 2
	// links while that turn is in flight.
	driver.stage(record(2, 2, 10));
	driver.stage(record(2, 3, over()));
	Outcome open2, below; task::spawn(ref, wait(open2, 2, 4)); loop.run();
	const auto& write = driver.begin(); loop.run();
	task::spawn(ref, wait(below, 2, 2)); loop.run();
	check(!below.done, "a waiter below the hole links while its record is in flight");
	write.file->write_at(write.offset, write.bytes); write.file->sync_data(); driver.completed(); loop.run();
	check(open2.done && is<std::length_error>(open2.error) && below.done && !below.error, "the waiter on the refused group's in-flight record resolves: " + what(below.error));
	check(settle(), "a waiter linked below the hole leaves the driver idle");
	driver.seal(2, nothing);
	check(settle(), "registering a refused group's sealer again leaves the driver idle");
	if (!driver.idle()) { return; }  // run() and the blocking entry would spin
	Outcome entry; std::exception_ptr ran;
	try { j::run(driver, wait(entry, 2, 1)); } catch (...) { ran = std::current_exception(); }
	check(!ran && entry.done && !entry.error, "the blocking entry returns: " + what(ran) + what(entry.error));
	j::InlineCompletion inline_io; task::cancel_source stop; bool ended = false;
	auto runner = [&]() -> task::task<void> {
		try { co_await driver.run(j::completion_ref::of(inline_io), stop.token()); } catch (...) { ran = std::current_exception(); }
		ended = true;
	};
	task::spawn(ref, runner()); loop.run();
	check(!ended && !driver.in_flight(), "run() sleeps with nothing to write");
	stop.request(); loop.run();
	check(ended && !ran, "run() ends cleanly: " + what(ran));
}

// A2, closing a refused group. forget() retires through the record past the refused ones, the
// open record a writer may hold, as it does past a plain group's last record: a wait on it fails
// group_closed, and a reopened incarnation's first record never acknowledges it.
void forget() {
	jt::Disk disk; jt::MemoryIO io(disk);
	j::TurnDriver driver(io, identity, options);
	driver.stage(record(1, 1, 10, j::TurnRecordKind::Checkpoint));
	driver.stage(record(3, 1, 10, j::TurnRecordKind::Checkpoint));
	while (!driver.idle()) { driver.commit(); }
	driver.stage(record(1, 2, over()));
	while (!driver.idle()) { driver.commit(); }
	driver.forget(1); driver.forget(3);
	check(driver.retired(1) == 3 && driver.retired(3) == 2, "retired: the refused group through 3, the plain one through 2: " + std::to_string(driver.retired(1)) + ", " + std::to_string(driver.retired(3)));
	task::inline_scheduler loop; auto ref = task::scheduler_ref::of(loop);
	std::exception_ptr refused, plain; bool refused_done = false, plain_done = false;
	auto wait = [&](std::uint64_t group, std::uint64_t sequence, std::exception_ptr& error, bool& done) -> task::task<void> {
		try { co_await driver.durable(group, sequence); } catch (...) { error = std::current_exception(); }
		done = true;
	};
	task::spawn(ref, wait(1, 3, refused, refused_done));  // the old writer's open record
	task::spawn(ref, wait(3, 2, plain, plain_done));
	loop.run();
	check(plain_done && is<j::group_closed>(plain), "a plain group's old open-record wait fails group_closed: " + what(plain));
	check(refused_done && is<j::group_closed>(refused), "a refused group's old open-record wait fails group_closed: " + what(refused));
	// The reopened incarnation follows retired().
	driver.stage(record(1, driver.retired(1) + 1, 10, j::TurnRecordKind::Checkpoint));
	while (!driver.idle()) { driver.commit(); }
	loop.run();
	check(refused_done && refused, "the new incarnation's first record does not acknowledge the old wait");
	check(driver.sequence(1) == 4 && !driver.failed(), "the reopened group commits past the retired sequences");
}

// A3. Power is cut at every I/O operation of a run of two waves, each larger than a segment, so
// cuts fall inside a turn and between two turns of one wave. Recovery keeps every acknowledged
// record, and the turn in flight at the cut whole or not at all; no turn ever exceeds a segment.
bool crash_run(std::uint64_t seed, std::uint64_t cut, bool& reached, unsigned& between) {
	jt::Disk disk; disk.random.seed(seed * 7919 + cut); jt::MemoryIO io(disk);
	std::mt19937_64 random(seed);
	Model acknowledged; std::vector<Staged> staged;
	std::size_t folded = 0;  // staged[0, folded) are acknowledged, in turn order
	reached = false;
	bool split = false;  // a wave has acknowledged turns and records still to write
	const auto at = "seed " + std::to_string(seed) + " cut " + std::to_string(cut);
	try {
		disk.cut = cut;
		j::TurnDriver driver(io, identity, options);
		for (unsigned round = 0; round < 2; ++round) {
			const auto first = staged.size();
			for (std::uint64_t g = 1; g <= 64; ++g) {
				const std::uint64_t base = round * 2;
				const auto kind = round == 0 || random() % 4 == 0 ? j::TurnRecordKind::Checkpoint : j::TurnRecordKind::Batch;
				for (auto r : {record(g, base + 1, random() % 200 + 1, kind), record(g, base + 2, random() % 200 + 1)}) {
					auto op = std::make_shared<Op>(io, disk);
					staged.push_back({r, op});
					driver.stage(std::move(r), op);
				}
			}
			while (!driver.idle()) {
				driver.commit();
				for (; folded < staged.size() && staged[folded].op->acknowledged(); ++folded) { fold(acknowledged, staged[folded].record); }
				split = folded > first && folded < staged.size();
			}
			check(folded == staged.size(), "every record of the wave is acknowledged: " + at);
			split = false;
			driver.log().prepare();
		}
		reached = true;
	} catch (const jt::PowerLoss&) {
	} catch (const std::exception& error) {
		check(false, "run threw " + std::string(error.what()) + ": " + at);
	}
	if (split) { ++between; }
	std::vector<j::TurnRecord> in_flight;
	for (std::size_t i = folded; i < staged.size(); ++i) {
		if (staged[i].op->live && !staged[i].op->acknowledged()) { in_flight.push_back(staged[i].record); }
	}
	disk.power_loss();
	try {
		j::TurnDriver after(io, identity, options);
		const auto& recovered = after.recovered();
		check(!recovered.damaged && recovered.lost.empty(), "power loss alone damages nothing: " + at);
		auto with = acknowledged;
		for (const auto& r : in_flight) { fold(with, r); }
		check(recovered.groups == acknowledged || recovered.groups == with, "every acknowledged record survives and the in-flight turn is whole or absent: " + at);
	} catch (const std::exception& error) {
		check(false, "recovery threw " + std::string(error.what()) + ": " + at);
	}
	return reached;
}
void crash() {
	unsigned runs = 0, between = 0;
	for (std::uint64_t seed = 1; seed <= 6; ++seed) {
		bool reached = false;
		for (std::uint64_t cut = 1; ; ++cut) {
			crash_run(seed, cut, reached, between); ++runs;
			if (reached) { break; }  // the run finished before the cut: every point is covered
		}
	}
	check(between > 0, "some cuts fall between two turns of one wave");
	std::cout << "crash: " << runs << " cuts, " << between << " between turns of a wave\n";
}
}  // namespace

int main(int argc, char** argv) {
	const std::vector<std::pair<std::string, std::function<void()>>> cases{
		{"wave", wave}, {"wave_run", wave_run}, {"bound", bound}, {"oversize", oversize}, {"owed", owed}, {"forget", forget}, {"crash", crash},
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
