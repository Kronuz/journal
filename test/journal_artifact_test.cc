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

// Checkpoints and artifacts on the memory disk: preparation, publication, reclamation, resource
// plans and the incremental verification of an artifact. Run by journal_test.cc.
#include "journal_support.h"

void checkpoint_basics() {
	Checksum checksum; checksum.update("123"); checksum.update("456"); checksum.update("789");
	check(checksum.value() == crc32c("123456789"), "streamed checksum matches complete bytes");
	auto model = initialized(3);
	{
		MemoryIO io(model); Journal journal(io, 1024); journal.recover([](auto, auto) {});
		auto application = prepare(journal, "application"); auto bundle = prepare(journal, "first");
		std::array dependencies{application};
		auto frontier = journal.publish_checkpoint(bundle, dependencies, 1);
		check(frontier.version == 2 && frontier.generation == 2 && frontier.sequence == 1 && frontier.base_sequence == 1,
			"checkpoint generation preserves the covered storage sequence");
		check(journal.append_batch("second").sequence == 2, "first post-checkpoint append continues the storage sequence");
		check(throws([&] { journal.publish_checkpoint(bundle, dependencies, 1); }) && !journal.fenced(), "stale cutover frontier is rejected before I/O");
		check(throws([&] { journal.publish_checkpoint(bundle, std::array{application, application}, 2); }) && !journal.fenced(), "duplicate immutable dependencies are rejected before I/O");
	}
	model.power_loss(false);
	auto [frontier, batches] = replay_checkpoint(model);
	check(batches == std::vector<std::string>({"first", "second"}) && frontier.sequence == 2 && frontier.base_sequence == 1,
		"recovery restores opaque checkpoint then replays its journal suffix");
	check(throws([&] { replay(model); }), "checkpoint store requires an explicit restoration callback");

	auto foreign_model = initialized(); MemoryIO foreign_io(foreign_model); Journal foreign(foreign_io, 1024);
	foreign.recover([](auto, auto) {}); auto prepared = prepare(foreign, "foreign");
	MemoryIO local_io(model); Journal local(local_io, 1024);
	local.recover([](auto, auto) {}, [](auto&, auto&, auto) {});
	check(throws([&] { local.publish_checkpoint(prepared, {}, 2); }) && !local.fenced(), "foreign owner session cannot publish an artifact");
}



void artifact_verification() {
	auto model = initialized(); MemoryIO io(model); Journal journal(io, 1024); journal.recover([](auto, auto) {});
	auto artifact = prepare(journal, "immutable"); journal.verify_artifact(artifact);
	auto& bytes = model.visible.at(artifact_name(artifact.descriptor()))->visible;
	bytes.back() ^= 1;
	check(throws([&] { journal.verify_artifact(artifact); }) && journal.fenced(), "optional preflight readback detects corruption and fences owner");
	check(throws([&] { journal.publish_checkpoint(artifact, {}, 1); }), "failed verification prevents a later cutover");
}

void checkpoint_successive_generations() {
	auto model = initialized();
	{
		MemoryIO io(model); Journal journal(io, 1024); journal.recover([](auto, auto) {});
		auto application = prepare(journal, "application"); auto bundle = prepare(journal, "first");
		journal.publish_checkpoint(bundle, std::array{application}, 1);
	}
	{
		MemoryIO io(model); Journal journal(io, 1024);
		auto selected = journal.recover([](auto, auto) {}, [](auto&, auto&, auto) {});
		auto application = journal.pin_artifact(selected.dependencies.at(0));
		// Reuse a referenced immutable dependency after closing and recovering.
		auto bundle = prepare(journal, "first");
		auto next = journal.publish_checkpoint(bundle, std::array{application}, 1);
		check(next.generation == 3 && next.journal_identity != selected.journal_identity, "successive publication uses an exclusive physical generation");
		check(journal.append_batch("second").sequence == 2, "reused dependency preserves sequence continuity");
	}
	model.power_loss(false);
	check(replay_checkpoint(model).second == std::vector<std::string>{"first", "second"}, "successive generation and pinned dependency recover");
	{
		MemoryIO io(model); Journal journal(io, 1024);
		check(throws([&] { journal.recover([](auto, auto) {}, [](auto&, auto&, auto) { throw std::runtime_error("restore"); }); }) && journal.fenced(), "restoration callback failure keeps recovered state unpublished");
	}
}

void preparation_ownership_and_failures() {
	auto model = initialized(); MemoryIO io(model);
	auto journal = std::make_unique<Journal>(io, 1024); journal->recover([](auto, auto) {});
	std::optional<ArtifactBuilder> builder; builder.emplace(journal->prepare_artifact()); builder->append_chunk("first");
	check(throws([&] { journal->prepare_artifact(); }) && !journal->fenced(), "only one chunked preparation is active");
	check(throws([&] { builder->append_chunk(std::string(detail::artifact_chunk_size + 1, 'x')); }) && !journal->fenced(), "oversized preparation chunk has no I/O effect");
	journal.reset();
	{
		MemoryIO another_io(model); Journal another(another_io);
		check(throws([&] { another.recover([](auto, auto) {}); }), "preparation retains stable lock after journal closes");
	}
	std::optional<PreparedArtifact> artifact; artifact.emplace(builder->finish());
	check(throws([&] { builder->append_chunk("late"); }), "sealed artifact cannot receive further writes");
	builder.reset();
	{
		MemoryIO another_io(model); Journal another(another_io);
		check(throws([&] { another.recover([](auto, auto) {}); }), "prepared handle retains its owner session");
	}
	artifact.reset(); check(replay(model) == std::vector<std::string>{"first"}, "closing preparations allows ordinary recovery");

	for (auto chunk : {std::numeric_limits<std::size_t>::max(), std::size_t{3}}) {
		auto baseline = initialized(); baseline.chunk = chunk; std::size_t operations;
		{
			auto complete = baseline.clone(); MemoryIO complete_io(complete); Journal complete_journal(complete_io, 1024);
			complete_journal.recover([](auto, auto) {}); complete.operations = 0; prepare(complete_journal, "first"); operations = complete.operations;
		}
		for (std::size_t operation = 1; operation <= operations; ++operation) {
			for (bool after : {false, true}) {
				auto failed = baseline.clone();
				{
					MemoryIO failed_io(failed); Journal failed_journal(failed_io, 1024); failed_journal.recover([](auto, auto) {});
					failed.operations = 0; failed.fail_operation = operation; failed.fail_after = after;
					check(throws([&] { prepare(failed_journal, "first"); }) && failed_journal.fenced(), "every uncertain preparation operation fences writer");
				}
				for (bool visible : {false, true}) {
					auto crashed = failed.clone(); crashed.power_loss(visible);
					check(replay(crashed) == std::vector<std::string>{"first"}, "unfinished preparation cannot replace active history");
				}
			}
		}
		std::cout << "artifact preparation operations tested: " << operations << '\n';
	}
	{
		auto bounded = initialized(); MemoryIO bounded_io(bounded); Journal writer(bounded_io, 1024, 3);
		writer.recover([](auto, auto) {});
		auto stream = writer.prepare_artifact(); stream.append_chunk("abc");
		check(throws([&] { stream.append_chunk("d"); }) && !writer.fenced(), "cumulative preparation limit rejects before I/O");
		std::vector<PreparedArtifact> handles; handles.push_back(stream.finish());
		for (unsigned i = 1; i < detail::maximum_prepared_artifacts; ++i) { handles.push_back(prepare(writer, "")); }
		check(throws([&] { writer.prepare_artifact(); }) && !writer.fenced(), "prepared artifact leases have a bounded slot count");
		handles.pop_back(); handles.push_back(prepare(writer, "new"));
		check(handles.size() == detail::maximum_prepared_artifacts, "releasing a preparation lease frees its slot");
	}
}

namespace {
void reclaim_all(Journal& journal, std::size_t budget = 1) {
	for (std::size_t steps = 0; steps < 1000; ++steps) {
		auto stats = journal.reclaim_step(budget); check(stats.scanned <= budget, "reclamation bounds scanned entries including protected and unknown names");
		if (stats.complete) { return; }
	}
	throw std::runtime_error("reclamation pass did not complete");
}
}  // namespace
void reclamation_roots_and_unknowns() {
	auto model = reclaim_fixture();
	MemoryIO io(model); std::optional<ArtifactReader> escaped;
	Journal journal(io, 1024);
	journal.recover([](auto, auto) {}, [&](auto&, auto& bundle, auto) { escaped.emplace(std::move(bundle)); });
	check(throws([&] { journal.reclaim_step(0); }) && !journal.fenced(), "invalid scan admission leaves journal usable");
	auto escaped_name = artifact_name(escaped->descriptor());
	std::string prepared_name, active_name;
	{
		auto held = prepare(journal, "held"); prepared_name = artifact_name(held.descriptor());
		std::set<std::string> previous_names;
		for (const auto& [name, inode] : model.visible) { previous_names.insert(name); }
		auto builder = journal.prepare_artifact(); builder.append_chunk("active");
		for (const auto& [name, inode] : model.visible) { if (!previous_names.contains(name)) { active_name = name; } }
		reclaim_all(journal);
		check(model.visible.contains(prepared_name) && model.visible.contains(active_name), "prepared handles and active builder protect their names");
		auto finished = builder.finish();
		check(model.visible.contains(artifact_name(finished.descriptor())), "protected builder remains finishable after reclamation");
	}
	auto sentinel = std::string("artifact-00000000000000000000000000000000");
	model.visible[sentinel] = std::make_shared<Inode>();
	auto paused = journal.reclaim_step(1);
	check(!paused.complete && paused.scanned == 1, "reclamation retains its cursor across a partial scan");
	{
		auto bundle = prepare(journal, "first"); auto app = prepare(journal, "application"); journal.publish_checkpoint(bundle, std::array{app}, 1);
	}
	auto newest = journal.frontier();
	reclaim_all(journal);
	check(model.visible.contains(artifact_name(*newest.checkpoint)) && model.visible.contains(artifact_name(newest.dependencies[0])) &&
		model.visible.contains("generation-" + detail::hexadecimal(newest.journal_identity)), "publication between scan steps rechecks and protects newly authoritative roots");
	io.remove(sentinel); io.sync_directory();
	check(model.visible.contains(escaped_name), "escaped recovery reader pins obsolete artifact across publication");
	escaped.reset(); reclaim_all(journal);
	check(!model.visible.contains(escaped_name) && !model.visible.contains(prepared_name) && !model.visible.contains(active_name), "released pins make only their obsolete artifacts reclaimable");
	check(model.visible.size() == 5 && model.visible.contains("manifest") && model.visible.contains("owner.lock"), "reclamation retains exactly active generation and required roots");
	// Keep partial, foreign, basename-mismatched and unrelated files intact.
	auto unclassified = "artifact-" + detail::hexadecimal(detail::random_identity());
	model.visible[unclassified] = std::make_shared<Inode>(); model.visible[unclassified]->visible.assign(56 + 1024, '\0');
	auto mismatch = "artifact-" + detail::hexadecimal(detail::random_identity());
	model.visible[mismatch] = model.visible.at(artifact_name(*journal.frontier().checkpoint));
	Model foreign;
	std::string foreign_name;
	{
		MemoryIO foreign_io(foreign); Journal other(foreign_io, 1024); auto id = identity(); id[0] = 'Z'; other.create(id);
		auto file = prepare(other, "foreign"); foreign_name = artifact_name(file.descriptor());
		model.visible[foreign_name] = foreign.visible.at(foreign_name);
	}
	model.visible["notes.txt"] = std::make_shared<Inode>();
	auto stats = journal.reclaim_step(4096);
	check(stats.complete && stats.unknown_files == 3 && stats.removed == 0 && !journal.fenced(), "unknown ownership is reported without deleting or fencing");
	check(model.visible.contains(unclassified) && model.visible.contains(mismatch) && model.visible.contains(foreign_name) && model.visible.contains("notes.txt"), "unknown and unrelated files remain untouched");
}

void durable_staging_ownership() {
	auto model = initialized(); std::string abandoned;
	{
		MemoryIO io(model); Journal journal(io, 1024); journal.recover([](auto, auto) {});
		auto builder = journal.prepare_artifact();
		for (const auto& [name, inode] : model.visible) { if (name.starts_with("artifact-")) { abandoned = name; } }
		auto& inode = model.visible.at(abandoned);
		check(inode->durable.size() == detail::artifact_header_size && inode->durable.substr(0, 44) == inode->visible.substr(0, 44), "builder synchronizes ownership before accepting payload");
		auto prefix = inode->visible.substr(0, 44);
		for (unsigned chunk = 0; chunk < 8; ++chunk) { builder.append_chunk(std::string(64 * 1024, 'x')); }
		check(inode->visible.substr(0, 44) == prefix, "payload writes cannot overwrite immutable ownership");
		// Model spontaneous payload writeback and a directory barrier from an
		// interleaved append, without sealing the abandoned preparation.
		inode->durable = inode->visible; journal.append_batch("second");
	}
	model.power_loss(false);
	{
		MemoryIO io(model); Journal journal(io, 1024); std::vector<std::string> replayed;
		journal.recover([&](auto, auto bytes) { replayed.emplace_back(bytes); });
		auto stats = journal.reclaim_step(4096);
		check(stats.removed == 1 && stats.logical_bytes == detail::artifact_header_size + 8 * 64 * 1024 && !model.visible.contains(abandoned), "restart reclaims large unsealed artifact from durable ownership only");
		check(replayed == std::vector<std::string>{"first", "second"}, "large orphan cleanup preserves every acknowledged append");
		auto builder = journal.prepare_artifact(); std::string name;
		for (const auto& [candidate, inode] : model.visible) { if (candidate.starts_with("artifact-")) { name = candidate; } }
		auto prefix = model.visible.at(name)->visible.substr(0, 44); builder.append_chunk("sealed"); auto artifact = builder.finish();
		check(model.visible.at(name)->visible.substr(0, 44) == prefix, "finish replaces only the seal and preserves ownership bytes");
	}
}
void mixed_artifact_formats() {
	auto model = reclaim_fixture(); auto copy = model.clone(); auto selected = replay_checkpoint(copy).first;
	auto legacy = selected.dependencies.at(0); auto legacy_name = artifact_name(legacy);
	auto& inode = model.visible.at(legacy_name); inode->visible = detail::artifact_header_v1(selected.identity, legacy) + "application"; inode->durable = inode->visible;
	{
		MemoryIO io(model); std::optional<ArtifactReader> old_reader, new_reader; Journal journal(io, 1024);
		journal.recover([](auto, auto) {}, [&](auto&, auto& bundle, auto dependencies) { new_reader.emplace(std::move(bundle)); old_reader.emplace(std::move(dependencies[0])); });
		check(read_artifact(*old_reader) == "application" && read_artifact(*new_reader) == "first", "mixed v1 dependency and v2 bundle recover with distinct payload offsets");
		*new_reader = std::move(*old_reader);
		check(read_artifact(*new_reader) == "application", "reader move assignment preserves detected legacy offset"); old_reader.reset(); new_reader.reset();
		auto pinned = journal.pin_artifact(legacy); auto bundle = prepare(journal, "first"); journal.publish_checkpoint(bundle, std::array{pinned}, 1);
	}
	model.power_loss(false); check(replay_checkpoint(model).second == std::vector<std::string>{"first"}, "new publication can reuse an immutable legacy dependency");
	{
		MemoryIO io(model); Journal journal(io, 1024); journal.recover([](auto, auto) {}, [](auto&, auto&, auto) {});
		{ auto bundle = prepare(journal, "first"); auto app = prepare(journal, "application"); journal.publish_checkpoint(bundle, std::array{app}, 1); }
		reclaim_all(journal); check(!model.visible.contains(legacy_name), "obsolete sealed legacy artifact remains reclaimable");
	}
	for (std::size_t length : {44u, 67u, 68u}) {
		auto missing_seal = model.clone(); auto state = replay_checkpoint(missing_seal).first;
		auto& required = missing_seal.visible.at(artifact_name(*state.checkpoint)); required->visible.resize(length); required->durable = required->visible;
		check(throws([&] { replay_checkpoint(missing_seal); }), "referenced artifacts reject incomplete seals or payloads");
	}
}

namespace {
template <class Operation> void check_plan(Model& model, MutationPlan plan, Operation operation) {
	auto before = visible_resources(model); auto peak = detail::resources_add(before, plan.peak);
	model.observe = [&] { check(detail::resources_fit(visible_resources(model), peak), "every primitive filesystem state is covered by the planned simultaneous peak"); };
	operation(); model.observe = {};
	check(visible_resources(model) == detail::resources_subtract(detail::resources_add(before, plan.added), plan.removed), "successful mutation matches planned additions and removed manifest only");
}
}  // namespace
void footprint_plans() {
	check(Journal::append_footprint(5).peak == StorageResources{361, 1}, "format-only append bound is available before journal startup");
	if constexpr (sizeof(std::size_t) > sizeof(std::uint32_t)) {
		check(throws([] { Journal::append_footprint(static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1); }), "format-only planner rejects unrepresentable payloads without IO");
	}
	for (auto chunk : {std::numeric_limits<std::size_t>::max(), std::size_t{3}}) {
		Model model; model.chunk = chunk; MemoryIO io(model); Journal journal(io, 1024);
		check_plan(model, Journal::bootstrap_plan(), [&] { journal.create(identity()); });
		check_plan(model, journal.append_plan(5), [&] { journal.append_batch("first"); });
		auto waiting_append = journal.append_plan(5);
		for (std::size_t count : {2u, 9u, 1u}) {
			std::vector<std::uint64_t> lengths(count, 3);
			auto plan = journal.checkpoint_plan(lengths);
			check_plan(model, plan, [&] {
				std::vector<PreparedArtifact> artifacts;
				for (std::size_t artifact = 0; artifact < count; ++artifact) { artifacts.push_back(prepare(journal, "app")); }
				journal.publish_checkpoint(artifacts[0], std::span<const PreparedArtifact>(artifacts.data() + 1, artifacts.size() - 1), journal.frontier().sequence);
			});
		}
		check_plan(model, waiting_append, [&] { journal.append_batch("later"); });
		auto footprint = journal.artifact_footprint(5);
		check_plan(model, {footprint, footprint, {}}, [&] { auto builder = journal.prepare_artifact(); builder.append_chunk("known"); });
		check(throws([&] { journal.append_plan(1025); }) && throws([&] { journal.checkpoint_plan({}); }) && !journal.fenced(), "invalid footprint admission is rejected before mutation without fencing");
	}
	auto model = initialized(); MemoryIO io(model);
	auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - detail::artifact_header_size;
	Journal journal(io, 1024, maximum); journal.recover([](auto, auto) {});
	std::array<std::uint64_t, 3> lengths{maximum, maximum, maximum};
	check(throws([&] { journal.checkpoint_plan(lengths); }) && !journal.fenced(), "aggregate checkpoint plan overflow rejects before any IO");
}

void checkpoint_publication_retirement() {
	for (bool terminal : {false, true}) {
		auto model = initialized();
		{
			auto io = std::make_shared<MemoryIO>(model); Journal journal(io, 1024); journal.recover([](auto, auto) {});
			auto bundle = prepare(journal, "first"); std::array dependencies{prepare(journal, "application")};
			{
				auto canceled = journal.begin_checkpoint(bundle, dependencies, 1); auto before = model.operations;
				check(throws([&] { journal.append_batch("overlap"); }) && !journal.fenced() && model.operations == before, "unsubmitted publication exclusively owns mutation gate before IO");
			}
			check(!journal.fenced() && journal.frontier().generation == 1, "unsubmitted checkpoint cancellation leaves old facade usable");
			{
				auto dropped = journal.begin_checkpoint(bundle, dependencies, 1);
				if (terminal) { drive_synchronously(*dropped); dropped->result(); }
				else { dropped->submitted(); dropped->complete(detail::execute_primitive(dropped->io(), dropped->request())); }
			}
			check(journal.fenced(), "discarding a started publication without frontier settlement fences stale metadata");
		}
		auto [frontier, values] = replay_checkpoint(model);
		check(values == std::vector<std::string>{"first"} && frontier.generation == (terminal ? 2 : 1), "retired unsettled publication recovers the physically selected acknowledged history");
	}
}

void suspended_artifact_open() {
	for (bool legacy : {false, true}) { for (auto chunk : {std::size_t{3}, std::numeric_limits<std::size_t>::max()}) {
		auto model = initialized(); auto io = std::make_shared<MemoryIO>(model); Journal journal(io, 1024); journal.recover([](auto, auto) {});
		std::optional<PreparedArtifact> artifact(prepare(journal, "candidate")); auto descriptor = artifact->descriptor(); auto inode = model.visible.at(artifact_name(descriptor));
		auto header = legacy ? detail::artifact_v1_header_size : detail::artifact_header_size;
		if (legacy) { inode->visible = detail::artifact_header_v1(journal.frontier().identity, descriptor) + "candidate"; inode->durable = inode->visible; }
		model.chunk = chunk; std::size_t payload_reads = 0; model.observe_read = [&](const Inode* source, auto offset, auto count) { if (source == inode.get() && offset + count > header) { ++payload_reads; } };
		auto before = model.operations; auto operation = *journal.begin_artifact_open(*artifact);
		check(model.operations == before && throws([&] { operation->take_verifier(); }) && !journal.fenced(), "metadata capability capture has no IO or premature verifier result");
		artifact.reset(); journal.reclaim_step(4096); check(model.visible.contains(artifact_name(descriptor)), "pre-open metadata pin protects exact artifact after preparation interest disappears");
		while (!operation->done()) {
			operation->submitted(); auto completed = detail::execute_primitive(operation->io(), operation->request());
			check(throws([&] { operation->take_verifier(); }) && !journal.fenced(), "executed metadata cannot produce a verifier before original reap");
			MutationCompletion stale; stale.token = operation->request().token; ++stale.token.step;
			check(!operation->complete(std::move(stale)) && operation->in_flight(), "stale metadata callback cannot release original primitive");
			check(operation->complete(std::move(completed)), "original metadata callback advances bounded open states");
		}
		auto verifier = operation->take_verifier(); check(payload_reads == 0 && verifier.offset() == 0 && throws([&] { operation->take_verifier(); }), "metadata result is single-consumption and reads no payload bytes");
		operation.reset(); std::array<char, 64> buffer{};
		while (verifier.offset() < descriptor.length) { verifier.read_next(buffer); }
		auto reader = std::move(verifier).finish(); check(read_artifact(reader) == "candidate", "both artifact formats promote from the same suspended metadata algorithm");
	} }
	{
		auto model = initialized(); auto io = std::make_shared<MemoryIO>(model); auto journal = std::make_unique<Journal>(io, 1024); journal->recover([](auto, auto) {});
		std::weak_ptr<MemoryIO> backend = io; std::optional<PreparedArtifact> artifact(prepare(*journal, "candidate")); auto operation = *journal->begin_artifact_open(*artifact); operation->submitted();
		artifact.reset(); journal.reset(); io.reset(); check(!backend.expired() && model.locked, "accepted metadata open retains backend and owner lock after facade retirement");
		auto completed = detail::execute_primitive(operation->io(), operation->request()); operation->complete(std::move(completed)); drive_synchronously(*operation);
		{ auto verifier = operation->take_verifier(); std::array<char, 64> buffer{}; while (verifier.offset() < verifier.descriptor().length) { verifier.read_next(buffer); } auto reader = std::move(verifier).finish(); check(read_artifact(reader) == "candidate", "retired metadata owner can still produce and promote its exact verifier"); }
		operation.reset(); check(backend.expired() && !model.locked, "final metadata capability release retires backend and owner lock together");
	}
}

void suspended_artifact_reads() {
	for (auto chunk : {std::size_t{3}, std::size_t{65536}}) {
		auto model = initialized(); auto io = std::make_shared<MemoryIO>(model); Journal journal(io, 1024); journal.recover([](auto, auto) {});
		auto artifact = prepare(journal, "candidate"); auto verifier = journal.begin_artifact_verification(artifact); model.chunk = chunk;
		auto before = model.operations; auto canceled = verifier->begin_read_next(64); canceled.reset();
		check(model.operations == before && verifier->offset() == 0 && !journal.fenced(), "unsubmitted read cancellation leaves checksum cursor and storage unchanged");
		std::string candidate; std::shared_ptr<ArtifactRead> old;
		while (verifier->offset() < artifact.descriptor().length) {
			auto offset = verifier->offset(); auto operation = verifier->begin_read_next(64);
			old.reset(); check(throws([&] { verifier->begin_read_next(64); }) && !journal.fenced(), "held verifier read rejects overlapping work without fencing");
			operation->submitted(); auto completed = detail::execute_primitive(operation->io(), operation->request());
			check(verifier->offset() == offset && throws([&] { operation->result(); }) && throws([&] { verifier->finish_read_next(operation); }), "executed read cannot expose results or checksum progress before original reap");
			MutationCompletion stale; stale.token = operation->request().token; stale.count = 1; ++stale.token.operation;
			check(!operation->complete(std::move(stale)) && operation->in_flight(), "stale read completion cannot release original slot");
			check(operation->complete(std::move(completed)) && verifier->offset() == offset, "matching reap exposes only owned candidate bytes before verification settlement");
			auto bytes = operation->result(); candidate.append(bytes.data(), bytes.size());
			check(verifier->finish_read_next(operation) == bytes.size() && verifier->offset() == offset + bytes.size(), "verification settlement advances exact cursor and checksum once");
			check(throws([&] { verifier->finish_read_next(operation); }) && !journal.fenced(), "duplicate verification settlement rejects without changing cursor"); old = operation;
		}
		check(candidate == "candidate", "suspended verification preserves bounded short-read payload");
		auto reader = std::move(*verifier).finish(); auto operation = reader.begin_read_at(0, 3); old.reset();
		check(throws([&] { reader.begin_read_at(0, 3); }) && !journal.fenced(), "reader retains one original read slot despite old capability destruction");
		drive_synchronously(*operation); auto bytes = operation->result(); check(std::string_view(bytes.data(), bytes.size()) == "can", "promoted exact file supports owned payload reads");
	}
	for (int fault : {0, 1, 2, 3}) {
		auto model = initialized(); auto io = std::make_shared<MemoryIO>(model); Journal journal(io, 1024); journal.recover([](auto, auto) {});
		auto artifact = prepare(journal, "candidate"); auto verifier = journal.begin_artifact_verification(artifact); auto operation = verifier->begin_read_next(64);
		if (fault < 2) { model.fail_operation = model.operations + 1; model.fail_after = fault == 1; }
		else if (fault == 2) { model.chunk = 0; } else { model.oversized_read = true; }
		drive_synchronously(*operation);
		check(operation->done() && throws([&] { operation->result(); }) && journal.fenced() && verifier->offset() == 0, "uncertain or invalid read fences without advancing verification");
	}
	{
		auto model = initialized(); MemoryIO io(model); Journal journal(io, 1024); journal.recover([](auto, auto) {}); auto artifact = prepare(journal, "candidate"); auto verifier = journal.begin_artifact_verification(artifact); auto before = model.operations;
		check(throws([&] { verifier->begin_read_next(64); }) && model.operations == before && !journal.fenced(), "borrowed backend rejects escaped completion read before IO");
	}
	{
		auto model = initialized(); auto io = std::make_shared<MemoryIO>(model); Journal journal(io, 1024); journal.recover([](auto, auto) {}); auto artifact = prepare(journal, "candidate"); auto verifier = journal.begin_artifact_verification(artifact); auto operation = verifier->begin_read_next(64); operation->submitted(); auto completed = detail::execute_primitive(operation->io(), operation->request()); journal.fence_storage();
		check(operation->complete(std::move(completed)) && operation->done() && throws([&] { operation->result(); }), "fenced owner still reaps original read and exposes no successful bytes");
	}
}

void incremental_artifact_verification() {
	for (bool legacy : {false, true}) { for (std::size_t chunk : {std::size_t{3}, std::numeric_limits<std::size_t>::max()}) {
		for (const std::string& payload : {std::string{}, std::string("candidate"), std::string(65553, 'x')}) {
			auto model = initialized(); MemoryIO io(model); Journal journal(io, 1024); journal.recover([](auto, auto) {});
			auto artifact = prepare(journal, payload); auto inode = model.visible.at(artifact_name(artifact.descriptor()));
			auto header = legacy ? detail::artifact_v1_header_size : detail::artifact_header_size;
			if (legacy) { inode->visible = detail::artifact_header_v1(journal.frontier().identity, artifact.descriptor()) + payload; inode->durable = inode->visible; }
			model.chunk = chunk; std::size_t payload_reads = 0, largest = 0;
			model.observe_read = [&](const Inode* source, auto offset, auto count) {
				if (source == inode.get() && offset + count > header) { ++payload_reads; largest = std::max(largest, count); }
			};
			auto verifier = journal.begin_artifact_verification(artifact);
			check(verifier && payload_reads == 0 && verifier->offset() == 0, "incremental open validates fixed metadata without payload scanning");
			if (!verifier) { continue; }
			check(throws([&] { verifier->read_next(std::span<char>{}); }) == !payload.empty() && !journal.fenced(), "empty destination rejects before EOF without fencing");
			if (!payload.empty()) { check(throws([&] { std::move(*verifier).finish(); }) && !journal.fenced(), "premature promotion preserves healthy incomplete verification"); }
			std::array<char, 65536> buffer{}; std::string candidate;
			while (verifier->offset() < payload.size()) {
				auto before = payload_reads, operations = model.operations; auto offset = verifier->offset(); auto count = verifier->read_next(buffer);
				check(payload_reads == before + 1 && model.operations == operations + 1 && count > 0 && count <= 65536 && verifier->offset() == offset + count, "each verification step makes one bounded read and preserves partial progress");
				candidate.append(buffer.data(), count);
			}
			auto reads = payload_reads, operations = model.operations;
			check(verifier->read_next(buffer) == 0 && payload_reads == reads, "verified EOF makes no extra payload call");
			auto reader = std::move(*verifier).finish();
			check(model.operations == operations && payload_reads == reads && largest <= 65536 && candidate == payload, "promotion retains the verified descriptor without reopening or rereading");
			check(throws([&] { verifier->read_next(buffer); }) && !journal.fenced() && read_artifact(reader) == payload, "promoted reader reads both formats while moved verifier rejects harmlessly");
		}
	} }
	{
		auto model = initialized(); MemoryIO io(model); Journal journal(io, 1024); journal.recover([](auto, auto) {});
		auto original = prepare(journal, "x"), artifact = std::move(original); auto before = model.operations;
		check(throws([&] { journal.begin_artifact_verification(original); }) && model.operations == before && !journal.fenced(), "moved prepared handle rejects before IO without fencing");
		std::vector<ArtifactVerifier> handles;
		for (unsigned i = 0; i < 9; ++i) { auto handle = journal.begin_artifact_verification(artifact); if (handle) { handles.push_back(std::move(*handle)); } }
		before = model.operations;
		check(handles.size() == 9 && !journal.begin_artifact_verification(artifact) && model.operations == before && !journal.fenced(), "nine verification leases reject further opens before IO");
		std::array<char, 8> buffer{}; handles.back().read_next(buffer); std::optional<ArtifactReader> reader(std::move(handles.back()).finish()); handles.pop_back();
		check(!journal.begin_artifact_verification(artifact), "promoted reader keeps its verification lease");
		auto moved = std::move(handles.back()); handles.pop_back();
		check(!journal.begin_artifact_verification(artifact), "moving a verifier keeps its bounded lease");
		reader.reset(); auto reopened = journal.begin_artifact_verification(artifact);
		check(reopened.has_value() && !journal.begin_artifact_verification(artifact), "closing the promoted reader releases exactly one lease");
		check(moved.descriptor() == artifact.descriptor(), "moved verification keeps its exact artifact metadata");
		std::array<char, 65537> oversized{}; before = model.operations;
		check(throws([&] { moved.read_next(oversized); }) && model.operations == before && !journal.fenced(), "oversized caller buffer rejects before IO without fencing");
		Model foreign_model; MemoryIO foreign_io(foreign_model); Journal foreign(foreign_io, 1024); foreign.create(identity()); auto foreign_artifact = prepare(foreign, "foreign");
		check(throws([&] { journal.begin_artifact_verification(foreign_artifact); }) && model.operations == before && !journal.fenced(), "foreign prepared artifact rejects without IO or fencing even under slot pressure");
	}
	for (unsigned corruption = 0; corruption < 4; ++corruption) {
		auto model = initialized(); MemoryIO io(model); Journal journal(io, 1024); journal.recover([](auto, auto) {});
		auto artifact = prepare(journal, "candidate"); auto& bytes = model.visible.at(artifact_name(artifact.descriptor()))->visible;
		if (corruption == 0) { bytes[0] ^= 1; }
		if (corruption == 1) { bytes.back() ^= 1; }
		check(throws([&] {
			auto verifier = journal.begin_artifact_verification(artifact); std::array<char, 64> buffer{};
			if (corruption == 2) { bytes.resize(detail::artifact_header_size); }
			if (corruption == 3) { model.oversized_read = true; }
			while (verifier->offset() < verifier->descriptor().length) { verifier->read_next(buffer); }
			auto reader = std::move(*verifier).finish();
		}) && journal.fenced(), "header, checksum, zero progress and oversized backend reads fence verification");
	}
	{
		auto model = initialized(); MemoryIO io(model); Journal journal(io, 1024); journal.recover([](auto, auto) {});
		auto artifact = prepare(journal, "candidate"); model.chunk = 3;
		auto source = journal.begin_artifact_verification(artifact), destination = journal.begin_artifact_verification(artifact);
		std::array<char, 64> buffer{}; source->read_next(buffer); *destination = std::move(*source);
		check(destination->offset() == 3 && throws([&] { source->read_next(buffer); }) && !journal.fenced(), "move assignment preserves checksum progress and invalidates only the source");
		while (destination->offset() < destination->descriptor().length) { destination->read_next(buffer); }
		auto reader = std::move(*destination).finish(); check(read_artifact(reader) == "candidate", "move-assigned verification promotes a complete correctly checksummed image");
	}
	{
		auto model = initialized(); MemoryIO io(model); std::optional<ArtifactVerifier> escaped;
		{
			Journal journal(io, 1024); journal.recover([](auto, auto) {}); auto artifact = prepare(journal, "escaped"); escaped = journal.begin_artifact_verification(artifact);
		}
		std::array<char, 64> buffer{}; check(model.locked, "escaped verifier retains the stable owner lock after Journal destruction");
		escaped->read_next(buffer); std::optional<ArtifactReader> reader(std::move(*escaped).finish()); escaped.reset();
		check(model.locked && read_artifact(*reader) == "escaped", "promoted escaped reader retains its owner and verified file");
		reader.reset(); check(!model.locked, "final reader destruction releases the escaped owner session");
	}
}

void published_artifact_selection() {
	for (bool legacy : {false, true}) { for (const std::string& payload : {std::string{}, std::string("published"), std::string(65553, 'p')}) {
		auto model = initialized(); MemoryIO io(model); Journal journal(io, 1024); journal.recover([](auto, auto) {});
		auto before = model.operations;
		check(!journal.select_published_dependency(0) && model.operations == before, "publication selection without checkpoint performs no IO");
		ArtifactDescriptor descriptor;
		{ auto app = prepare(journal, payload), bundle = prepare(journal, "bundle"); descriptor = app.descriptor(); journal.publish_checkpoint(bundle, std::array{app}, 1); }
		auto inode = model.visible.at(artifact_name(descriptor));
		if (legacy) { inode->visible = detail::artifact_header_v1(journal.frontier().identity, descriptor) + payload; inode->durable = inode->visible; }
		before = model.operations; auto selection = journal.select_published_dependency(0);
		check(selection && selection->descriptor() == descriptor && selection->checkpoint() == *journal.frontier().checkpoint && selection->base_sequence() == 1 && !journal.select_published_dependency(1) && model.operations == before, "selection binds complete current publication without IO");
		journal.append_batch("ordinary"); std::size_t reads = 0;
		model.observe_read = [&](const Inode* source, auto offset, auto count) { if (source == inode.get() && offset + count > (legacy ? detail::artifact_v1_header_size : detail::artifact_header_size)) { ++reads; } };
		auto verifier = journal.begin_published_verification(*selection);
		check(verifier && reads == 0, "ordinary append preserves selection and opening reads only fixed metadata");
		model.chunk = 3; std::array<char, 65536> buffer{}; std::string actual;
		if (!payload.empty()) { check(throws([&] { auto reader = std::move(*verifier).finish(); }) && !journal.fenced(), "published premature promotion is a healthy caller error"); }
		while (verifier->offset() < descriptor.length) {
			before = model.operations; auto count = verifier->read_next(buffer);
			check(count > 0 && count <= 65536 && model.operations == before + 1, "published verification makes one bounded partial payload read"); actual.append(buffer.data(), count);
		}
		before = model.operations; auto reader = std::move(*verifier).finish();
		check(actual == payload && model.operations == before, "published verification promotes both formats without IO");
		auto app = journal.pin_artifact(descriptor), next_bundle = prepare(journal, "next"); journal.publish_checkpoint(next_bundle, std::array{app}, 2);
		before = model.operations;
		check(!journal.begin_published_verification(*selection) && model.operations == before && !journal.fenced(), "replacement invalidates selection even when dependency is reused");
	} }
	{
		auto model = initialized(); MemoryIO io(model); Journal journal(io, 1024); journal.recover([](auto, auto) {});
		{ auto app = prepare(journal, "app"), bundle = prepare(journal, "bundle"); journal.publish_checkpoint(bundle, std::array{app}, 1); }
		auto selection = journal.select_published_dependency(0); auto artifact = journal.pin_artifact(selection->descriptor());
		auto staged = journal.begin_artifact_verification(artifact); std::vector<ArtifactVerifier> handles;
		for (unsigned i = 0; i < 8; ++i) { auto handle = journal.begin_published_verification(*selection); handles.push_back(std::move(*handle)); }
		auto before = model.operations;
		check(!journal.begin_published_verification(*selection) && !journal.begin_artifact_verification(artifact) && model.operations == before && !journal.fenced(), "staged and published verification share nine slots before IO");
		Model other_model; MemoryIO other_io(other_model); Journal other(other_io, 1024); other.create(identity());
		{ auto app = prepare(other, "app"), bundle = prepare(other, "bundle"); other.publish_checkpoint(bundle, std::array{app}, 0); }
		auto foreign = other.select_published_dependency(0); before = model.operations;
		check(throws([&] { journal.begin_published_verification(*foreign); }) && model.operations == before && !journal.fenced(), "foreign selection rejects before shared verification pressure and IO");
		std::array<char, 8> buffer{}; handles.back().read_next(buffer); std::optional<ArtifactReader> reader(std::move(handles.back()).finish()); handles.pop_back();
		check(!journal.begin_published_verification(*selection), "promoted published reader retains its verification slot");
		auto moved = std::move(handles.back()); handles.pop_back(); check(!journal.begin_published_verification(*selection), "moving published verifier retains its slot");
		reader.reset(); check(journal.begin_published_verification(*selection).has_value(), "closing published reader releases its verification slot");
		staged.reset(); handles.clear(); auto fresh = journal.begin_published_verification(*selection); moved = std::move(*fresh);
		std::vector<PreparedArtifact> prepared; for (unsigned i = 1; i < 9; ++i) { prepared.push_back(journal.pin_artifact(selection->descriptor())); }
		check(throws([&] { journal.pin_artifact(selection->descriptor()); }) && journal.begin_published_verification(*selection).has_value(), "published open does not consume an exhausted prepared-artifact slot");
		auto copied = *selection; auto moved_selection = std::move(copied); before = model.operations;
		check(throws([&] { journal.begin_published_verification(copied); }) && model.operations == before && !journal.fenced(), "moved selection rejects before IO without fencing");
		check(journal.begin_published_verification(moved_selection).has_value(), "moved-to selection preserves session provenance");
	}
}

void checkpoint_corruption() {
	auto baseline = initialized(); Frontier frontier;
	{
		MemoryIO io(baseline); Journal journal(io, 1024); journal.recover([](auto, auto) {});
		auto bundle = prepare(journal, "first"); std::array dependencies{prepare(journal, "application")};
		journal.publish_checkpoint(bundle, dependencies, 1); frontier = journal.append_batch("second");
	}
	std::vector<std::string> required{"manifest", "generation-" + detail::hexadecimal(frontier.journal_identity), artifact_name(*frontier.checkpoint)};
	for (const auto& dependency : frontier.dependencies) { required.push_back(artifact_name(dependency)); }
	for (const auto& name : required) {
		for (std::size_t byte = 0; byte < baseline.visible.at(name)->visible.size(); ++byte) {
			auto corrupted = baseline.clone(); corrupted.visible.at(name)->visible[byte] ^= 1;
			check(throws([&] { replay_checkpoint(corrupted); }), "every corrupt required-generation byte fails closed");
		}
		auto missing = baseline.clone(); missing.visible.erase(name);
		check(throws([&] { replay_checkpoint(missing); }), "missing referenced generation artifact fails closed without fallback");
	}
	{
		auto malicious = baseline.clone(); auto& manifest = malicious.visible.at("manifest")->visible;
		std::string length; put64(length, std::numeric_limits<std::uint64_t>::max());
		manifest.replace(88, 8, length); // v2 bundle length, with a valid frame CRC.
		std::string checksum; put32(checksum, crc32c(std::string_view(manifest).substr(0, manifest.size() - 4)));
		manifest.replace(manifest.size() - 4, 4, checksum);
		check(throws([&] { replay_checkpoint(malicious); }), "checksummed oversized artifact cannot overflow allocation or file offsets");
	}
}
