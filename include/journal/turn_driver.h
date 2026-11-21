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

// One core's turn driver: the groups' Stores (shared backend) stage their records here,
// and each turn writes the oldest ones that fit one segment with one data barrier; the rest
// wait for the next turn. A record staged with an operation completes that operation at its
// turn's barrier, or fails it with the turn. A record no segment holds fails its operation,
// and its group's later records theirs, before any turn takes them. run() is the
// loop's task that writes turns through completion's seam; journal::run (blocking.h) commits
// them inline instead. One of them holds the driver at a time.

#pragma once

#include "turn_log.h"
#include "completion/seam.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace kronuz::journal {

class TurnDriver;

// A durable() wait whose group closed (TurnDriver::forget) before its record was durable: the
// record is never written, so it is never acknowledged.
struct group_closed : std::runtime_error {
	group_closed(std::uint64_t closed, std::uint64_t record)
		: std::runtime_error("the group closed before record " + std::to_string(record) + " was durable"), group(closed), sequence(record) {}
	std::uint64_t group, sequence;
};

// A turn's write and data barrier as one operation, for a completion driver: Write, then
// SyncData, on the Turn lane. The file belongs to the log, which keeps it while the turn is in flight.
class TurnBarrier final : public IOOperation {
public:
	TurnBarrier(IO& io, const TurnLog::TurnWrite& write, const Identity& owner) : io_(io) {
		request_.token = {owner, write.turn, 1};
		request_.kind = PrimitiveKind::Write;
		request_.file = std::shared_ptr<File>(std::shared_ptr<File>{}, write.file);
		request_.offset = write.offset;
		request_.bytes = write.bytes;
	}
	const MutationRequest& request() const override { return request_; }
	void submitted() override {
		if (done_ || in_flight_) { throw std::logic_error("turn barrier cannot submit"); }
		in_flight_ = true;
	}
	bool complete(MutationCompletion completion) noexcept override {
		if (!in_flight_ || completion.token != request_.token) { return false; }
		in_flight_ = false;
		try {
			if (completion.error) { std::rethrow_exception(completion.error); }
			if (request_.kind == PrimitiveKind::Write) {
				if (completion.count != request_.bytes.size()) { throw std::runtime_error("short turn write"); }
				request_.kind = PrimitiveKind::SyncData; request_.token.step = 2; request_.bytes = {}; request_.offset = 0;
				return true;
			}
		} catch (...) { error_ = std::current_exception(); }
		done_ = true;
		return true;
	}
	bool done() const noexcept override { return done_; }
	bool in_flight() const noexcept override { return in_flight_; }
	IO& io() const noexcept override { return io_; }
	IOLane lane() const noexcept override { return IOLane::Turn; }
	std::exception_ptr error() const noexcept { return error_; }
private:
	IO& io_;
	MutationRequest request_;
	std::exception_ptr error_;
	bool in_flight_ = false, done_ = false;
};

namespace detail {
// A caller suspended on the driver, in the caller's frame: for staged work (the runner, group
// 0) or for its group's sequence to be durable. The loop links and completes it; a token's
// callback, on any thread, claims it and hops to the loop (post_call), which unlinks it before
// the resume. Only the claim's winner resumes the caller.
struct TurnWaiter {
	enum : int { linked = 0, completed = 1, cancelled = 2, closed = 3, failed = 4 };
	bool claim(int outcome) noexcept { int expected = linked; return state.compare_exchange_strong(expected, outcome, std::memory_order_acq_rel); }
	std::atomic<int> state{linked};
	std::coroutine_handle<> handle;
	task::scheduler_ref home;
	TurnDriver* owner = nullptr;  // set while linked
	TurnWaiter *prev = nullptr, *next = nullptr;
	std::uint64_t group = 0, sequence = 0;
};
// Queues a no-op on `loop`: true when its scheduler declares post_call, where wake() and a
// cancel's hop reach it.
inline bool posts_calls(task::scheduler_ref loop) { return loop.post_call([](void*) {}, nullptr); }
}  // namespace detail

class TurnDriver {
public:
	TurnDriver(IO& io, Identity identity, TurnLogOptions options = {}, const TurnLogGroups& known = {})
		: io_(io), identity_(identity), log_(io, identity, recovered_, options, known) {}
	TurnDriver(const TurnDriver&) = delete;
	TurnDriver& operator=(const TurnDriver&) = delete;
	~TurnDriver() {
		for (auto* waiter = waiters_; waiter; waiter = waiter->next) { waiter->owner = nullptr; }
	}

	// What recovery kept, per group, from its latest checkpoint record.
	const TurnRecovery& recovered() const noexcept { return recovered_; }
	bool has_group(std::uint64_t group) const { auto found = recovered_.groups.find(group); return found != recovered_.groups.end() && !found->second.empty(); }
	bool lost(std::uint64_t group) const { return recovered_.lost.contains(group); }
	// A group's recovered records, released once its Store has replayed them.
	const std::vector<TurnRecord>& records(std::uint64_t group) const {
		static const std::vector<TurnRecord> none;
		auto found = recovered_.groups.find(group); return found == recovered_.groups.end() ? none : found->second;
	}
	void release(std::uint64_t group) { recovered_.groups.erase(group); }
	// A group no Store holds leaves memory but not the log (TurnLog::evict); live() reads its
	// records back for the next Store. Nothing of it may be staged. Its refusal goes too: the
	// refused records never reached the log, and no waiter of the old holder remains.
	void evict(std::uint64_t group) {
		if (staged(group)) { throw std::logic_error("the group has records staged or in flight"); }
		log_.evict(group); recovered_.groups.erase(group); refused_.erase(group);
	}
	bool evicted(std::uint64_t group) const { return log_.evicted(group); }
	std::vector<TurnRecord> live(std::uint64_t group) { return log_.live(group); }
	// A closed group leaves the log (TurnLog::forget) and its sealer is unregistered: no
	// sealer of it runs once this returns. Nothing of it may be staged. Every durable() waiter
	// of the group fails with group_closed, and the sequences it could hold are retired. The
	// driver keeps the floor only past `implied`, the floor its host's table already gives
	// (retire_with); a bare driver keeps every one.
	void forget(std::uint64_t group, std::uint64_t implied = 0) {
		if (staged(group)) { throw std::logic_error("the group has records staged or in flight"); }
		auto floor = std::max(kept(group), log_.sequence(group) + 1);  // the open record a writer may hold
		for (auto* waiter = waiters_; waiter; waiter = waiter->next) { if (waiter->group == group) { floor = std::max(floor, waiter->sequence); } }
		if (auto found = refused_.find(group); found != refused_.end()) { floor = std::max(floor, found->second.last + 1); refused_.erase(found); }  // and the open record past the refused ones
		if (floor > implied) { retired_[group] = floor; } else { retired_.erase(group); }
		resume([group](const detail::TurnWaiter& waiter) { return waiter.group == group; }, detail::TurnWaiter::closed);
		{
			std::lock_guard hold(sealers_mutex_);
			if (auto found = sealers_.find(group); found != sealers_.end()) {
				if (running_ && running_ == found->second.fn) { running_forgotten_ = true; }
				drop(found);
			}
		}
		log_.forget(group); recovered_.groups.erase(group);
	}
	// A group's sealer (docs/mechanisms/turn-seal.md). Each turn calls it, on the loop and holding
	// no lock of the driver's, after the turn's pending records and before encoding, and again
	// while it returns records and room is left. It gets the room left (turn_log_detail::record_cost
	// units) and whether no record has sealed in the turn yet; it returns a record of the group,
	// at the group's last staged sequence + 1, whose record_cost fits the room, or nothing. A
	// record that breaks that fails the turn, as a refused stage does. A durable() waiter above
	// the group's staged sequence, or wake(), asks for a turn.
	using Sealer = std::function<std::optional<TurnRecord>(std::uint64_t room, bool first)>;
	// Registers the group's sealer for `owner`, replacing an earlier one; returns the group's
	// last staged sequence, which its next record follows.
	// On the loop.
	std::uint64_t seal(std::uint64_t group, Sealer fn, const void* owner = nullptr) {
		if (!group) { throw std::invalid_argument("group 0 is reserved"); }
		if (!fn) { throw std::invalid_argument("a sealer needs a function"); }
		std::uint64_t wanted = 0;
		for (auto* waiter = waiters_; waiter; waiter = waiter->next) { if (waiter->group == group) { wanted = std::max(wanted, waiter->sequence); } }
		const auto staged = last_staged(group);
		{
			std::lock_guard hold(sealers_mutex_);
			auto& sealing = sealers_[group];
			sealing.fn = std::make_shared<Sealer>(std::move(fn)); sealing.owner = owner;
			sealing.staged = staged; sealing.wanted = wanted; sealing.quiet = false;
			mark(sealing);
		}
		if (owed_.load(std::memory_order_acquire)) { resume([](const detail::TurnWaiter& waiter) { return waiter.group == 0; }); }
		return staged;
	}
	// Any thread: unregisters the group's sealer if `owner` registered it, so a later
	// registration survives an earlier owner's unseal. Returns once no sealer `owner` registered
	// for the group is running, even one the table no longer holds, and none ever will, as a
	// timer's cancel does: the caller must not hold a lock the sealer takes. From inside the
	// sealer itself it returns at once and the call finishes.
	void unseal(std::uint64_t group, const void* owner) noexcept {
		std::unique_lock hold(sealers_mutex_);
		if (auto found = sealers_.find(group); found != sealers_.end() && found->second.owner == owner) { drop(found); }
		auto running = [&] { return running_ && running_group_ == group && running_owner_ == owner; };
		if (running() && running_thread_ != std::this_thread::get_id()) { sealer_done_.wait(hold, [&] { return !running(); }); }
	}
	bool sealed(std::uint64_t group) const { std::lock_guard hold(sealers_mutex_); return sealers_.contains(group); }
	// Any thread, under any lock: an open record took its first transaction, so the next turn
	// seals it. Takes only a lock no sealer call runs under; posts to the holder's loop (Hold):
	// run()'s, which it resumes, or the blocking entry's, whose sleep the post ends. From another
	// thread, that post_call must be thread-safe: task's asio_scheduler and blocking_scheduler
	// are; inline_scheduler is not.
	void wake() noexcept {
		if (woken_.exchange(true, std::memory_order_acq_rel)) { return; }
		std::lock_guard hold(loop_mutex_);
		if (!loop_) { return; }
		if (!wakeup_) { loop_.post_call([](void*) {}, nullptr); return; }  // the entry reads idle() on its next pass
		wakeup_->refs.fetch_add(1, std::memory_order_relaxed);
		loop_.post_call(&TurnDriver::woken, wakeup_);  // never refused: run() checked its loop
	}
	// Records of the group wait for a turn or are in the one in flight.
	bool staged(std::uint64_t group) const {
		auto of = [group](const auto& item) { return item.record.group == group; };
		return std::any_of(pending_.begin(), pending_.end(), of) || std::any_of(flight_.begin(), flight_.end(), of);
	}
	// The log has the group, durably or staged: its state is the log's, not the host table's.
	bool holds(std::uint64_t group) const { return log_.sequence(group) || staged(group); }
	// The group's last durable record sequence, 0 for none.
	std::uint64_t sequence(std::uint64_t group) const { return log_.sequence(group); }
	// The last sequence an earlier incarnation of the group could hand out, 0 for none: a
	// reopened group's records follow it, and a durable() at or below it fails with group_closed.
	std::uint64_t retired(std::uint64_t group) const { return retired_by_ ? std::max(kept(group), retired_by_(group)) : kept(group); }
	// A host answers retired() from its table: a group it closed retires through its close's
	// sequence + 1, so forget() keeps no entry per closed group. Called on the loop, maybe under
	// the sealers' lock; it must not throw.
	void retire_with(std::function<std::uint64_t(std::uint64_t)> floor) { retired_by_ = std::move(floor); }
	// A host's table commit before each turn that has records, ahead of encoding: a group's
	// create or wake is durable before the turn carrying its first record (docs/design/idle-groups.md
	// D5). On the loop; one that throws fails the driver, as a failed turn does.
	void before_turn(std::function<void()> hook) { before_turn_ = std::move(hook); }
	bool failed() const noexcept { return log_failed_; }

	// Stages a record for the next turn that has room for it; its operation, if any, completes
	// at that turn's barrier with the token it was staged with. A record no segment holds is
	// refused at the next turn: its operation, and those of its group's later records, fail with
	// std::length_error, and so do durable() waits on them, until the group is forgotten or evicted.
	// Returns the record's sequence.
	std::uint64_t stage(TurnRecord record, std::shared_ptr<IOOperation> operation = {}, MutationToken token = {}) {
		if (log_failed_) { throw std::logic_error("turn driver failed"); }
		if (sealing_) { throw std::logic_error("a sealer cannot stage records"); }
		const auto group = record.group, sequence = record.sequence;
		if (sequence <= retired(group)) { throw std::logic_error("a record reuses a sequence its group's closed incarnation retired"); }
		pending_.push_back({std::move(record), std::move(operation), token});
		{
			std::lock_guard hold(sealers_mutex_);
			if (auto found = sealers_.find(group); found != sealers_.end()) { found->second.staged = sequence; mark(found->second); }
		}
		resume([](const detail::TurnWaiter& waiter) { return waiter.group == 0; });
		return sequence;
	}
	// Nothing waits for a turn: no record is pending, no sealer owes a waited-for record, no
	// wake() is unanswered, and no sealer's failure waits for its turn to complete.
	bool idle() const noexcept { return pending_.empty() && !owed_.load(std::memory_order_acquire) && !woken_.load(std::memory_order_acquire) && !deferred_; }
	bool in_flight() const noexcept { return write_.has_value(); }

	// Stages the oldest pending records that fit a segment as one turn and submits their
	// operations; the host writes write().bytes at write().offset, calls TurnLog::flush(write), then
	// completed() or failed().
	const TurnLog::TurnWrite& begin() {
		if (holder_ == Holder::driver) { throw std::logic_error("run() holds the turn driver"); }
		auto* write = start();
		if (!write) { throw std::logic_error("no records to write"); }
		return *write;
	}
	const TurnLog::TurnWrite& write() const { if (!write_) { throw std::logic_error("no turn in flight"); } return *write_; }
	void completed() {
		if (!write_) { throw std::logic_error("no turn in flight"); }
		// Reclamation after the barrier can fail; the operations then fail too, which is
		// safe: a durable record reported failed is merely unacknowledged.
		try { log_.completed(); } catch (...) { log_.failed(); write_.reset(); fail(std::current_exception()); throw; }
		write_.reset();
		auto flight = std::move(flight_); flight_.clear();
		for (auto& item : flight) { finish(item, nullptr); }
		resume([this](const detail::TurnWaiter& waiter) { return waiter.group && log_.sequence(waiter.group) >= waiter.sequence; });
		// A repeat sealer call failed while sealing this turn: its records are acknowledged above,
		// then the driver fails with that call's error.
		if (deferred_) { auto error = std::exchange(deferred_, nullptr); fail(error); std::rethrow_exception(error); }
	}
	// The turn's write or barrier failed: the log fails, and so does every operation
	// staged with it or after it.
	void failed(std::exception_ptr error) noexcept { log_.failed(); write_.reset(); fail(error); }

	// Writes and syncs the oldest pending and sealed records that fit a segment as one turn;
	// when there are none, writes nothing if a waiter asked the sealers, and throws otherwise.
	void commit() {
		if (holder_ == Holder::driver) { throw std::logic_error("run() holds the turn driver"); }
		const bool asked = !idle();
		const auto* staged = start();
		if (!staged) { if (!asked) { throw std::logic_error("no records to write"); } return; }
		const auto& write = *staged;
		try {
			if (write.file->write_at(write.offset, write.bytes) != write.bytes.size()) { throw std::runtime_error("short turn write"); }
			log_.flush(write);
		} catch (...) { failed(std::current_exception()); throw; }
		completed();
	}

	// The loop's task: writes each turn as records are staged, through `io`, one turn at a
	// time, with no caller's token: a staged record is always written. Returns once `stop` is
	// requested and nothing is staged or in flight; throws the failure when a turn fails. Its
	// loop's scheduler must declare post_call, where wake() reaches it: on one that declares
	// none, run() throws std::logic_error before it holds the driver.
	task::task<void> run(completion_ref io, task::cancel_token stop = {}) {
		if (!detail::posts_calls(task::current())) { throw std::logic_error("run() needs a loop whose scheduler declares post_call"); }
		Hold hold(*this, Holder::driver, task::current());
		struct Loop {
			TurnDriver& turns;
			explicit Loop(TurnDriver& driver) : turns(driver) {
				auto* wakeup = new Wakeup{&driver};
				std::lock_guard hold(turns.loop_mutex_); turns.wakeup_ = wakeup;
			}
			// A wake still queued on the loop finds no driver: the owner may destroy it now.
			~Loop() {
				Wakeup* wakeup = nullptr;
				{ std::lock_guard hold(turns.loop_mutex_); wakeup = std::exchange(turns.wakeup_, nullptr); }
				wakeup->driver.store(nullptr, std::memory_order_release);
				Wakeup::release(wakeup);
			}
		} loop(*this);
		for (;;) {
			if (log_failed_) { std::rethrow_exception(failure_); }
			if (idle()) {
				if (stop.requested()) { co_return; }
				try { co_await Wait(*this, 0, 0, stop); } catch (const task::cancelled&) {}
				continue;
			}
			const auto* staged = start();
			if (!staged) { continue; }
			const auto& write = *staged;
			auto barrier = std::make_shared<TurnBarrier>(io_, write, identity_);
			std::exception_ptr error;
			try { co_await io.run(barrier); } catch (...) { error = std::current_exception(); }
			if (!error) { error = barrier->error(); }
			if (error) { failed(error); std::rethrow_exception(error); }
			completed();
		}
	}
	// Resumes once the group's records through `sequence` are durable. Throws the turn
	// driver's failure, group_closed when the group closes first or `sequence` is retired, or
	// task::cancelled when `token` is requested first; a cancelled wait leaves its record
	// staged, written with its turn. A wait that suspends needs the caller's scheduler to
	// declare post_call, where the token's callback hops: on one that declares none, it throws
	// std::logic_error before it links.
	task::task<void> durable(std::uint64_t group, std::uint64_t sequence, task::cancel_token token = {}) {
		if (!group) { throw std::invalid_argument("group 0 is reserved"); }
		co_await Wait(*this, group, sequence, token);
	}
	// The groups a caller waits on, in order: what a stalled blocking entry names.
	std::vector<std::uint64_t> waiting_groups() const {
		std::vector<std::uint64_t> groups;
		for (auto* waiter = waiters_; waiter; waiter = waiter->next) { if (waiter->group) { groups.push_back(waiter->group); } }
		std::sort(groups.begin(), groups.end());
		groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
		return groups;
	}

	// Who drives the turns: none, run() (a reactor loop), or the blocking entry. Each refuses
	// a driver the other holds. The holder's loop, which outlives the hold, is where wake() posts: Hold
	// requires it and throws std::logic_error on an empty one, before it holds the driver.
	enum class Holder : std::uint8_t { none, driver, entry };
	Holder holder() const noexcept { return holder_; }
	struct Hold {
		TurnDriver& turns;
		Hold(TurnDriver& driver, Holder holder, task::scheduler_ref loop) : turns(driver) {
			if (!loop) { throw std::logic_error("a Hold needs a loop"); }
			if (turns.holder_ != Holder::none) { throw std::logic_error("the turn driver is held by another driver"); }
			{ std::lock_guard hold(turns.loop_mutex_); turns.loop_ = loop; }
			turns.holder_ = holder;
		}
		Hold(const Hold&) = delete;
		Hold& operator=(const Hold&) = delete;
		~Hold() {
			{ std::lock_guard hold(turns.loop_mutex_); turns.loop_ = {}; }
			turns.holder_ = Holder::none;
		}
	};

	TurnLog& log() noexcept { return log_; }

private:
	struct Wait {
		Wait(TurnDriver& driver, std::uint64_t group, std::uint64_t sequence, task::cancel_token token) : turns(driver), token(std::move(token)) {
			waiter.group = group; waiter.sequence = sequence;
		}
		Wait(const Wait&) = delete;
		Wait& operator=(const Wait&) = delete;
		~Wait() {
			callback.reset();  // waits for a callback running on another thread
			if (waiter.owner) { waiter.owner->unlink(waiter); }
		}
		bool await_ready() noexcept {
			if (waiter.group && waiter.sequence <= turns.retired(waiter.group)) { waiter.state.store(detail::TurnWaiter::closed, std::memory_order_relaxed); return true; }
			return token.requested() || turns.satisfied(waiter);
		}
		void await_suspend(std::coroutine_handle<> handle) {
			waiter.handle = handle; waiter.home = task::current();
			if (!detail::posts_calls(waiter.home)) { throw std::logic_error("waiting on a turn driver needs a running scheduler that declares post_call"); }
			turns.link(waiter);
			callback.emplace(token, Cancel{&waiter});
		}
		void await_resume() {
			const auto state = waiter.state.load(std::memory_order_acquire);
			if (state == detail::TurnWaiter::closed) { throw group_closed(waiter.group, waiter.sequence); }
			if (state == detail::TurnWaiter::cancelled || (token.requested() && !turns.satisfied(waiter))) { throw task::cancelled{}; }
			if (auto error = turns.refusal(waiter.group, waiter.sequence)) { std::rethrow_exception(error); }
			// A waiter its turn made durable is answered even if the driver failed after that turn.
			if (waiter.group && turns.log_failed_ && state != detail::TurnWaiter::completed) { std::rethrow_exception(turns.failure_); }
		}
		struct Cancel {
			detail::TurnWaiter* waiter;
			void operator()() const noexcept {
				if (!waiter->claim(detail::TurnWaiter::cancelled)) { return; }
				waiter->home.post_call(&hop, waiter);  // never refused: await_suspend checked the home
			}
			// On the waiter's loop: unlinks it if the driver has not, then resumes it.
			static void hop(void* raw) {
				auto* waiter = static_cast<detail::TurnWaiter*>(raw);
				if (waiter->owner) { waiter->owner->unlink(*waiter); }
				waiter->home.post(waiter->handle);
			}
		};
		TurnDriver& turns;
		task::cancel_token token;
		detail::TurnWaiter waiter;
		std::optional<task::cancel_callback<Cancel>> callback;
	};
	bool satisfied(const detail::TurnWaiter& waiter) const {
		if (!waiter.group) { return !idle(); }
		return log_failed_ || refusal(waiter.group, waiter.sequence) || log_.sequence(waiter.group) >= waiter.sequence;
	}
	// A group's waiter asks its sealer, if any, for a turn.
	void link(detail::TurnWaiter& waiter) noexcept {
		waiter.owner = this; waiter.prev = nullptr; waiter.next = waiters_;
		if (waiters_) { waiters_->prev = &waiter; }
		waiters_ = &waiter;
		if (!waiter.group) { return; }
		bool owed = false;
		{
			std::lock_guard hold(sealers_mutex_);
			auto found = sealers_.find(waiter.group);
			if (found == sealers_.end() || refused_.contains(waiter.group)) { return; }  // a refused group never owes
			found->second.quiet = false; found->second.wanted = std::max(found->second.wanted, waiter.sequence);
			mark(found->second); owed = found->second.owed;
		}
		if (owed) { resume([](const detail::TurnWaiter& other) { return other.group == 0; }); }
	}
	void unlink(detail::TurnWaiter& waiter) noexcept {
		(waiter.prev ? waiter.prev->next : waiters_) = waiter.next;
		if (waiter.next) { waiter.next->prev = waiter.prev; }
		waiter.owner = nullptr; waiter.prev = waiter.next = nullptr;
	}
	// Unlinks every waiter `ready` accepts, then resumes those whose claim of `outcome` wins.
	template <class Ready> void resume(Ready ready, int outcome = detail::TurnWaiter::completed) noexcept {
		for (auto* waiter = waiters_; waiter;) {
			auto* next = waiter->next;
			if (ready(*waiter)) {
				unlink(*waiter);
				if (waiter->claim(outcome)) { waiter->home.post(waiter->handle); }
			}
			waiter = next;
		}
	}
	// Refuses the records no segment holds, seals, then stages as one turn the oldest pending
	// records that encode in an empty segment; nullptr when none are pending. A turn of sealed
	// records fits whole: seal_turn spends an empty segment's room after the staged records.
	const TurnLog::TurnWrite* start() {
		if (write_) { throw std::logic_error("a turn is already in flight"); }
		refuse();
		try { seal_turn(); } catch (...) { fail(std::current_exception()); throw; }
		if (pending_.empty()) { return nullptr; }
		std::vector<TurnRecord> records; records.reserve(pending_.size());
		std::uint64_t used = 0;
		for (auto& item : pending_) { used += turn_log_detail::record_cost(item.record.payload.size()); records.push_back(std::move(item.record)); }
		// Within the room every record fits; past it, the log measures. refuse() left each record
		// fitting alone, so the first one always fits.
		const auto take = used <= whole_room() ? records.size() : log_.fitting(records);
		for (std::size_t i = take; i < records.size(); ++i) { pending_[i].record = std::move(records[i]); }
		records.resize(take);
		if (before_turn_) {
			try { before_turn_(); }
			catch (...) {
				for (std::size_t i = 0; i < take; ++i) { pending_[i].record = std::move(records[i]); }
				fail(std::current_exception()); throw;
			}
		}
		flight_.assign(std::make_move_iterator(pending_.begin()), std::make_move_iterator(pending_.begin() + static_cast<std::ptrdiff_t>(take)));
		pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(take));
		try { write_ = log_.stage(records); }
		catch (...) { fail(std::current_exception()); throw; }
		// An operation that refuses submission (its Store fenced) is dropped, never told; its
		// record goes with the turn, so its group's sequence stays whole and its neighbors run.
		for (auto& item : flight_) {
			if (!item.operation) { continue; }
			try { item.operation->submitted(); item.submitted = true; } catch (...) { item.operation.reset(); }
		}
		return &*write_;
	}
	// The record cost one turn may carry in an empty segment.
	std::uint64_t whole_room() const {
		const auto& options = log_.options();
		return turn_log_detail::turn_room(options.segment_bytes, options.block_bytes);
	}
	// The error a group's refused record left for `sequence`, null when none: sequences from
	// the refused one on never reach the log.
	std::exception_ptr refusal(std::uint64_t group, std::uint64_t sequence) const {
		if (!group) { return nullptr; }
		auto found = refused_.find(group);
		return found != refused_.end() && sequence >= found->second.hole ? found->second.error : nullptr;
	}
	// Takes out of pending_ each record that does not encode alone in an empty segment, as encode()
	// refuses it, and every later record of its group, and fails their operations and the durable()
	// waits on them; the rest keep their order. A refused group seals nothing more, and its sealer
	// owes nothing, until it is forgotten or evicted.
	void refuse() {
		const auto room = whole_room();
		std::vector<Pending> refused;
		auto kept = pending_.begin();
		for (auto& item : pending_) {
			const auto group = item.record.group, sequence = item.record.sequence;
			auto found = refused_.find(group);
			if (found == refused_.end() && turn_log_detail::record_cost(item.record.payload.size()) > room && !log_.fitting(std::span(&item.record, 1))) {
				auto error = std::make_exception_ptr(std::length_error("turn record exceeds a segment"));
				found = refused_.emplace(group, Refusal{sequence, sequence, error}).first;
			}
			if (found != refused_.end() && sequence >= found->second.hole) {
				found->second.last = std::max(found->second.last, sequence);
				refused.push_back(std::move(item));
				continue;
			}
			if (&*kept != &item) { *kept = std::move(item); }
			++kept;
		}
		if (refused.empty()) { return; }
		pending_.erase(kept, pending_.end());
		{
			std::lock_guard hold(sealers_mutex_);  // a refused group's sealer owes no turn again
			for (const auto& [group, _] : refused_) { if (auto found = sealers_.find(group); found != sealers_.end()) { found->second.quiet = true; mark(found->second); } }
		}
		for (auto& item : refused) { settle(item, refused_.at(item.record.group).error); }
		resume([this](const detail::TurnWaiter& waiter) { return refusal(waiter.group, waiter.sequence) != nullptr; }, detail::TurnWaiter::failed);
	}
	// Calls each sealer, from the one the last turn left waiting, until it returns nothing or the
	// room is spent (docs/mechanisms/turn-seal.md). The first sealed record may fill an empty
	// segment (so a full-size record seals); later ones share segment_bytes / 2, each charged its
	// record_cost before the next call. A sealer runs with no lock of the driver's held;
	// sealers_mutex_ guards only the table and running_, which covers all of a group's calls.
	void seal_turn() {
		woken_.store(false, std::memory_order_release);  // a wake from here on asks for the next turn
		const auto& options = log_.options();
		const auto whole = turn_log_detail::turn_room(options.segment_bytes, options.block_bytes);
		const auto half = turn_log_detail::turn_room(options.segment_bytes / 2, options.block_bytes);
		std::uint64_t used = 0;
		for (const auto& item : pending_) { used += turn_log_detail::record_cost(item.record.payload.size()); }
		std::vector<std::uint64_t> order;
		{
			std::lock_guard hold(sealers_mutex_);
			if (sealers_.empty()) { return; }
			order.reserve(sealers_.size());
			for (auto it = sealers_.lower_bound(next_seal_); it != sealers_.end(); ++it) { order.push_back(it->first); }
			for (auto it = sealers_.begin(); it != sealers_.end() && it->first < next_seal_; ++it) { order.push_back(it->first); }
		}
		struct Guard { bool& flag; explicit Guard(bool& value) : flag(value) { flag = true; } ~Guard() { flag = false; } } guard(sealing_);
		// Clears running_ once a group's calls end, however they end.
		struct Running { TurnDriver& turns; ~Running() { std::lock_guard hold(turns.sealers_mutex_); turns.running_.reset(); turns.sealer_done_.notify_all(); } };
		std::optional<std::uint64_t> resume_at;
		bool first = true;
		auto room_left = [&]() -> std::uint64_t { const auto limit = first ? whole : half; return limit > used ? limit - used : 0; };
		for (const auto group : order) {
			std::shared_ptr<Sealer> fn;  // alive while it runs, even if its group is unsealed
			{
				std::lock_guard hold(sealers_mutex_);
				auto found = sealers_.find(group);
				if (found == sealers_.end() || refused_.contains(group)) { continue; }  // unsealed, forgotten or refused
				if (room_left() < turn_log_detail::record_cost(0)) { resume_at = group; break; }
				fn = found->second.fn; running_ = fn; running_group_ = group; running_owner_ = found->second.owner;
				running_thread_ = std::this_thread::get_id(); running_forgotten_ = false;
			}
			Running running{*this};
			for (bool repeat = false;; repeat = true) {
				const auto room = room_left();
				if (repeat) {
					if (room < turn_log_detail::record_cost(0)) { break; }  // the next group, if any, starts the next turn
					std::lock_guard hold(sealers_mutex_);
					auto found = sealers_.find(group);
					if (found == sealers_.end() || found->second.fn != fn) { break; }  // unsealed, forgotten or replaced
				}
				// A repeat call that throws or breaks the contract leaves the group's earlier records
				// in the turn, and the next turn fails, as if that call had begun it.
				std::optional<TurnRecord> record;
				try { record = (*fn)(room, first); }
				catch (...) { if (!repeat) { throw; } deferred_ = std::current_exception(); return; }
				std::unique_lock hold(sealers_mutex_);
				auto found = sealers_.find(group);
				const bool same = found != sealers_.end() && found->second.fn == fn;
				if (!record) {
					// A first call with nothing under the whole room: quiet until a waiter asks again.
					// Nothing under less, as a share or behind staged records: called first next turn,
					// when it may fill it. A repeat call's nothing ends a group this turn served.
					if (same && !repeat) { if (room == whole) { found->second.quiet = true; mark(found->second); } else if (!resume_at) { resume_at = group; } }
					break;
				}
				const auto staged = found != sealers_.end() ? found->second.staged : last_staged(group);
				std::exception_ptr refused;
				// A record sealed before an unseal returned is the group's; one whose group was
				// forgotten during the call is not.
				if (running_forgotten_) { refused = std::make_exception_ptr(std::logic_error("a sealer returned a record for the group it forgot")); }
				else if (record->group != group) { refused = std::make_exception_ptr(std::invalid_argument("a sealer returned another group's record")); }
				else if (record->sequence != staged + 1) { refused = std::make_exception_ptr(std::invalid_argument("sealed record out of its group's sequence")); }
				else if (turn_log_detail::record_cost(record->payload.size()) > room) { refused = std::make_exception_ptr(std::invalid_argument("sealed record past its room")); }
				if (refused) { if (!repeat) { std::rethrow_exception(refused); } deferred_ = refused; return; }
				used += turn_log_detail::record_cost(record->payload.size()); first = false;
				if (found != sealers_.end()) { found->second.staged = record->sequence; mark(found->second); }
				hold.unlock();
				pending_.push_back({std::move(*record), {}, {}});
			}
		}
		if (resume_at) { next_seal_ = *resume_at; }
	}
	std::uint64_t last_staged(std::uint64_t group) const {
		auto of = [group](const auto& item) { return item.record.group == group; };
		if (auto found = std::find_if(pending_.rbegin(), pending_.rend(), of); found != pending_.rend()) { return found->record.sequence; }
		if (auto found = std::find_if(flight_.rbegin(), flight_.rend(), of); found != flight_.rend()) { return found->record.sequence; }
		return std::max(log_.sequence(group), retired(group));
	}
	// A sealer owes a turn while a waiter wants a sequence past its staged one and it has not
	// answered nothing to the whole budget since that waiter linked.
	struct Sealing { std::shared_ptr<Sealer> fn; const void* owner = nullptr; std::uint64_t staged = 0, wanted = 0; bool quiet = false, owed = false; };
	// What a posted wake() reaches: run()'s driver while that run() lasts, then nothing. Held by
	// run() and by each queued wake.
	struct Wakeup {
		std::atomic<TurnDriver*> driver;
		std::atomic<int> refs{1};
		static void release(Wakeup* wakeup) noexcept { if (wakeup->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) { delete wakeup; } }
	};
	// On the loop, after a wake() from any thread: resumes run() if it still runs and sleeps.
	static void woken(void* raw) {
		auto* wakeup = static_cast<Wakeup*>(raw);
		if (auto* driver = wakeup->driver.load(std::memory_order_acquire)) { driver->resume([](const detail::TurnWaiter& waiter) { return waiter.group == 0; }); }
		Wakeup::release(wakeup);
	}
	// Under sealers_mutex_.
	void mark(Sealing& sealing) noexcept {
		const bool owed = !sealing.quiet && sealing.wanted > sealing.staged;
		if (owed != sealing.owed) { sealing.owed = owed; owed ? owed_.fetch_add(1, std::memory_order_acq_rel) : owed_.fetch_sub(1, std::memory_order_acq_rel); }
	}
	void drop(std::map<std::uint64_t, Sealing>::iterator found) noexcept {
		if (found->second.owed) { owed_.fetch_sub(1, std::memory_order_acq_rel); }
		sealers_.erase(found);
	}

	struct Pending { TurnRecord record; std::shared_ptr<IOOperation> operation; MutationToken token; bool submitted = false; };
	static void finish(Pending& item, std::exception_ptr error) noexcept {
		if (!item.operation) { return; }
		MutationCompletion completion; completion.token = item.token; completion.error = error;
		item.operation->complete(std::move(completion));
	}
	// Fails an item's operation; one not yet submitted is submitted first, so failing it settles it.
	static void settle(Pending& item, std::exception_ptr error) noexcept {
		if (!item.operation) { return; }
		if (!item.submitted) { try { item.operation->submitted(); item.submitted = true; } catch (...) { return; } }
		finish(item, error);
	}
	void fail(std::exception_ptr error) noexcept {
		log_failed_ = true; failure_ = error ? error : std::make_exception_ptr(std::logic_error("turn driver failed"));
		deferred_ = nullptr;
		auto flight = std::move(flight_); flight_.clear();
		for (auto& item : pending_) { flight.push_back(std::move(item)); }
		pending_.clear();
		for (auto& item : flight) { settle(item, error); }
		resume([](const detail::TurnWaiter& waiter) { return waiter.group != 0; }, detail::TurnWaiter::failed);
	}

	IO& io_;
	Identity identity_;
	TurnRecovery recovered_;
	TurnLog log_;
	std::vector<Pending> pending_, flight_;
	mutable std::mutex sealers_mutex_;  // the sealer table and running_; never held while a sealer runs
	std::condition_variable sealer_done_;  // a sealer call finished
	std::map<std::uint64_t, Sealing> sealers_;
	std::map<std::uint64_t, std::uint64_t> retired_;  // per group forgotten past its host's floor: retired()
	std::function<std::uint64_t(std::uint64_t)> retired_by_;  // the host's floor: retire_with()
	std::function<void()> before_turn_;  // the host's table commit: before_turn()
	std::uint64_t kept(std::uint64_t group) const { auto found = retired_.find(group); return found == retired_.end() ? 0 : found->second; }
	// A group whose record at `hole` no segment holds: its records from there through `last` failed.
	struct Refusal { std::uint64_t hole = 0, last = 0; std::exception_ptr error; };
	std::map<std::uint64_t, Refusal> refused_;  // on the loop, until the group is forgotten or evicted
	std::shared_ptr<Sealer> running_;  // the sealer being called, on running_thread_
	std::uint64_t running_group_ = 0;  // and the group and owner it was called for
	const void* running_owner_ = nullptr;
	std::thread::id running_thread_;
	bool running_forgotten_ = false;  // its group was forgotten during the call
	std::atomic<std::size_t> owed_{0};
	std::uint64_t next_seal_ = 0;
	bool sealing_ = false;
	std::atomic<bool> woken_{false};
	std::mutex loop_mutex_;  // guards loop_ and wakeup_ for wake(); only the loop's post is taken under it
	task::scheduler_ref loop_;  // the holder's loop, while it holds the driver
	Wakeup* wakeup_ = nullptr;  // run()'s, while it runs
	std::optional<TurnLog::TurnWrite> write_;
	detail::TurnWaiter* waiters_ = nullptr;
	std::exception_ptr failure_;
	std::exception_ptr deferred_;  // a repeat sealer call's failure: fails the driver once its turn completes
	Holder holder_ = Holder::none;
	bool log_failed_ = false;
};

}  // namespace kronuz::journal
