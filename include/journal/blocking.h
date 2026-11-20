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

// The blocking entry (async-drivers D5): runs a task on the calling thread with no event loop,
// holding the turn driver, and commits each staged turn inline. It returns only after every
// staged record is written, and throws `stalled` on a lost wake instead of waiting forever.

#pragma once

#include "turn_driver.h"
#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace kronuz::journal {

// A lost wake: the task waits on something nothing in the entry can complete. A defect in a
// driver or its machine, not a condition of the run, so not retryable. `groups` names the
// groups that held a waiter.
struct stalled : std::logic_error {
	explicit stalled(std::vector<std::uint64_t> waiting) : std::logic_error(message(waiting)), groups(std::move(waiting)) {}
	std::vector<std::uint64_t> groups;
private:
	static std::string message(const std::vector<std::uint64_t>& groups) {
		std::string text = "blocking entry stalled: a lost wake, waiting groups [";
		for (std::size_t i = 0; i < groups.size(); ++i) { text += (i ? ", " : "") + std::to_string(groups[i]); }
		return text + "]";
	}
};

namespace detail {
// The entry's root frame, which the entry owns and destroys: a stalled task's chain is
// destroyed with it, unlinking its waiters.
struct EntryRoot {
	struct promise_type {
		EntryRoot get_return_object() noexcept { return {std::coroutine_handle<promise_type>::from_promise(*this)}; }
		std::suspend_always initial_suspend() noexcept { return {}; }
		std::suspend_always final_suspend() noexcept { return {}; }
		void return_void() noexcept {}
		void unhandled_exception() noexcept { std::terminate(); }
	};
	std::coroutine_handle<promise_type> handle;
};
template <class T> struct EntryResult {
	std::optional<std::conditional_t<std::is_void_v<T>, std::monostate, T>> value;
	std::exception_ptr error;
	bool finished = false;
};
template <class T> EntryRoot entry_root(task::task<T> work, EntryResult<T>& result) {
	try {
		if constexpr (std::is_void_v<T>) { co_await std::move(work); result.value.emplace(); }
		else { result.value.emplace(co_await std::move(work)); }
	} catch (...) { result.error = std::current_exception(); }
	result.finished = true;
}
}  // namespace detail

// Runs `work` on `scheduler`, driven by the calling thread, holding `turns` (a driver run()
// holds is refused). Each pass runs the scheduler until its queue is empty; staged records
// are then committed inline; a finished task returns only once nothing is staged; otherwise,
// with an operation deadline armed it sleeps toward it (a post from another thread, such as a
// cancel's hop or a wake(), cuts the sleep short), and with none it throws `stalled`. A turn that fails
// is the task's to see; if the task succeeds anyway, the entry throws the turn's failure.
// After `stalled`, the scheduler may still queue a cancel's hop into the destroyed task: the
// caller drops it and never drives it again.
template <class T> T run(TurnDriver& turns, task::blocking_scheduler& scheduler, task::task<T> work) {
	TurnDriver::Hold hold(turns, TurnDriver::Holder::entry, task::scheduler_ref::of(scheduler));
	detail::EntryResult<T> result;
	auto root = detail::entry_root(std::move(work), result);
	struct Destroy { std::coroutine_handle<> handle; ~Destroy() { handle.destroy(); } } destroy{root.handle};
	std::exception_ptr turn_error;
	task::scheduler_ref::of(scheduler).post(root.handle);
	for (;;) {
		const bool settled = scheduler.drive([&] { return result.finished || !turns.idle(); });
		if (!turns.idle()) {
			try { turns.commit(); } catch (...) { if (!turn_error) { turn_error = std::current_exception(); } }
			continue;
		}
		if (result.finished) { break; }
		if (!settled) { throw stalled(turns.waiting_groups()); }
	}
	if (result.error) { std::rethrow_exception(result.error); }
	if (turn_error) { std::rethrow_exception(turn_error); }
	if constexpr (!std::is_void_v<T>) { return std::move(*result.value); }
}
// The same, on a scheduler of its own.
template <class T> T run(TurnDriver& turns, task::task<T> work) {
	task::blocking_scheduler scheduler;
	return run(turns, scheduler, std::move(work));
}

}  // namespace kronuz::journal
