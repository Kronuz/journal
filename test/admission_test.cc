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

#include "journal/admission.h"
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace kronuz::journal;
namespace {
int checks = 0, failures = 0;
void check(bool value, std::string_view message) { ++checks; if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
template <class F> bool throws(F&& function) { try { function(); return false; } catch (const std::exception&) { return true; } }
InventoryStats census(StorageResources used) { InventoryStats result; result.complete = true; result.logical_bytes = used.logical_bytes; result.entries = used.entries; return result; }
const AdmissionLimits limits{{1000, 100}, {100, 10}, {300, 30}, 8};
void bounded(const Admission& admission) {
	auto stats = admission.stats();
	auto total = detail::resources_add(detail::resources_add(stats.used, stats.outstanding), detail::resources_add(stats.control_available, stats.replacement_available));
	check(detail::resources_fit(total, limits.hard) && stats.tickets <= limits.maximum_tickets, "used protected and outstanding capacity obey both hard limits");
}
void pools() {
	Admission ledger(census({100, 10}), limits); check(ledger.stats().normal_ready, "funded startup allows normal admission");
	check(!ledger.reserve(AdmissionClass::Normal, {501, 0}) && !ledger.reserve(AdmissionClass::Normal, {0, 51}), "normal work cannot steal either protected pool");
	auto normal = ledger.reserve(AdmissionClass::Normal, {500, 50}); check(normal.has_value(), "normal may exhaust unprotected capacity");
	auto control = ledger.reserve(AdmissionClass::Control, {100, 10}); auto replacement = ledger.reserve(AdmissionClass::Replacement, {300, 30});
	check(control && replacement && !ledger.reserve(AdmissionClass::Replacement, {1, 1}), "exhausted normal work preserves distinct control and one replacement cycle"); bounded(ledger);
	check(ledger.stats().normal_ready && !ledger.reserve(AdmissionClass::Normal, {1, 1}), "held protection remains funded but leaves no unprotected capacity here");
	normal->mark_started(); normal->settle({500, 50}); control->mark_started(); control->settle({20, 2}); replacement->mark_started(); replacement->settle({100, 10}, {100, 10});
	check(ledger.stats().used == StorageResources{620, 62} && !ledger.stats().normal_ready, "actual additions stay charged and only known removals receive credit"); bounded(ledger);
	ledger.credit_durable_reclaim({120, 12}); check(ledger.stats().normal_ready, "durable deletion refills both protected pools"); bounded(ledger);
	{
		auto permit = ledger.reserve(AdmissionClass::Control, {100, 10});
		ledger.credit_durable_reclaim({100, 10});
		check(ledger.stats().control_available == StorageResources{}, "refill never funds capacity already held by a permit twice"); bounded(ledger);
	}
	check(ledger.stats().control_available == limits.control_pool, "pre-IO cancellation returns its originating protected reservation"); bounded(ledger);
	Admission pressure(census({950, 95}), limits);
	check(!pressure.stats().normal_ready && pressure.stats().control_available == StorageResources{50, 5} && pressure.stats().replacement_available == StorageResources{}, "partial startup funds control first and reports pressure");
	check(!pressure.reserve(AdmissionClass::Normal, {1, 1}), "pressure refuses new normal work");
	{
		auto claimed = pressure.reserve(AdmissionClass::Replacement, {1, 1});
		check(claimed && pressure.stats().control_available == StorageResources{49, 4}, "after a census replacement work spends its claim on control");
	}
	check(pressure.stats().control_available == StorageResources{50, 5} && pressure.stats().replacement_available == StorageResources{}, "its cancellation returns the claim to control");
	{ auto partial = pressure.reserve(AdmissionClass::Control, {50, 5}); check(partial.has_value(), "funded partial control permit remains usable"); bounded(pressure); }
	for (auto over : {StorageResources{1001, 0}, StorageResources{0, 101}, StorageResources{std::numeric_limits<std::uint64_t>::max(), 1}}) {
		Admission startup(census(over), limits);
		check(startup.stats().over_limit && !startup.stats().normal_ready && !startup.reserve(AdmissionClass::Control, {0, 1}) && !startup.reserve(AdmissionClass::Replacement, {1, 0}), "above-limit startup cannot make positive reservations in either dimension");
	}
}
void uncertainty() {
	Admission clean(census({100, 10}), limits);
	{ auto ticket = clean.reserve(AdmissionClass::Normal, {10, 1}); }
	check(!clean.stats().tainted && clean.stats().outstanding == StorageResources{}, "unused tickets cancel before IO");
	{ auto ticket = clean.reserve(AdmissionClass::Normal, {10, 1}); ticket->mark_started(); }
	check(clean.stats().tainted && clean.stats().outstanding == StorageResources{10, 1} && !clean.reserve(AdmissionClass::Control, {1, 1}), "lost completion taints accounting and retains uncertain reservation");
	std::optional<StoragePermit> detached_uncertain;
	{
		Admission temporary(census({100, 10}), limits);
		detached_uncertain = temporary.reserve(AdmissionClass::Normal, {10, 1});
	}
	auto detached_peak = detached_uncertain->peak();
	detached_uncertain->abandon();
	check(detached_uncertain->peak() == detached_peak,
		"explicit abandonment preserves permit metadata after facade destruction");
	Admission explicit_failure(census({100, 10}), limits); auto failure = explicit_failure.reserve(AdmissionClass::Normal, {10, 1}); failure->abandon();
	check(explicit_failure.stats().tainted, "explicit uncertain failure stops admission");
	Admission invalid(census({100, 10}), limits); auto ticket = invalid.reserve(AdmissionClass::Normal, {10, 1});
	check(throws([&] { ticket->settle({1, 1}); }) && !invalid.stats().tainted, "settlement without starting has no IO accounting effect");
	ticket->mark_started(); check(throws([&] { ticket->settle({11, 1}); }) && invalid.stats().tainted, "over-reservation settlement taints accounting");
	Admission double_credit(census({100, 10}), limits);
	check(throws([&] { double_credit.credit_durable_reclaim({101, 1}); }) && double_credit.stats().tainted, "credit exceeding charged resources fails closed");
	Admission moved(census({100, 10}), limits);
	{
		auto first = moved.reserve(AdmissionClass::Control, {20, 2}); auto second = moved.reserve(AdmissionClass::Normal, {20, 2});
		check(second.has_value(), "held control protection allows normal work from unprotected capacity"); second.reset();
		auto destination = moved.reserve(AdmissionClass::Control, {20, 2}); *destination = std::move(*first);
		check(moved.stats().tickets == 1 && moved.stats().outstanding == StorageResources{20, 2}, "move assignment cancels old pre-IO reservation once");
	}
	check(!moved.stats().tainted && moved.stats().tickets == 0, "moved-from destruction cannot double refund");
	std::optional<StoragePermit> escaped;
	{ Admission temporary(census({100, 10}), limits); escaped = temporary.reserve(AdmissionClass::Normal, {10, 1}); }
	Admission recovered(census({200, 20}), limits);
	escaped->mark_started(); escaped->settle({5, 1});
	check(recovered.stats().used == StorageResources{200, 20} && throws([&] { escaped->mark_started(); }), "late settlement outlives old facade without crediting a recovered session");
	InventoryStats unready; check(throws([&] { Admission rejected(unready, limits); }), "only ready census initializes an accounting session");
	auto invalid_limits = limits; invalid_limits.replacement_pool = {901, 91};
	check(throws([&] { Admission rejected(census({0, 0}), invalid_limits); }), "protected pool sums cannot exceed hard limits");
	auto invalid_count = limits; invalid_count.maximum_tickets = 2;
	check(throws([&] { Admission rejected(census({0, 0}), invalid_count); }), "permit bound must fund a slot for each admission class");
}
void slots() {
	auto small = limits; small.maximum_tickets = 3;
	{
		Admission mixed(census({100, 10}), small);
		auto control = mixed.reserve(AdmissionClass::Control, {1, 1});
		auto replacement = mixed.reserve(AdmissionClass::Replacement, {1, 1});
		auto normal = mixed.reserve(AdmissionClass::Normal, {1, 1});
		check(control && replacement && normal, "occupied protected slots do not block the remaining normal slot");
	}
	for (bool replacement_first : {false, true}) {
		Admission ledger(census({100, 10}), small);
		auto normal = ledger.reserve(AdmissionClass::Normal, {1, 1});
		check(normal && !ledger.reserve(AdmissionClass::Normal, {1, 1}), "normal ticket saturation preserves two protected slots");
		auto first = ledger.reserve(replacement_first ? AdmissionClass::Replacement : AdmissionClass::Control, {1, 1});
		auto second = ledger.reserve(replacement_first ? AdmissionClass::Control : AdmissionClass::Replacement, {1, 1});
		check(first && second && ledger.stats().tickets == 3 && !ledger.reserve(AdmissionClass::Control, {1, 1}), "either protected admission order fits after normal saturation");
		first->mark_started(); first->settle({1, 1}); second.reset(); normal.reset();
		check(ledger.stats().tickets == 0 && !ledger.stats().tainted, "protected completion and cancellation return ticket slots once");
	}
	Admission ledger(census({100, 10}), small);
	auto first = ledger.reserve(AdmissionClass::Control, {1, 1}); auto second = ledger.reserve(AdmissionClass::Control, {1, 1});
	check(first && second && !ledger.reserve(AdmissionClass::Control, {1, 1}), "control ticket saturation preserves replacement's slot");
	auto replacement = ledger.reserve(AdmissionClass::Replacement, {1, 1});
	check(replacement && ledger.stats().tickets == 3, "replacement retains its slot after control saturation"); replacement.reset();
	check(!ledger.reserve(AdmissionClass::Control, {1, 1}) && ledger.reserve(AdmissionClass::Replacement, {1, 1}), "released replacement slot cannot be stolen by another control ticket");
}
void interleaved_replacement() {
	Admission ledger(census({100, 10}), limits);
	auto replacement = ledger.reserve(AdmissionClass::Replacement, {300, 30});
	check(replacement && ledger.stats().normal_ready, "fully reserved replacement remains funded during preparation");
	auto normal = ledger.reserve(AdmissionClass::Normal, {500, 50});
	check(normal && !ledger.reserve(AdmissionClass::Normal, {1, 1}), "interleaved appends consume only remaining unprotected capacity"); bounded(ledger);
	replacement->mark_started(); replacement->settle({300, 30});
	check(!ledger.stats().normal_ready && !ledger.reserve(AdmissionClass::Normal, {1, 1}), "settled replacement pauses normal work when consumed protection cannot refill"); bounded(ledger);
	normal->mark_started(); normal->settle({500, 50});
	ledger.credit_durable_reclaim({300, 30});
	check(ledger.stats().normal_ready && ledger.stats().replacement_available == limits.replacement_pool, "durable cleanup restores replacement protection after interleaving"); bounded(ledger);
}
void control_pack_slots() {
	auto configured = limits; configured.maximum_tickets = 6; configured.control_slots = 3;
	for (bool replacement_first : {false, true}) {
		Admission ledger(census({100, 10}), configured);
		auto normal = ledger.reserve(AdmissionClass::Normal, {1, 1});
		auto second = ledger.reserve(AdmissionClass::Normal, {1, 1});
		check(normal && second && !ledger.reserve(AdmissionClass::Normal, {1, 1}), "normal saturation preserves a complete three-write control pack and replacement");
		std::optional<StoragePermit> replacement;
		if (replacement_first) { replacement = ledger.reserve(AdmissionClass::Replacement, {1, 1}); }
		std::vector<StoragePermit> pack;
		for (unsigned i = 0; i < 3; ++i) {
			auto permit = ledger.reserve(AdmissionClass::Control, {1, 1});
			check(permit.has_value(), "every control continuation fits after normal saturation");
			if (permit) { pack.push_back(std::move(*permit)); }
		}
		if (!replacement_first) { replacement = ledger.reserve(AdmissionClass::Replacement, {1, 1}); }
		check(replacement && ledger.stats().tickets == 6, "either pack/replacement order preserves all protected slots");
		pack.clear();
		check(!ledger.reserve(AdmissionClass::Normal, {1, 1}), "canceled control pack restores protection before new normal admission");
		normal.reset(); second.reset(); replacement.reset();
		check(ledger.stats().tickets == 0 && !ledger.stats().tainted, "pack cancellation releases all counters once");
	}
	for (auto slots : {std::size_t{0}, std::size_t{5}}) {
		configured.control_slots = slots;
		check(throws([&] { Admission invalid(census({0, 0}), configured); }), "invalid protected slot configurations reject before use");
	}
	configured.maximum_tickets = 5; configured.control_slots = 3;
	{
		Admission minimum(census({100, 10}), configured);
		auto replacement = minimum.reserve(AdmissionClass::Replacement, {1, 1});
		auto one = minimum.reserve(AdmissionClass::Control, {1, 1});
		auto two = minimum.reserve(AdmissionClass::Control, {1, 1});
		auto three = minimum.reserve(AdmissionClass::Control, {1, 1});
		auto normal = minimum.reserve(AdmissionClass::Normal, {1, 1});
		check(replacement && one && two && three && normal, "five-slot minimum fits protected pack before normal admission");
		one->mark_started(); one->settle({1, 1});
		check(!minimum.reserve(AdmissionClass::Normal, {1, 1}) && minimum.reserve(AdmissionClass::Control, {1, 1}), "settled control slot remains reusable exclusively by protected work at saturation");
	}
	Admission lifecycle(census({100, 10}), configured);
	auto replacement = lifecycle.reserve(AdmissionClass::Replacement, {1, 1});
	auto first = lifecycle.reserve(AdmissionClass::Control, {1, 1});
	auto second = lifecycle.reserve(AdmissionClass::Control, {1, 1});
	*second = std::move(*first);
	auto normal = lifecycle.reserve(AdmissionClass::Normal, {1, 1});
	check(normal && !lifecycle.reserve(AdmissionClass::Normal, {1, 1}), "move assignment preserves exact remaining control protection");
	second->mark_started(); second->settle({1, 1});
	check(!lifecycle.reserve(AdmissionClass::Normal, {1, 1}), "settlement restores the vacant protected control slot");
	auto control = lifecycle.reserve(AdmissionClass::Control, {1, 1});
	control->mark_started(); control->abandon();
	check(lifecycle.stats().tainted && lifecycle.stats().tickets == 2, "abandonment releases its slot once while fencing uncertain accounting");
}
void replacement_growth() {
	Admission ledger(census({100, 10}), limits);
	auto permit = ledger.reserve(AdmissionClass::Replacement, {100, 10});
	check(permit && ledger.grow_replacement(*permit, {200, 20}), "replacement grows within its protected pool");
	check(permit->peak() == StorageResources{200, 20} && ledger.stats().tickets == 1,
		"growth preserves the same ticket");
	auto before = ledger.stats();
	check(!ledger.grow_replacement(*permit, {301, 31}), "growth cannot steal ordinary or control capacity");
	auto after = ledger.stats();
	check(after.outstanding == before.outstanding && after.replacement_available == before.replacement_available &&
		after.tickets == before.tickets && !after.tainted, "refused growth changes no accounting");
	check(ledger.grow_replacement(*permit, {150, 15}) && permit->peak() == StorageResources{200, 20},
		"growth never shrinks an existing permit");
	permit->mark_started();
	check(ledger.grow_replacement(*permit, {250, 25}), "known started preparation permits further funding");
	bounded(ledger);
	Admission other(census({100, 10}), limits);
	check(throws([&] { other.grow_replacement(*permit, {260, 26}); }), "foreign growth is rejected");
	auto normal = ledger.reserve(AdmissionClass::Normal, {10, 1});
	check(normal && throws([&] { ledger.grow_replacement(*normal, {20, 2}); }), "normal tickets cannot grow through replacement funding");
	normal.reset();
	permit->settle({120, 12});
	check(ledger.stats().tickets == 0 && ledger.stats().replacement_available == limits.replacement_pool,
		"settlement refunds the unused grown capacity");
	check(throws([&] { ledger.grow_replacement(*permit, {260, 26}); }), "settled growth is rejected");
	permit = ledger.reserve(AdmissionClass::Replacement, {100, 10});
	check(permit && ledger.grow_replacement(*permit, {300, 30}), "a fresh replacement can grow after settlement");
	permit.reset();
	check(ledger.stats().tickets == 0 && ledger.stats().replacement_available == limits.replacement_pool,
		"unstarted cancellation refunds all grown capacity");
	bounded(ledger);
}
// An owned store at its limit whose Control appends drained part of its control pool, then
// crashed: the recovered ledger still funds a whole replacement cycle and a checkpoint completes
// (docs/mechanisms/admission.md, Recovery).
void recovery() {
	struct Case { const char* name; std::uint64_t used, control, replacement, drained; };
	for (auto k : {Case{"R >= C", 1u << 20, 64u << 10, 512u << 10, 48u << 10}, Case{"R < C - c", 1u << 20, 256u << 10, 64u << 10, 250u << 10}}) {
		const std::string name = k.name;
		const AdmissionLimits owned{{k.used + k.control + k.replacement, 8192}, {k.control, 3}, {k.replacement, 4}, 64, 3};
		Admission before(census({k.used, 10}), owned);
		for (auto left = k.drained; left;) {
			auto step = std::min<std::uint64_t>(left, 4096); auto permit = before.reserve(AdmissionClass::Control, {step, 0});
			if (!permit) { check(false, name + ": control drains at the limit"); return; }
			permit->mark_started(); permit->settle({step, 0}); left -= step;
		}
		auto pre = before.stats();
		check(pre.control_available == StorageResources{k.control - k.drained, 3} && pre.replacement_available == StorageResources{k.replacement, 4},
			name + ": before the crash the drain is control's alone");
		check(before.reserve(AdmissionClass::Replacement, {k.replacement, 1}).has_value(), name + ": before the crash a whole replacement cycle is funded");
		Admission after(census(pre.used), owned);
		auto post = after.stats(); auto funding = k.control + k.replacement - k.drained, control = std::min(k.control, funding);
		check(post.used == pre.used && post.control_available == StorageResources{control, 3} &&
			post.replacement_available == StorageResources{funding - control, 4}, name + ": recovery funds control first, as it opens today");
		check(after.reserve(AdmissionClass::Control, {114, 0}).has_value(), name + ": the pre-Start pack is admitted");
		check(after.stats().control_available == StorageResources{control, 3}, name + ": the pack's cancellation refunds it");
		auto cycle = after.reserve(AdmissionClass::Replacement, {k.replacement, 1});
		check(cycle.has_value(), name + ": after the crash a whole replacement cycle is funded");
		if (!cycle) { continue; }
		auto held = after.stats();
		check(held.control_available == StorageResources{k.control - k.drained, 3} && held.replacement_available == StorageResources{0, 3} &&
			held.outstanding == StorageResources{k.replacement, 1}, name + ": the cycle takes the drained bytes back from control");
		cycle->mark_started(); cycle->settle({k.replacement / 2, 1});
		after.credit_durable_reclaim({k.replacement / 2 + k.drained, 1});
		auto done = after.stats();
		check(done.used == StorageResources{k.used, 10} && done.control_available == StorageResources{k.control, 3} &&
			done.replacement_available == StorageResources{k.replacement, 4} && done.normal_ready && !done.tainted,
			name + ": the checkpoint and its reclaim leave both pools whole");
	}
}
// The recovery claim's transitions by value (docs/mechanisms/admission.md, Recovery).
void claim() {
	Admission recovered(census({700, 70}), limits);
	check(recovered.stats().control_available == StorageResources{100, 10} && recovered.stats().replacement_available == StorageResources{200, 20},
		"a census funds control first");
	{
		auto cycle = recovered.reserve(AdmissionClass::Replacement, {250, 25});
		check(cycle && recovered.stats().control_available == StorageResources{50, 5} && recovered.stats().replacement_available == StorageResources{}, "a reservation spends its pool, then the claim");
		bounded(recovered);
	}
	check(recovered.stats().control_available == StorageResources{100, 10} && recovered.stats().replacement_available == StorageResources{200, 20} && !recovered.stats().tainted,
		"a cancelled reservation returns its claim to control");
	auto cycle = recovered.reserve(AdmissionClass::Replacement, {200, 20});
	check(cycle && recovered.stats().control_available == StorageResources{100, 10}, "a reservation its pool funds takes nothing from control");
	check(recovered.grow_replacement(*cycle, {300, 30}) && recovered.stats().control_available == StorageResources{}, "growth spends the claim");
	check(!recovered.grow_replacement(*cycle, {301, 31}) && cycle->peak() == StorageResources{300, 30}, "growth never passes the replacement pool");
	cycle->mark_started(); cycle->settle({150, 15});
	check(recovered.stats().control_available == StorageResources{} && recovered.stats().replacement_available == StorageResources{150, 15}, "a settlement leaves the claimed bytes replacement's");
	recovered.credit_durable_reclaim({250, 25});
	check(recovered.stats().normal_ready && recovered.stats().control_available == StorageResources{100, 10} && recovered.stats().replacement_available == StorageResources{300, 30},
		"reclaim refills control first and makes both pools whole"); bounded(recovered);
	{
		auto drain = recovered.reserve(AdmissionClass::Control, {60, 6}); drain->mark_started(); drain->settle({60, 6});
		auto whole = recovered.reserve(AdmissionClass::Replacement, {300, 30}); whole->mark_started(); whole->settle({300, 30});
	}
	check(recovered.stats().control_available == StorageResources{40, 4} && recovered.stats().replacement_available == StorageResources{} &&
		!recovered.reserve(AdmissionClass::Replacement, {1, 1}), "once replacement was whole the claim is gone: a live deficit takes nothing from control");
	// Control drained by (30, 3) and a checkpoint's (20, 2) staged at the crash: reclaiming the staged
	// files leaves the claim at the drain, and the cycle takes exactly that back.
	Admission staged(census({650, 65}), limits);
	check(staged.stats().control_available == StorageResources{100, 10} && staged.stats().replacement_available == StorageResources{250, 25}, "staged files charge the census");
	staged.credit_durable_reclaim({20, 2});
	check(staged.stats().replacement_available == StorageResources{270, 27}, "reclaimed staged files refill replacement");
	check(!staged.reserve(AdmissionClass::Replacement, {301, 31}), "the refill caps the claim: a reservation never passes the replacement pool");
	auto republish = staged.reserve(AdmissionClass::Replacement, {300, 30});
	check(republish && staged.stats().control_available == StorageResources{70, 7}, "the claim is the drain alone after the staged reclaim");
	republish.reset();
	{
		auto taking = staged.reserve(AdmissionClass::Replacement, {300, 30});
		staged.credit_durable_reclaim({30, 3});
		check(staged.stats().control_available == StorageResources{100, 10}, "a reclaim during the cycle refills control");
	}
	check(staged.stats().control_available == StorageResources{100, 10} && staged.stats().replacement_available == StorageResources{300, 30} && !staged.stats().tainted,
		"a cancellation returns only what control lacks"); bounded(staged);
	Admission live(census({600, 60}), limits);
	{ auto whole = live.reserve(AdmissionClass::Replacement, {300, 30}); whole->mark_started(); whole->settle({300, 30}); }
	check(live.stats().replacement_available == StorageResources{} && live.stats().control_available == StorageResources{100, 10} &&
		!live.reserve(AdmissionClass::Replacement, {1, 1}), "a census with both pools whole has no claim");
}
void schedules() {
	Admission ledger(census({100, 10}), limits); std::mt19937 random(0x504f4f4c); std::vector<StoragePermit> held;
	for (unsigned step = 0; step < 2000; ++step) {
		auto mode = random() % 4;
		if (mode < 2) {
			auto kind = static_cast<AdmissionClass>(random() % 3); auto permit = ledger.reserve(kind, {1 + random() % 20, 1 + random() % 2});
			if (permit) { held.push_back(std::move(*permit)); }
		} else if (mode == 2 && !held.empty()) {
			auto peak = held.back().peak(); held.back().mark_started(); held.back().settle({peak.logical_bytes / 2, peak.entries / 2}); held.pop_back();
		} else {
			auto used = ledger.stats().used; ledger.credit_durable_reclaim({std::min<std::uint64_t>(used.logical_bytes, 10), std::min<std::uint64_t>(used.entries, 1)});
		}
		bounded(ledger); check(!ledger.stats().tainted, "known scheduled outcomes preserve accounting health");
	}
	held.clear(); bounded(ledger);
}
}
int main(int argc, char** argv) {
	// One case by name (`recovery`), or every case.
	try {
		if (argc > 1 && std::string_view(argv[1]) == "recovery") { recovery(); }
		else { replacement_growth(); pools(); uncertainty(); slots(); interleaved_replacement(); control_pack_slots(); recovery(); claim(); schedules(); }
	} catch (const std::exception& error) { check(false, error.what()); }
	std::cout << checks << " admission checks, " << failures << " failures\n"; return failures ? 1 : 0;
}
