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

// Host::delete_group (docs/mechanisms/host-delete.md): a deleted group reopens empty and accepts
// writes while its neighbors keep every acknowledged record; its old records never come back,
// across reopens, later commits of its slot and a re-partition; a power cut at every step of the
// delete, and of the open that finishes one, keeps the whole closed group or none of it; an open
// group, a held one or one that is not closed is refused, changing nothing; and the group's own
// loop thread deletes it right after closing it, with no handoff. `host_delete_test [case]`.
#undef NDEBUG  // the range table's owner check (docs/mechanisms/host-ranges.md) runs in every build
#include "faultlab/faultfs.h"
#include "journal/store.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace j = kronuz::journal;
namespace fl = kronuz::faultlab;
namespace hd = kronuz::journal::host_detail;

namespace {
std::atomic<int> checks{0}, failures{0};  // loop_thread checks from two threads
void check(bool value, const std::string& what) {
	++checks;
	if (!value) { ++failures; std::cerr << "FAIL: " << what << '\n'; }
}
const j::Identity host_identity{'D', 'E', 'L', 'E', 'T', 'E'};
const j::TurnLogOptions turn_options{16 * 1024, 1024, 1, 4, 4096};
const j::AdmissionLimits limits{{64u << 20, 4096}, {1u << 20, 8}, {8u << 20, 8}, 64, 3};
const fl::crash_policy sector_cut{.size = fl::size_pick::sector};
j::Identity identity(std::uint64_t group) { j::Identity id{'G'}; for (unsigned i = 0; i < 8; ++i) { id[1 + i] = static_cast<char>(group >> (8 * i)); } return id; }

std::shared_ptr<j::IO> host_io(fl::filesystem& disk) {
	const auto names = disk.list();
	if (std::find(names.begin(), names.end(), "host") == names.end()) { disk.make_directory("host"); }
	return std::make_shared<fl::io>(disk, "host");
}
std::string read(j::ArtifactReader& reader) {
	std::string bytes(static_cast<std::size_t>(reader.descriptor().length), '\0');
	for (std::size_t at = 0; at < bytes.size();) { at += reader.read_at(at, std::span<char>(bytes.data() + at, bytes.size() - at)); }
	return bytes;
}
// A group's state as recovery gives it: its checkpoint's bundle and the batches after it.
struct State {
	std::string bundle; std::vector<std::string> batches;
	bool operator==(const State&) const = default;
};
std::string show(const State& state) {
	std::string text = "{" + state.bundle + ":";
	for (const auto& batch : state.batches) { text += " " + batch; }
	return text + "}";
}

// One group's Store and its loop's driver, on whichever thread runs that loop.
struct Group {
	j::Host& host;
	std::uint64_t id;
	std::unique_ptr<j::Store> store;
	State recovered;
	j::TurnDriver& turns() { return host.driver_of(id); }
	void commit() { while (!turns().idle()) { turns().commit(); } }
	// Creates the group when the host holds none of it, else recovers it.
	j::Store& open() {
		if (store) { return *store; }
		store = std::make_unique<j::Store>(host, id, limits);
		if (host.status(id) == j::GroupStatus::none) { store->create(identity(id)); }
		else {
			recovered = {};
			store->recover([&](std::uint64_t, std::string_view bytes) { recovered.batches.emplace_back(bytes); },
				[&](const j::Frontier&, j::ArtifactReader& bundle, std::span<j::ArtifactReader>) { recovered.bundle = read(bundle); });
		}
		commit();
		while (!store->inventory_step(64).complete) {}
		return *store;
	}
	void append(const std::string& batch) {
		auto& s = open();
		auto operation = s.begin_append(std::move(*s.reserve_append(j::AdmissionClass::Normal, batch.size())), batch);
		commit(); operation->result();
	}
	void checkpoint(const std::string& bundle) {
		auto& s = open();
		auto artifact = s.reserve_replacement(1024, 1u << 20);
		s.begin_artifact(*artifact, j::ArtifactPart::Application); s.write_chunk(*artifact, "application"); s.finish_artifact(*artifact);
		s.begin_artifact(*artifact, j::ArtifactPart::Bundle); s.write_chunk(*artifact, bundle); s.finish_artifact(*artifact);
		auto publication = s.begin_publication(*artifact, s.frontier().sequence);
		commit(); publication->result();
	}
	void close() { open().close(); store.reset(); }
};

struct Node {
	fl::filesystem& disk;
	std::unique_ptr<j::Host> host;
	std::map<std::uint64_t, Group> groups;
	Node(fl::filesystem& d, std::uint32_t loops = 1) : disk(d), host(std::make_unique<j::Host>(host_io(d), host_identity, j::HostOptions{loops, turn_options})) {}
	~Node() { groups.clear(); host.reset(); }
	Group& operator[](std::uint64_t id) { return groups.try_emplace(id, Group{*host, id, nullptr, {}}).first->second; }
	j::GroupStatus status(std::uint64_t id) const { return host->status(id); }
	// The group's state as a recover reads it.
	State recover(std::uint64_t id) { auto& group = (*this)[id]; group.open(); return group.recovered; }
	std::set<std::string> files(std::uint64_t id) const {
		std::set<std::string> names;
		for (const auto& name : disk.list("host")) { if (name.starts_with(hd::group_prefix(id))) { names.insert(name); } }
		return names;
	}
};

// Ids in group's slot: one table shard holds them all until it splits.
std::vector<std::uint64_t> slot_mates(std::uint64_t group, std::size_t count) {
	std::vector<std::uint64_t> ids{group};
	for (std::uint64_t id = group + 1; ids.size() < count; ++id) { if (hd::slot(id) == hd::slot(group)) { ids.push_back(id); } }
	return ids;
}

// A1. Group a (two checkpoints, a batch between) closes and is deleted; b, open with batches, and
// c, closed, share its shard; d lives in another slot. a's checkpoint record and artifacts go, and
// the next create starts it empty, as its second incarnation, writes after its old records' floor
// and keeps what it writes across a reopen while the log still holds the old records. b, c and d
// keep every acknowledged record.
void reopen_empty() {
	fl::filesystem disk;
	const auto ids = slot_mates(1, 3);
	const auto a = ids[0], b = ids[1], c = ids[2];
	std::uint64_t d = 2;
	while (hd::slot(d) == hd::slot(a)) { ++d; }
	const State b_state{"b-state", {"b-1", "b-2"}}, c_state{"c-state", {}}, d_state{"d-state", {"d-1"}}, a_new{"a-new", {"a-new-2"}};
	std::uint64_t floor = 0;
	{
		Node node(disk);
		node[a].checkpoint("a-old"); node[a].append("a-1"); node[a].checkpoint("a-old-2");
		node[b].checkpoint("b-state"); node[b].append("b-1"); node[b].append("b-2");
		node[c].checkpoint("c-state"); node[c].close();
		node[d].checkpoint("d-state"); node[d].append("d-1");
		const auto old = node.files(a);
		check(old.size() >= 2, "group a's checkpoints left artifacts: " + std::to_string(old.size()));
		node[a].close();
		floor = node.host->entry(a)->base;
		node.host->delete_group(a);
		check(node.status(a) == j::GroupStatus::none, "a deleted group's status is none");
		const auto entry = node.host->entry(a);
		check(entry && entry->state == j::GroupState::deleted && entry->manifest.empty() && entry->base == floor && entry->incarnation == 1 && entry->mark == entry->high,
			"its entry is deleted: no checkpoint record, its floor and incarnation kept, no serials left");
		check(node.files(a).empty(), "its artifacts are gone");
		node[a].append("a-new-1"); node[a].checkpoint("a-new"); node[a].append("a-new-2");
		check(node.host->entry(a)->incarnation == 2, "the next create is its second incarnation");
		check(node[a].turns().sequence(a) > floor + 1, "its records follow the deleted incarnation's floor");
		for (const auto& name : node.files(a)) { check(name.starts_with(hd::group_prefix(a) + "artifact-" + hd::hex(2, 16)), "its artifacts are named by the new incarnation: " + name); }
		for (const auto& name : old) { check(!node.files(a).contains(name), "no old artifact is back: " + name); }
	}
	Node node(disk);
	check(node.recover(a) == a_new, "a reopened host recovers the new incarnation alone: " + show(node.recover(a)));
	check(node.recover(b) == b_state, "b keeps every acknowledged record: " + show(node.recover(b)));
	check(node.status(c) == j::GroupStatus::closed && node.recover(c) == c_state, "c stays closed with its checkpoint: " + show(node.recover(c)));
	check(node.recover(d) == d_state, "d keeps every acknowledged record: " + show(node.recover(d)));
	node[a].append("a-new-3");
	check(node[a].turns().sequence(a) > floor + 1, "the new incarnation accepts writes after the reopen");
}

// A1. A deleted group stays deleted: a reopen while the log still holds its old records, a later
// commit of its slot (a create of a slot mate), a reopen after it, and a re-partition all keep it
// at none; then it is created on the re-partitioned host and keeps its new records.
void stays_deleted() {
	fl::filesystem disk;
	const auto ids = slot_mates(1, 2);
	const auto a = ids[0], e = ids[1];
	{
		Node node(disk);
		node[a].checkpoint("a-old"); node[a].append("a-1"); node[a].checkpoint("a-old-2"); node[a].close();
		node.host->delete_group(a);
	}
	{
		Node node(disk);
		check(node.status(a) == j::GroupStatus::none && !node[a].turns().has_group(a), "a reopen with the old records still in the log keeps the group deleted");
		node[e].checkpoint("e-state");  // a create of a slot mate commits the slot's shard
	}
	{
		Node node(disk);
		check(node.status(a) == j::GroupStatus::none && node.host->entry(a)->state == j::GroupState::deleted, "a later commit of its slot keeps the deleted entry");
		check(node.recover(e) == State{"e-state", {}}, "the slot mate keeps its checkpoint");
	}
	{
		Node node(disk, 3);
		check(node.status(a) == j::GroupStatus::none, "a re-partition keeps the group deleted");
		node[a].append("fresh-1"); node[a].append("fresh-2");
	}
	Node node(disk, 3);
	check(node.recover(a) == State{"", {"fresh-1", "fresh-2"}}, "the group created after the re-partition keeps its records: " + show(node.recover(a)));
	check(node.recover(e) == State{"e-state", {}}, "the slot mate keeps its checkpoint after the re-partition");
}

// The base disk of the crash cases: a closed with two checkpoints' artifacts, b open with batches
// in its shard, c closed in its shard.
struct Base { fl::filesystem disk; std::uint64_t a = 0, b = 0, c = 0; std::set<std::string> artifacts; };
void populate(Base& base) {
	const auto ids = slot_mates(1, 3);
	base.a = ids[0]; base.b = ids[1]; base.c = ids[2];
	Node node(base.disk);
	node[base.a].checkpoint("a-old"); node[base.a].append("a-1"); node[base.a].checkpoint("a-state"); node[base.a].close();
	node[base.b].checkpoint("b-state"); node[base.b].append("b-1"); node[base.b].append("b-2");
	node[base.c].checkpoint("c-state"); node[base.c].close();
	base.artifacts = node.files(base.a);
}
// After a cut and a reopen: group a whole (closed, every artifact, its checkpoint recovered) or
// none of it (status none, a deleted entry, no artifact), and b and c intact. Whether a is gone.
bool verify(fl::filesystem& disk, const Base& base, const std::string& at) {
	Node node(disk);
	bool gone = false;
	const auto status = node.status(base.a);
	if (status == j::GroupStatus::closed) {
		check(node.files(base.a) == base.artifacts, "a whole group keeps every artifact: " + at);
		State state;
		try { state = node.recover(base.a); } catch (const std::exception& error) { state.bundle = std::string("recover failed: ") + error.what(); }
		check(state == State{"a-state", {}}, "a whole group recovers its checkpoint: " + show(state) + ", " + at);
	} else if (status == j::GroupStatus::none) {
		gone = true;
		const auto entry = node.host->entry(base.a);
		check(entry && entry->state == j::GroupState::deleted && entry->mark == entry->high, "a gone group's entry is deleted with no serials left: " + at);
		check(node.files(base.a).empty(), "a gone group has no artifact: " + at);
	} else {
		check(false, "group a is closed or gone, never another status (" + std::to_string(static_cast<int>(status)) + "): " + at);
	}
	check(node.recover(base.b) == State{"b-state", {"b-1", "b-2"}}, "b keeps every acknowledged record: " + at);
	check(node.status(base.c) == j::GroupStatus::closed && node.recover(base.c) == State{"c-state", {}}, "c stays closed with its checkpoint: " + at);
	if (gone) { node[base.a].append("again"); }
	return gone;
}
// The mutations a delete of a makes on a host open on a snapshot of the base disk.
std::uint64_t deleting(const Base& base) {
	auto disk = base.disk.snapshot();
	Node node(disk);
	const auto start = disk.mutations();
	node.host->delete_group(base.a);
	return disk.mutations() - start;
}
// The state of group a in its shard file as an open would read it, if the file holds it.
std::optional<j::GroupEntry> filed(fl::filesystem& disk, std::uint64_t group) {
	const auto name = hd::shard_name({hd::slot(group), 1});
	const auto names = disk.list("host");
	if (std::find(names.begin(), names.end(), name) == names.end()) { return std::nullopt; }
	auto entries = hd::decode_shard(disk.view("host/" + name), host_identity, {hd::slot(group), 1});
	if (auto found = entries.find(group); found != entries.end()) { return found->second; }
	return std::nullopt;
}

// A2. A power cut after each mutation of delete_group, under several crash draws, then a reopen:
// group a is whole or gone, never part, and b and c keep every record. Both outcomes occur.
void crash() {
	Base setup; populate(setup);
	const auto total = deleting(setup);
	check(total > 0, "the delete writes");
	std::size_t whole = 0, gone = 0;
	for (std::uint64_t cut = 0; cut <= total; ++cut) {
		for (std::uint64_t seed = 0; seed < 4; ++seed) {
			auto disk = setup.disk.snapshot();
			const auto at = "cut " + std::to_string(cut) + " of " + std::to_string(total) + ", seed " + std::to_string(seed);
			{
				Node node(disk);
				disk.power_off_after(cut);
				try { node.host->delete_group(setup.a); } catch (const std::exception&) {}
			}
			disk.crash(cut * 7919 + seed, sector_cut);
			(verify(disk, setup, at) ? gone : whole) += 1;
		}
	}
	check(whole > 0 && gone > 0, "cuts leave the whole group and none of it: " + std::to_string(whole) + " whole, " + std::to_string(gone) + " gone");
}

// A2. A delete recorded with its artifacts still on disk (a cut after its first commit), then a
// power cut after each mutation of the open that finishes it, then a reopen: the group is gone.
void crash_finish() {
	Base setup; populate(setup);
	const auto total = deleting(setup);
	std::size_t unfinished = 0;
	for (std::uint64_t cut = 0; cut < total; ++cut) {
		for (std::uint64_t seed = 0; seed < 4; ++seed) {
			auto recorded = setup.disk.snapshot();
			{
				Node node(recorded);
				recorded.power_off_after(cut);
				try { node.host->delete_group(setup.a); } catch (const std::exception&) {}
			}
			recorded.crash(cut * 7919 + seed, sector_cut);
			const auto entry = filed(recorded, setup.a);
			bool left = false;
			for (const auto& name : recorded.list("host")) { left |= name.starts_with(hd::group_prefix(setup.a)); }
			if (!entry || entry->state != j::GroupState::deleted || !left) { continue; }
			++unfinished;
			std::uint64_t opening = 0;
			{
				auto probe = recorded.snapshot();
				const auto start = probe.mutations();
				{ Node node(probe); }
				opening = probe.mutations() - start;
			}
			for (std::uint64_t at = 0; at <= opening; ++at) {
				auto disk = recorded.snapshot();
				const auto where = "delete cut " + std::to_string(cut) + " seed " + std::to_string(seed) + ", open cut " + std::to_string(at) + " of " + std::to_string(opening);
				disk.power_off_after(at);
				try { Node node(disk); } catch (const std::exception&) {}
				disk.crash(at * 31 + cut, sector_cut);
				check(verify(disk, setup, where), "a recorded delete stays deleted: " + where);
			}
		}
	}
	check(unfinished > 0, "some cut leaves a recorded delete unfinished: " + std::to_string(unfinished));
}

// A3. Deleting an open group (held or released), one the host does not hold, a deleted one, or a
// closed one while a hold of it is live is refused with its named error, and changes nothing.
void refused() {
	fl::filesystem disk;
	Node node(disk);
	const std::uint64_t a = 1, b = 2;
	node[a].checkpoint("a-state"); node[a].append("a-1");
	node[b].checkpoint("b-state"); node[b].close();
	auto refuses = [&](std::uint64_t group, const std::function<bool(const std::exception&)>& named, const std::string& what) {
		const auto before = disk.mutations();
		bool refused = false;
		try { node.host->delete_group(group); } catch (const std::exception& error) { refused = named(error); }
		check(refused, what + " is refused with its named error");
		check(disk.mutations() == before, what + " changes nothing on disk");
	};
	auto logic = [](const std::exception& error) { return dynamic_cast<const std::logic_error*>(&error) != nullptr; };
	auto busy = [](const std::exception& error) {
		const auto* system = dynamic_cast<const std::system_error*>(&error);
		return system && system->code() == std::error_code(EWOULDBLOCK, std::generic_category());
	};
	refuses(a, logic, "an open group a Store holds");
	node[a].store.reset();
	node.host->settle(node.host->loop(a));
	check(!node.host->holding(a), "group a is released");
	refuses(a, logic, "an open group no Store holds");
	refuses(99, logic, "a group the host does not hold");
	{
		auto pin = node.host->hold(b);
		refuses(b, busy, "a closed group with a live hold");
	}
	check(node.status(b) == j::GroupStatus::closed && node.recover(b) == State{"b-state", {}}, "the refused group stays closed with its checkpoint");
	node[b].close();
	node.host->delete_group(b);
	refuses(b, logic, "a deleted group");
	check(node.recover(a) == State{"a-state", {"a-1"}}, "the open group keeps its records");
}

// Whether `run` aborts, run in a forked child (this process must hold no other thread), its
// stderr silenced; a child that returns exits 0.
bool aborts(const std::function<void()>& run) {
	std::cout.flush(); std::cerr.flush();
	const auto child = ::fork();
	if (child == 0) {
		if (const auto null = ::open("/dev/null", O_WRONLY); null >= 0) { ::dup2(null, STDERR_FILENO); }
		try { run(); } catch (...) { ::_exit(2); }
		::_exit(0);
	}
	int status = 0;
	return child > 0 && ::waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

// A7. Each loop's own thread, the one that owns its range table, creates, writes, closes and at
// once deletes its group, three times, while the other loop's thread does the same: no handoff
// (adopt_loop), no settle, and the owner check, on in this build, passes. The next incarnation
// starts empty each time. A delete from a thread that does not own the loop aborts on the check.
void loop_thread() {
	fl::filesystem disk;
	auto host = std::make_unique<j::Host>(host_io(disk), host_identity, j::HostOptions{2, turn_options});
	std::uint64_t ids[2] = {0, 0};
	for (std::uint64_t id = 1; !ids[0] || !ids[1]; ++id) { if (!ids[host->loop(id)]) { ids[host->loop(id)] = id; } }
	std::atomic<int> done{0};
	std::string errors[2];
	auto run = [&](int loop) {
		try {
			for (int round = 0; round < 3; ++round) {
				Group group{*host, ids[loop], nullptr, {}};
				group.open();
				check(group.recovered == State{}, "each incarnation starts empty");
				group.append("batch-" + std::to_string(round)); group.checkpoint("state-" + std::to_string(round));
				group.close();
				host->delete_group(ids[loop]);
				check(host->status(ids[loop]) == j::GroupStatus::none, "the loop's thread deletes its group right after closing it");
			}
			done += 1;
		} catch (const std::exception& error) { errors[loop] = error.what(); }
	};
	std::thread zero(run, 0), one(run, 1);
	zero.join(); one.join();
	check(done == 2, "both loops' threads delete with no handoff: " + errors[0] + errors[1]);
	check(host->entry(ids[1])->incarnation == 3, "loop 1's group went through three incarnations");
	// The check is live: on a thread that does not own the loop, a delete aborts.
	check(aborts([&] { host->adopt_loop(1); Group group{*host, ids[1], nullptr, {}}; group.open(); group.close(); std::thread([&] { host->delete_group(ids[1]); }).join(); }),
		"a delete from a thread that does not own the loop aborts on the owner check");
}
}  // namespace

int main(int argc, char** argv) {
	const std::string only = argc > 1 ? argv[1] : "";
	const std::vector<std::pair<std::string, void (*)()>> cases{
		{"reopen_empty", reopen_empty}, {"stays_deleted", stays_deleted}, {"crash", crash}, {"crash_finish", crash_finish}, {"refused", refused}, {"loop_thread", loop_thread}};
	for (const auto& [name, run] : cases) {
		if (!only.empty() && only != name) { continue; }
		try { run(); } catch (const std::exception& error) { check(false, name + ": " + error.what()); }
	}
	std::cout << checks << " host-delete checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
