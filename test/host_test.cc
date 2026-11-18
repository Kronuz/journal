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
// incarnation; a failed close that leaves the group open; a failed reopen that leaves it closed;
// two loops' table commits free of races under TSan; a close behind an acknowledged record
// refused; a close with a sealed open record in flight fails its wait, never acknowledging it,
// under power loss too; a second recovery on one host refused; and one owner lock, the
// directory's own.
// `host_test [case]` runs one case.
#include "completion/posix.h"
#include "journal/memory_io.h"
#include "journal/store.h"
#include <cerrno>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <thread>
#include <unistd.h>

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
// it fails the next table shard write as a full disk would, or the next directory sync after
// the change it would make durable (an EIO from fsync on the directory).
class RecordingIO final : public j::IO {
public:
	explicit RecordingIO(std::shared_ptr<j::IO> io) : io_(std::move(io)) {}
	std::vector<std::string> names; std::size_t scans = 0; std::optional<j::OwnerLockMode> lock_mode; bool full = false, sync_fails = false;
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
	void sync_directory() override {
		if (sync_fails) { sync_fails = false; throw std::system_error(EIO, std::generic_category(), "the directory sync failed"); }
		io_->sync_directory();
	}
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

// A recover of a closed group whose table commit fails after its shard's replace, then a
// create in the same slot: the group stays closed in the live host and after a restart, and
// recovers its checkpoint.
void failed_reopen() {
	jt::Disk disk;
	const std::uint64_t group = 7;
	std::uint64_t neighbor = group + 1;
	while (hd::slot(neighbor) != hd::slot(group)) { ++neighbor; }
	{
		Node node(disk);
		node.append(group, "a1"); node.checkpoint(group, "bundle-a1");
		node.store(group).close(); node.stores.erase(group);
		node.io->sync_fails = true;
		bool threw = false;
		try { node.store(group); } catch (const std::system_error&) { threw = true; }
		node.stores.erase(group);
		check(threw && !node.io->sync_fails, "the reopen's table commit fails at its directory sync");
		check(node.host->status(group) == j::GroupStatus::closed, "the group stays closed in the live host");
		node.store(neighbor);
		check(node.host->status(group) == j::GroupStatus::closed, "and after a create in its slot");
	}
	Node node(disk);
	check(node.host->status(group) == j::GroupStatus::closed, "after a restart the group is closed");
	node.store(group);
	check(node.recovered[group].bundle == "bundle-a1" && node.recovered[group].batches.empty(), "it recovers its checkpoint");
}

// Two loops on their own threads commit table slots that share a 64-slot word: run under TSan,
// the slots' shard flags must not race. A real directory, as each loop's IO is.
void loop_slots() {
	const auto directory = std::filesystem::current_path() / ".scratch" / ("host-loop-slots-" + std::to_string(::getpid()));
	std::filesystem::remove_all(directory); std::filesystem::create_directories(directory);
	std::vector<std::uint64_t> side[2];
	for (std::uint64_t group = 1; side[0].size() < 40 || side[1].size() < 40; ++group) {
		const auto slot = hd::slot(group);
		if (slot < 1344 || slot > 1407) { continue; }  // one word across the loop 0 / loop 1 boundary of 3 loops
		const auto loop = hd::loop(slot, 3);
		if (loop < 2 && side[loop].size() < 40) { side[loop].push_back(group); }
	}
	{
		j::Host host(std::make_shared<j::PosixIO>(directory), host_identity, j::HostOptions{3, j::TurnLogOptions{64 * 1024, 1024, 1, 2, 4096}});
		std::thread threads[2];
		for (int loop = 0; loop < 2; ++loop) {
			threads[loop] = std::thread([&, loop] { for (auto group : side[loop]) { host.create_group(group); } });
		}
		for (auto& thread : threads) { thread.join(); }
		bool open = true;
		for (const auto& groups : side) { for (auto group : groups) { auto entry = host.entry(group); open &= entry && entry->state == j::GroupState::open; } }
		check(open, "every group either loop created is open in the table");
	}
	std::filesystem::remove_all(directory);
}

// journal-host-guards A1. A close whose record is below the group's last acknowledged one, here
// the group's real checkpoint record with two batches acknowledged after it, is refused: the
// forget would drop those batches. The group stays open in the table and the log, retention
// keeps the batches, and recovery returns them. A reopened group's close below the table's
// earlier base is refused the same way.
void close_behind() {
	jt::Disk disk;
	{ Node node(disk); node.checkpoint(7, "state"); }
	std::uint64_t last = 0;
	{
		Node node(disk);
		const auto records = node.turns(7).records(7);
		check(records.size() == 1 && records[0].kind == j::TurnRecordKind::Checkpoint, "the log holds the group's checkpoint record");
		const auto checkpoint = records[0];
		node.store(7); node.append(7, "a1"); node.append(7, "a2");
		last = node.turns(7).sequence(7);
		check(last == checkpoint.sequence + 2, "two batches are acknowledged after the checkpoint");
		std::string error;
		try { node.host->close_group(7, checkpoint.sequence, checkpoint.payload); }
		catch (const std::logic_error& refused) { error = refused.what(); }
		check(error == "a close behind the group's last acknowledged record", "a close at the checkpoint's sequence is refused: " + error);
		check(node.host->entry(7)->state == j::GroupState::open && node.host->entry(7)->manifest.empty(), "the table keeps the group open, with no record");
		check(node.turns(7).sequence(7) == last && node.host->status(7) == j::GroupStatus::stored, "the log keeps the group");
		node.stores.erase(7);
		for (int b = 0; b < 200; ++b) { node.append(8, std::string(900, 'x')); }
	}
	{
		Node node(disk);
		node.store(7);
		check(node.recovered[7].bundle == "state" && node.recovered[7].batches == std::vector<std::string>{"a1", "a2"}, "recovery keeps both acknowledged batches");
		node.checkpoint(7, "closing");
		const auto base = node.turns(7).sequence(7);
		node.store(7).close(); node.stores.erase(7);
		check(node.host->entry(7)->base == base, "the close at the last record is taken");
		node.host->reopen_group(7);
		bool refused = false;
		try { node.host->close_group(7, base - 1, node.host->entry(7)->manifest); } catch (const std::logic_error&) { refused = true; }
		check(refused && node.host->entry(7)->state == j::GroupState::open && node.host->entry(7)->base == base, "a reopened group's close below the table's base is refused");
		node.host->close_group(7, base, node.host->entry(7)->manifest);
		check(node.host->status(7) == j::GroupStatus::closed, "its close at the base is taken");
	}
	Node node(disk);
	node.store(7);
	check(node.recovered[7].bundle == "closing" && node.recovered[7].batches.empty(), "the closed group recovers its last checkpoint");
}

// A writer's wait on group 7's sealed open record, as a turn-seal consumer's: the record joins
// the group's next turn unless the group closes first.
struct Writer {
	j::TurnDriver& turns;
	std::optional<j::TurnRecord> open;
	std::vector<std::string> acknowledged;  // in the order their waits completed
	std::vector<std::string> closed;  // whose waits failed with group_closed
	std::vector<std::string> errors;  // any other failure
	j::TurnDriver::Sealer sealer() { return [this](std::uint64_t, bool) { return std::exchange(open, std::nullopt); }; }
	task::task<void> wait(std::uint64_t sequence, std::string batch) {
		try { co_await turns.durable(7, sequence); acknowledged.push_back(std::move(batch)); }
		catch (const j::group_closed& error) { closed.push_back(std::move(batch)); errors.emplace_back(error.what()); }
		catch (const std::exception& error) { errors.emplace_back(error.what()); }
	}
};
// The reopened group's checkpoint record, as Journal stages it: after the table's base and
// after every sequence the closed incarnation retired.
void restage(j::Host& host, std::uint64_t group, const std::string& manifest) {
	auto& turns = host.driver_of(group);
	const auto base = host.entry(group)->base;
	turns.stage({group, std::max(base, turns.retired(group)) + 1, j::TurnRecordKind::Checkpoint, manifest, base});
}

// The durability review's F1: a writer's transaction sits in group 7's sealed open record
// (sequence 2), its wait linked, when the group closes at its durable sequence. The close fails
// the wait with group_closed. The reopened group's records follow the retired sequence 2, so
// neither its checkpoint record nor a later wait for 2 acknowledges the dropped record, and
// recovery has no trace of it.
void sealer_close() {
	jt::Disk disk;
	{
		Node node(disk);
		auto& host = *node.host;
		auto& turns = node.turns(7);
		host.create_group(7);
		turns.stage({7, 1, j::TurnRecordKind::Checkpoint, "m1", 0}); turns.commit();
		Writer writer{turns};
		writer.open = j::TurnRecord{7, 2, j::TurnRecordKind::Batch, "t1", 0};
		turns.seal(7, writer.sealer(), &writer);
		turns.wake();
		task::inline_scheduler loop;
		task::spawn(task::scheduler_ref::of(loop), writer.wait(2, "t1")); loop.run();
		check(writer.acknowledged.empty() && writer.errors.empty(), "the writer waits for its open record (sequence 2)");
		host.close_group(7, turns.sequence(7), "m1"); loop.run();
		check(writer.closed == std::vector<std::string>{"t1"} && writer.errors == std::vector<std::string>{"the group closed before record 2 was durable"},
			"the close fails the writer's wait with group_closed");
		check(writer.acknowledged.empty() && turns.retired(7) == 2, "the close acknowledges nothing and retires sequence 2");
		host.reopen_group(7);
		bool reused = false;
		try { turns.stage({7, 2, j::TurnRecordKind::Checkpoint, "m1", 1}); } catch (const std::logic_error&) { reused = true; }
		check(reused, "a record at the retired sequence 2 is refused");
		if (reused) { restage(host, 7, "m1"); }
		turns.commit(); loop.run();
		check(writer.acknowledged.empty(), "the reopen checkpoint does not acknowledge the dropped open record");
		task::spawn(task::scheduler_ref::of(loop), writer.wait(2, "late")); loop.run();
		check(writer.acknowledged.empty() && writer.closed == std::vector<std::string>{"t1", "late"}, "a later wait for sequence 2 fails with group_closed");
	}
	Node node(disk);
	const auto& records = node.turns(7).records(7);
	check(records.size() == 1 && records[0].sequence == 3 && records[0].payload == "m1", "recovery holds the reopen checkpoint record, after the retired sequence");
	for (const auto& record : records) { check(record.payload != "t1", "the dropped open record is nowhere"); }
}

// The comma-joined batches a test checkpoint names, and back.
std::string join(const std::vector<std::string>& batches) {
	std::string text;
	for (const auto& batch : batches) { text += (text.empty() ? "" : ",") + batch; }
	return text;
}
std::vector<std::string> split(std::string_view text) {
	std::vector<std::string> batches;
	while (!text.empty()) { const auto comma = text.find(','); batches.emplace_back(text.substr(0, comma)); text.remove_prefix(comma == text.npos ? text.size() : comma + 1); }
	return batches;
}
bool prefix(const std::vector<std::string>& shorter, const std::vector<std::string>& longer) {
	return shorter.size() <= longer.size() && std::equal(shorter.begin(), shorter.end(), longer.begin());
}
// F1 under power loss: group 7's writer appends a batch to the sealed open record and waits; each
// round either commits it or closes the group at its durable sequence with the open record in
// flight, then reopens it from the table. Power is cut at every I/O operation: recovery holds
// every acknowledged batch, at most the one in flight besides, and never a closed one.
bool sealer_close_run(std::uint64_t cut) {
	jt::Disk disk;
	task::inline_scheduler loop;
	std::optional<Node> node;
	std::optional<Writer> writer;
	std::string flight;
	bool reached = false;
	const auto at = "cut " + std::to_string(cut);
	try {
		disk.cut = cut;
		node.emplace(disk);
		auto& host = *node->host;
		auto& turns = node->turns(7);
		writer.emplace(Writer{turns});
		host.create_group(7);
		turns.stage({7, 1, j::TurnRecordKind::Checkpoint, "", 0}); turns.commit();
		auto staged = turns.seal(7, writer->sealer(), &*writer);
		for (unsigned round = 0; round < 9; ++round) {
			const auto batch = "b" + std::to_string(round);
			writer->open = j::TurnRecord{7, staged + 1, j::TurnRecordKind::Batch, batch, 0}; turns.wake();
			task::spawn(task::scheduler_ref::of(loop), writer->wait(staged + 1, batch)); loop.run();
			if (round % 3 == 1) {
				host.close_group(7, turns.sequence(7), join(writer->acknowledged)); loop.run();
				check(writer->closed.size() == round / 3 + 1 && writer->closed.back() == batch, "the close fails the open record's wait: " + at);
				writer->open.reset();
				host.reopen_group(7); restage(host, 7, host.entry(7)->manifest); turns.commit(); loop.run();
				staged = turns.seal(7, writer->sealer(), &*writer);
			} else {
				flight = batch; turns.commit(); loop.run(); flight.clear();
				check(!writer->acknowledged.empty() && writer->acknowledged.back() == batch, "the committed open record is acknowledged: " + at);
				staged = turns.sequence(7);
			}
		}
		reached = true;
	} catch (const jt::PowerLoss&) {
	} catch (const std::exception& error) { check(false, "the run threw " + std::string(error.what()) + ": " + at); }
	if (node) { node->turns(7).failed(std::make_exception_ptr(std::runtime_error("power lost"))); }
	loop.run();
	const auto acknowledged = writer ? writer->acknowledged : std::vector<std::string>{};
	const auto closed = writer ? writer->closed : std::vector<std::string>{};
	auto possible = acknowledged; if (!flight.empty()) { possible.push_back(flight); }
	writer.reset(); node.reset();
	disk.power_loss();
	try {
		Node again(disk);
		std::vector<std::string> state;
		if (again.host->status(7) != j::GroupStatus::none) {
			auto& turns = again.turns(7);
			if (!turns.has_group(7)) { state = split(again.host->entry(7)->manifest); }
			else {
				const auto& records = turns.records(7);
				check(!records.empty() && records.front().kind == j::TurnRecordKind::Checkpoint, "the group's records start at its checkpoint: " + at);
				state = split(records.front().payload);
				for (std::size_t i = 1; i < records.size(); ++i) { state.push_back(records[i].payload); }
			}
		}
		check(prefix(acknowledged, state) && prefix(state, possible), "recovery keeps every acknowledged batch and at most the one in flight: " + at + ", recovered [" + join(state) + "], acknowledged [" + join(acknowledged) + "]");
		for (const auto& batch : closed) { check(std::find(state.begin(), state.end(), batch) == state.end(), "a batch whose wait the close failed is never recovered: " + at); }
	} catch (const std::exception& error) { check(false, "recovery threw " + std::string(error.what()) + ": " + at); }
	return reached;
}
void sealer_close_crash() {
	std::uint64_t cut = 1;
	while (!sealer_close_run(cut)) { ++cut; }
	std::cout << "sealer_close_crash: " << cut << " cuts\n";
}

// A group closed, reopened from the table, then recovered by a second Store of the same host
// (docs/mechanisms/group-lifetime.md): while the first Store lives the recovery is busy and
// changes nothing; once it ends, the second recovers the group's latest state from the log in
// the live host, the loop keeps running, and the reopened host recovers the same state.
void second_recovery() {
	jt::Disk disk;
	{
		Node node(disk);
		node.checkpoint(7, "old");
		node.store(7).close(); node.stores.erase(7);
		node.store(7);
		node.checkpoint(7, "new"); node.append(7, "after-new");
		node.append(9, "neighbor");
		bool busy = false;
		try {
			j::Store store(*node.host, 7, limits);
			store.recover([](std::uint64_t, std::string_view) { check(false, "a busy recovery replays nothing"); },
				[](const j::Frontier&, j::ArtifactReader&, std::span<j::ArtifactReader>) { check(false, "a busy recovery restores nothing"); });
		} catch (const std::system_error& error) { busy = error.code() == std::errc::operation_would_block; }
		check(busy, "a second recovery while the first Store lives is busy");
		node.stores.erase(7);
		node.store(7);
		check(node.recovered[7].bundle == "new" && node.recovered[7].batches == std::vector<std::string>{"after-new"}, "the second recovery in the live host has the latest state");
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
		{"closed_reopen", closed_reopen}, {"recreate", recreate}, {"failed_close", failed_close}, {"failed_reopen", failed_reopen}, {"loop_slots", loop_slots}, {"close_behind", close_behind},
		{"sealer_close", sealer_close}, {"sealer_close_crash", sealer_close_crash}, {"second_recovery", second_recovery}, {"lock", lock}};
	for (const auto& [name, run] : cases) {
		if (!only.empty() && only != name) { continue; }
		try { run(); } catch (const std::exception& error) { check(false, name + ": " + error.what()); }
	}
	std::cout << checks << " host checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
