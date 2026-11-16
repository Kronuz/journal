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
// and each turn writes them with one data barrier. A record staged with an operation
// completes that operation at its turn's barrier, or fails it with the turn. run() is the
// loop's task that writes turns through completion's seam; journal::run (blocking.h) commits
// them inline instead. One of them holds the driver at a time.

#pragma once

#include "turn_log.h"
#include "completion/seam.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

namespace kronuz::journal {

class TurnDriver;

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
	enum : int { linked = 0, completed = 1, cancelled = 2 };
	bool claim(int outcome) noexcept { int expected = linked; return state.compare_exchange_strong(expected, outcome, std::memory_order_acq_rel); }
	std::atomic<int> state{linked};
	std::coroutine_handle<> handle;
	task::scheduler_ref home;
	TurnDriver* owner = nullptr;  // set while linked
	TurnWaiter *prev = nullptr, *next = nullptr;
	std::uint64_t group = 0, sequence = 0;
};
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
	// A closed group leaves the log (TurnLog::forget); nothing of it may be staged.
	void forget(std::uint64_t group) {
		for (const auto& item : pending_) { if (item.record.group == group) { throw std::logic_error("the group has records staged"); } }
		for (const auto& item : flight_) { if (item.record.group == group) { throw std::logic_error("the group has records in flight"); } }
		log_.forget(group); recovered_.groups.erase(group);
	}
	// The group's last durable record sequence, 0 for none.
	std::uint64_t sequence(std::uint64_t group) const { return log_.sequence(group); }
	bool failed() const noexcept { return log_failed_; }

	// Stages a record for the next turn; its operation, if any, completes at that turn's
	// barrier with the token it was staged with.
	// Returns the record's sequence.
	std::uint64_t stage(TurnRecord record, std::shared_ptr<IOOperation> operation = {}, MutationToken token = {}) {
		if (log_failed_) { throw std::logic_error("turn driver failed"); }
		const auto sequence = record.sequence;
		pending_.push_back({std::move(record), std::move(operation), token});
		wake([](const detail::TurnWaiter& waiter) { return waiter.group == 0; });
		return sequence;
	}
	bool idle() const noexcept { return pending_.empty(); }
	bool in_flight() const noexcept { return write_.has_value(); }

	// Stages the pending records as one turn and submits their operations; the host writes
	// write().bytes at write().offset, calls File::sync_data, then completed() or failed().
	const TurnLog::TurnWrite& begin() {
		if (holder_ == Holder::driver) { throw std::logic_error("run() holds the turn driver"); }
		return start();
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
		wake([this](const detail::TurnWaiter& waiter) { return waiter.group && log_.sequence(waiter.group) >= waiter.sequence; });
	}
	// The turn's write or barrier failed: the log fails, and so does every operation
	// staged with it or after it.
	void failed(std::exception_ptr error) noexcept { log_.failed(); write_.reset(); fail(error); }

	// Writes and syncs the pending turn synchronously.
	void commit() {
		const auto& write = begin();
		try {
			if (write.file->write_at(write.offset, write.bytes) != write.bytes.size()) { throw std::runtime_error("short turn write"); }
			write.file->sync_data();
		} catch (...) { failed(std::current_exception()); throw; }
		completed();
	}

	// The loop's task: writes each turn as records are staged, through `io`, one turn at a
	// time, with no caller's token: a staged record is always written. Returns once `stop` is
	// requested and nothing is staged or in flight; throws the failure when a turn fails.
	task::task<void> run(completion_ref io, task::cancel_token stop = {}) {
		Hold hold(*this, Holder::driver);
		for (;;) {
			if (log_failed_) { std::rethrow_exception(failure_); }
			if (pending_.empty()) {
				if (stop.requested()) { co_return; }
				try { co_await Wait(*this, 0, 0, stop); } catch (const task::cancelled&) {}
				continue;
			}
			const auto& write = start();
			auto barrier = std::make_shared<TurnBarrier>(io_, write, identity_);
			std::exception_ptr error;
			try { co_await io.run(barrier); } catch (...) { error = std::current_exception(); }
			if (!error) { error = barrier->error(); }
			if (error) { failed(error); std::rethrow_exception(error); }
			completed();
		}
	}
	// Resumes once the group's records through `sequence` are durable. Throws the turn
	// driver's failure, or task::cancelled when `token` is requested first; a cancelled wait
	// leaves its record staged, written with its turn.
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
	// a driver the other holds.
	enum class Holder : std::uint8_t { none, driver, entry };
	Holder holder() const noexcept { return holder_; }
	struct Hold {
		TurnDriver& turns;
		Hold(TurnDriver& driver, Holder holder) : turns(driver) {
			if (turns.holder_ != Holder::none) { throw std::logic_error("the turn driver is held by another driver"); }
			turns.holder_ = holder;
		}
		Hold(const Hold&) = delete;
		Hold& operator=(const Hold&) = delete;
		~Hold() { turns.holder_ = Holder::none; }
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
		bool await_ready() const noexcept { return token.requested() || turns.satisfied(waiter); }
		void await_suspend(std::coroutine_handle<> handle) {
			waiter.handle = handle; waiter.home = task::current();
			if (!waiter.home) { throw std::logic_error("waiting on a turn driver needs a running scheduler"); }
			turns.link(waiter);
			callback.emplace(token, Cancel{&waiter});
		}
		void await_resume() {
			if (waiter.state.load(std::memory_order_acquire) == detail::TurnWaiter::cancelled || (token.requested() && !turns.satisfied(waiter))) { throw task::cancelled{}; }
			if (waiter.group && turns.log_failed_) { std::rethrow_exception(turns.failure_); }
		}
		struct Cancel {
			detail::TurnWaiter* waiter;
			void operator()() const noexcept {
				if (!waiter->claim(detail::TurnWaiter::cancelled)) { return; }
				if (!waiter->home.post_call(&hop, waiter)) { hop(waiter); }
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
		if (!waiter.group) { return !pending_.empty(); }
		return log_failed_ || log_.sequence(waiter.group) >= waiter.sequence;
	}
	void link(detail::TurnWaiter& waiter) noexcept {
		waiter.owner = this; waiter.prev = nullptr; waiter.next = waiters_;
		if (waiters_) { waiters_->prev = &waiter; }
		waiters_ = &waiter;
	}
	void unlink(detail::TurnWaiter& waiter) noexcept {
		(waiter.prev ? waiter.prev->next : waiters_) = waiter.next;
		if (waiter.next) { waiter.next->prev = waiter.prev; }
		waiter.owner = nullptr; waiter.prev = waiter.next = nullptr;
	}
	// Unlinks every waiter `ready` accepts, then resumes those whose claim it wins.
	template <class Ready> void wake(Ready ready) noexcept {
		for (auto* waiter = waiters_; waiter;) {
			auto* next = waiter->next;
			if (ready(*waiter)) {
				unlink(*waiter);
				if (waiter->claim(detail::TurnWaiter::completed)) { waiter->home.post(waiter->handle); }
			}
			waiter = next;
		}
	}
	const TurnLog::TurnWrite& start() {
		if (write_) { throw std::logic_error("a turn is already in flight"); }
		if (pending_.empty()) { throw std::logic_error("no records to write"); }
		std::vector<TurnRecord> records; records.reserve(pending_.size());
		for (auto& item : pending_) { records.push_back(std::move(item.record)); }
		flight_ = std::move(pending_); pending_.clear();
		try { write_ = log_.stage(records); }
		catch (...) { fail(std::current_exception()); throw; }
		for (auto& item : flight_) { if (item.operation) { item.operation->submitted(); item.submitted = true; } }
		return *write_;
	}

	struct Pending { TurnRecord record; std::shared_ptr<IOOperation> operation; MutationToken token; bool submitted = false; };
	static void finish(Pending& item, std::exception_ptr error) noexcept {
		if (!item.operation) { return; }
		MutationCompletion completion; completion.token = item.token; completion.error = error;
		item.operation->complete(std::move(completion));
	}
	void fail(std::exception_ptr error) noexcept {
		log_failed_ = true; failure_ = error ? error : std::make_exception_ptr(std::logic_error("turn driver failed"));
		auto flight = std::move(flight_); flight_.clear();
		for (auto& item : pending_) { flight.push_back(std::move(item)); }
		pending_.clear();
		// An operation not yet submitted is submitted first, so failing it settles it.
		for (auto& item : flight) {
			if (!item.operation) { continue; }
			if (!item.submitted) { try { item.operation->submitted(); } catch (...) { continue; } }
			finish(item, error);
		}
		wake([](const detail::TurnWaiter& waiter) { return waiter.group != 0; });
	}

	IO& io_;
	Identity identity_;
	TurnRecovery recovered_;
	TurnLog log_;
	std::vector<Pending> pending_, flight_;
	std::optional<TurnLog::TurnWrite> write_;
	detail::TurnWaiter* waiters_ = nullptr;
	std::exception_ptr failure_;
	Holder holder_ = Holder::none;
	bool log_failed_ = false;
};

}  // namespace kronuz::journal
