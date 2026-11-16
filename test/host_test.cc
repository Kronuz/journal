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

// One directory per host (A3): names by opaque group id, incarnation and serial, so groups never
// share a name and reclaiming one never touches another; reclamation and the admission census by
// name with no directory scan; a closed group reopened from the group table after retention
// recycled every segment that held it; a group whose first turn was lost created again as a new
// incarnation; a failed close that leaves the group open; a second recovery on one host refused;
// and one owner lock, the directory's own. `host_test [case]` runs one case.
#include "journal/memory_io.h"
#include "journal/store.h"
#include <cerrno>
#include <iostream>
#include <set>

namespace j = kronuz::journal;
namespace jt = kronuz::journal::testing;
namespace hd = kronuz::journal::host_detail;

namespace {
int checks = 0, failures = 0;
void check(bool value, const std::string& what) {
	++checks;
	if (!value) { ++failures; std::cerr << "FAIL: " << what << '\n'; }
}
const j::Identity host_identity{'H', 'O', 'S', 'T'};
const j::TurnLogOptions turn_options{16 * 1024, 1024, 1, 4, 4096};
const j::AdmissionLimits limits{{64u << 20, 4096}, {1u << 20, 8}, {8u << 20, 8}, 64, 3};
j::Identity identity(std::uint64_t group) { j::Identity id{'G'}; for (unsigned i = 0; i < 8; ++i) { id[1 + i] = static_cast<char>(group >> (8 * i)); } return id; }

// The host's IO, recording every name it is asked about, its scans and its lock's mode; armed,
// it fails the next table shard write as a full disk would.
class RecordingIO final : public j::IO {
public:
	explicit RecordingIO(std::shared_ptr<j::IO> io) : io_(std::move(io)) {}
	std::vector<std::string> names; std::size_t scans = 0; std::optional<j::OwnerLockMode> lock_mode; bool full = false;
	std::unique_ptr<j::OwnerLock> acquire_owner(bool create) override { return io_->acquire_owner(create); }
	void open_existing_read_only_into(std::string_view name, j::File& file) override { note(name); io_->open_existing_read_only_into(name, file); }
	bool open_reclaim_candidate_read_only_into(std::string_view name, j::File& file) override { note(name); return io_->open_reclaim_candidate_read_only_into(name, file); }
	std::unique_ptr<j::File> open_reclaim_candidate_read_only(std::string_view name) override { note(name); return io_->open_reclaim_candidate_read_only(name); }
	std::unique_ptr<j::File> open_existing(std::string_view name) override { note(name); return io_->open_existing(name); }
	std::unique_ptr<j::File> open_existing_read_only(std::string_view name) override { note(name); return io_->open_existing_read_only(name); }
	std::unique_ptr<j::File> create_exclusive(std::string_view name) override {
		note(name);
		if (full && name.starts_with("groups-")) { full = false; throw std::system_error(ENOSPC, std::generic_category(), "no space for the table shard"); }
		return io_->create_exclusive(name);
	}
	void replace(std::string_view source, std::string_view destination) override { note(source); note(destination); io_->replace(source, destination); }
	void remove(std::string_view name) override { note(name); io_->remove(name); }
	std::unique_ptr<j::DirectoryCursor> scan_directory() override { ++scans; return io_->scan_directory(); }
	std::optional<j::EntryFootprint> entry_footprint(std::string_view name) override { note(name); return io_->entry_footprint(name); }
	std::unique_ptr<j::File> open_reclaim_candidate(std::string_view name) override { note(name); return io_->open_reclaim_candidate(name); }
	void sync_directory() override { io_->sync_directory(); }
protected:
	std::unique_ptr<j::OwnerLock> lock_owner(const j::OwnerLockOptions& options) override { lock_mode = options.mode; return io_->acquire_owner(options); }
private:
	void note(std::string_view name) { names.emplace_back(name); }
	std::shared_ptr<j::IO> io_;
};

std::set<std::string> listing(j::IO& io) {
	std::set<std::string> names; auto cursor = io.scan_directory();
	while (auto name = cursor->next()) { names.insert(*name); }
	return names;
}
std::string read(j::ArtifactReader& reader) {
	std::string bytes(static_cast<std::size_t>(reader.descriptor().length), '\0');
	for (std::size_t at = 0; at < bytes.size();) { at += reader.read_at(at, std::span<char>(bytes.data() + at, bytes.size() - at)); }
	return bytes;
}
// A group's state as recovery gives it: its checkpoint's bundle and the batches after it.
struct State { std::string bundle; std::vector<std::string> batches; };

struct Node {
	jt::Disk& disk;
	std::shared_ptr<RecordingIO> io;
	std::unique_ptr<j::Host> host;
	std::map<std::uint64_t, std::unique_ptr<j::Store>> stores;
	std::map<std::uint64_t, State> recovered;
	Node(jt::Disk& d, std::uint32_t loops = 1) : disk(d), io(std::make_shared<RecordingIO>(std::make_shared<jt::MemoryIO>(d, "host/"))) {
		host = std::make_unique<j::Host>(io, host_identity, j::HostOptions{loops, turn_options});
	}
	~Node() { stores.clear(); host.reset(); }
	j::TurnDriver& turns(std::uint64_t group) { return host->driver_of(group); }
	void commit(std::uint64_t group) { while (!turns(group).idle()) { turns(group).commit(); } }
	j::Store& store(std::uint64_t group) {
		auto& store = stores[group];
		if (store) { return *store; }
		store = std::make_unique<j::Store>(*host, group, limits);
		if (host->status(group) == j::GroupStatus::none) { store->create(identity(group)); }
		else {
			auto& state = recovered[group]; state = {};
			store->recover([&](std::uint64_t, std::string_view bytes) { state.batches.emplace_back(bytes); },
				[&](const j::Frontier&, j::ArtifactReader& bundle, std::span<j::ArtifactReader>) { state.bundle = read(bundle); });
		}
		commit(group);
		while (!store->inventory_step(64).complete) {}
		return *store;
	}
	void append(std::uint64_t group, const std::string& batch) {
		auto& s = store(group);
		auto operation = s.begin_append(std::move(*s.reserve_append(j::AdmissionClass::Normal, batch.size())), batch);
		commit(group); operation->result();
	}
	void checkpoint(std::uint64_t group, const std::string& bundle) {
		auto& s = store(group);
		auto id = s.reserve_replacement(1024, 1u << 20);
		s.begin_artifact(*id, j::ArtifactPart::Application); s.write_chunk(*id, "application"); s.finish_artifact(*id);
		s.begin_artifact(*id, j::ArtifactPart::Bundle); s.write_chunk(*id, bundle); s.finish_artifact(*id);
		auto publication = s.begin_publication(*id, s.frontier().sequence);
		commit(group); publication->result();
	}
	// Reclaims until a whole pass removes nothing; the count removed.
	std::size_t reclaim(std::uint64_t group) {
		std::size_t removed = 0, quiet = 0;
		while (quiet < 2) { auto step = store(group).reclaim_step(16).removed; removed += step; quiet = step ? 0 : quiet + 1; }
		return removed;
	}
	std::set<std::string> artifacts(std::uint64_t group) {
		std::set<std::string> names;
		for (const auto& name : listing(*io)) { if (name.starts_with(hd::group_prefix(group))) { names.insert(name); } }
		return names;
	}
};

// Ids whose hexadecimal forms nest (1, 0x10, 0x100), as index paths would: every artifact is
// named by its own group's fixed-width id, incarnation and serial, and nothing else is in the
// directory but the layout, the table and the logs.
void names() {
	jt::Disk disk;
	const std::vector<std::uint64_t> groups{1, 0x10, 0x100};
	{
		Node node(disk, 2);
		for (auto g : groups) { node.checkpoint(g, "bundle-" + std::to_string(g)); node.append(g, "batch-" + std::to_string(g)); }
		std::set<std::string> seen;
		for (const auto& name : listing(*node.io)) {
			const bool known = name == "layout" || hd::shard_slot(name) || hd::log_name(name);
			std::uint64_t owners = 0;
			for (auto g : groups) { owners += name.starts_with(hd::group_prefix(g)); }
			check(known || owners == 1, "a name is the layout, a table shard, a log segment, or one group's artifact: " + name);
		}
		for (auto g : groups) {
			const auto names = node.artifacts(g);
			check(names.size() == 2, "a group's checkpoint holds its two artifacts: group " + std::to_string(g));
			const auto frontier = node.store(g).frontier();
			const auto bundle = hd::group_prefix(g) + j::artifact_name(*frontier.checkpoint);
			check(names.contains(bundle), "the bundle is named by the group, its incarnation and its serial");
			check(bundle == hd::artifact_name(g, 1, 1), "the first incarnation's second serial names the bundle");
			for (auto other : groups) { if (other != g) { for (const auto& name : names) { check(!node.artifacts(other).contains(name), "no two groups share a name"); } } }
		}
	}
	Node again(disk, 2);
	for (auto g : groups) {
		again.store(g);
		check(again.recovered[g].bundle == "bundle-" + std::to_string(g) && again.recovered[g].batches == std::vector<std::string>{"batch-" + std::to_string(g)}, "each group recovers its own state");
	}
}

// Reclaiming one group names only its own files, never lists the directory, and leaves the
// other's artifacts as they were.
void reclaim_isolation() {
	jt::Disk disk;
	Node node(disk);
	for (int round = 0; round < 3; ++round) { node.checkpoint(1, "a" + std::to_string(round)); node.checkpoint(0x10, "b" + std::to_string(round)); }
	check(node.artifacts(1).size() == 6 && node.artifacts(0x10).size() == 6, "three checkpoints leave six artifacts each");
	const auto before = node.artifacts(0x10);
	node.io->names.clear(); node.io->scans = 0;
	const auto removed = node.reclaim(1);
	check(node.io->scans == 0, "reclamation probes names; it never lists the directory");
	bool own = true;
	for (const auto& name : node.io->names) { own &= name.starts_with(hd::group_prefix(1)); }
	check(own && !node.io->names.empty(), "reclaiming group 1 names only group 1's files");
	check(removed == 4 && node.artifacts(1).size() == 2, "the four replaced artifacts go, the published two stay");
	check(node.artifacts(0x10) == before, "the other group's artifacts are as they were");
	check(node.reclaim(0x10) == 4 && node.artifacts(0x10).size() == 2, "the other group reclaims its own");
}

// The admission census counts a group's artifacts by name, with no directory scan.
void census() {
	jt::Disk disk;
	{ Node node(disk); node.checkpoint(3, std::string(5000, 'c')); node.checkpoint(3, std::string(7000, 'd')); }
	Node node(disk);
	node.io->scans = 0;
	auto store = std::make_unique<j::Store>(*node.host, 3, limits);
	store->recover([](std::uint64_t, std::string_view) {}, [](const j::Frontier&, j::ArtifactReader&, std::span<j::ArtifactReader>) {});
	j::InventoryStats census;
	do { census = store->inventory_step(64); } while (!census.complete);
	check(node.io->scans == 0, "the census never lists the directory");
	std::uint64_t bytes = 0; std::size_t count = 0;
	for (const auto& name : node.artifacts(3)) { bytes += node.io->entry_footprint(name)->logical_bytes; ++count; }
	check(census.entries == count && census.logical_bytes == bytes && count == 4, "the census counts the group's four artifacts and their bytes");
}

// A closed group lives in the table alone: once retention recycled every segment that held its
// records, the reopened host recovers it from the table without reading a turn log.
void closed_reopen() {
	jt::Disk disk;
	{
		Node node(disk);
		node.checkpoint(7, "state"); node.append(7, "pending");
		bool refused = false;
		try { node.store(7).close(); } catch (const std::logic_error&) { refused = true; }
		check(refused, "a group with batches after its checkpoint does not close");
		node.checkpoint(7, "state");
		const auto held = node.turns(7).log().data_segments();
		node.store(7).close(); node.stores.erase(7);
		check(node.host->status(7) == j::GroupStatus::closed, "the table holds the group closed");
		for (int b = 0; b < 200; ++b) { node.append(8, std::string(900, 'x')); }
		const auto names = listing(*node.io);
		bool gone = true;
		for (auto segment : held) { gone &= !names.contains(hd::log_prefix(1, 0) + j::turn_log_detail::segment_name(segment)); }
		check(gone, "retention recycled every segment that held the closed group's records");
	}
	{
		Node node(disk);
		node.io->names.clear();
		auto store = std::make_unique<j::Store>(*node.host, 7, limits);
		std::string bundle;
		store->recover([](std::uint64_t, std::string_view) { check(false, "a closed group replays no batch"); },
			[&](const j::Frontier&, j::ArtifactReader& reader, std::span<j::ArtifactReader>) { bundle = read(reader); });
		bool logs = false;
		for (const auto& name : node.io->names) { logs |= hd::log_name(name).has_value(); }
		check(!logs && !node.io->names.empty(), "reopening reads the group's artifacts and no turn log");
		check(bundle == "state" && node.host->status(7) == j::GroupStatus::stored, "the reopened group has its checkpoint");
		node.stores[7] = std::move(store); node.commit(7);
		while (!node.stores[7]->inventory_step(64).complete) {}
		node.append(7, "after");
	}
	Node node(disk);
	node.store(7);
	check(node.recovered[7].bundle == "state" && node.recovered[7].batches == std::vector<std::string>{"after"}, "the reopened group keeps what it appended");
}

// A group whose first turn never became durable is created again as its next incarnation; the
// first one's artifacts go.
void recreate() {
	jt::Disk disk;
	std::string first;
	{
		Node node(disk);
		auto store = std::make_unique<j::Store>(*node.host, 5, limits);
		store->create(identity(5));
		while (!store->inventory_step(64).complete) {}
		auto id = store->reserve_replacement(1024, 1u << 20);
		store->begin_artifact(*id, j::ArtifactPart::Application); store->write_chunk(*id, "orphan"); store->finish_artifact(*id);
		first = hd::artifact_name(5, 1, 0);
		check(node.artifacts(5).contains(first), "the first incarnation's artifact exists");
		store->cancel_replacement(*id); store.reset();
		node.host->driver(0).failed(std::make_exception_ptr(std::runtime_error("power lost")));
	}
	disk.power_loss();
	Node node(disk);
	check(node.host->status(5) == j::GroupStatus::none, "a group whose first turn was lost is created again");
	node.checkpoint(5, "second");
	check(node.host->entry(5)->incarnation == 2, "as its next incarnation");
	check(!node.artifacts(5).contains(first), "the first incarnation's artifacts are gone");
	check(node.artifacts(5).contains(hd::artifact_name(5, 2, 1)), "the new ones are named by the new incarnation");
}

// A close whose table write fails (a full disk) leaves the group open and its records live in
// the log: retention keeps them, and after a restart the group recovers, never offered for create.
void failed_close() {
	jt::Disk disk;
	{
		Node node(disk);
		node.checkpoint(7, "state");
		node.append(8, "warm");
		node.io->full = true;
		bool threw = false;
		try { node.store(7).close(); } catch (const std::system_error&) { threw = true; }
		check(threw && !node.io->full, "the close's table write fails");
		check(node.host->status(7) == j::GroupStatus::stored && node.host->entry(7)->state == j::GroupState::open, "the table still holds the group open");
		node.stores.erase(7);
		for (int b = 0; b < 200; ++b) { node.append(8, std::string(900, 'x')); }
	}
	Node node(disk);
	check(node.host->status(7) == j::GroupStatus::stored, "after a restart the group is stored, not offered for create");
	node.store(7);
	check(node.recovered[7].bundle == "state" && node.host->entry(7)->incarnation == 1, "it recovers its checkpoint in its first incarnation");
}

// A group closed, reopened from the table, then recovered by a second Store of the same host:
// its state is in the log, not the table, so the recovery is refused; its loop keeps running,
// and the reopened host recovers the group's latest state.
void second_recovery() {
	jt::Disk disk;
	{
		Node node(disk);
		node.checkpoint(7, "old");
		node.store(7).close(); node.stores.erase(7);
		node.store(7);
		node.checkpoint(7, "new"); node.append(7, "after-new");
		node.append(9, "neighbor");
		node.stores.erase(7);
		bool refused = false;
		try {
			j::Store store(*node.host, 7, limits);
			store.recover([](std::uint64_t, std::string_view) { check(false, "a refused recovery replays nothing"); },
				[](const j::Frontier&, j::ArtifactReader&, std::span<j::ArtifactReader>) { check(false, "a refused recovery restores nothing"); });
		} catch (const j::Corruption&) { refused = true; }
		check(refused, "a second recovery on the same host is refused");
		node.commit(7);
		check(!node.host->driver(0).failed(), "the group's loop keeps running");
		node.append(9, "still");
	}
	Node node(disk);
	node.store(7); node.store(9);
	check(node.recovered[7].bundle == "new" && node.recovered[7].batches == std::vector<std::string>{"after-new"}, "the reopened host recovers the latest state");
	check(node.recovered[9].batches == std::vector<std::string>{"neighbor", "still"}, "the neighbor keeps both batches");
}

// One owner lock, the directory's own (what strata's cold tier takes): a second host on the
// directory is refused while the first holds it.
void lock() {
	jt::Disk disk;
	auto io = std::make_shared<RecordingIO>(std::make_shared<jt::MemoryIO>(disk, "host/"));
	{
		j::Host host(io, host_identity, j::HostOptions{1, turn_options});
		check(io->lock_mode == j::OwnerLockMode::directory, "the host takes the directory's lock");
		bool busy = false;
		try { j::Host other(std::make_shared<jt::MemoryIO>(disk, "host/"), host_identity, j::HostOptions{1, turn_options}); }
		catch (const std::system_error&) { busy = true; }
		check(busy, "a second host on the directory is refused");
	}
	j::Host again(std::make_shared<jt::MemoryIO>(disk, "host/"), host_identity, j::HostOptions{1, turn_options});
	check(again.loops() == 1, "the lock is released with the host");
}
}  // namespace

int main(int argc, char** argv) {
	const std::string only = argc > 1 ? argv[1] : "";
	const std::vector<std::pair<std::string, void (*)()>> cases{
		{"names", names}, {"reclaim_isolation", reclaim_isolation}, {"census", census},
		{"closed_reopen", closed_reopen}, {"recreate", recreate}, {"failed_close", failed_close}, {"second_recovery", second_recovery}, {"lock", lock}};
	for (const auto& [name, run] : cases) {
		if (!only.empty() && only != name) { continue; }
		try { run(); } catch (const std::exception& error) { check(false, name + ": " + error.what()); }
	}
	std::cout << checks << " host checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
