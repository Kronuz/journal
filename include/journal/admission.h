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

#pragma once

#include "inventory.h"
#include "resources.h"
#include <algorithm>
#include <memory>
#include <utility>

namespace kronuz::journal {

enum class AdmissionClass { Normal, Control, Replacement };
struct AdmissionLimits {
	StorageResources hard, control_pool, replacement_pool;
	std::size_t maximum_tickets = 64;
	std::size_t control_slots = 1;
};
struct AdmissionStats {
	StorageResources used, control_available, replacement_available, outstanding;
	std::size_t tickets = 0;
	bool tainted = false, normal_ready = false, over_limit = false;
};
namespace detail {
struct AdmissionState {
	AdmissionLimits limits;
	StorageResources control_available, replacement_available;
	std::size_t tickets = 0;
	bool tainted = false;
	std::optional<allocators::ReservationBook<2>> book;
	StorageResources used() const noexcept { return storage_resources(book->used()); }
	StorageResources outstanding() const noexcept { return storage_resources(book->reserved()); }
	StorageResources control_held, replacement_held;
	bool replacement_active = false;
	std::size_t active_control = 0;
	StorageResources free() const {
		auto charged = resources_add(resources_add(used(), outstanding()), resources_add(control_available, replacement_available));
		if (!resources_fit(charged, limits.hard)) { return {}; }
		return resources_subtract(limits.hard, charged);
	}
	void refill() {
		for (auto pair : {std::pair{&control_available, resources_subtract(limits.control_pool, control_held)}, std::pair{&replacement_available, resources_subtract(limits.replacement_pool, replacement_held)}}) {
			auto available = free(); auto needed = resources_subtract(pair.second, *pair.first);
			*pair.first = resources_add(*pair.first, {std::min(available.logical_bytes, needed.logical_bytes), std::min(available.entries, needed.entries)});
		}
	}
	AdmissionStats stats() const noexcept {
		AdmissionStats result{used(), control_available, replacement_available, outstanding(), tickets, tainted};
		result.over_limit = !resources_fit(used(), limits.hard);
		// Held protected permits remain funded through outstanding peaks.
		// Preparation can interleave with normal work using only free capacity.
		result.normal_ready = !tainted && !result.over_limit &&
			StorageResources{control_available.logical_bytes + control_held.logical_bytes, control_available.entries + control_held.entries} == limits.control_pool &&
			StorageResources{replacement_available.logical_bytes + replacement_held.logical_bytes, replacement_available.entries + replacement_held.entries} == limits.replacement_pool;
		return result;
	}
};
} // namespace detail

class Admission;
// Move-only permits retain their ledger state. All operations, including
// destruction, run on the owning storage executor. No IO happens here.
class StoragePermit {
public:
	StoragePermit(const StoragePermit&) = delete;
	StoragePermit& operator=(const StoragePermit&) = delete;
	StoragePermit(StoragePermit&& other) noexcept
		: state_(std::move(other.state_)), reservation_(std::move(other.reservation_)), class_(other.class_), started_(other.started_) {}
	StoragePermit& operator=(StoragePermit&& other) noexcept {
		if (this != &other) { release(); state_ = std::move(other.state_); reservation_ = std::move(other.reservation_); class_ = other.class_; started_ = other.started_; }
		return *this;
	}
	~StoragePermit() { release(); }
	StorageResources peak() const noexcept { return detail::storage_resources(reservation_.peak()); }
	void mark_started() {
		if (!state_ || state_->tainted) { throw std::logic_error("storage permit unavailable"); }
		started_ = true;
	}
	// added/removed describe final charged additions and already durable
	// removals, not cumulative transient creation traffic. Reserve the worst
	// simultaneous additions BEFORE IO. Removed resources must be charged
	// existing resources, and are never credited twice.
	void settle(StorageResources added, StorageResources removed = {}) {
		if (!state_ || !started_ || state_->tainted) { throw std::logic_error("storage permit cannot settle"); }
		if (!detail::resources_fit(added, peak()) || !detail::resources_fit(removed, state_->used())) {
			state_->tainted = true; throw std::invalid_argument("settlement exceeds storage reservation or charged removals");
		}
		auto state = state_;
		try {
			auto refund = refund_values(detail::resources_subtract(peak(), added));
			state_->book->settle(reservation_, detail::resource_vector(added), detail::resource_vector(removed));
			apply_refund(refund);
			finish(); state_.reset(); state->refill();
		} catch (...) { state->tainted = true; throw; }
	}
	// Explicit uncertainty, or destruction after mark_started without settle,
	// prevents further admission. Reserved capacity is not silently refunded.
	void abandon() noexcept { if (state_) { started_ = true; release(); } }
private:
	friend class Admission;
	StoragePermit(std::shared_ptr<detail::AdmissionState> state, allocators::Reservation<2> reservation, AdmissionClass kind)
		: state_(std::move(state)), reservation_(std::move(reservation)), class_(kind) {}
	struct Refund {
		StorageResources control_held, control_available, replacement_held, replacement_available;
	};
	Refund refund_values(StorageResources unused) const {
		Refund value{state_->control_held, state_->control_available, state_->replacement_held, state_->replacement_available};
		if (class_ == AdmissionClass::Control) {
			value.control_held = detail::resources_subtract(value.control_held, peak());
			value.control_available = detail::resources_add(value.control_available, unused);
		}
		if (class_ == AdmissionClass::Replacement) {
			value.replacement_held = detail::resources_subtract(value.replacement_held, peak());
			value.replacement_available = detail::resources_add(value.replacement_available, unused);
		}
		return value;
	}
	void apply_refund(const Refund &value) noexcept {
		state_->control_held = value.control_held;
		state_->control_available = value.control_available;
		state_->replacement_held = value.replacement_held;
		state_->replacement_available = value.replacement_available;
	}
	void finish() noexcept {
		--state_->tickets;
		if (class_ == AdmissionClass::Control) { --state_->active_control; }
		if (class_ == AdmissionClass::Replacement) { state_->replacement_active = false; }
	}
	void release() noexcept {
		if (!state_) { return; }
		if (started_) { state_->tainted = true; }
		else {
			try { auto refund = refund_values(peak()); state_->book->cancel(reservation_); apply_refund(refund); state_->refill(); }
			catch (...) { state_->tainted = true; }
		}
		reservation_.abandon();
		finish(); state_.reset();
	}
	std::shared_ptr<detail::AdmissionState> state_;
	allocators::Reservation<2> reservation_;
	AdmissionClass class_;
	bool started_ = false;
};

// Accounting only: the host must use permits for EVERY mutation and fence
// Journal on actual IO failure. Rebuild after close/recovery/quiescent census;
// never reset a tainted live ledger or credit undurable/unperformed deletion.
class Admission {
public:
	Admission(const InventoryStats& inventory, AdmissionLimits limits) {
		if (!inventory.ready() || limits.maximum_tickets < 3 || limits.maximum_tickets > 4096 ||
			limits.control_slots == 0 || limits.control_slots > limits.maximum_tickets - 2 ||
			!detail::resources_fit(detail::resources_add(limits.control_pool, limits.replacement_pool), limits.hard)) {
			throw std::invalid_argument("unready census or invalid storage admission limits");
		}
		state_ = std::make_shared<detail::AdmissionState>(); state_->limits = limits;
		state_->book.emplace(detail::resource_vector(limits.hard), detail::resource_vector({inventory.logical_bytes, inventory.entries})); state_->refill();
	}
	Admission(const Admission&) = delete;
	Admission& operator=(const Admission&) = delete;
	AdmissionStats stats() const noexcept { return state_->stats(); }
	std::optional<StoragePermit> reserve(AdmissionClass kind, StorageResources peak) {
		if (kind != AdmissionClass::Normal && kind != AdmissionClass::Control && kind != AdmissionClass::Replacement) {
			throw std::invalid_argument("invalid storage admission class");
		}
		if (peak == StorageResources{} || state_->tainted || state_->tickets == state_->limits.maximum_tickets) { return std::nullopt; }
		// Already held protected permits occupy their reserved slots. Preserve
		// only the remaining protection when admitting ordinary work.
		auto remaining_control = state_->limits.control_slots - std::min(state_->limits.control_slots, state_->active_control);
		auto remaining_replacement = state_->replacement_active ? 0u : 1u;
		if ((kind == AdmissionClass::Normal && state_->tickets >= state_->limits.maximum_tickets - remaining_control - remaining_replacement) ||
			(kind == AdmissionClass::Control && state_->tickets >= state_->limits.maximum_tickets - (state_->replacement_active ? 0 : 1))) { return std::nullopt; }
		auto available = kind == AdmissionClass::Normal ? state_->free() :
			(kind == AdmissionClass::Control ? state_->control_available : state_->replacement_available);
		if ((kind == AdmissionClass::Normal && !state_->stats().normal_ready) ||
			(kind == AdmissionClass::Replacement && state_->replacement_active) || !detail::resources_fit(peak, available)) { return std::nullopt; }
		auto control_available = state_->control_available;
		auto replacement_available = state_->replacement_available;
		auto control_held = state_->control_held;
		auto replacement_held = state_->replacement_held;
		if (kind == AdmissionClass::Control) {
			control_available = detail::resources_subtract(available, peak);
			control_held = detail::resources_add(control_held, peak);
		}
		if (kind == AdmissionClass::Replacement) {
			replacement_available = detail::resources_subtract(available, peak);
			replacement_held = detail::resources_add(replacement_held, peak);
		}
		auto reservation = state_->book->try_reserve(detail::resource_vector(peak), detail::resource_vector(available));
		if (!reservation) { return std::nullopt; }
		state_->control_available = control_available;
		state_->replacement_available = replacement_available;
		state_->control_held = control_held;
		state_->replacement_held = replacement_held;
		if (kind == AdmissionClass::Control) { ++state_->active_control; }
		if (kind == AdmissionClass::Replacement) { state_->replacement_active = true; }
		++state_->tickets; return StoragePermit(state_, std::move(*reservation), kind);
	}
	// Grow the same protected ticket, including after known preparation IO.
	bool grow_replacement(StoragePermit& permit, StorageResources minimum) {
		if (permit.state_ != state_ || permit.class_ != AdmissionClass::Replacement) {
			throw std::invalid_argument("foreign or non-replacement storage permit");
		}
		if (state_->tainted) { throw std::logic_error("storage admission tainted"); }
		StorageResources target{std::max(minimum.logical_bytes, permit.peak().logical_bytes),
			std::max(minimum.entries, permit.peak().entries)};
		auto delta = detail::resources_subtract(target, permit.peak());
		if (!detail::resources_fit(delta, state_->replacement_available)) { return false; }
		auto held = detail::resources_add(state_->replacement_held, delta);
		auto available = detail::resources_subtract(state_->replacement_available, delta);
		if (!state_->book->try_grow(permit.reservation_, detail::resource_vector(target), detail::resource_vector(state_->replacement_available))) { return false; }
		state_->replacement_held = held;
		state_->replacement_available = available;
		return true;
	}
	void credit_durable_reclaim(StorageResources removed) {
		if (state_->tainted) { throw std::logic_error("storage admission tainted"); }
		if (!detail::resources_fit(removed, state_->used())) {
			state_->tainted = true; throw std::invalid_argument("reclamation exceeds charged storage");
		}
		state_->book->reclaim(detail::resource_vector(removed)); state_->refill();
	}
	void taint() noexcept { state_->tainted = true; }
private:
	std::shared_ptr<detail::AdmissionState> state_;
};

} // namespace kronuz::journal
