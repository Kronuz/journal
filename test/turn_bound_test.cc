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
			check(!driver.failed() && commits >= 3 && driver.log().stats().turns - turns == commits, "the wave takes several turns and the driver stays healthy: " + at);
			unsigned last = 1, unacknowledged = 0;
			bool ordered = true;
			std::map<unsigned, std::uint64_t> per_turn;
			for (const auto& s : staged) {
				if (!s.op->acknowledged()) { ++unacknowledged; continue; }
				ordered = ordered && s.op->turn >= last; last = s.op->turn;
				per_turn[s.op->turn] += d::record_cost(s.record.payload.size());
			}
			check(!unacknowledged, std::to_string(unacknowledged) + " operations unacknowledged: " + at);
			check(ordered, "each turn takes the oldest records left: " + at);
			for (const auto& [turn, used] : per_turn) { check(used <= room, "turn " + std::to_string(turn) + " carries " + std::to_string(used) + " of " + std::to_string(room) + ": " + at); }
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
		auto big = stage(record(2, 2, room - d::record_cost(0) + 1), false);  // one byte past a turn's room
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
		check(driver.retired(2) >= 4, "forgetting the refused group retires its refused sequences: " + std::to_string(driver.retired(2)));
		model.erase(2);
	}
	j::TurnDriver recovered(io, identity, options);
	auto groups = recovered.recovered().groups; groups.erase(2);
	check(groups == model, "recovery holds every record the driver acknowledged");
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
		{"wave", wave}, {"wave_run", wave_run}, {"oversize", oversize}, {"crash", crash},
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
