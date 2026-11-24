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

// The Store over the memory disk: admission, publication and append completions, replacement,
// reclamation before inventory, and the published selection. Run by journal_test.cc.
#include "journal_support.h"

namespace {
const AdmissionLimits store_limits{{4096, 32}, {512, 2}, {1024, 4}, 8};
void ready_store(Store& store) { while (!store.inventory_step(1).complete) {} check(store.accounting().has_value(), "store admits accounting only after complete inventory"); }
void store_covered(Model& model, const Store& store) {
	auto stats = store.accounting();
	if (stats) { check(detail::resources_fit(visible_resources(model), detail::resources_add(stats->used, stats->outstanding)), "every primitive Store mutation is covered by charged usage plus outstanding reservations"); }
	else { check(detail::resources_fit(visible_resources(model), store_limits.hard), "bootstrap remains within prechecked hard quota before census"); }
}
void store_exact(Model& model, const Store& store) {
	check(store.accounting()->used == visible_resources(model) && !store.fenced(), "completed Store operation exactly accounts for all existing files");
}
void store_replacement(Store& store, const ReplacementId& replacement) {
	store.begin_artifact(replacement, ArtifactPart::Application); store.write_chunk(replacement, "app"); store.finish_artifact(replacement);
	store.begin_artifact(replacement, ArtifactPart::Bundle); store.write_chunk(replacement, "bundle"); store.finish_artifact(replacement);
	store.publish(replacement, store.frontier().sequence);
}
}  // namespace
void store_publication_completions() {
	for (bool detached : {false, true}) {
		for (bool uncertain : {false, true}) {
			auto model = initialized(); auto io = std::make_shared<MemoryIO>(model);
			AdmissionLimits limits{{10000, 128}, {2048, 4}, {4096, 4}, 16, 3}; auto store = std::make_unique<Store>(io, limits, 1024);
			store->recover([](auto, auto) {}); store->inventory_step(128); auto id = store->reserve_replacement(128, 128);
			store->begin_artifact(*id, ArtifactPart::Application); store->write_chunk(*id, "application"); store->finish_artifact(*id);
			store->begin_artifact(*id, ArtifactPart::Bundle); store->write_chunk(*id, "first"); store->finish_artifact(*id);
			auto job = store->begin_publication(*id, 1); auto used = store->accounting()->used; auto outstanding = store->accounting()->outstanding;
			job->submitted(); auto original = detail::execute_primitive(job->io(), job->request());
			auto before = model.operations;
			check(store->frontier().generation == 1 && store->accounting()->used == used && store->accounting()->outstanding == outstanding && throws([&] { store->cancel_replacement(*id); }) && model.operations == before, "held publication owns the whole cycle and exposes only the previous frontier");
			if (uncertain) { original.error = std::make_exception_ptr(std::runtime_error("injected publication uncertainty")); }
			if (detached) { store.reset(); io.reset(); check(model.locked, "detached publication retains storage and accounting ownership"); }
			check(job->complete(std::move(original)), "original publication primitive reaped after possible facade retirement");
			while (!job->done()) {
				job->submitted(); auto completion = detail::execute_primitive(job->io(), job->request());
				check(throws([&] { job->result(); }), "publication result remains hidden before owner applies original final barrier");
				if (store) { check(store->frontier().generation == 1 && store->accounting()->outstanding == outstanding, "every publication primitive retains old frontier and full reservation until terminal settlement"); }
				check(job->complete(std::move(completion)), "publication owner applies matching primitive");
			}
			if (uncertain) {
				check(throws([&] { job->result(); }), "uncertain detached publication preserves storage failure");
				if (store) { check(store->fenced() && store->accounting()->tainted && store->accounting()->outstanding == outstanding, "uncertain publication retains conservative whole-cycle capacity"); }
			} else {
				check(job->result().generation == 2 && job->result().sequence == 1, "publication reaches selected durable generation despite facade retirement");
				if (store) {
					check(store->frontier().generation == 2 && !store->accounting()->tainted && store->accounting()->outstanding == StorageResources{}, "owner terminal publication atomically selects frontier and settles reservation");
					check(throws([&] { store->cancel_replacement(*id); }), "published cycle capability becomes stale");
					auto next = store->reserve_replacement(128, 128); check(bool(next), "completed async publication permits a fresh replacement cycle"); if (next) { store->cancel_replacement(*next); }
				}
			}
			job.reset(); store.reset(); io.reset(); check(!model.locked, "publication drain releases backend and stable lock");
			model.fail_operation = 0; auto [frontier, values] = replay_checkpoint(model);
			check(values == std::vector<std::string>{"first"} && frontier.generation == (uncertain ? 1 : 2), "detached publication recovers selected history rather than becoming canceled preparation");
		}
	}
}

void detached_old_artifact_lease() {
	auto model = initialized(); auto io = std::make_shared<MemoryIO>(model);
	AdmissionLimits limits{{10000, 128}, {2048, 4}, {4096, 4}, 16, 3}; auto store = std::make_unique<Store>(io, limits, 1024);
	store->recover([](auto, auto) {}); store->inventory_step(128); auto id = store->reserve_replacement(128, 128);
	{ auto creation = store->begin_artifact_operation(*id, ArtifactPart::Application); drive_synchronously(*creation); creation->result(); }
	auto old = store->begin_write_chunk(*id, ""); auto payload = store->begin_write_chunk(*id, "new");
	payload->submitted(); auto original = detail::execute_primitive(payload->io(), payload->request());
	store.reset(); io.reset(); old.reset();
	check(payload->in_flight() && model.locked && payload->complete(std::move(original)), "retiring old empty job after detachment cannot dispose the active replacement lease");
	drive_synchronously(*payload); payload->result(); payload.reset();
	check(!model.locked && replay(model) == std::vector<std::string>{"first"}, "detached payload settles independently of retired old jobs");
}

void store_append_completions() {
	auto model = initialized(3); auto io = std::make_shared<MemoryIO>(model); Store store(io, store_limits, 1024);
	store.recover([](auto, auto) {}); ready_store(store); auto initial = store.accounting()->used;
	{
		auto permit = store.reserve_append(AdmissionClass::Normal, 6); auto before = model.operations;
		auto operation = store.begin_append(std::move(*permit), "second");
		check(model.operations == before && store.accounting()->tickets == 1, "owned Store append reserves before IO");
	}
	check(!store.fenced() && store.accounting()->outstanding == StorageResources{} && store.accounting()->used == initial, "unsubmitted Store append cancellation refunds without fencing");
	auto permit = store.reserve_append(AdmissionClass::Normal, 6); auto operation = store.begin_append(std::move(*permit), "second");
	auto held = store.accounting()->outstanding;
	while (!operation->done()) {
		operation->submitted(); auto completion = detail::execute_primitive(operation->io(), operation->request());
		check(store.frontier().sequence == 1 && store.accounting()->used == initial && store.accounting()->outstanding == held, "executed but undelivered Store completion cannot publish or refund");
		check(operation->complete(std::move(completion)), "Store reaps matching primitive before accounting changes");
	}
	check(operation->result().sequence == 2 && store.frontier().sequence == 2 && store.accounting()->outstanding == StorageResources{} && !store.accounting()->tainted, "terminal barrier atomically settles journal and permit");
	store_exact(model, store);

	permit = store.reserve_append(AdmissionClass::Normal, 5); auto failed = store.begin_append(std::move(*permit), "third");
	held = store.accounting()->outstanding; auto used = store.accounting()->used;
	failed->submitted(); auto completion = detail::execute_primitive(failed->io(), failed->request()); store.fence_storage();
	check(failed->complete(std::move(completion)) && failed->done() && throws([&] { failed->result(); }), "fencing during IO still reaps original completion without a success result");
	check(store.accounting()->tainted && store.accounting()->outstanding == held && store.accounting()->used == used, "uncertain Store mutation retains conservative charges without refund");
}

void store_reclaim_before_inventory() {
	for (bool partial : {false, true}) {
		auto model = initialized(); MemoryIO io(model); std::string orphan;
		{ Journal journal(io, 1024); journal.recover([](auto, auto) {}); auto artifact = prepare(journal, "orphan"); orphan = artifact_name(artifact.descriptor()); }
		Store store(io, store_limits, 1024); store.recover([](auto, auto) {});
		if (partial) { auto census = store.inventory_step(1); check(census.entries == 1 && !census.complete, "fixture holds a partial census before cleanup"); }
		check(!store.accounting(), "pre-census cleanup has no admission facade");
		bool reclaimed = false;
		try { auto stats = store.reclaim_step(4096); reclaimed = stats.complete && stats.removed == 1 && !model.visible.contains(orphan); }
		catch (const std::exception&) {}
		check(reclaimed && !store.fenced(), "reclamation removes owned orphans before completed census without fencing");
		if (!reclaimed) { continue; }
		check(!store.accounting() && store.inventory_stats().entries == 0, "pre-census mutation invalidates the old partial inventory");
		ready_store(store); store_exact(model, store);
		check(store.accounting()->used.entries == model.visible.size(), "fresh post-cleanup census charges exactly the surviving namespace");
	}
}

void store_replacement_growth() {
	for (bool owned : {false, true}) {
		Model model; auto io = std::make_shared<MemoryIO>(model);
		Store store(io, store_limits, 1024, 8192);
		store.create(identity()); ready_store(store);
		auto append = store.reserve_append(AdmissionClass::Normal, 1);
		store.append(*append, "x");
		auto id = store.reserve_replacement(16, 16);
		check(bool(id), "growth fixture reserves one replacement");
		if (!id) { continue; }
		if (owned) {
			auto job = store.begin_artifact_operation(*id, ArtifactPart::Application);
			check(throws([&] { store.grow_replacement(*id, 128); }), "growth waits for the original preparation quantum");
			drive_synchronously(*job); job->result(); job.reset();
		} else { store.begin_artifact(*id, ArtifactPart::Application); }
		auto operations = model.operations;
		check(store.grow_replacement(*id, 128) && store.grow_replacement(*id, 64),
			"started application preparation permits monotonic bundle growth");
		check(model.operations == operations, "reservation growth performs no IO");
		auto before = *store.accounting();
		check(!store.grow_replacement(*id, 4096) && !store.grow_replacement(*id, 8193),
			"protected capacity and artifact bounds refuse growth");
		auto after = *store.accounting();
		check(before.outstanding == after.outstanding && before.replacement_available == after.replacement_available &&
			before.tickets == after.tickets && !after.tainted && !store.fenced(),
			"refused growth preserves the live reservation and accounting");
		store.write_chunk(*id, "app"); store.finish_artifact(*id);
		store.verify_artifact(*id, ArtifactPart::Application);
		check(store.grow_replacement(*id, 256), "verified application retains the same growable replacement");
		store.begin_artifact(*id, ArtifactPart::Bundle);
		check(throws([&] { store.grow_replacement(*id, 512); }) && !store.fenced(),
			"bundle preparation closes the funding-growth phase");
		store.write_chunk(*id, std::string(256, 'b')); store.finish_artifact(*id);
		store.publish(*id, 1); store_exact(model, store);
		check(throws([&] { store.grow_replacement(*id, 512); }), "published replacement IDs cannot grow new work");
		auto canceled = store.reserve_replacement(16, 16);
		check(canceled && store.grow_replacement(*canceled, 128), "a later cycle grows independently");
		store.cancel_replacement(*canceled); store_exact(model, store);
		check(store.accounting()->tickets == 0 && !store.fenced(), "grown unstarted cancellation settles its ticket");
	}
}

void store_basics() {
	Model model; model.chunk = 3; MemoryIO io(model);
	{
		Store store(io, store_limits, 1024); model.observe = [&] { store_covered(model, store); };
		store.create(identity());
		check(throws([&] { store.reserve_append(AdmissionClass::Normal, 5); }), "Store blocks writes until the census completes");
		check(throws([&] { store.inventory_step(0); }) && !store.fenced(), "invalid inventory budgets have no uncertain mutation");
		ready_store(store); auto append = store.reserve_append(AdmissionClass::Normal, 5); store.append(*append, "first"); store_exact(model, store);
		check(throws([&] { store.append(*append, "again"); }) && !store.fenced(), "settled append reservation cannot be reused");
		auto replacement = store.reserve_replacement(128, 128); check(replacement.has_value(), "Store reserves complete replacement before capture");
		auto waiting = store.reserve_append(AdmissionClass::Normal, 5);
		store.begin_artifact(*replacement, ArtifactPart::Application); store.write_chunk(*replacement, "app");
		append = store.reserve_append(AdmissionClass::Normal, 5); check(append.has_value(), "Store interleaves append admission with active preparation"); store.append(*append, "later");
		store.finish_artifact(*replacement); store.verify_artifact(*replacement, ArtifactPart::Application);
		store.begin_artifact(*replacement, ArtifactPart::Bundle); store.write_chunk(*replacement, "bundle"); store.finish_artifact(*replacement);
		check(throws([&] { store.publish(*replacement, 1); }) && !store.fenced(), "stale covered sequence rejects without fencing prepared state");
		store.publish(*replacement, 2); store_exact(model, store);
		store.append(*waiting, "after"); store_exact(model, store);
		check(throws([&] { store.cancel_replacement(*replacement); }), "completed replacement IDs cannot mutate new work");
		{
			auto canceled = store.reserve_replacement(128, 128); store.begin_artifact(*canceled, ArtifactPart::Application); store.write_chunk(*canceled, "orphan");
			check(throws([&] { store.write_chunk(*canceled, std::string(129, 'x')); }) && !store.fenced(), "chunk reservation rejects overflow before IO");
			store.cancel_replacement(*canceled); store_exact(model, store);
		}
		while (!store.reclaim_step(1).complete) {} store_exact(model, store); model.observe = {};
	}
	model.power_loss(false);
	{
		Store reopened(io, store_limits, 1024); std::string bundle, application;
		std::vector<std::string> suffix;
		reopened.recover([&](auto, auto bytes) { suffix.emplace_back(bytes); }, [&](auto&, auto& reader, auto deps) { bundle = read_artifact(reader); application = read_artifact(deps[0]); });
		ready_store(reopened); check(bundle == "bundle" && application == "app" && reopened.frontier().sequence == 3 && suffix == std::vector<std::string>{"after"}, "Store restart restores checkpoint and append reserved across manifest migration"); store_exact(model, reopened);
	}
}
void store_failures() {
	for (auto chunk : {std::numeric_limits<std::size_t>::max(), std::size_t{3}}) {
		auto baseline = initialized(); std::size_t operations;
		baseline.chunk = chunk;
		{
			auto model = baseline.clone(); MemoryIO io(model); Store store(io, store_limits, 1024); store.recover([](auto, auto) {}); ready_store(store);
			auto replacement = store.reserve_replacement(128, 128); model.operations = 0; store_replacement(store, *replacement); operations = model.operations;
		}
		for (std::size_t operation = 1; operation <= operations; ++operation) {
			for (bool after : {false, true}) {
				auto model = baseline.clone();
				{
					MemoryIO io(model); Store store(io, store_limits, 1024); store.recover([](auto, auto) {}); ready_store(store);
					auto replacement = store.reserve_replacement(128, 128); model.operations = 0; model.fail_operation = operation; model.fail_after = after;
					check(throws([&] { store_replacement(store, *replacement); }) && store.fenced() && store.accounting()->tainted, "uncertain Store preparation or publication fences storage and accounting");
				}
				for (bool visible : {false, true}) {
					auto crash = model.clone(); crash.power_loss(visible); MemoryIO io(crash); Store recovered(io, store_limits, 1024); std::vector<std::string> history;
					recovered.recover([&](auto, auto bytes) { history.emplace_back(bytes); }, [&](auto&, auto& bundle, auto deps) {
						check(read_artifact(bundle) == "bundle" && read_artifact(deps[0]) == "app", "published Store generation recovers complete required artifacts"); history.emplace_back("first");
					}); ready_store(recovered);
					check(history == std::vector<std::string>{"first"}, "Store uncertain result never loses previously acknowledged history"); store_exact(crash, recovered);
				}
			}
		}
		std::cout << "Store replacement operations tested: " << operations << '\n';
	}
}
void store_append_and_cleanup_failures() {
	for (bool cleanup : {false, true}) {
		auto baseline = reclaim_fixture(); std::size_t operations;
		{
			auto model = baseline.clone(); MemoryIO io(model); Store store(io, store_limits, 1024);
			store.recover([](auto, auto) {}, [](auto&, auto&, auto) {}); ready_store(store);
			auto append = cleanup ? std::optional<AppendReservation>{} : store.reserve_append(AdmissionClass::Normal, 4);
			model.operations = 0; if (cleanup) { store.reclaim_step(4096); } else { store.append(*append, "next"); } operations = model.operations;
		}
		for (std::size_t operation = 1; operation <= operations; ++operation) {
			for (bool after : {false, true}) {
				auto model = baseline.clone();
				{
					MemoryIO io(model); std::optional<ArtifactReader> escaped; Store store(io, store_limits, 1024);
					store.recover([](auto, auto) {}, [&](auto&, auto& bundle, auto) { escaped.emplace(std::move(bundle)); }); ready_store(store);
					auto append = cleanup ? std::optional<AppendReservation>{} : store.reserve_append(AdmissionClass::Normal, 4);
					auto charged_before = store.accounting()->used;
					model.operations = 0; model.fail_operation = operation; model.fail_after = after;
					check(throws([&] { if (cleanup) { store.reclaim_step(4096); } else { store.append(*append, "next"); } }) && store.fenced() && store.accounting()->tainted, "Store append or cleanup uncertainty fences wrapper accounting");
					check(store.accounting()->used == charged_before && throws([&] { read_artifact(*escaped); }), "uncertain cleanup never credits deletion and fencing invalidates escaped readers");
				}
				for (bool visible : {false, true}) {
					auto crash = model.clone(); crash.power_loss(visible); MemoryIO io(crash); Store recovered(io, store_limits, 1024); std::vector<std::string> suffix;
					recovered.recover([&](auto, auto bytes) { suffix.emplace_back(bytes); }, [&](auto&, auto& bundle, auto deps) { check(read_artifact(bundle) == "first" && read_artifact(deps[0]) == "application", "cleanup or append failure preserves required checkpoint"); });
					ready_store(recovered); store_exact(crash, recovered);
					check(suffix.empty() || (!cleanup && suffix == std::vector<std::string>{"next"}), "recovery observes only optional uncertain append and required old state");
				}
			}
		}
		std::cout << "Store " << (cleanup ? "cleanup" : "append") << " operations tested: " << operations << '\n';
	}
}
void store_limits_and_pins() {
	for (bool entry_limit : {false, true}) {
		Model model; MemoryIO io(model); auto limits = store_limits;
		if (entry_limit) { limits.hard.entries = 9; } else { limits.hard.logical_bytes = 1700; }
		Store store(io, limits, 1024); store.create(identity()); ready_store(store);
		check(!store.reserve_append(AdmissionClass::Normal, 1), "Store refuses admission when either hard resource leaves no normal peak");
		check(store.reserve_append(AdmissionClass::Control, 1).has_value(), "normal pressure preserves protected control capacity");
		auto replacement = store.reserve_replacement(128, 128); check(replacement.has_value(), "normal pressure preserves complete replacement capacity"); store.cancel_replacement(*replacement);
	}
	auto model = reclaim_fixture(); MemoryIO io(model); std::optional<ArtifactReader> escaped;
	{
		Store store(io, store_limits, 1024); std::string pinned;
		store.recover([](auto, auto) {}, [&](auto& frontier, auto& bundle, auto) { pinned = artifact_name(*frontier.checkpoint); escaped.emplace(std::move(bundle)); }); ready_store(store);
		auto replacement = store.reserve_replacement(128, 128); store_replacement(store, *replacement);
		while (!store.reclaim_step(1).complete) {} store_exact(model, store);
		check(model.visible.contains(pinned) && read_artifact(*escaped) == "first", "Store cleanup keeps escaped old checkpoint reader pinned and charged");
		escaped.reset(); while (!store.reclaim_step(1).complete) {} store_exact(model, store);
		check(!model.visible.contains(pinned), "Store credits old pinned artifact only after actual durable unlink");
	}
	Model other_model; MemoryIO other_io(other_model); Store other(other_io, store_limits, 1024); other.create(identity()); ready_store(other);
	auto foreign_append = other.reserve_append(AdmissionClass::Normal, 1); auto foreign_replacement = other.reserve_replacement(128, 128);
	Store store(io, store_limits, 1024); store.recover([](auto, auto) {}, [](auto&, auto&, auto) {}); ready_store(store);
	check(throws([&] { store.append(*foreign_append, "x"); }) && throws([&] { store.begin_artifact(*foreign_replacement, ArtifactPart::Application); }) && !store.fenced(), "foreign Store capabilities reject before mutation without fencing");
}
void store_incremental_verification() {
	Model model; MemoryIO io(model); Store store(io, store_limits, 1024); store.create(identity()); ready_store(store);
	auto before = store.accounting()->used; auto replacement = store.reserve_replacement(64, 64);
	check(throws([&] { store.begin_artifact_verification(*replacement, ArtifactPart::Application); }) && !store.fenced(), "unsealed replacement artifact rejects without fencing");
	store.begin_artifact(*replacement, ArtifactPart::Application); store.write_chunk(*replacement, "candidate"); auto descriptor = store.finish_artifact(*replacement);
	auto verifier = store.begin_artifact_verification(*replacement, ArtifactPart::Application); auto name = artifact_name(descriptor);
	store.cancel_replacement(*replacement); while (!store.reclaim_step(1).complete) {}
	check(model.visible.contains(name) && store.accounting()->used.logical_bytes == before.logical_bytes + 77, "canceled staging retains verifier pin and exact charges through GC");
	std::array<char, 64> buffer{}; verifier->read_next(buffer); std::optional<ArtifactReader> reader(std::move(*verifier).finish()); verifier.reset();
	while (!store.reclaim_step(1).complete) {}
	check(model.visible.contains(name) && read_artifact(*reader) == "candidate", "promoted reader retains the canceled artifact across durable reclamation");
	reader.reset(); while (!store.reclaim_step(1).complete) {}
	check(!model.visible.contains(name) && store.accounting()->used == before, "only final handle release and durable GC credit canceled artifact resources");
	for (bool after : {false, true}) { for (unsigned fault = 1; fault <= 5; ++fault) {
		Model failed; MemoryIO backend(failed); Store owned(backend, store_limits, 1024); owned.create(identity()); ready_store(owned);
		auto id = owned.reserve_replacement(64, 64); owned.begin_artifact(*id, ArtifactPart::Application); owned.write_chunk(*id, "candidate"); owned.finish_artifact(*id);
		failed.fail_operation = failed.operations + fault; failed.fail_after = after;
		check(throws([&] { auto v = owned.begin_artifact_verification(*id, ArtifactPart::Application); v->read_next(buffer); auto r = std::move(*v).finish(); }) && owned.fenced(), "before/after verification-open and payload IO failures fence Store");
	} }
}

void store_published_selection() {
	Model model; MemoryIO io(model); std::optional<PublishedArtifactSelection> escaped;
	{
		Store store(io, store_limits, 1024); store.create(identity()); ready_store(store);
		auto replacement = store.reserve_replacement(128, 128); store_replacement(store, *replacement);
		escaped = store.select_published_dependency(0); auto selected = *escaped; auto old_name = artifact_name(selected.descriptor()); auto before = store.accounting()->used;
		auto verifier = store.begin_published_verification(selected);
		check(store.accounting()->used == before && store.accounting()->outstanding == StorageResources{}, "published opening reserves no Replacement bytes or tickets");
		replacement = store.reserve_replacement(128, 128); store_replacement(store, *replacement);
		while (!store.reclaim_step(1).complete) {}
		check(model.visible.contains(old_name), "published verifier pins old dependency after replacement and reclamation");
		std::array<char, 8> buffer{}; verifier->read_next(buffer); std::optional<ArtifactReader> reader(std::move(*verifier).finish()); verifier.reset();
		while (!store.reclaim_step(1).complete) {}
		check(model.visible.contains(old_name) && read_artifact(*reader) == "app", "promoted published reader retains old dependency pin");
		before = store.accounting()->used; reader.reset(); while (!store.reclaim_step(1).complete) {}
		check(!model.visible.contains(old_name) && store.accounting()->used.logical_bytes + 71 == before.logical_bytes, "selection alone does not pin and durable GC credits the exact old file"); store_exact(model, store);
		escaped = store.select_published_dependency(0);
	}
	check(!model.locked, "escaped publication selection does not retain the owner lock");
	{
		Store store(io, store_limits, 1024); store.recover([](auto, auto) {}, [](auto&, auto&, auto) {}); ready_store(store); auto before = model.operations;
		check(throws([&] { store.begin_published_verification(*escaped); }) && model.operations == before && !store.fenced(), "old selection rejects against identical recovered publication without IO");
		auto fresh = store.select_published_dependency(0); check(store.begin_published_verification(*fresh).has_value(), "reopened Store creates a new usable selection");
	}
	for (bool after : {false, true}) { for (unsigned fault = 1; fault <= 5; ++fault) {
		Model failed; MemoryIO backend(failed); Store owned(backend, store_limits, 1024); owned.create(identity()); ready_store(owned);
		auto id = owned.reserve_replacement(128, 128); store_replacement(owned, *id); auto selected = owned.select_published_dependency(0);
		failed.fail_operation = failed.operations + fault; failed.fail_after = after;
		check(throws([&] { auto verifier = owned.begin_published_verification(*selected); std::array<char, 8> buffer{}; verifier->read_next(buffer); auto reader = std::move(*verifier).finish(); }) && owned.fenced(), "published metadata and payload IO faults fence Store before and after effects");
	} }
	for (bool header : {false, true}) {
		Model failed; MemoryIO backend(failed); Store owned(backend, store_limits, 1024); owned.create(identity()); ready_store(owned);
		auto id = owned.reserve_replacement(128, 128); store_replacement(owned, *id); auto selected = owned.select_published_dependency(0);
		auto& bytes = failed.visible.at(artifact_name(selected->descriptor()))->visible; bytes[header ? 0 : bytes.size() - 1] ^= 1;
		check(throws([&] { auto verifier = owned.begin_published_verification(*selected); std::array<char, 8> buffer{}; verifier->read_next(buffer); auto reader = std::move(*verifier).finish(); }) && owned.fenced(), "published header and CRC corruption fence shared owner");
	}
}

void store_posix() {
	auto directory = std::filesystem::current_path() / ".scratch" / ("store-test-" + std::to_string(::getpid()));
	if (!std::filesystem::create_directory(directory)) { throw std::runtime_error("Store test directory exists"); } ::chmod(directory.c_str(), 0700);
	struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{directory};
	std::optional<PublishedArtifactSelection> old_selection;
	{
		PosixIO io(directory, PosixIOOptions{FlushMode::full}); Store store(io, store_limits, 1024); store.create(identity()); ready_store(store);
		auto append = store.reserve_append(AdmissionClass::Normal, 5); store.append(*append, "first");
		auto replacement = store.reserve_replacement(128, 128); store_replacement(store, *replacement);
		auto cancel = store.reserve_replacement(128, 128); store.begin_artifact(*cancel, ArtifactPart::Application); store.write_chunk(*cancel, "orphan"); store.cancel_replacement(*cancel);
		while (!store.reclaim_step(1).complete) {}
		Inventory census(io); while (!census.step(1).complete) {}
		check(store.accounting()->used == StorageResources{census.stats().logical_bytes, census.stats().entries}, "real POSIX Store accounts for publication cancellation and cleanup exactly");
		old_selection = store.select_published_dependency(0);
	}
	{
		PosixIO io(directory, PosixIOOptions{FlushMode::full}); Store store(io, store_limits, 1024); std::string bundle, application;
		store.recover([](auto, auto) {}, [&](auto&, auto& reader, auto deps) { bundle = read_artifact(reader); application = read_artifact(deps[0]); }); ready_store(store);
		check(bundle == "bundle" && application == "app" && store.frontier().sequence == 1, "actual POSIX Store reopens acknowledged replacement");
		check(throws([&] { store.begin_published_verification(*old_selection); }) && !store.fenced(), "POSIX reopen rejects previous selection while acquiring the stable lock");
		auto selected = store.select_published_dependency(0); auto source_name = artifact_name(selected->descriptor()); auto source = store.begin_published_verification(*selected);
		auto newer = store.reserve_replacement(128, 128); store_replacement(store, *newer);
		while (!store.reclaim_step(1).complete) {}
		check(std::filesystem::exists(directory / source_name), "POSIX published verifier pins old dependency across replacement");
		std::array<char, 3> published_buffer{}; source->read_next(published_buffer); auto published_reader = std::move(*source).finish(); source.reset();
		check(read_artifact(published_reader) == "app", "POSIX published verifier promotes exact old image");
		{ auto released = std::move(published_reader); }
		while (!store.reclaim_step(1).complete) {}
		check(!std::filesystem::exists(directory / source_name), "POSIX published pin release permits durable old-file reclamation");
		auto id = store.reserve_replacement(128, 128); store.begin_artifact(*id, ArtifactPart::Application); store.write_chunk(*id, "verified POSIX image"); auto descriptor = store.finish_artifact(*id);
		auto verifier = store.begin_artifact_verification(*id, ArtifactPart::Application); store.cancel_replacement(*id);
		while (!store.reclaim_step(1).complete) {}
		check(std::filesystem::exists(directory / artifact_name(descriptor)), "POSIX incremental verifier pins canceled staging");
		std::array<char, 3> buffer{}; std::string candidate;
		while (verifier->offset() < descriptor.length) { auto count = verifier->read_next(buffer); candidate.append(buffer.data(), count); }
		std::optional<ArtifactReader> reader(std::move(*verifier).finish()); verifier.reset();
		check(candidate == "verified POSIX image" && read_artifact(*reader) == candidate, "POSIX partial verification promotes the same complete application payload");
		reader.reset(); while (!store.reclaim_step(1).complete) {}
		check(!std::filesystem::exists(directory / artifact_name(descriptor)), "POSIX durable GC removes staging only after verified reader closes");
	}
}
