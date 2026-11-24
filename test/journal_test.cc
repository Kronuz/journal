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

// The Journal on a memory disk: append, recovery, corruption and POSIX, and the sweeps main runs
// under several argument sets. The other topics are in journal_artifact_test.cc,
// journal_store_test.cc and journal_reopen_test.cc.
#include "journal_support.h"

int failures = 0, checks = 0;

namespace {
void checkpoint_publication_failures(std::size_t chunk = std::numeric_limits<std::size_t>::max(), bool suspended = false) {
	auto publish = [&](Journal& journal, const PreparedArtifact& bundle, std::span<const PreparedArtifact> dependencies) {
		if (!suspended) { return journal.publish_checkpoint(bundle, dependencies, 1); }
		auto job = journal.begin_checkpoint(bundle, dependencies, 1);
		while (!job->done()) {
			job->submitted(); auto completed = detail::execute_primitive(job->io(), job->request());
			check(journal.frontier().generation == 1 && throws([&] { job->result(); }), "executed checkpoint primitive cannot expose selected generation before original completion");
			MutationCompletion stale; stale.token = completed.token; ++stale.token.operation;
			check(!job->complete(std::move(stale)) && job->in_flight(), "stale publication completion retains original primitive");
			auto token = completed.token; check(job->complete(std::move(completed)), "matching publication primitive reaped");
			MutationCompletion repeated; repeated.token = token; check(!job->complete(std::move(repeated)), "duplicate publication completion cannot advance another phase");
		}
		job->result(); check(journal.frontier().generation == 1, "final publication barrier still requires explicit owner settlement");
		return journal.finish_checkpoint(job);
	};
	auto baseline = initialized(); baseline.chunk = chunk; std::size_t operations;
	{
		auto complete = baseline.clone(); auto io = std::make_shared<MemoryIO>(complete); Journal journal(io, 1024); journal.recover([](auto, auto) {});
		auto bundle = prepare(journal, "first"); std::array dependencies{prepare(journal, "application")};
		complete.operations = 0; publish(journal, bundle, dependencies); operations = complete.operations;
	}
	for (std::size_t operation = 1; operation <= operations; ++operation) {
		for (bool after : {false, true}) {
			auto failed = baseline.clone();
			{
				auto io = std::make_shared<MemoryIO>(failed); Journal journal(io, 1024); journal.recover([](auto, auto) {});
				auto bundle = prepare(journal, "first"); std::array dependencies{prepare(journal, "application")};
				failed.operations = 0; failed.fail_operation = operation; failed.fail_after = after;
				check(throws([&] { publish(journal, bundle, dependencies); }) && journal.fenced(), "uncertain generation publication fences writer");
				check(throws([&] { journal.append_batch("after failure"); }), "ambiguous publication cannot resume old generation");
			}
			std::optional<Model> reference;
			if (suspended) {
				reference = baseline.clone(); MemoryIO io(*reference); Journal journal(io, 1024); journal.recover([](auto, auto) {});
				auto bundle = prepare(journal, "first"); std::array dependencies{prepare(journal, "application")};
				reference->operations = 0; reference->fail_operation = operation; reference->fail_after = after;
				check(throws([&] { journal.publish_checkpoint(bundle, dependencies, 1); }) && journal.fenced(), "synchronous publication receives identical injected fault");
			}
			for (bool visible : {false, true}) {
				auto crashed = failed.clone(); crashed.power_loss(visible);
				auto [frontier, batches] = replay_checkpoint(crashed);
				if (reference) {
					auto comparison = reference->clone(); comparison.power_loss(visible); auto [selected, values] = replay_checkpoint(comparison);
					check(selected.generation == frontier.generation && values == batches, "both publication drivers recover identical generation and history under identical faults");
				}
				check(batches == std::vector<std::string>{"first"} && frontier.sequence == 1, "either recovered generation preserves covered history");
				if (operation == operations && after) { check(frontier.generation == 2, "completed generation directory barrier survives reported failure"); }
			}
			failed.fail_operation = 0; failed.operations = 0;
			auto [selected, batches] = replay_checkpoint(failed); failed.power_loss(false);
			auto [sealed, restored] = replay_checkpoint(failed);
			check(sealed.generation == selected.generation && restored == batches, "process restart seals selected generation before later power loss");
		}
	}
	std::cout << "checkpoint publication operations tested: " << operations << '\n';
}


void store_artifact_completions(bool grown = false) {
	for (unsigned phase = 0; phase < 3; ++phase) {
		for (bool detached : {false, true}) {
			for (bool uncertain : {false, true}) {
				auto model = initialized(); auto io = std::make_shared<MemoryIO>(model);
				AdmissionLimits limits{{10000, 128}, {2048, 4}, {4096, 4}, 16, 3};
				auto store = std::make_unique<Store>(io, limits, 1024); store->recover([](auto, auto) {}); store->inventory_step(128);
				auto id = store->reserve_replacement(128, 128); auto used = store->accounting()->used;
				check(bool(id), "completion preparation reserves complete replacement cycle");
				if (!id) { return; }
				if (grown) { check(store->grow_replacement(*id, 256), "original-completion fixture grows replacement funding"); }
				auto job = store->begin_artifact_operation(*id, ArtifactPart::Application);
				if (phase > 0) { drive_synchronously(*job); job->result(); job = store->begin_write_chunk(*id, "payload"); }
				if (phase > 1) { drive_synchronously(*job); job->result(); job = store->begin_finish_artifact(*id); }
				auto outstanding = store->accounting()->outstanding;
				job->submitted(); auto completion = detail::execute_primitive(job->io(), job->request());
				auto before = model.operations;
				check(throws([&] { store->cancel_replacement(*id); }) && throws([&] { store->write_chunk(*id, "overlap"); }) && throws([&] { store->reclaim_step(1); }) && !store->fenced() && before == model.operations, "pending quantum rejects cancellation and competing mutation without IO or fencing");
				check(store->accounting()->used == used && store->accounting()->outstanding == outstanding, "executed but undelivered artifact quantum retains entire reservation");
				MutationCompletion stale; stale.token = completion.token; ++stale.token.operation;
				check(!job->complete(std::move(stale)) && job->in_flight(), "stale artifact completion cannot settle replacement lease");
				if (uncertain) { completion.error = std::make_exception_ptr(std::runtime_error("injected artifact completion uncertainty")); }
				if (detached) { store.reset(); io.reset(); check(model.locked, "detached replacement retains backend and stable owner lock"); }
				check(job->complete(std::move(completion)), "original artifact completion reaped through admitted job");
				drive_synchronously(*job);
				if (uncertain) {
					check(throws([&] { job->result(); }), "uncertain artifact job preserves original failure");
					if (store) { check(store->fenced() && store->accounting()->tainted && store->accounting()->used == used && store->accounting()->outstanding == outstanding, "uncertain preparation fences and retains conservative capacity"); }
				} else {
					job->result();
					if (store) {
						if (phase == 0) { store->write_chunk(*id, "payload"); }
						auto descriptor = phase == 2 ? job->descriptor() : store->finish_artifact(*id);
						check(descriptor.length == 7 && descriptor.checksum == crc32c("payload"), "terminal artifact quantum transfers exact visible length and seal into cycle");
						store->cancel_replacement(*id);
						check(store->accounting()->outstanding == StorageResources{} && store->accounting()->used.logical_bytes == used.logical_bytes + detail::artifact_header_size + 7 && !store->accounting()->tainted, "known canceled preparation keeps staged bytes charged and releases unused reservation");
					}
				}
				job.reset(); store.reset(); io.reset(); check(!model.locked, "terminal artifact retirement closes handles and releases stable ownership");
				model.fail_operation = 0; check(replay(model) == std::vector<std::string>{"first"}, "detached or uncertain preparation never changes acknowledged journal history");
			}
		}
	}
	{
		auto model = initialized(); auto io = std::make_shared<MemoryIO>(model);
		AdmissionLimits limits{{10000, 128}, {2048, 4}, {4096, 4}, 16, 3}; Store store(io, limits, 1024); store.recover([](auto, auto) {}); store.inventory_step(128);
		auto id = store.reserve_replacement(128, 128); auto before = model.operations;
		{ auto canceled = store.begin_artifact_operation(*id, ArtifactPart::Application); }
		check(before == model.operations && !store.fenced(), "unsubmitted replacement job cancellation performs no IO");
		store.cancel_replacement(*id); check(store.accounting()->outstanding == StorageResources{}, "unsubmitted cycle cancellation refunds whole reservation");
		id = store.reserve_replacement(128, 128); auto creation = store.begin_artifact_operation(*id, ArtifactPart::Application); drive_synchronously(*creation); creation->result(); creation.reset();
		auto empty = store.begin_write_chunk(*id, ""); check(empty->done(), "empty preparation quantum performs no IO");
		auto payload = store.begin_write_chunk(*id, "new"); empty.reset();
		check(throws([&] { store.cancel_replacement(*id); }), "retiring old completed lease cannot release a newer preparation job");
		payload.reset(); store.cancel_replacement(*id);
	}
}

void reclamation_failures(std::size_t chunk = std::numeric_limits<std::size_t>::max(), bool suspended = false) {
	auto baseline = reclaim_fixture(); baseline.chunk = chunk; std::size_t operations;
	auto reclaim = [&](Journal& journal) {
		if (!suspended) { return journal.reclaim_step(4096); }
		auto job = journal.begin_reclaim(4096);
		while (!job->done()) {
			job->submitted(); auto original = detail::execute_primitive(job->io(), job->request());
			check(throws([&] { job->result(); }), "executed reclamation cannot settle before original reap");
			MutationCompletion stale; stale.token = job->request().token; ++stale.token.operation;
			check(!job->complete(std::move(stale)) && job->in_flight(), "stale reclamation callback cannot release original primitive");
			check(job->complete(std::move(original)), "matching reclamation callback advances owned sequencing");
		}
		return journal.finish_reclaim(job);
	};
	auto proof = baseline.clone(); auto expected = replay_checkpoint(proof).first;
	{
		auto model = baseline.clone(); auto io = std::make_shared<MemoryIO>(model); Journal journal(io, 1024); journal.recover([](auto, auto) {}, [](auto&, auto&, auto) {});
		model.operations = 0; auto stats = reclaim(journal); operations = model.operations;
		check(stats.complete && stats.removed == 4 && stats.logical_bytes > 0, "obsolete generations and sealed artifacts are durably reclaimed");
	}
	for (std::size_t operation = 1; operation <= operations; ++operation) {
		for (bool after : {false, true}) {
			auto failed = baseline.clone();
			{
				auto io = std::make_shared<MemoryIO>(failed); Journal journal(io, 1024); journal.recover([](auto, auto) {}, [](auto&, auto&, auto) {});
				failed.operations = 0; failed.fail_operation = operation; failed.fail_after = after;
				check(throws([&] { reclaim(journal); }) && journal.fenced(), "uncertain reclamation I/O fences the owner");
				check(throws([&] { journal.append_batch("unavailable"); }), "failed reclamation cannot resume writes");
			}
			failed.fail_operation = 0;
			for (bool choose_new : {false, true}) {
				auto crashed = failed.clone(); crashed.power_loss(choose_new);
				auto [selected, restored] = replay_checkpoint(crashed);
				check(restored == std::vector<std::string>{"first"} && selected.generation == expected.generation && selected.journal_identity == expected.journal_identity && selected.sequence == expected.sequence,
					"either cleanup namespace outcome preserves exact latest acknowledged generation and state");
			}
		}
	}
	std::cout << "reclamation operations tested: " << operations << '\n';
}

void basics() {
	check(crc32c("123456789") == 0xe3069283u, "CRC32C standard vector");
	check(crc32c("") == 0, "CRC32C empty vector");
	auto model = initialized(3);
	check(replay(model) == std::vector<std::string>{"first"}, "short reads and writes round-trip");
	MemoryIO io(model); Journal journal(io, 1024);
	journal.recover([](auto, auto) {});
	check(throws([&] { journal.append_batch(std::string(1025, 'x')); }) && !journal.fenced(), "oversize admission has no storage side effect");
	check(journal.append_batch("").sequence == 2, "empty atomic batch is valid");
	MemoryIO other(model); Journal second(other);
	check(throws([&] { second.recover([](auto, auto) {}); }), "stable lock excludes concurrent owner");
	model.zero_write = true;
	check(throws([&] { journal.append_batch("failed"); }) && journal.fenced(), "zero-progress write fences journal");
	check(throws([&] { journal.append_batch("retry"); }), "fenced journal cannot acknowledge later batches");
	Journal third(other);
	check(throws([&] { third.recover([](auto, auto) {}); }), "fenced owner retains stable lock");
}

void append_failures(std::size_t chunk = std::numeric_limits<std::size_t>::max(), bool checkpointed = false, bool suspended = false) {
	auto append = [&](Journal& journal) {
		if (!suspended) { return journal.append_batch("second"); }
		auto operation = journal.begin_append("second");
		while (!operation->done()) {
			check(journal.frontier().sequence == 1, "suspended append keeps public frontier unchanged");
			operation->submitted();
			auto completion = detail::execute_primitive(operation->io(), operation->request());
			check(journal.frontier().sequence == 1, "executed but undelivered completion cannot publish frontier");
			auto stale = MutationCompletion{}; stale.token = completion.token; ++stale.token.operation;
			check(!operation->complete(std::move(stale)) && operation->in_flight(), "foreign completion cannot reap an accepted primitive");
			auto token = completion.token;
			check(operation->complete(std::move(completion)), "matching completion is reaped exactly once");
			auto repeated = MutationCompletion{}; repeated.token = token;
			check(!operation->complete(std::move(repeated)), "duplicate completion cannot advance another step");
		}
		return journal.finish_append(operation);
	};
	auto baseline = initialized(chunk);
	if (checkpointed) {
		MemoryIO io(baseline); Journal journal(io, 1024); journal.recover([](auto, auto) {});
		auto bundle = prepare(journal, "first"); std::array dependencies{prepare(journal, "application")};
		journal.publish_checkpoint(bundle, dependencies, 1);
	}
	auto recover_values = [&](Model& model) { return checkpointed ? replay_checkpoint(model).second : replay(model); };
	std::size_t operation_count = 0;
	{
		auto model = baseline.clone(); auto io = std::make_shared<MemoryIO>(model); Journal journal(io, 1024);
		journal.recover([](auto, auto) {}, [](auto&, auto&, auto) {}); model.operations = 0;
		append(journal); operation_count = model.operations;
	}
	for (std::size_t operation = 1; operation <= operation_count; ++operation) {
		for (bool after : {false, true}) {
			auto model = baseline.clone();
			{
				auto io = std::make_shared<MemoryIO>(model); Journal journal(io, 1024); journal.recover([](auto, auto) {}, [](auto&, auto&, auto) {});
				model.operations = 0; model.fail_operation = operation; model.fail_after = after;
				check(throws([&] { append(journal); }) && journal.fenced(), "each uncertain append operation fences");
				check(throws([&] { journal.append_batch("third"); }), "failure prevents subsequent acknowledgement");
			}
			std::optional<Model> synchronous;
			if (suspended) {
				synchronous = baseline.clone();
				MemoryIO backend(*synchronous); Journal reference(backend, 1024);
				reference.recover([](auto, auto) {}, [](auto&, auto&, auto) {});
				synchronous->operations = 0; synchronous->fail_operation = operation; synchronous->fail_after = after;
				check(throws([&] { reference.append_batch("second"); }) && reference.fenced(), "reference driver receives identical injected fault");
			}
			for (bool keep_visible : {false, true}) {
				auto crash = model.clone(); crash.power_loss(keep_visible);
				auto batches = recover_values(crash);
				if (synchronous) {
					auto comparison = synchronous->clone(); comparison.power_loss(keep_visible);
					check(batches == recover_values(comparison), "synchronous and suspended drivers recover identical histories under identical faults");
				}
				check(batches == std::vector<std::string>{"first"} || batches == std::vector<std::string>({"first", "second"}),
					"crash keeps the previous acknowledged prefix and at most the interrupted batch");
				if (operation == operation_count && after) {
					check(batches.size() == 2, "completed directory barrier preserves new frontier even when its call reports failure");
				}
			}
			// Process death preserves the visible namespace, unlike power loss.
			// Recovery must seal its selected frontier before a later power loss.
			model.fail_operation = 0; model.operations = 0;
			auto selected = recover_values(model);
			model.power_loss(false);
			check(recover_values(model) == selected, "restart seals selected namespace against later power loss");
		}
	}
	std::cout << "append publication operations tested: " << operation_count << '\n';
}

void suspended_artifact_operations() {
	auto baseline = initialized();
	for (auto chunk : {std::size_t{3}, std::numeric_limits<std::size_t>::max()}) {
		baseline.chunk = chunk;
		auto prepare_owned = [](Journal& journal, bool suspended) {
			if (!suspended) { return prepare(journal, "artifact-payload"); }
			auto drive = [&](const std::shared_ptr<ArtifactMutation>& operation) {
				while (!operation->done()) {
					auto before = operation->request().token;
					operation->submitted(); auto completion = detail::execute_primitive(operation->io(), operation->request());
					check(throws([&] { operation->result(); }), "executed artifact primitive cannot report completion before delivery");
					MutationCompletion stale; stale.token = before; ++stale.token.step;
					check(!operation->complete(std::move(stale)) && operation->in_flight(), "stale artifact completion retains original primitive");
					check(operation->complete(std::move(completion)), "matching artifact primitive reaped");
					MutationCompletion duplicate; duplicate.token = before;
					check(!operation->complete(std::move(duplicate)), "duplicate artifact completion cannot advance another phase");
				}
				operation->result();
			};
			auto creation = journal.begin_artifact_preparation(); drive(creation);
			auto builder = creation->take_builder(); creation.reset();
			auto payload = builder.begin_append_chunk("artifact-payload"); drive(payload); payload.reset();
			auto seal = builder.begin_finish(); drive(seal); return seal->prepared_result();
		};
		std::size_t count = 0;
		{
			auto model = baseline.clone(); auto io = std::make_shared<MemoryIO>(model); Journal journal(io, 1024);
			journal.recover([](auto, auto) {}); model.operations = 0;
			auto prepared = prepare_owned(journal, true); count = model.operations;
			check(prepared.descriptor().length == 16 && prepared.descriptor().checksum == crc32c("artifact-payload"), "completion artifact settles exact descriptor after seal barriers");
			journal.verify_artifact(prepared);
		}
		for (std::size_t fault = 1; fault <= count; ++fault) {
			for (bool after : {false, true}) {
				std::array<Model, 2> outcomes{baseline.clone(), baseline.clone()};
				for (unsigned driver = 0; driver < 2; ++driver) {
					auto& model = outcomes[driver];
					auto io = std::make_shared<MemoryIO>(model); Journal journal(io, 1024); journal.recover([](auto, auto) {});
					model.operations = 0; model.fail_operation = fault; model.fail_after = after;
					check(throws([&] { prepare_owned(journal, driver == 1); }) && journal.fenced(), "both artifact drivers fence each uncertain primitive outcome");
					check(model.operations == fault, "artifact failure never executes a later primitive");
				}
				auto footprints = [](const Model& model) {
					std::vector<std::pair<std::size_t, std::size_t>> sizes;
					for (const auto& [name, inode] : model.visible) { sizes.emplace_back(inode->visible.size(), inode->durable.size()); }
					std::sort(sizes.begin(), sizes.end()); return sizes;
				};
				check(footprints(outcomes[0]) == footprints(outcomes[1]), "both artifact drivers leave identical visible/durable file footprints under the same fault");
				for (bool visible : {false, true}) {
					for (auto& outcome : outcomes) {
						auto crash = outcome.clone(); crash.power_loss(visible);
						check(replay(crash) == std::vector<std::string>{"first"}, "artifact faults preserve acknowledged history under both namespace outcomes");
					}
				}
			}
		}
	}
}
void suspended_artifact_ownership() {
	auto model = initialized(); auto io = std::make_shared<MemoryIO>(model);
	auto journal = std::make_unique<Journal>(io, 1024); journal->recover([](auto, auto) {});
	{
		auto canceled = journal->begin_artifact_preparation();
		check(throws([&] { journal->prepare_artifact(); }), "unsubmitted creation owns the bounded preparation slot");
	}
	check(!journal->fenced(), "unsubmitted artifact cancellation performs no IO and leaves owner healthy");
	auto creation = journal->begin_artifact_preparation(); creation->submitted();
	auto first = detail::execute_primitive(creation->io(), creation->request());
	auto before = model.operations;
	check(throws([&] { journal->append_batch("overlap"); }) && before == model.operations, "pending artifact primitive rejects overlapping mutations before IO");
	journal.reset(); io.reset();
	check(model.locked && creation->complete(std::move(first)), "creation keeps backend and stable lock after facade retirement");
	drive_synchronously(*creation); auto builder = creation->take_builder();
	check(throws([&] { creation->take_builder(); }), "creation exposes exactly one builder facade"); creation.reset();
	{
		auto canceled = builder.begin_append_chunk("discarded");
	}
	auto payload = builder.begin_append_chunk("retained");
	{
		auto old = payload;
		drive_synchronously(*payload); payload->result();
		auto seal = builder.begin_finish(); payload.reset(); old.reset();
		check(throws([&] { builder.begin_append_chunk("overlap"); }), "retiring completed payload cannot release the sealing gate");
		while (!seal->done()) {
			seal->submitted(); auto completion = detail::execute_primitive(seal->io(), seal->request());
			check(throws([&] { seal->prepared_result(); }), "seal capability remains hidden until original directory barrier is delivered");
			check(seal->complete(std::move(completion)), "sealing retains matching completion ownership");
		}
		auto prepared = seal->prepared_result();
		check(prepared.descriptor().length == 8 && prepared.descriptor().checksum == crc32c("retained"), "unsubmitted payload cancellation leaves checksum and length unchanged");
	}
}

void suspended_append_ownership() {
	auto model = initialized(3);
	std::weak_ptr<MemoryIO> backend;
	{
	auto io = std::make_shared<MemoryIO>(model);
	backend = io;
	auto journal = std::make_unique<Journal>(io, 1024);
	journal->recover([](auto, auto) {});
	auto before = model.operations;
	check(throws([&] { journal->begin_append(std::string(1025, 'x')); }) && model.operations == before && !journal->fenced(), "owned append validates before IO");
	{
		auto canceled = journal->begin_append("canceled");
		check(model.operations == before, "begin append performs no IO");
	}
	check(!journal->fenced(), "unsubmitted cancellation keeps journal healthy");
	auto builder = journal->prepare_artifact();
	auto operation = journal->begin_append("second"); before = model.operations;
	check(throws([&] { journal->append_batch("other"); }) && throws([&] { journal->reclaim_step(); }) && throws([&] { builder.append_chunk("other"); }) && throws([&] { builder.finish(); }) && model.operations == before && !journal->fenced(), "pending append excludes mutations before IO without fencing");
	operation->submitted();
	auto completion = detail::execute_primitive(operation->io(), operation->request());
	journal.reset(); io.reset();
	check(model.locked && !backend.expired() && operation->in_flight(), "accepted operation retains backend file and stable lock after facade closes");
	check(operation->complete(std::move(completion)), "late completion returns to retained operation");
	drive_synchronously(*operation);
	check(model.locked && !backend.expired(), "completed operation retains storage lifetime until released");
	operation.reset();
	// The escaped builder continues to own the storage session until it closes.
	check(model.locked && !backend.expired(), "escaped preparation retains owned backend lifetime");
	}
	check(!model.locked && backend.expired(), "drained operation and final pin release backend and owner lock");
	check(replay(model) == std::vector<std::string>({"first", "second"}), "owner disappearance does not interrupt accepted durable sequencing");
}

void fenced_append_submission() {
	for (bool started : {false, true}) {
		auto model = initialized();
		{
			auto io = std::make_shared<MemoryIO>(model); Journal journal(io, 1024); journal.recover([](auto, auto) {});
			auto operation = journal.begin_append("second");
			if (started) { operation->submitted(); operation->complete(detail::execute_primitive(operation->io(), operation->request())); }
			journal.fence_storage(); auto before = model.operations; bool rejected = false;
			try { operation->submitted(); } catch (const std::exception&) { rejected = true; }
			// Reap a pre-correction accepted primitive instead of dropping in-flight IO.
			if (!rejected) { operation->complete(detail::execute_primitive(operation->io(), operation->request())); }
			check(rejected && model.operations == before && !operation->in_flight(), "fencing blocks new append primitives both before submission and between reaped steps");
		}
		check(replay(model) == std::vector<std::string>{"first"}, "rejected fenced submission preserves acknowledged history");
	}
	auto model = initialized();
	{
		auto io = std::make_shared<MemoryIO>(model); Journal journal(io, 1024); journal.recover([](auto, auto) {});
		auto operation = journal.begin_append("second"); operation->submitted();
		auto original = detail::execute_primitive(operation->io(), operation->request()); journal.fence_storage(); auto before = model.operations;
		check(operation->complete(std::move(original)) && operation->done() && !operation->in_flight() && throws([&] { operation->result(); }) && model.operations == before, "fencing still reaps the original accepted primitive without executing a later step");
	}
	check(replay(model) == std::vector<std::string>{"first"}, "original reaping after fencing cannot acknowledge interrupted append");
}

void suspended_append_retirement() {
	auto model = initialized(); auto io = std::make_shared<MemoryIO>(model); Journal journal(io, 1024);
	journal.recover([](auto, auto) {});
	auto first = journal.begin_append("second"); drive_synchronously(*first);
	check(journal.frontier().sequence == 1 && journal.finish_append(first).sequence == 2, "only explicit terminal settlement advances public frontier");
	auto second = journal.begin_append("third"); first.reset(); auto before = model.operations;
	check(throws([&] { journal.begin_append("overlap"); }) && model.operations == before && !journal.fenced(), "retiring old settled operation cannot release a newer mutation gate");
	drive_synchronously(*second); journal.finish_append(second); second.reset();
	{
		auto dropped = journal.begin_append("fourth"); drive_synchronously(*dropped);
	}
	check(journal.fenced(), "discarding durable but unsettled mutation fences stale facade metadata");

	auto abandoned = initialized(); auto backend = std::make_shared<MemoryIO>(abandoned); Journal owned(backend, 1024); owned.recover([](auto, auto) {});
	{
		auto dropped = owned.begin_append("uncertain"); dropped->submitted();
		dropped->complete(detail::execute_primitive(dropped->io(), dropped->request()));
	}
	check(owned.fenced(), "abandonment after a reaped mutating primitive fences without hidden IO");
}

void recovery_failures(bool extra_tail = false, bool checkpointed = false) {
	auto baseline = initialized();
	if (checkpointed) {
		MemoryIO io(baseline); Journal journal(io, 1024); journal.recover([](auto, auto) {});
		auto bundle = prepare(journal, "first"); auto application = prepare(journal, "application");
		journal.publish_checkpoint(bundle, std::array{application}, 1);
	}
	auto recover_values = [&](Model& model) { return checkpointed ? replay_checkpoint(model).second : replay(model); };
	if (extra_tail) {
		auto name = std::string(data_name);
		if (checkpointed) {
			auto selected = replay_checkpoint(baseline).first;
			name = "generation-" + detail::hexadecimal(selected.journal_identity);
		}
		auto& inode = baseline.visible.at(name);
		inode->visible += "extra unacknowledged bytes";
		inode->durable = inode->visible;
	}
	std::size_t operation_count;
	{
		auto model = baseline.clone(); recover_values(model); operation_count = model.operations;
	}
	for (std::size_t operation = 1; operation <= operation_count; ++operation) {
		for (bool after : {false, true}) {
			auto model = baseline.clone(); model.fail_operation = operation; model.fail_after = after;
			{
				MemoryIO io(model); Journal journal(io, 1024);
				check(throws([&] { journal.recover([](auto, auto) {}, [](auto&, auto& checkpoint, auto dependencies) { read_artifact(checkpoint); for (auto& dependency : dependencies) { read_artifact(dependency); } }); }) && journal.fenced(), "recovery I/O failure fences");
				check(throws([&] { journal.append_batch("after failed recovery"); }), "failed recovery remains unavailable");
			}
			model.fail_operation = 0;
			check(recover_values(model) == std::vector<std::string>{"first"}, "reopen after recovery failure preserves acknowledged data");
		}
	}
	{
		auto model = baseline.clone(); MemoryIO io(model); Journal journal(io, 1024);
		check(throws([&] { journal.recover([](auto, auto) { throw std::runtime_error("callback"); }); }) && journal.fenced(),
			"callback failure leaves state unpublished and journal fenced");
	}
	std::cout << "recovery operations tested: " << operation_count << '\n';
}

void corruption() {
	auto baseline = initialized();
	for (const auto name : {std::string_view("manifest"), data_name}) {
		auto length = baseline.visible.at(std::string(name))->visible.size();
		for (std::size_t byte = 0; byte < length; ++byte) {
			auto model = baseline.clone(); model.visible.at(std::string(name))->visible[byte] ^= 1;
			check(throws([&] { replay(model); }), "every corrupted durable byte fails closed");
		}
	}
	for (std::size_t length = 0; length < baseline.visible.at(std::string(data_name))->visible.size(); ++length) {
		auto model = baseline.clone(); model.visible.at(std::string(data_name))->visible.resize(length);
		check(throws([&] { replay(model); }), "every truncation of acknowledged history fails closed");
	}
	for (auto missing : {std::string("manifest"), std::string(data_name), std::string("owner.lock")}) {
		auto model = baseline.clone(); model.visible.erase(missing);
		check(throws([&] { replay(model); }), "missing initialized metadata cannot be treated as empty state");
	}
	{
		auto model = baseline.clone(); model.visible.erase("owner.lock"); model.visible.erase(std::string(data_name));
		auto previous_manifest = model.visible.at("manifest")->visible;
		MemoryIO io(model); Journal journal(io);
		check(throws([&] { journal.create(identity()); }), "existing manifest prevents initialization when lock and data are missing");
		check(model.visible.at("manifest")->visible == previous_manifest, "failed initialization preserves damaged store's metadata");
	}
	{
		auto model = baseline.clone();
		auto& bytes = model.visible.at(std::string(data_name))->visible;
		std::string header;
		put32(header, 0x3142544bu); put32(header, 1025); put64(header, 1); put32(header, crc32c("first")); put32(header, crc32c(header));
		bytes.replace(28, 24, header);
		check(throws([&] { replay(model); }), "checksummed oversized length is rejected before allocation");
	}
	{
		auto model = baseline.clone();
		auto& bytes = model.visible.at(std::string(data_name))->visible;
		std::string header;
		put32(header, 0x3142544bu); put32(header, 5); put64(header, 2); put32(header, crc32c("first")); put32(header, crc32c(header));
		bytes.replace(28, 24, header);
		check(throws([&] { replay(model); }), "checksummed noncontiguous sequence is rejected");
	}
	{
		auto model = baseline.clone(); auto& inode = model.visible.at(std::string(data_name));
		inode->visible += "unacknowledged corrupt tail";
		check(replay(model) == std::vector<std::string>{"first"}, "unacknowledged tail is discarded after frontier verification");
		model.power_loss(false);
		check(replay(model) == std::vector<std::string>{"first"}, "discarded tail is durably truncated");
	}
}

void initialization_failures() {
	Model complete;
	{
		MemoryIO io(complete); Journal journal(io, 1024); journal.create(identity());
	}
	auto count = complete.operations;
	for (std::size_t operation = 1; operation <= count; ++operation) {
		for (bool after : {false, true}) {
			Model model; model.fail_operation = operation; model.fail_after = after;
			{
				MemoryIO io(model); Journal journal(io, 1024);
				check(throws([&] { journal.create(identity()); }) && journal.fenced(), "interrupted initialization fences");
			}
			for (bool keep_visible : {false, true}) {
				auto crash = model.clone(); crash.power_loss(keep_visible);
				try { check(replay(crash).empty(), "initialized crash can recover only the zero frontier"); }
				catch (const std::exception&) { check(true, "incomplete initialization fails closed"); }
				if (crash.visible.contains("owner.lock")) {
					MemoryIO io(crash); Journal journal(io);
					check(throws([&] { journal.create(identity()); }), "creation cannot erase interrupted initialization");
				}
			}
		}
	}
	std::cout << "initialization operations tested: " << count << '\n';
}

void posix() {
	// Repository scratch is caller-created; tests own this unique directory.
	auto directory = std::filesystem::current_path() / ".scratch" / ("journal-test-" + std::to_string(::getpid()));
	std::filesystem::create_directories(directory);
	::chmod(directory.c_str(), 0700);
	struct Cleanup { std::filesystem::path directory; ~Cleanup() { std::filesystem::remove_all(directory); } } cleanup{directory};
	{
		PosixIO io(directory, PosixIOOptions{FlushMode::full}); Journal journal(io, 1024);
		journal.create(identity()); journal.append_batch("real filesystem");
		PosixIO second_io(directory, PosixIOOptions{FlushMode::full}); Journal second(second_io);
		check(throws([&] { second.recover([](auto, auto) {}); }), "POSIX lock excludes another open owner");
		check(throws([&] { io.create_exclusive("../escape"); }), "POSIX filename cannot escape directory");
	}
	{
		PosixIO io(directory, PosixIOOptions{FlushMode::full}); Journal journal(io, 1024);
		std::vector<std::string> batches;
		journal.recover([&](auto, std::string_view batch) { batches.emplace_back(batch); });
		check(batches == std::vector<std::string>{"real filesystem"}, "POSIX reopen replays acknowledged batch");
		check(journal.append_batch("after reopen").sequence == 2, "POSIX reopened writer advances sequence");
		auto bundle = prepare(journal, "prefix image"); std::array dependencies{prepare(journal, "app")};
		journal.publish_checkpoint(bundle, dependencies, 2); journal.append_batch("tail");
	}
	{
		PosixIO io(directory, PosixIOOptions{FlushMode::full}); Journal journal(io, 1024); std::string image, dependency, tail;
		auto frontier = journal.recover([&](auto sequence, std::string_view batch) { check(sequence == 3, "POSIX checkpoint suffix sequence continues"); tail = batch; },
			[&](const Frontier&, ArtifactReader& bundle, std::span<ArtifactReader> dependencies) { image = read_artifact(bundle); dependency = read_artifact(dependencies[0]); });
		check(frontier.base_sequence == 2 && image == "prefix image" && dependency == "app" && tail == "tail", "POSIX checkpoint generation restores declared artifact and suffix");
		std::vector<std::string> preserved;
		auto reserved_name = [&] { auto name = "artifact-" + detail::hexadecimal(detail::random_identity()); preserved.push_back(name); return name; };
		auto symlink = reserved_name(); std::filesystem::create_symlink("manifest", directory / symlink);
		auto partial = reserved_name(); auto partial_file = io.create_exclusive(partial); detail::write_all(*partial_file, 0, std::string(1024, '\0')); partial_file->sync(); partial_file.reset();
		auto hardlink = reserved_name(); std::filesystem::create_hard_link(directory / partial, directory / hardlink);
		auto fifo = reserved_name(); check(::mkfifo((directory / fifo).c_str(), 0600) == 0, "create reserved FIFO candidate");
		std::filesystem::create_directory(directory / reserved_name());
		ArtifactDescriptor foreign{detail::random_identity(), 0, crc32c("")}; auto foreign_store = identity(); foreign_store[0] = 'Z';
		preserved.push_back(artifact_name(foreign)); auto foreign_file = io.create_exclusive(artifact_name(foreign)); detail::write_all(*foreign_file, 0, detail::artifact_header(foreign_store, foreign)); foreign_file->sync(); foreign_file.reset();
		ArtifactDescriptor corrupt{detail::random_identity(), 0, crc32c("")}; auto corrupt_header = detail::artifact_header(identity(), corrupt); corrupt_header[40] ^= 1;
		preserved.push_back(artifact_name(corrupt)); auto corrupt_file = io.create_exclusive(artifact_name(corrupt)); detail::write_all(*corrupt_file, 0, corrupt_header); corrupt_file->sync(); corrupt_file.reset();
		io.sync_directory();
		auto stats = journal.reclaim_step(4096);
		check(stats.complete && stats.removed == 1 && !std::filesystem::exists(directory / std::string(data_name)), "POSIX reclaimer removes obsolete owned generation");
		for (const auto& name : preserved) { check(std::filesystem::symlink_status(directory / name).type() != std::filesystem::file_type::not_found, "POSIX reclaimer preserves foreign unknown and unsafe candidates"); }
		check(stats.unknown_files == preserved.size(), "POSIX skipped ownership candidates are reported");
	}
	std::filesystem::create_symlink("manifest", directory / "symlink");
	PosixIO io(directory, PosixIOOptions{FlushMode::full});
	check(throws([&] { io.open_existing("symlink"); }), "POSIX symlink file is rejected");
	std::filesystem::create_hard_link(directory / "manifest", directory / "hardlink");
	check(throws([&] { io.open_existing("hardlink"); }), "POSIX shared inode is rejected");
	check(::mkfifo((directory / "fifo").c_str(), 0600) == 0, "create FIFO fixture");
	check(throws([&] { io.open_existing("fifo"); }), "POSIX special file is rejected without blocking");
}

}  // namespace
// `journal_test reopen_<case>` runs one case; with none, every case but the long reopen ones.
int main(int argc, char** argv) {
	const std::map<std::string_view, std::function<void()>> cases{
		{"reopen_clean_close", reopen_clean_close}, {"reopen_released", [] { reopen_released(false); reopen_released(true); }},
		{"reopen_two_stores", reopen_two_stores}, {"reopen_refused", reopen_refused}, {"reopen_race", reopen_race}, {"reopen_crash", reopen_crash},
		{"reopen_churn", reopen_churn}, {"reopen_heap", reopen_heap}, {"reopen_distinct", reopen_distinct}, {"reclaim_syncs", reclaim_syncs}, {"park_count", park_count}};
	if (argc > 1) {
		const auto found = cases.find(argv[1]);
		if (found == cases.end()) { std::cerr << "no case " << argv[1] << '\n'; return 2; }
		std::cout << argv[1] << ":\n";
		try { found->second(); } catch (const std::exception& error) { check(false, error.what()); }
		std::cout << checks << " journal checks, " << failures << " failures\n";
		return failures ? 1 : 0;
	}
	try { park_count(); reopen_clean_close(); reopen_released(false); reopen_released(true); reopen_two_stores(); reopen_refused(); reopen_race(); basics(); append_failures(); append_failures(3); append_failures(std::numeric_limits<std::size_t>::max(), true); append_failures(3, true); append_failures(std::numeric_limits<std::size_t>::max(), false, true); append_failures(3, false, true); append_failures(std::numeric_limits<std::size_t>::max(), true, true); append_failures(3, true, true); suspended_artifact_operations(); suspended_artifact_ownership(); suspended_append_ownership(); fenced_append_submission(); suspended_append_retirement(); recovery_failures(); recovery_failures(true); recovery_failures(false, true); recovery_failures(true, true); corruption(); initialization_failures(); checkpoint_basics(); checkpoint_successive_generations(); suspended_artifact_open(); suspended_artifact_reads(); incremental_artifact_verification(); store_incremental_verification(); published_artifact_selection(); store_published_selection(); artifact_verification(); preparation_ownership_and_failures(); checkpoint_publication_failures(); checkpoint_publication_failures(3); checkpoint_publication_failures(std::numeric_limits<std::size_t>::max(), true); checkpoint_publication_failures(3, true); checkpoint_corruption(); reclamation_roots_and_unknowns(); reclamation_failures(); reclamation_failures(3); reclamation_failures(std::numeric_limits<std::size_t>::max(), true); reclamation_failures(3, true); durable_staging_ownership(); mixed_artifact_formats(); footprint_plans(); checkpoint_publication_retirement(); store_publication_completions(); store_artifact_completions(); store_artifact_completions(true); detached_old_artifact_lease(); store_append_completions(); store_reclaim_before_inventory(); store_replacement_growth(); store_basics(); store_failures(); store_append_and_cleanup_failures(); store_limits_and_pins(); store_posix(); posix(); }
	catch (const std::exception& error) { check(false, error.what()); }
	std::cout << checks << " journal checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
