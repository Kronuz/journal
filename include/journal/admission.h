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
inline StorageResources resources_min(StorageResources a, StorageResources b) noexcept {
	return {std::min(a.logical_bytes, b.logical_bytes), std::min(a.entries, b.entries)};
}
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
	// Replacement's claim on control's available bytes after a census (docs/mechanisms/admission.md).
	StorageResources claim;
	StorageResources free() const {
		auto charged = resources_add(resources_add(used(), outstanding()), resources_add(control_available, replacement_available));
		if (!resources_fit(charged, limits.hard)) { return {}; }
		return resources_subtract(limits.hard, charged);
	}
	void refill() {
		for (auto pair : {std::pair{&control_available, resources_subtract(limits.control_pool, control_held)}, std::pair{&replacement_available, resources_subtract(limits.replacement_pool, replacement_held)}}) {
			*pair.first = resources_add(*pair.first, resources_min(free(), resources_subtract(pair.second, *pair.first)));
		}
		claim = resources_min(claim, resources_subtract(limits.replacement_pool, resources_add(replacement_held, replacement_available)));
	}
	// The part of `need` a Replacement takes from control: what its pool lacks, within the claim.
	StorageResources claimed(StorageResources need) const {
		return resources_min(resources_min(claim, control_available), resources_subtract(need, resources_min(need, replacement_available)));
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
		: state_(std::move(other.state_)), reservation_(std::move(other.reservation_)), class_(other.class_), claimed_(other.claimed_), started_(other.started_) {}
	StoragePermit& operator=(StoragePermit&& other) noexcept {
		if (this != &other) { release(); state_ = std::move(other.state_); reservation_ = std::move(other.reservation_); class_ = other.class_; claimed_ = other.claimed_; started_ = other.started_; }
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
			auto refund = refund_values(detail::resources_subtract(peak(), added), false);
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
	StoragePermit(std::shared_ptr<detail::AdmissionState> state, allocators::Reservation<2> reservation, AdmissionClass kind, StorageResources claimed)
		: state_(std::move(state)), reservation_(std::move(reservation)), class_(kind), claimed_(claimed) {}
	struct Refund {
		StorageResources control_held, control_available, replacement_held, replacement_available, claim;
	};
	// A cancellation returns what the permit claimed to control, as far as control lacks it; a
	// settlement leaves it replacement's.
	Refund refund_values(StorageResources unused, bool cancelled) const {
		Refund value{state_->control_held, state_->control_available, state_->replacement_held, state_->replacement_available, state_->claim};
		if (class_ == AdmissionClass::Control) {
			value.control_held = detail::resources_subtract(value.control_held, peak());
			value.control_available = detail::resources_add(value.control_available, unused);
		}
		if (class_ == AdmissionClass::Replacement) {
			auto lacking = detail::resources_subtract(state_->limits.control_pool, detail::resources_add(value.control_held, value.control_available));
			auto returned = cancelled ? detail::resources_min(claimed_, lacking) : StorageResources{};
			value.replacement_held = detail::resources_subtract(value.replacement_held, peak());
			value.replacement_available = detail::resources_add(value.replacement_available, detail::resources_subtract(unused, returned));
			value.control_available = detail::resources_add(value.control_available, returned);
			value.claim = detail::resources_add(value.claim, returned);
		}
		return value;
	}
	void apply_refund(const Refund &value) noexcept {
		state_->control_held = value.control_held;
		state_->control_available = value.control_available;
		state_->replacement_held = value.replacement_held;
		state_->replacement_available = value.replacement_available;
		state_->claim = value.claim;
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
			try { auto refund = refund_values(peak(), true); state_->book->cancel(reservation_); apply_refund(refund); state_->refill(); }
			catch (...) { state_->tainted = true; }
		}
		reservation_.abandon();
		finish(); state_.reset();
	}
	std::shared_ptr<detail::AdmissionState> state_;
	allocators::Reservation<2> reservation_;
	AdmissionClass class_;
	StorageResources claimed_;
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
		state_->book.emplace(detail::resource_vector(limits.hard), detail::resource_vector({inventory.logical_bytes, inventory.entries}));
		// The census cannot tell which pool a deficit came from: control is funded first, and
		// replacement claims what replacement-first funding would have given it beyond that.
		auto funding = state_->free(); state_->refill();
		state_->claim = detail::resources_subtract(detail::resources_min(limits.replacement_pool, funding), state_->replacement_available);
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
		auto control_available = state_->control_available;
		auto replacement_available = state_->replacement_available;
		auto claim = state_->claim;
		StorageResources taken;
		if (kind == AdmissionClass::Replacement) {
			taken = state_->claimed(peak);
			control_available = detail::resources_subtract(control_available, taken);
			replacement_available = detail::resources_add(replacement_available, taken);
			claim = detail::resources_subtract(claim, taken);
		}
		auto available = kind == AdmissionClass::Normal ? state_->free() :
			(kind == AdmissionClass::Control ? control_available : replacement_available);
		if ((kind == AdmissionClass::Normal && !state_->stats().normal_ready) ||
			(kind == AdmissionClass::Replacement && state_->replacement_active) || !detail::resources_fit(peak, available)) { return std::nullopt; }
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
		state_->claim = claim;
		if (kind == AdmissionClass::Control) { ++state_->active_control; }
		if (kind == AdmissionClass::Replacement) { state_->replacement_active = true; }
		++state_->tickets; return StoragePermit(state_, std::move(*reservation), kind, taken);
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
		auto taken = state_->claimed(delta);
		auto spendable = detail::resources_add(state_->replacement_available, taken);
		if (!detail::resources_fit(delta, spendable)) { return false; }
		auto held = detail::resources_add(state_->replacement_held, delta);
		if (!state_->book->try_grow(permit.reservation_, detail::resource_vector(target), detail::resource_vector(spendable))) { return false; }
		state_->control_available = detail::resources_subtract(state_->control_available, taken);
		state_->claim = detail::resources_subtract(state_->claim, taken);
		permit.claimed_ = detail::resources_add(permit.claimed_, taken);
		state_->replacement_held = held;
		state_->replacement_available = detail::resources_subtract(spendable, delta);
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
