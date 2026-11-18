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

// Stores on the shared backend: several groups of one host, their batches and checkpoint
// records in one turn log and their artifacts in the host's one directory, on faultlab's faultfs.
// Power is cut after every mutating I/O operation of a randomized run: every acknowledged batch
// and checkpoint survives, nothing unstaged appears, and the groups carry on. Checkpoints
// cross every publication step (artifacts durable, record staged, barrier, reclamation),
// which is Condition 25's ordering under T9. Each run goes twice: once through the synchronous
// calls and TurnDriver::commit, once through Store's tasks and TurnDriver::run over a
// HeldCompletion on an inline_scheduler, whose primitives complete in the order issued. The two
// must write the same history (the same count of disk mutations) and keep the same batches.
#include "faultlab/faultfs.h"
#include "journal/store.h"
#include <deque>
#include <iostream>
#include <map>
#include <optional>
#include <random>

namespace j = kronuz::journal;
namespace fl = kronuz::faultlab;

namespace {
int checks = 0, failures = 0;
std::uint64_t published = 0, acknowledged_batches = 0, reclaimed = 0, recovered_checkpoints = 0, reopened = 0;
void check(bool value, const std::string& what) {
	++checks;
	if (!value) { ++failures; if (failures < 20) { std::cerr << "FAIL: " << what << '\n'; } }
}
const j::Identity log_identity{'S', 'H', 'A', 'R', 'E', 'D'};
const j::TurnLogOptions options{16 * 1024, 1024, 1, 4, 4096};
const j::AdmissionLimits limits{{64u << 20, 4096}, {1u << 20, 8}, {8u << 20, 8}, 64, 3};
constexpr std::uint64_t group_count = 4;

j::Identity identity(std::uint64_t group) { j::Identity id{'G'}; id[1] = static_cast<char>(group); return id; }

std::string encode(const std::vector<std::string>& entries) {
	std::string bytes;
	for (const auto& entry : entries) { j::put32(bytes, static_cast<std::uint32_t>(entry.size())); bytes += entry; }
	return bytes;
}
std::vector<std::string> decode(std::string_view bytes) {
	std::vector<std::string> entries;
	while (!bytes.empty()) { auto size = j::get32(bytes); entries.emplace_back(bytes.substr(0, size)); bytes.remove_prefix(size); }
	return entries;
}

struct Group {
	std::unique_ptr<j::Store> store;
	std::vector<std::string> attempted, acknowledged;  // every batch staged; those completed
	// The record a reopen staged, its sequence and manifest, while no publication followed it.
	std::optional<std::pair<std::uint64_t, std::string>> restaged;
};

bool tasks = false;  // drive the rounds through the coroutine drivers

// An append or publication run as a task: its outcome once done.
struct Call { std::uint64_t group = 0; std::string batch; bool done = false; std::exception_ptr error; };

struct Node {
	fl::filesystem& disk;
	std::unique_ptr<j::Host> host;
	j::TurnDriver* driver = nullptr;
	std::map<std::uint64_t, Group> groups;
	std::vector<std::pair<std::uint64_t, std::shared_ptr<j::StoreAppend>>> appends;
	task::inline_scheduler loop;
	j::HeldCompletion held;
	std::deque<Call> calls;  // the task round's appends, in order
	explicit Node(fl::filesystem& d) : disk(d) {}
	~Node() { close(); }
	// The process ends: the Stores, then the host and its lock.
	void close() {
		if (driver) { settle_tasks(); }
		for (auto& [g, group] : groups) { group.store.reset(); }
		driver = nullptr; host.reset();
	}

	// Opens the host and every group: recovers it, or creates it when it has no records.
	void open() {
		host = std::make_unique<j::Host>(std::make_shared<fl::io>(disk, "host"), log_identity, j::HostOptions{1, options});
		driver = &host->driver(0);
		for (std::uint64_t g = 1; g <= group_count; ++g) {
			auto& group = groups[g];
			group.store.reset();
			group.store = std::make_unique<j::Store>(*host, g, limits);
			if (host->status(g) == j::GroupStatus::stored || host->status(g) == j::GroupStatus::closed) {
				std::vector<std::string> recovered;
				group.store->recover([&](std::uint64_t sequence, std::string_view bytes) {
					check(sequence == recovered.size() + 1, "batches replay in sequence");
					recovered.emplace_back(bytes);
				}, [&](const j::Frontier& frontier, j::ArtifactReader& bundle, std::span<j::ArtifactReader>) {
					std::string bytes(static_cast<std::size_t>(bundle.descriptor().length), '\0');
					for (std::size_t at = 0; at < bytes.size();) { at += bundle.read_at(at, std::span<char>(bytes.data() + at, bytes.size() - at)); }
					recovered = decode(bytes); ++recovered_checkpoints;
					check(recovered.size() == frontier.base_sequence, "a checkpoint holds the batches it covers");
				});
				group.acknowledged = recovered; group.attempted = recovered;
			} else {
				check(group.acknowledged.empty() && host->status(g) == j::GroupStatus::none, "a group without records acknowledged nothing");
				group.store->create(identity(g));
				group.acknowledged.clear(); group.attempted.clear();
			}
			while (!group.store->inventory_step(64).complete) {}
		}
	}

	// A group whose checkpoint covers every batch closes into the host's table and reopens from
	// it, staging that record again as its first.
	void cycle(std::uint64_t g) {
		groups[g].store->close();
		reopen(g);
	}
	// A close at a reopened group's record, with batches acknowledged after it, is refused and
	// writes nothing, so every crash point also follows a refused close. One taken instead
	// reopens the group from that record, without those batches.
	void stale_close(std::uint64_t g) {
		auto& group = groups[g];
		if (!group.restaged || driver->sequence(g) <= group.restaged->first) { return; }
		bool refused = false;
		try { host->close_group(g, group.restaged->first, group.restaged->second); } catch (const std::logic_error&) { refused = true; }
		check(refused, "a close behind the group's acknowledged batches is refused");
		if (refused) { return; }
		group.store.reset();
		const auto recovered = reopen(g);
		group.acknowledged = recovered; group.attempted = recovered;
	}
	// Recovers the closed group from the table into a new Store: what it holds.
	std::vector<std::string> reopen(std::uint64_t g) {
		auto& group = groups[g];
		group.store = std::make_unique<j::Store>(*host, g, limits);
		std::vector<std::string> recovered;
		group.store->recover([&](std::uint64_t, std::string_view) { check(false, "a closed group replays no batch"); },
			[&](const j::Frontier&, j::ArtifactReader& bundle, std::span<j::ArtifactReader>) {
				std::string bytes(static_cast<std::size_t>(bundle.descriptor().length), '\0');
				for (std::size_t at = 0; at < bytes.size();) { at += bundle.read_at(at, std::span<char>(bytes.data() + at, bytes.size() - at)); }
				recovered = decode(bytes);
			});
		check(recovered == group.acknowledged, "a reopened group has every acknowledged batch");
		while (!driver->idle()) { driver->commit(); }
		while (!group.store->inventory_step(64).complete) {}
		++reopened;
		group.restaged.emplace(driver->sequence(g), host->entry(g)->manifest);
		return recovered;
	}
	// Every append that reported success is acknowledged, whenever power is lost after.
	void collect() {
		for (auto& call : calls) {
			if (!call.done || call.error) { continue; }
			auto& group = groups[call.group];
			group.acknowledged.push_back(group.attempted[group.acknowledged.size()]); ++acknowledged_batches;
		}
		calls.clear();
		for (auto& [g, append] : appends) {
			if (!append->done()) { continue; }
			try { append->result(); } catch (...) { continue; }
			auto& group = groups[g];
			group.acknowledged.push_back(group.attempted[group.acknowledged.size()]); ++acknowledged_batches;
		}
		appends.clear();
	}
	void round(std::mt19937_64& random) {
		if (tasks) { return task_round(random); }
		std::vector<std::pair<std::uint64_t, std::shared_ptr<j::StorePublication>>> publications;
		for (auto& [g, group] : groups) {
			auto& store = *group.store;
			if (random() % 8 == 0) {
				// A checkpoint of the acknowledged batches: artifacts first, then the record.
				auto id = store.reserve_replacement(1024, 1u << 20);
				if (id) {
					store.begin_artifact(*id, j::ArtifactPart::Application); store.write_chunk(*id, "application"); store.finish_artifact(*id);
					store.begin_artifact(*id, j::ArtifactPart::Bundle);
					auto bytes = encode(group.acknowledged);
					for (std::size_t at = 0; at < bytes.size(); at += 65536) { store.write_chunk(*id, std::string_view(bytes).substr(at, 65536)); }
					store.finish_artifact(*id);
					publications.emplace_back(g, store.begin_publication(*id, store.frontier().sequence));
					continue;
				}
			}
			if (random() % 4 != 0) {
				std::string batch = "g" + std::to_string(g) + "-" + std::to_string(group.attempted.size() + 1) + std::string(random() % 300, 'x');
				auto permit = store.reserve_append(j::AdmissionClass::Normal, batch.size());
				if (!permit) { continue; }
				group.attempted.push_back(batch);
				appends.emplace_back(g, store.begin_append(std::move(*permit), batch));
			}
		}
		while (!driver->idle()) { driver->commit(); }
		for (auto& [g, append] : appends) { check(append->done(), "an append completes at its turn's barrier"); append->result(); }
		collect();
		for (auto& [g, publication] : publications) {
			check(publication->done(), "a publication completes at its turn's barrier");
			publication->result(); ++published; groups[g].restaged.reset();
		}
		for (auto& [g, group] : groups) { stale_close(g); }
		for (auto& [g, publication] : publications) { if (random() % 2 == 0) { cycle(g); } }
		for (auto& [g, group] : groups) { if (random() % 4 == 0) { reclaimed += group.store->reclaim_step(16).removed; } }
	}

	// The same round as tasks: the groups' work in order (artifact IO awaited, appends and
	// publications staged by tasks of their own), then one runner drains the staged turn, then
	// reclamation. Each held primitive completes in the order issued, so the disk sees the
	// synchronous round's operations; the first failure ends the round as the exception does there.
	void task_round(std::mt19937_64& random) {
		const auto ref = task::scheduler_ref::of(loop);
		const auto io = j::completion_ref::of(held);
		std::deque<Call> publications;
		Call body;
		auto staging = [&]() -> task::task<void> {
			for (auto& [g, group] : groups) {
				auto& store = *group.store;
				if (random() % 8 == 0) {
					auto id = store.reserve_replacement(1024, 1u << 20);
					if (id) {
						co_await store.prepare_artifact(*id, j::ArtifactPart::Application, io); co_await store.write_chunk(*id, "application", io); co_await store.finish_artifact(*id, io);
						co_await store.prepare_artifact(*id, j::ArtifactPart::Bundle, io);
						const auto bytes = encode(group.acknowledged);
						for (std::size_t at = 0; at < bytes.size(); at += 65536) { co_await store.write_chunk(*id, std::string_view(bytes).substr(at, 65536), io); }
						co_await store.finish_artifact(*id, io);
						auto& call = publications.emplace_back(); call.group = g;
						task::spawn(ref, track(call, [&store, id = *id, io] { return store.publish(id, store.frontier().sequence, io); }));
						continue;
					}
				}
				if (random() % 4 != 0) {
					std::string batch = "g" + std::to_string(g) + "-" + std::to_string(group.attempted.size() + 1) + std::string(random() % 300, 'x');
					auto permit = store.reserve_append(j::AdmissionClass::Normal, batch.size());
					if (!permit) { continue; }
					group.attempted.push_back(batch);
					auto& call = calls.emplace_back(); call.group = g; call.batch = std::move(batch);
					task::spawn(ref, track(call, [&store, &call, permit = std::make_shared<j::AppendReservation>(std::move(*permit)), io] { return store.append(std::move(*permit), call.batch, io); }));
				}
			}
		};
		task::spawn(ref, track(body, staging));
		drive([&] { return body.done; });
		if (body.error) { fail(body.error); }
		task::cancel_source stop; stop.request();  // drain what is staged, then end
		Call runner;
		task::spawn(ref, track(runner, [&] { return driver->run(io, stop.token()); }));
		drive([&] { return runner.done; });
		if (runner.error) { fail(runner.error); }
		for (auto& call : calls) { check(call.done, "an append completes at its turn's barrier"); if (call.error) { fail(call.error); } }
		for (auto& call : publications) {
			check(call.done, "a publication completes at its turn's barrier");
			if (call.error) { fail(call.error); }
			++published; groups[call.group].restaged.reset();
		}
		collect();
		for (auto& [g, group] : groups) { stale_close(g); }
		for (auto& call : publications) { if (random() % 2 == 0) { cycle(call.group); } }
		for (auto& [g, group] : groups) {
			if (random() % 4 != 0) { continue; }
			Call reclaim; std::uint64_t removed = 0;
			auto& store = *group.store;
			task::spawn(ref, track(reclaim, [&]() -> task::task<void> { removed = (co_await store.reclaim_step(16, io)).removed; }));
			drive([&] { return reclaim.done; });
			if (reclaim.error) { fail(reclaim.error); }
			reclaimed += removed;
		}
	}
	template <class F> static task::task<void> track(Call& call, F make) {
		try { co_await make(); } catch (...) { call.error = std::current_exception(); }
		call.done = true;
	}
	// Runs the loop, completing held primitives in order, until `done` or nothing moves.
	template <class Done> void drive(Done done) {
		for (loop.run(); !done() && held.held(); loop.run()) { held.complete(held.tickets().front()); }
		if (!done()) { throw std::logic_error("a task round stalled"); }
	}
	// A failure ends the round: what is staged fails with it, so no task stays suspended.
	[[noreturn]] void fail(std::exception_ptr error) {
		settle_tasks(error);
		std::rethrow_exception(error);
	}
	void settle_tasks(std::exception_ptr error = {}) noexcept {
		if (!driver->idle() || driver->in_flight()) { driver->failed(error ? error : std::make_exception_ptr(std::runtime_error("node closed"))); }
		for (auto ticket : held.tickets()) { held.drop(ticket); }
		loop.run();
	}
};

std::uint64_t run(std::uint64_t seed, std::uint64_t cut, unsigned rounds) {
	fl::filesystem disk; disk.make_directory("host");
	std::mt19937_64 random(seed);
	Node node(disk);
	const auto at = "seed " + std::to_string(seed) + " cut " + std::to_string(cut);
	try {
		if (cut != std::numeric_limits<std::uint64_t>::max()) { disk.power_off_after(cut - 1); }
		node.open();
		for (unsigned r = 0; r < rounds; ++r) { node.round(random); }
	} catch (const std::exception& error) { check(disk.powered_off(), "only the power cut fails the run: " + at + ": " + error.what()); }
	node.collect();
	const auto operations = disk.mutations();
	node.close();
	disk.crash(seed * 7919 + cut, {.size = fl::size_pick::sector});
	try {
		Node again(disk);
		for (auto& [g, group] : node.groups) { again.groups[g].acknowledged = group.acknowledged; }
		again.open();
		check(!again.driver->recovered().damaged, "power loss alone damages nothing: " + at);
		for (auto& [g, group] : again.groups) {
			const auto& before = node.groups[g];
			bool kept = group.acknowledged.size() >= before.acknowledged.size() && group.acknowledged.size() <= before.attempted.size() &&
				std::equal(group.acknowledged.begin(), group.acknowledged.end(), before.attempted.begin());
			check(kept, "group " + std::to_string(g) + " keeps every acknowledged batch and invents none: " + at);
		}
		for (unsigned r = 0; r < 12; ++r) { again.round(random); }
		for (auto& [g, group] : again.groups) { check(!group.store->fenced(), "the groups carry on after recovery: " + at); }
	} catch (const std::exception& error) { check(false, at + ": " + error.what()); }
	return operations;
}
}  // namespace

int main(int argc, char** argv) {
	const std::uint64_t seeds = argc > 1 ? std::stoull(argv[1]) : 3;
	// `shared_store_test N sync` or `N tasks` runs one driver; both by default.
	const std::string only = argc > 2 ? argv[2] : "";
	std::uint64_t points = 0;
	for (bool mode : {false, true}) {
		if ((only == "sync" && mode) || (only == "tasks" && !mode)) { continue; }
		tasks = mode;
		for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
			const auto total = run(seed, std::numeric_limits<std::uint64_t>::max(), 40);
			if (tasks) {
				tasks = false; const auto sync = run(seed, std::numeric_limits<std::uint64_t>::max(), 40); tasks = true;
				check(total == sync, "seed " + std::to_string(seed) + ": the tasks write the synchronous history (" + std::to_string(total) + " operations, " + std::to_string(sync) + " synchronously)");
			}
			for (std::uint64_t cut = 1; cut <= total; ++cut, ++points) { run(seed, cut, 40); }
		}
		std::cout << (mode ? "tasks: " : "sync: ") << points << " crash points so far, " << failures << " failures\n";
	}
	std::cout << points << " crash points, " << checks << " shared store checks, " << failures << " failures (" << acknowledged_batches << " batches and "
		<< published << " checkpoints acknowledged, " << recovered_checkpoints << " checkpoints recovered, " << reclaimed << " artifacts reclaimed, " << reopened << " groups closed and reopened)\n";
	return failures ? 1 : 0;
}
