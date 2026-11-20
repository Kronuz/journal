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

// The coroutine drivers: TurnDriver::run and Store's tasks over HeldCompletion on an
// inline_scheduler, and the blocking entry's rules over blocking_scheduler (async-drivers D5):
// `driver_test [case]`, every case by default.
#include "journal/blocking.h"
#include "faultlab/faultfs.h"
#include "journal/store.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>

namespace j = kronuz::journal;
namespace fl = kronuz::faultlab;

namespace {
int checks = 0, failures = 0;
void check(bool value, const std::string& what) {
	++checks;
	if (!value) { ++failures; std::cerr << "FAIL: " << what << '\n'; }
}
const j::Identity log_identity{'D', 'R', 'I', 'V', 'E', 'R'};
const j::TurnLogOptions options{16 * 1024, 1024, 1, 4, 4096};
const j::AdmissionLimits limits{{64u << 20, 4096}, {1u << 20, 8}, {8u << 20, 8}, 64, 3};
j::Identity identity(std::uint64_t group) { j::Identity id{'G'}; id[1] = static_cast<char>(group); return id; }

// One core: a turn log and shared Stores on one in-memory disk.
struct Core {
	fl::filesystem disk;
	std::unique_ptr<j::Host> host;
	j::TurnDriver* turns = nullptr;
	std::map<std::uint64_t, std::unique_ptr<j::Store>> stores;
	Core() { disk.make_directory("host"); open(); }
	void open() {
		stores.clear(); host.reset();
		host = std::make_unique<j::Host>(std::make_shared<fl::io>(disk, "host"), log_identity, j::HostOptions{1, options});
		turns = &host->driver(0);
	}
	j::Store& store(std::uint64_t group) {
		auto& store = stores[group];
		if (!store) {
			store = std::make_unique<j::Store>(*host, group, limits);
			if (host->status(group) == j::GroupStatus::stored) { store->recover([](std::uint64_t, std::string_view) {}); }
			else { store->create(identity(group)); }
			while (!turns->idle()) { turns->commit(); }  // the group's first checkpoint record: sequence 1
			while (!store->inventory_step(64).complete) {}
		}
		return *store;
	}
	j::AppendReservation reserve(std::uint64_t group, std::size_t bytes) { return *store(group).reserve_append(j::AdmissionClass::Normal, bytes); }
};

template <class T> struct Outcome {
	std::optional<T> value;
	std::exception_ptr error;
	bool done = false;
};
template <class T, class F> task::task<void> capture(Outcome<T>& outcome, F make) {
	try { outcome.value.emplace(co_await make()); } catch (...) { outcome.error = std::current_exception(); }
	outcome.done = true;
}
template <class E> bool throws(const std::exception_ptr& error) {
	if (!error) { return false; }
	try { std::rethrow_exception(error); } catch (const E&) { return true; } catch (...) { return false; }
}

// run() writes a turn as Write then SyncData on the Turn lane, and an append completes only
// once both are delivered; stop ends it once nothing is staged.
void runner() {
	Core core;
	auto& store = core.store(1);
	auto& other = core.store(10);  // a group appends one batch at a time
	task::inline_scheduler loop;
	auto ref = task::scheduler_ref::of(loop);
	j::HeldCompletion held;
	task::cancel_source stop;
	Outcome<int> ran;
	task::spawn(ref, capture(ran, [&]() -> task::task<int> { co_await core.turns->run(j::completion_ref::of(held), stop.token()); co_return 0; }));
	Outcome<j::Frontier> first, second;
	task::spawn(ref, capture(first, [&] { return store.append(core.reserve(1, 3), "one", j::completion_ref::of(held)); }));
	loop.run();
	check(held.held() == 1, "the runner issues the turn's write");
	auto ticket = held.tickets().front();
	check(held.request(ticket).kind == j::PrimitiveKind::Write, "a turn starts with its write");
	check(!first.done, "an append waits for its turn's barrier");
	// A record staged while the turn is in flight waits for the next turn.
	task::spawn(ref, capture(second, [&] { return other.append(core.reserve(10, 3), "two", j::completion_ref::of(held)); }));
	loop.run();
	held.complete(ticket); loop.run();
	check(held.held() == 1 && held.request(held.tickets().front()).kind == j::PrimitiveKind::SyncData, "the write is followed by its data barrier");
	check(!first.done, "a written record is not acknowledged before its barrier");
	held.complete(held.tickets().front()); loop.run();
	check(first.done && first.value && first.value->sequence == 1, "the append completes at its barrier with its frontier");
	check(core.turns->sequence(1) == 2 && core.turns->sequence(10) == 1 && !second.done, "the next turn holds the record staged in flight");
	for (int i = 0; i < 2 && held.held(); ++i) { held.complete(held.tickets().front()); loop.run(); }
	check(second.done && second.value && second.value->sequence == 1 && core.turns->sequence(10) == 2, "the second append completes in the next turn");
	check(!ran.done, "the runner waits for work");
	stop.request(); loop.run();
	check(ran.done && !ran.error, "stop ends the runner once nothing is staged");
	check(core.turns->holder() == j::TurnDriver::Holder::none, "the runner releases the driver");
}

// A caller cancelled while its append waits is released at once; the record is still written.
void cancel_waiting() {
	Core core;
	auto& store = core.store(2);
	task::inline_scheduler loop;
	auto ref = task::scheduler_ref::of(loop);
	j::HeldCompletion held;
	task::cancel_source stop, caller;
	Outcome<int> ran;
	task::spawn(ref, capture(ran, [&]() -> task::task<int> { co_await core.turns->run(j::completion_ref::of(held), stop.token()); co_return 0; }));
	Outcome<j::Frontier> append;
	task::spawn(ref, capture(append, [&] { return store.append(core.reserve(2, 5), "kept", j::completion_ref::of(held), caller.token()); }));
	loop.run();
	caller.request(); loop.run();
	check(append.done && throws<task::cancelled>(append.error), "the cancelled caller completes with task::cancelled");
	check(core.turns->sequence(2) == 1, "its record is not yet durable");
	while (held.held()) { held.complete(held.tickets().front()); loop.run(); }
	check(core.turns->sequence(2) == 2 && store.frontier().sequence == 1, "the cancelled append's record is written with its turn");
	stop.request(); loop.run();
	check(ran.done && !ran.error, "the runner ends");
	// Reopened, the record is there.
	core.open();
	check(core.turns->sequence(2) == 2 && core.turns->records(2).size() == 2 && core.turns->records(2).back().payload == "kept", "recovery keeps the cancelled append's record");
	// A token requested before the call starts nothing.
	Outcome<j::Frontier> refused;
	task::cancel_source early; early.request();
	auto& again = core.store(2);
	task::spawn(ref, capture(refused, [&] { return again.append(core.reserve(2, 1), "x", j::completion_ref::of(held), early.token()); }));
	loop.run();
	check(refused.done && throws<task::cancelled>(refused.error) && core.turns->idle(), "a caller cancelled before the call stages nothing");
}

// A cancel that claims a waiter after its barrier completed, before the cancel's hop runs: the
// barrier unlinks it but loses the claim, so only the hop resumes the caller, once.
void cancel_races_barrier() {
	Core core;
	auto& store = core.store(11);
	task::inline_scheduler loop;
	auto ref = task::scheduler_ref::of(loop);
	j::HeldCompletion held;
	task::cancel_source stop, caller;
	Outcome<int> ran;
	task::spawn(ref, capture(ran, [&]() -> task::task<int> { co_await core.turns->run(j::completion_ref::of(held), stop.token()); co_return 0; }));
	int resumes = 0;
	Outcome<j::Frontier> append;
	task::spawn(ref, capture(append, [&]() -> task::task<j::Frontier> {
		auto result = store.append(core.reserve(11, 4), "race", j::completion_ref::of(held), caller.token());
		struct Count { int& n; ~Count() { ++n; } } count{resumes};
		co_return co_await std::move(result);
	}));
	loop.run();
	held.complete(held.tickets().front()); loop.run();  // the write; the barrier is held
	held.complete(held.tickets().front());  // the barrier's completion queues the runner
	caller.request();  // the claim: its hop queues behind the runner
	loop.run();
	check(append.done && throws<task::cancelled>(append.error) && resumes == 1, "the cancel's claim wins and the caller resumes once");
	check(core.turns->sequence(11) == 2 && store.frontier().sequence == 1, "the record is durable all the same");
	stop.request(); loop.run();
	check(ran.done && !ran.error, "the runner ends");
}

// A failed write fails the turn: the append and the runner throw it, and the Store fences.
void failed_turn() {
	Core core;
	auto& store = core.store(3);
	task::inline_scheduler loop;
	auto ref = task::scheduler_ref::of(loop);
	j::HeldCompletion held;
	Outcome<int> ran;
	task::spawn(ref, capture(ran, [&]() -> task::task<int> { co_await core.turns->run(j::completion_ref::of(held)); co_return 0; }));
	Outcome<j::Frontier> append;
	task::spawn(ref, capture(append, [&] { return store.append(core.reserve(3, 4), "lost", j::completion_ref::of(held)); }));
	loop.run();
	held.fail(held.tickets().front(), std::make_exception_ptr(std::runtime_error("injected write failure"))); loop.run();
	check(append.done && throws<std::runtime_error>(append.error), "the append fails with its turn");
	check(ran.done && throws<std::runtime_error>(ran.error), "the runner throws the turn's failure");
	check(core.turns->failed() && store.fenced(), "the driver fails and the Store fences");
}

// The blocking entry: an append is written with no clock and no thread.
void entry_append() {
	Core core;
	auto& store = core.store(4);
	auto frontier = j::run(*core.turns, store.append(core.reserve(4, 6), "inline", {}));
	check(frontier.sequence == 1 && core.turns->sequence(4) == 2, "the entry returns the append durable");
	check(core.turns->idle() && core.turns->holder() == j::TurnDriver::Holder::none, "nothing is left staged and the entry releases the driver");
}

// A lost wake throws `stalled` naming the waiting group, without waiting.
void entry_lost_wake() {
	Core core;
	core.store(7);
	const auto started = std::chrono::steady_clock::now();
	try {
		j::run(*core.turns, core.turns->durable(7, 3));
		check(false, "a lost wake throws");
	} catch (const j::stalled& error) {
		check(error.groups == std::vector<std::uint64_t>{7}, "stalled names the waiting group");
		check(std::string(error.what()).find('7') != std::string::npos, "what() names it too");
	}
	check(std::chrono::steady_clock::now() - started < std::chrono::seconds(5), "the entry does not wait on a lost wake");
	check(core.turns->holder() == j::TurnDriver::Holder::none, "a stalled entry releases the driver");
}

// A live token (its source in the caller's frame) is no reason to wait: still a lost wake.
void entry_live_token() {
	Core core;
	core.store(8);
	task::cancel_source live;
	try {
		j::run(*core.turns, core.turns->durable(8, 2, live.token()));
		check(false, "a lost wake under a live token throws");
	} catch (const j::stalled& error) {
		check(error.groups == std::vector<std::uint64_t>{8}, "stalled names the group under a live token");
	}
}

// An append staged and then cancelled returns task::cancelled, and only after its record is
// written: the entry commits what is staged before it returns.
void entry_staged_cancelled() {
	Core core;
	auto& store = core.store(5);
	task::blocking_scheduler scheduler;
	task::cancel_source source;
	bool cancelled = false, finished = false;
	auto append = [&]() -> task::task<void> {
		try { co_await store.append(core.reserve(5, 6), "staged", {}, source.token()); } catch (const task::cancelled&) { cancelled = true; }
		finished = true;
	};
	auto root = [&]() -> task::task<void> {
		task::spawn(task::current(), append());
		co_await task::yield();  // the append stages its record and waits
		check(!core.turns->idle(), "the record is staged before the cancel");
		source.request();
		while (!finished) { co_await task::yield(); }
		check(core.turns->sequence(5) == 1, "the caller is released before its record is written");
		if (cancelled) { throw task::cancelled{}; }
	};
	try {
		j::run(*core.turns, scheduler, root());
		check(false, "the cancelled append returns task::cancelled");
	} catch (const task::cancelled&) {}
	check(cancelled, "the append saw the cancel");
	check(core.turns->idle() && core.turns->sequence(5) == 2 && store.frontier().sequence == 1, "its record is written before the entry returns");
}

// The holder tag: the entry refuses a driver run() holds, and run() one the entry holds.
void entry_holder() {
	Core core;
	core.store(6);
	task::inline_scheduler loop;
	j::HeldCompletion held;
	task::cancel_source stop;
	Outcome<int> ran;
	task::spawn(task::scheduler_ref::of(loop), capture(ran, [&]() -> task::task<int> { co_await core.turns->run(j::completion_ref::of(held), stop.token()); co_return 0; }));
	loop.run();
	check(core.turns->holder() == j::TurnDriver::Holder::driver, "run() holds the driver");
	bool refused = false;
	try { j::run(*core.turns, core.turns->durable(6, 2)); } catch (const std::logic_error& error) { refused = !dynamic_cast<const j::stalled*>(&error); }
	check(refused, "the entry refuses a driver run() holds");
	refused = false;
	try { core.turns->commit(); } catch (const std::logic_error&) { refused = true; }
	check(refused, "a manual turn is refused while run() holds the driver");
	stop.request(); loop.run();
	check(ran.done && !ran.error, "the runner ends");
	Outcome<int> nested;
	auto inner = [&]() -> task::task<void> {
		task::inline_scheduler other;
		task::spawn(task::scheduler_ref::of(other), capture(nested, [&]() -> task::task<int> { co_await core.turns->run(j::completion_ref::of(held)); co_return 0; }));
		other.run();
		co_return;
	};
	j::run(*core.turns, inner());
	check(nested.done && nested.error && !throws<j::stalled>(nested.error), "run() refuses a driver the entry holds");
}

// A cancel requested on another thread wakes an entry asleep toward a 30 s deadline, which
// returns task::cancelled at once, round after round (run under TSan).
void entry_threads() {
	constexpr int rounds = 100;
	Core core;
	core.store(9);
	int cancelled = 0;
	auto slowest = std::chrono::steady_clock::duration::zero();
	for (int round = 0; round < rounds; ++round) {
		task::blocking_scheduler scheduler;
		task::cancel_source outer;
		auto work = [&]() -> task::task<void> {
			task::deadline<task::blocking_scheduler> limit(scheduler, std::chrono::steady_clock::now() + std::chrono::seconds(30), outer.token());
			co_await core.turns->durable(9, 2, limit.token());
		};
		const auto started = std::chrono::steady_clock::now();
		std::thread other([&] { std::this_thread::sleep_for(std::chrono::milliseconds(2)); outer.request(); });
		try { j::run(*core.turns, scheduler, work()); } catch (const task::cancelled&) { ++cancelled; } catch (...) {}
		other.join();
		slowest = std::max(slowest, std::chrono::steady_clock::now() - started);
	}
	check(cancelled == rounds, "every cross-thread cancel returns task::cancelled (" + std::to_string(cancelled) + " of " + std::to_string(rounds) + ")");
	check(slowest < std::chrono::seconds(5), "no round waits for its deadline");
	std::cout << "entry_threads: slowest round " << std::chrono::duration_cast<std::chrono::microseconds>(slowest).count() << " us\n";
}
// A cancel from another thread between drives only claims the waiter and queues its hop: once
// that thread is joined, the waiter is still linked, and only the loop's next drive unlinks it.
void cancel_hops() {
	Core core;
	core.store(14);
	task::inline_scheduler loop;
	task::cancel_source caller;
	Outcome<int> wait;
	task::spawn(task::scheduler_ref::of(loop), capture(wait, [&]() -> task::task<int> { co_await core.turns->durable(14, 2, caller.token()); co_return 0; }));
	loop.run();
	check(!wait.done && core.turns->waiting_groups() == std::vector<std::uint64_t>{14}, "the waiter is linked");
	std::thread other([&] { caller.request(); });
	other.join();
	check(!wait.done && core.turns->waiting_groups() == std::vector<std::uint64_t>{14}, "the cancelling thread leaves the waiter linked for the loop");
	loop.run();
	check(wait.done && throws<task::cancelled>(wait.error) && core.turns->waiting_groups().empty(), "the loop's hop unlinks it and resumes it cancelled");
}
// A scheduler that declares post and now only, no post_call; one thread drives it with run().
struct NoPostCall {
	std::deque<std::coroutine_handle<>> ready;
	void post(std::coroutine_handle<> h) { ready.push_back(h); }
	std::uint64_t now() const noexcept { return 0; }
	void run() {
		task::running_on scope(task::scheduler_ref::of(*this));
		while (!ready.empty()) { auto h = ready.front(); ready.pop_front(); h.resume(); }
	}
};
// Review F6: durable() on a scheduler that declares no post_call throws std::logic_error before
// it links, so no cancel's hop runs off the loop (cancel_hops shows the hop where it is declared).
void bare_home_refused() {
	Core core;
	core.store(15);
	NoPostCall bare;
	task::cancel_source caller;
	Outcome<int> wait;
	task::spawn(task::scheduler_ref::of(bare), capture(wait, [&]() -> task::task<int> { co_await core.turns->durable(15, 2, caller.token()); co_return 0; }));
	bare.run();
	check(wait.done && throws<std::logic_error>(wait.error), "durable() on a scheduler without post_call throws std::logic_error");
	check(core.turns->waiting_groups().empty(), "the refused wait links no waiter");
}
// A cancel from another thread while the loop links and unlinks other waiters: the callback
// only claims and hops, so the loop alone touches the driver's waiters (run under TSan).
void entry_threads_busy() {
	constexpr int rounds = 200;
	Core core;
	core.store(12); core.store(13);
	int cancelled = 0;
	for (int round = 0; round < rounds; ++round) {
		task::blocking_scheduler scheduler;
		task::cancel_source outer;
		std::atomic<bool> busy{false};
		bool done = false;
		auto wait = [&]() -> task::task<void> {
			task::deadline<task::blocking_scheduler> limit(scheduler, std::chrono::steady_clock::now() + std::chrono::seconds(30), outer.token());
			try { co_await core.turns->durable(13, 2, limit.token()); } catch (const task::cancelled&) { ++cancelled; }
			done = true;
		};
		auto appends = [&]() -> task::task<void> {
			auto& store = core.store(12);
			for (int n = 0; !done && n < 100000; ++n) {
				busy.store(true, std::memory_order_release);
				co_await store.append(core.reserve(12, 1), "b", {});
			}
		};
		auto root = [&]() -> task::task<void> {
			task::spawn(task::current(), wait());
			co_await appends();
		};
		std::thread other([&] { while (!busy.load(std::memory_order_acquire)) { std::this_thread::yield(); } outer.request(); });
		try { j::run(*core.turns, scheduler, root()); } catch (const std::exception& error) { check(false, std::string("busy round threw: ") + error.what()); }
		other.join();
	}
	check(cancelled == rounds, "every cancel lands while the loop is busy (" + std::to_string(cancelled) + " of " + std::to_string(rounds) + ")");
}
// A wake() from another thread ends the entry's sleep toward a 10 s deadline at once: the
// sealer's quiet group gets its open record written and the waiter returns, round after round
// (run under TSan). An entry deaf to the wake sleeps out the deadline and the round fails.
void entry_wake_threads() {
	constexpr int rounds = 100;
	fl::filesystem disk; fl::io io(disk);
	j::TurnDriver driver(io, log_identity, options);
	struct Open { std::mutex mutex; std::string payload; std::uint64_t sequence = 0; } open;
	driver.seal(1, [&](std::uint64_t, bool) -> std::optional<j::TurnRecord> {
		std::lock_guard hold(open.mutex);
		if (open.payload.empty()) { return std::nullopt; }
		return j::TurnRecord{1, open.sequence, j::TurnRecordKind::Batch, std::exchange(open.payload, {})};
	});
	int durable = 0;
	auto slowest = std::chrono::steady_clock::duration::zero();
	for (int round = 0; round < rounds; ++round) {
		const auto sequence = driver.sequence(1) + 1;
		task::blocking_scheduler scheduler;
		std::atomic<bool> waiting{false};
		auto work = [&]() -> task::task<void> {
			task::deadline<task::blocking_scheduler> limit(scheduler, std::chrono::steady_clock::now() + std::chrono::seconds(10));
			waiting.store(true, std::memory_order_release);
			co_await driver.durable(1, sequence, limit.token());  // the sealer has nothing: the entry sleeps
		};
		std::thread writer([&] {
			while (!waiting.load(std::memory_order_acquire)) { std::this_thread::yield(); }
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			std::lock_guard hold(open.mutex);
			open.sequence = sequence; open.payload = "w";
			driver.wake();  // the record's first transaction, under its mutex
		});
		const auto started = std::chrono::steady_clock::now();
		bool woken = true;
		try { j::run(driver, scheduler, work()); ++durable; } catch (const std::exception& error) { woken = false; check(false, std::string("a woken round threw: ") + error.what()); }
		writer.join();
		slowest = std::max(slowest, std::chrono::steady_clock::now() - started);
		if (!woken) { break; }  // one deadline slept out is the failure; the rest would repeat it
	}
	check(durable == rounds && driver.sequence(1) == rounds, "every woken record is durable (" + std::to_string(durable) + " of " + std::to_string(rounds) + ")");
	check(slowest < std::chrono::seconds(5), "no round waits for its deadline");
	std::cout << "entry_wake_threads: slowest round " << std::chrono::duration_cast<std::chrono::microseconds>(slowest).count() << " us\n";
}
}  // namespace

int main(int argc, char** argv) {
	const std::vector<std::pair<std::string, std::function<void()>>> cases{
		{"runner", runner}, {"cancel_waiting", cancel_waiting}, {"cancel_races_barrier", cancel_races_barrier}, {"failed_turn", failed_turn},
		{"entry_append", entry_append}, {"entry_lost_wake", entry_lost_wake}, {"entry_live_token", entry_live_token},
		{"entry_staged_cancelled", entry_staged_cancelled}, {"entry_holder", entry_holder}, {"entry_threads", entry_threads}, {"entry_threads_busy", entry_threads_busy}, {"cancel_hops", cancel_hops}, {"bare_home_refused", bare_home_refused},
		{"entry_wake_threads", entry_wake_threads},
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
