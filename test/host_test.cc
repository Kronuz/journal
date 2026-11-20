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
// incarnation; a failed close that leaves the group open, in doubt: held by nothing until the
// host reopens, under power loss too; a failed reopen's table commit that fails its driver;
// two loops' table commits free of races under TSan; a close behind an acknowledged record
// refused; a close with a sealed open record in flight fails its wait, never acknowledging it,
// under power loss too; a second recovery on one host refused; one owner lock, the directory's
// own; group views that keep their range however many dead views the host sweeps around them, in
// a range table bounded by twice the live ids; a view taken while its group is closed, which
// lists none of the closed incarnation's artifacts while the view held across the close keeps its;
// and two loops that take views and commit their tables at once, free of races under TSan;
// idle groups parked, woken, created and re-partitioned under power loss at every step, and
// parked groups that log damage never costs (journal-idle-groups A3).
// `host_test [case]` runs one case.
#include "completion/posix.h"
#include "faultlab/faultfs.h"
#include "journal/store.h"
#include <cerrno>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <set>
#include <thread>
#include <unistd.h>

namespace j = kronuz::journal;
namespace fl = kronuz::faultlab;
namespace hd = kronuz::journal::host_detail;

// Reaches Host::ranges_ with no change to Host: explicit instantiation ignores access.
template <class Tag, auto Member> struct Steal { friend constexpr auto steal(Tag) { return Member; } };
struct RangesTag { friend constexpr auto steal(RangesTag); };
template struct Steal<RangesTag, &j::Host::ranges_>;

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

// The host's directory on `disk`, made on first use.
std::shared_ptr<j::IO> host_io(fl::filesystem& disk) {
	const auto names = disk.list();
	if (std::find(names.begin(), names.end(), "host") == names.end()) { disk.make_directory("host"); }
	return std::make_shared<fl::io>(disk, "host");
}
// A crash keeps any sector-boundary prefix of an unsynced append, as a real device may.
const fl::crash_policy sector_cut{.size = fl::size_pick::sector};

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
	fl::filesystem& disk;
	std::shared_ptr<RecordingIO> io;
	std::unique_ptr<j::Host> host;
	std::map<std::uint64_t, std::unique_ptr<j::Store>> stores;
	std::map<std::uint64_t, State> recovered;
	Node(fl::filesystem& d, std::uint32_t loops = 1) : disk(d), io(std::make_shared<RecordingIO>(host_io(d))) {
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
	fl::filesystem disk;
	const std::vector<std::uint64_t> groups{1, 0x10, 0x100};
	{
		Node node(disk, 2);
		for (auto g : groups) { node.checkpoint(g, "bundle-" + std::to_string(g)); node.append(g, "batch-" + std::to_string(g)); }
		std::set<std::string> seen;
		for (const auto& name : listing(*node.io)) {
			const bool known = name == "layout" || hd::shard_of(name) || hd::log_name(name);
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
	fl::filesystem disk;
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
	fl::filesystem disk;
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
	fl::filesystem disk;
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
	fl::filesystem disk;
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
	disk.crash(0, sector_cut);
	Node node(disk);
	check(node.host->status(5) == j::GroupStatus::none, "a group whose first turn was lost is created again");
	node.checkpoint(5, "second");
	check(node.host->entry(5)->incarnation == 2, "as its next incarnation");
	check(!node.artifacts(5).contains(first), "the first incarnation's artifacts are gone");
	check(node.artifacts(5).contains(hd::artifact_name(5, 2, 1)), "the new ones are named by the new incarnation");
}

// A close whose table write fails (a full disk) leaves the group open, in doubt, and its records
// live in the log: retention keeps them, and after a restart the group recovers, never offered
// for create.
void failed_close() {
	fl::filesystem disk;
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
		bool refused = false;
		try { node.store(7); } catch (const std::system_error& error) { refused = error.code() == std::errc::io_error; }
		node.stores.erase(7);
		check(refused, "the close is in doubt: the live host refuses to hold the group");
		for (int b = 0; b < 200; ++b) { node.append(8, std::string(900, 'x')); }
	}
	Node node(disk);
	check(node.host->status(7) == j::GroupStatus::stored, "after a restart the group is stored, not offered for create");
	node.store(7);
	check(node.recovered[7].bundle == "state" && node.host->entry(7)->incarnation == 1, "it recovers its checkpoint in its first incarnation");
}

// A recover of a closed group whose table commit, made before the turn carrying its restaged
// record, fails at its directory sync: that turn never writes and the driver fails, as a failed
// turn does. After a restart the group recovers its checkpoint, whichever shard the disk kept.
void failed_reopen() {
	fl::filesystem disk;
	const std::uint64_t group = 7;
	{
		Node node(disk);
		node.append(group, "a1"); node.checkpoint(group, "bundle-a1");
		node.store(group).close(); node.stores.erase(group);
		const auto turns = node.turns(group).log().stats().turns;
		node.io->sync_fails = true;
		bool threw = false;
		try { node.store(group); } catch (const std::exception&) { threw = true; }
		node.stores.erase(group);
		check(threw && !node.io->sync_fails, "the wake's table commit fails at its directory sync");
		check(node.turns(group).failed() && node.turns(group).log().stats().turns == turns, "and fails the driver before its turn writes");
	}
	disk.crash(0, sector_cut);
	Node node(disk);
	const auto status = node.host->status(group);
	check(status == j::GroupStatus::closed || status == j::GroupStatus::stored, "after a restart the group is closed, or open from its table record");
	node.store(group);
	check(node.recovered[group].bundle == "bundle-a1" && node.recovered[group].batches.empty(), "it recovers its checkpoint");
}

// A close whose table commit fails at its directory sync, after its shard's replace: the shard
// on disk may say closed, so the closing Store is fenced and the live host refuses to hold the
// group, and no Store appends to it, until the host reopens. A later directory sync then makes the close durable, and the reopened
// host recovers the group closed with its checkpoint.
void close_in_doubt() {
	fl::filesystem disk;
	const std::uint64_t group = 7;
	std::uint64_t neighbor = group + 1;
	while (hd::slot(neighbor) == hd::slot(group)) { ++neighbor; }
	{
		Node node(disk);
		node.append(group, "a1"); node.checkpoint(group, "bundle-a1");
		node.io->sync_fails = true;
		bool threw = false;
		try { node.store(group).close(); } catch (const std::system_error&) { threw = true; }
		check(threw && !node.io->sync_fails, "the close's table commit fails at its directory sync");
		bool fenced = false;
		try { node.append(group, "x"); } catch (const std::exception&) { fenced = true; }
		node.stores.erase(group);
		check(fenced, "the Store whose close failed is fenced: it appends nothing");
		bool refused = false;
		try { node.append(group, "x"); } catch (const std::system_error& error) { refused = error.code() == std::errc::io_error; }
		node.stores.erase(group);
		check(refused, "the live host refuses to hold the group whose close is in doubt");
		node.store(neighbor);
		bool still = false;
		try { node.store(group); } catch (const std::system_error& error) { still = error.code() == std::errc::io_error; }
		node.stores.erase(group);
		check(still, "and still refuses after a later directory sync");
	}
	Node node(disk);
	check(node.host->status(group) == j::GroupStatus::closed, "after a restart the group is closed");
	node.store(group);
	check(node.recovered[group].bundle == "bundle-a1" && node.recovered[group].batches.empty(), "it recovers its checkpoint");
	node.append(group, "after");
}

// close_in_doubt's sweep: a close whose table commit fails at each of its operations, before or
// after the operation's effect; then a recovery of the group in the live host, appending if it
// is let; a create in the group's slot or another, whose commit syncs the directory; and a power
// cut. The reopened host recovers every record acknowledged: the checkpoint and any append.
bool failed_close_point(fl::op kind, bool after, bool same_slot, std::uint64_t seed) {
	fl::filesystem disk;
	const std::uint64_t group = 7;
	std::uint64_t neighbor = group + 1;
	while ((hd::slot(neighbor) == hd::slot(group)) != same_slot) { ++neighbor; }
	std::vector<std::string> acknowledged;
	const char* names[] = {"write", "sync", "sync_directory", "create", "replace", "remove"};
	const auto at = std::string(names[int(kind)]) + (after ? " after" : " before") + (same_slot ? ", same slot" : ", other slot") + ", seed " + std::to_string(seed) + ": ";
	{
		Node node(disk);
		node.append(group, "a1"); node.checkpoint(group, "bundle-a1");
		disk.add_fault(fl::fault{kind, 1, EIO, after});
		bool threw = false;
		try { node.store(group).close(); } catch (const std::system_error&) { threw = true; }
		disk.clear_faults();
		node.stores.erase(group);
		if (!threw) { check(false, at + "the close fails"); return false; }
		try { node.append(group, "x"); acknowledged.push_back("x"); } catch (const std::system_error&) {}
		node.stores.erase(group);
		node.append(neighbor, "n");
	}
	disk.crash(seed);
	Node node(disk);
	try { node.store(group); } catch (const std::exception& error) { check(false, at + "recovery: " + error.what()); return false; }
	const auto& state = node.recovered[group];
	const bool kept = state.bundle == "bundle-a1" && state.batches == acknowledged;
	check(kept, at + "recovery keeps every acknowledged record: acknowledged " + std::to_string(acknowledged.size()) + " batches, recovered " + std::to_string(state.batches.size()));
	return kept;
}
void failed_close_crash() {
	std::size_t points = 0, lost = 0;
	for (auto kind : {fl::op::remove, fl::op::create, fl::op::write, fl::op::sync, fl::op::replace, fl::op::sync_directory}) {
		for (bool after : {false, true}) {
			for (bool same_slot : {false, true}) {
				for (std::uint64_t seed = 0; seed < 8; ++seed) { ++points; lost += !failed_close_point(kind, after, same_slot, seed); }
			}
		}
	}
	std::cout << "failed_close_crash: " << points << " points, " << lost << " lost\n";
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
			threads[loop] = std::thread([&, loop] { for (auto group : side[loop]) { host.create_group(group); } host.commit_transitions(static_cast<std::uint32_t>(loop)); });
		}
		for (auto& thread : threads) { thread.join(); }
		bool open = true;
		for (const auto& groups : side) { for (auto group : groups) { auto entry = host.entry(group); open &= entry && entry->state == j::GroupState::open; } }
		check(open, "every group either loop created is open in the table");
	}
	std::filesystem::remove_all(directory);
}

// journal-host-ranges-race A1. Each of two loops' threads takes views of its groups, thousands
// of them so its range table sweeps, and creates some, each commit publishing to the views it
// holds, while the other does the same: under TSan neither loop's table races with the other's
// thread, and every held view of a created group lists its first artifact.
void loop_views() {
	const auto directory = std::filesystem::current_path() / ".scratch" / ("host-loop-views-" + std::to_string(::getpid()));
	std::filesystem::remove_all(directory); std::filesystem::create_directories(directory);
	constexpr std::size_t created = 20, viewed = 2000;  // each create is a full flush on macOS
	std::vector<std::uint64_t> made[2], seen[2];
	for (std::uint64_t group = 1; made[0].size() + seen[0].size() < created + viewed || made[1].size() + seen[1].size() < created + viewed; ++group) {
		const auto loop = hd::loop(hd::slot(group), 2);
		if (made[loop].size() < created) { made[loop].push_back(group); } else if (seen[loop].size() < viewed) { seen[loop].push_back(group); }
	}
	{
		j::Host host(std::make_shared<j::PosixIO>(directory), host_identity, j::HostOptions{2, j::TurnLogOptions{64 * 1024, 1024, 1, 2, 4096}});
		std::atomic<bool> go{false};
		std::atomic<int> done{0};
		std::vector<std::shared_ptr<j::IO>> views[2], held[2];
		std::thread threads[2];
		for (int loop = 0; loop < 2; ++loop) {
			threads[loop] = std::thread([&, loop] {
				while (!go) {}
				for (std::size_t i = 0; i < viewed || done < 2; ++i) {
					auto view = host.group_io(seen[loop][i % viewed]);
					if (i % 4 == 0 && held[loop].size() < viewed) { held[loop].push_back(std::move(view)); }
					if (i < created) { views[loop].push_back(host.group_io(made[loop][i])); host.create_group(made[loop][i]); host.next_artifact(made[loop][i]); }
					if (i + 1 == created) { ++done; }
				}
			});
		}
		go = true;
		for (auto& thread : threads) { thread.join(); }
		bool lists = true;
		for (int loop = 0; loop < 2; ++loop) {
			for (std::size_t i = 0; i < created; ++i) {
				host.io().create_exclusive(hd::artifact_name(made[loop][i], 1, 0));
				lists &= listing(*views[loop][i]) == std::set<std::string>{"artifact-" + hd::hex(1, 16) + hd::hex(0, 16)};
			}
		}
		check(lists, "every held view of a created group lists its first artifact");
	}
	std::filesystem::remove_all(directory);
}

// journal-host-guards A1. A close whose record is below the group's last acknowledged one, here
// the group's real checkpoint record with two batches acknowledged after it, is refused: the
// forget would drop those batches. The group stays open in the table and the log, retention
// keeps the batches, and recovery returns them. A reopened group's close below the table's
// earlier base is refused the same way.
void close_behind() {
	fl::filesystem disk;
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
	fl::filesystem disk;
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
	fl::filesystem disk;
	task::inline_scheduler loop;
	std::optional<Node> node;
	std::optional<Writer> writer;
	std::string flight;
	bool reached = false;
	const auto at = "cut " + std::to_string(cut);
	try {
		host_io(disk);
		disk.power_off_after(cut - 1);
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
	} catch (const std::exception& error) { check(disk.powered_off(), "only the power cut fails the run, which threw " + std::string(error.what()) + ": " + at); }
	if (node) { node->turns(7).failed(std::make_exception_ptr(std::runtime_error("power lost"))); }
	loop.run();
	const auto acknowledged = writer ? writer->acknowledged : std::vector<std::string>{};
	const auto closed = writer ? writer->closed : std::vector<std::string>{};
	auto possible = acknowledged; if (!flight.empty()) { possible.push_back(flight); }
	writer.reset(); node.reset();
	disk.crash(cut, sector_cut);
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
	fl::filesystem disk;
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

// Views of 100 groups, two each, taken before the groups exist, outlive 4,000 views that die at
// once: the range table holds one range per group, never more than twice the live ids (or the
// sweep floor), and each held view still lists its group's first artifact once the group is
// created. Once every view dies the table falls to the sweep floor.
void view_sweep() {
	fl::filesystem disk;
	Node node(disk);
	auto& host = *node.host;
	const auto& table = (host.*steal(RangesTag{}))[0].table;  // one loop: every group's
	constexpr std::uint64_t live = 100;
	const std::size_t bound = std::max<std::size_t>(2 * live, hd::sweep_floor);
	std::vector<std::shared_ptr<j::IO>> views;
	for (std::uint64_t g = 1; g <= live; ++g) { views.push_back(host.group_io(g)); views.push_back(host.group_io(g)); }
	check(table.size() == live, "two views of a group share its one range: " + std::to_string(table.size()));
	std::size_t largest = 0;
	std::uint64_t fresh = std::uint64_t{1} << 20;
	for (std::uint64_t round = 0; round < 10; ++round) {
		for (std::uint64_t i = 0; i < 4 * live; ++i) { host.group_io(fresh++); largest = std::max(largest, table.size()); }
		host.group_io(1 + round);
	}
	check(largest <= bound && largest > live, "dead views gather and are swept within twice the live ids: " + std::to_string(largest));
	const std::set<std::string> first{"artifact-" + hd::hex(1, 16) + hd::hex(0, 16)};
	for (std::uint64_t g = 1; g <= live; ++g) { host.create_group(g); host.next_artifact(g); node.io->create_exclusive(hd::artifact_name(g, 1, 0)); }
	for (std::size_t i = 0; i < views.size(); ++i) {
		check(listing(*views[i]) == first, "a held view lists its group's first artifact: group " + std::to_string(1 + i / 2));
	}
	views.clear();
	for (std::size_t i = 0; i < 2 * bound; ++i) { host.group_io(fresh++); }
	check(table.size() <= hd::sweep_floor, "once every view dies the table falls to the sweep floor: " + std::to_string(table.size()));
}

// A view held across its group's close keeps listing the closed incarnation's artifacts; a view
// taken while the group is closed lists none (its own range, as every view had before the table
// shared them); both list the reopened group's, then its next checkpoint's.
void closed_view() {
	fl::filesystem disk;
	Node node(disk);
	node.checkpoint(7, "state");
	auto held = node.host->group_io(7);
	check(listing(*held).size() == 2, "a view of an open group lists its artifacts: " + std::to_string(listing(*held).size()));
	node.store(7).close(); node.stores.erase(7);
	check(node.host->status(7) == j::GroupStatus::closed, "the table holds the group closed");
	check(listing(*held).size() == 2, "the view held across the close keeps its artifacts: " + std::to_string(listing(*held).size()));
	auto fresh = node.host->group_io(7);
	check(listing(*fresh).empty(), "a view taken while the group is closed lists none: " + std::to_string(listing(*fresh).size()));
	node.store(7);
	check(listing(*held).size() == 2 && listing(*fresh).size() == 2, "both views list the reopened group's artifacts: " + std::to_string(listing(*held).size()) + ", " + std::to_string(listing(*fresh).size()));
	node.append(7, "after"); node.checkpoint(7, "again");
	check(listing(*held).size() == 4 && listing(*fresh).size() == 4, "both views list the next checkpoint's: " + std::to_string(listing(*held).size()) + ", " + std::to_string(listing(*fresh).size()));
}

// One owner lock, the directory's own (what strata's cold tier takes): a second host on the
// directory is refused while the first holds it.
void lock() {
	fl::filesystem disk;
	auto io = std::make_shared<RecordingIO>(host_io(disk));
	{
		j::Host host(io, host_identity, j::HostOptions{1, turn_options});
		check(io->lock_mode == j::OwnerLockMode::directory, "the host takes the directory's lock");
		bool busy = false;
		try { j::Host other(host_io(disk), host_identity, j::HostOptions{1, turn_options}); }
		catch (const std::system_error&) { busy = true; }
		check(busy, "a second host on the directory is refused");
	}
	j::Host again(host_io(disk), host_identity, j::HostOptions{1, turn_options});
	check(again.loops() == 1, "the lock is released with the host");
}
}  // namespace


// journal-idle-groups A3 (docs/design/idle-groups.md "Crash at each step"). Groups 1-8 are
// parked and 9-12 open on one loop; then a host at L = 2 re-partitions, creates 20-23, wakes
// 1-4 (the table's record staged again, then a batch), appends to the woken and open groups,
// checkpoints 1, 2, 9 and 10 and parks them, and a host at L = 3 re-partitions again. Power is
// cut after every mutation; after the crash, three restarts at L = 3 each find every group at a
// state it reached, no older than its last acknowledged one, and none lost.
namespace idle {
using Content = std::vector<std::string>;  // a group's checkpoint payload, then its batches
struct Group { std::vector<Content> states{Content{}}; std::size_t acknowledged = 0; std::uint64_t sequence = 0; };
const j::TurnLogOptions turns{16 * 1024, 1024, 1, 4, 4096};
std::unique_ptr<j::Host> open(fl::filesystem& disk, std::uint32_t loops) { return std::make_unique<j::Host>(host_io(disk), host_identity, j::HostOptions{loops, turns}); }
void commit(j::Host& host) { for (std::uint32_t l = 0; l < host.loops(); ++l) { while (!host.driver(l).idle()) { host.driver(l).commit(); } } }
std::size_t park(j::Host& host) { std::size_t n = 0; for (std::uint32_t l = 0; l < host.loops(); ++l) { n += host.park(l); } return n; }
// The group's content as recovery gives it; "lost" for a lost group.
Content content(j::Host& host, std::uint64_t group) {
	switch (host.status(group)) {
	case j::GroupStatus::none: return {};
	case j::GroupStatus::lost: return {"lost"};
	case j::GroupStatus::closed: return {host.entry(group)->manifest};
	case j::GroupStatus::stored: break;
	}
	auto& turns = host.driver_of(group);
	if (!turns.has_group(group)) { return {host.entry(group)->manifest}; }
	Content state;
	for (const auto& record : turns.records(group)) { state.push_back(record.payload); }
	return state;
}
struct Model {
	std::map<std::uint64_t, Group> groups;
	// Stages a record and the state it leads to; acknowledge() marks the states durable.
	void stage(j::Host& host, std::uint64_t group, j::TurnRecordKind kind, const std::string& payload, std::uint64_t at = 0) {
		auto& g = groups[group];
		const auto covers = g.sequence;
		g.sequence = at ? at : g.sequence + 1;
		host.driver_of(group).stage(j::TurnRecord{group, g.sequence, kind, payload, covers});
		auto next = kind == j::TurnRecordKind::Checkpoint ? Content{payload} : g.states.back();
		if (kind == j::TurnRecordKind::Batch) { next.push_back(payload); }
		if (kind != j::TurnRecordKind::Checkpoint || next != g.states.back()) { g.states.push_back(next); }
	}
	void acknowledge() { for (auto& [_, g] : groups) { g.acknowledged = g.states.size() - 1; } }
	void verify(j::Host& host, const std::string& at) {
		for (const auto& [group, g] : groups) {
			const auto state = content(host, group);
			const auto found = std::find(g.states.begin() + static_cast<std::ptrdiff_t>(g.acknowledged), g.states.end(), state);
			check(found != g.states.end(), "group " + std::to_string(group) + " recovers a state it reached, no older than its acknowledged one: [" + join(state) + "] at " + at);
		}
	}
};
// The run up to the cut; the model of what it staged and acknowledged.
void run(fl::filesystem& disk, Model& model) {
	auto host = open(disk, 2);
	for (std::uint64_t g : {20u, 21u, 22u, 23u}) { host->create_group(g); model.stage(*host, g, j::TurnRecordKind::Checkpoint, "c" + std::to_string(g)); }
	commit(*host); model.acknowledge();
	for (std::uint64_t g = 1; g <= 4; ++g) {
		host->reopen_group(g);
		const auto entry = host->entry(g);
		model.groups[g].sequence = entry->base;
		model.stage(*host, g, j::TurnRecordKind::Checkpoint, entry->manifest, std::max(entry->base, host->driver_of(g).retired(g)) + 1);
		model.stage(*host, g, j::TurnRecordKind::Batch, "w" + std::to_string(g));
	}
	commit(*host); model.acknowledge();
	for (std::uint64_t g : {1u, 2u, 3u, 9u, 10u, 11u, 20u}) { model.stage(*host, g, j::TurnRecordKind::Batch, "a" + std::to_string(g)); }
	commit(*host); model.acknowledge();
	for (std::uint64_t g : {1u, 2u, 9u, 10u}) { model.stage(*host, g, j::TurnRecordKind::Checkpoint, "k" + std::to_string(g)); }
	commit(*host); model.acknowledge();
	for (std::uint64_t g : {1u, 2u, 9u, 10u}) { host->hold(g); }  // held, then released
	check(park(*host) == 5, "the checkpointed released groups park, and 12, idle since the open");
	host.reset();
	open(disk, 3);
}
// Ids of one slot, enough to split its shard: the first `count` from `from`.
std::vector<std::uint64_t> slot_ids(std::uint32_t slot, std::uint64_t from, std::size_t count) {
	std::vector<std::uint64_t> ids;
	for (auto id = from; ids.size() < count; ++id) { if (hd::slot(id) == slot) { ids.push_back(id); } }
	return ids;
}
std::size_t shard_files(const fl::filesystem& disk, std::uint32_t slot) {
	std::size_t n = 0;
	for (const auto& name : disk.list("host")) { n += name.starts_with("groups-" + hd::hex(slot, 3) + "-") && !name.ends_with(".pending"); }
	return n;
}
// The split run: one slot's groups created, parked and woken past leaf_bytes, so commits split
// its shard while the power may fail.
void split_run(fl::filesystem& disk, Model& model) {
	auto host = open(disk, 2);
	const auto fresh = slot_ids(7, 1, 160);
	for (std::size_t i = 40; i < 160; ++i) { host->create_group(fresh[i]); model.stage(*host, fresh[i], j::TurnRecordKind::Checkpoint, "s" + std::to_string(fresh[i])); }
	commit(*host); model.acknowledge();
	for (std::size_t i = 40; i < 160; i += 2) { host->hold(fresh[i]); }
	park(*host);
	for (std::size_t i = 0; i < 40; i += 3) {
		host->reopen_group(fresh[i]);
		const auto entry = host->entry(fresh[i]);
		model.groups[fresh[i]].sequence = entry->base;
		model.stage(*host, fresh[i], j::TurnRecordKind::Checkpoint, entry->manifest, std::max(entry->base, host->driver_of(fresh[i]).retired(fresh[i])) + 1);
		model.stage(*host, fresh[i], j::TurnRecordKind::Batch, "w" + std::to_string(fresh[i]));
	}
	commit(*host); model.acknowledge();
	host.reset();
	check(shard_files(disk, 7) > 1, "the slot's shard split");
	open(disk, 3);
}
}  // namespace idle
bool idle_crash_run(const fl::filesystem& base, const idle::Model& prepared, std::uint64_t cut, void (*run)(fl::filesystem&, idle::Model&) = idle::run) {
	fl::filesystem disk = base.snapshot();
	idle::Model model = prepared;
	bool reached = false;
	const auto at = "cut " + std::to_string(cut);
	disk.power_off_after(cut - 1);
	try { run(disk, model); reached = true; }
	catch (const std::exception& error) { check(disk.powered_off(), "only the power cut fails the run, which threw " + std::string(error.what()) + ": " + at); }
	disk.crash(cut, sector_cut);
	for (int restart = 0; restart < 3; ++restart) {
		try { auto host = idle::open(disk, 3); model.verify(*host, at + " restart " + std::to_string(restart)); }
		catch (const std::exception& error) { check(false, at + " restart " + std::to_string(restart) + ": " + error.what()); }
	}
	return reached;
}
void idle_crash() {
	fl::filesystem base;
	idle::Model model;
	{
		auto host = idle::open(base, 1);
		for (std::uint64_t g = 1; g <= 12; ++g) {
			host->create_group(g);
			model.stage(*host, g, j::TurnRecordKind::Checkpoint, "p" + std::to_string(g));
			model.stage(*host, g, j::TurnRecordKind::Batch, "b" + std::to_string(g));
		}
		idle::commit(*host);
		for (std::uint64_t g = 1; g <= 12; ++g) { model.stage(*host, g, j::TurnRecordKind::Checkpoint, "q" + std::to_string(g)); }
		idle::commit(*host); model.acknowledge();
		for (std::uint64_t g = 1; g <= 8; ++g) { host->hold(g); }
		check(idle::park(*host) == 8, "the released groups park");
	}
	std::uint64_t cut = 1;
	while (!idle_crash_run(base, model, cut)) { ++cut; }
	std::cout << "idle_crash: " << cut - 1 << " power cuts, every one of them recovered\n";
}

// journal-idle-groups A3 at a split: a slot's shard past leaf_bytes splits while the power may
// fail at every step, and every group recovers a state no older than its acknowledged one.
void split_crash() {
	fl::filesystem base;
	idle::Model model;
	{
		auto host = idle::open(base, 1);
		const auto ids = idle::slot_ids(7, 1, 40);
		for (auto g : ids) { host->create_group(g); model.stage(*host, g, j::TurnRecordKind::Checkpoint, "p" + std::to_string(g)); }
		idle::commit(*host); model.acknowledge();
		for (auto g : ids) { host->hold(g); }
		check(idle::park(*host) == ids.size(), "the slot's groups park");
		check(idle::shard_files(base, 7) == 1, "forty entries fit one shard");
	}
	std::uint64_t cut = 1;
	while (!idle_crash_run(base, model, cut, idle::split_run)) { ++cut; }
	std::cout << "split_crash: " << cut - 1 << " power cuts, every one of them recovered\n";
}

// A split node's removal is durable only with a later directory sync: open() finds the node beside
// the children that cover it, and the children, which are newer, win.
void split_stale_node() {
	fl::filesystem disk;
	const auto ids = idle::slot_ids(7, 1, 160);
	const std::string root = "groups-" + hd::hex(7, 3) + "-" + hd::hex(1, 9);
	std::string stale;
	{
		auto host = idle::open(disk, 1);
		auto& turns = host->driver(0);
		for (std::size_t i = 0; i < 40; ++i) { host->create_group(ids[i]); turns.stage(j::TurnRecord{ids[i], 1, j::TurnRecordKind::Checkpoint, "p" + std::to_string(ids[i])}); }
		idle::commit(*host);
		for (std::size_t i = 0; i < 40; ++i) { host->hold(ids[i]); }
		idle::park(*host);
		stale = std::string(disk.view("host/" + root));
		for (std::size_t i = 40; i < 160; ++i) { host->create_group(ids[i]); turns.stage(j::TurnRecord{ids[i], 1, j::TurnRecordKind::Checkpoint, "s" + std::to_string(ids[i])}); }
		idle::commit(*host);
		for (std::size_t i = 40; i < 160; i += 2) { host->hold(ids[i]); }
		idle::park(*host);
	}
	check(idle::shard_files(disk, 7) > 1 && !host_io(disk)->entry_footprint(root), "the slot's root split");
	{
		auto file = host_io(disk)->create_exclusive(root);
		file->write_at(0, stale); file->sync();
		host_io(disk)->sync_directory();
	}
	auto host = idle::open(disk, 1);
	check(!host_io(disk)->entry_footprint(root), "open drops the node its children cover");
	for (std::size_t i = 0; i < 160; ++i) {
		const auto g = ids[i];
		const bool parked = i < 40 || i % 2 == 0;
		const auto name = (i < 40 ? "p" : "s") + std::to_string(g);
		if (parked) { check(host->status(g) == j::GroupStatus::closed && host->entry(g)->manifest == name, "a parked group reads from the split's children: " + std::to_string(i)); }
		else { check(host->status(g) == j::GroupStatus::stored && host->driver(0).records(g).size() == 1, "an open group keeps its entry: " + std::to_string(i)); }
	}
}

// journal-idle-groups Q3: a damaged table shard fails only the groups it holds. The host opens;
// every id of that shard, parked, open or never created, is lost; every other group recovers.
void table_damage() {
	fl::filesystem disk;
	const auto damaged = idle::slot_ids(5, 1, 6), other = idle::slot_ids(6, 1, 6);
	{
		auto host = idle::open(disk, 1);
		auto& turns = host->driver(0);
		for (auto ids : {damaged, other}) {
			for (auto g : ids) { host->create_group(g); turns.stage(j::TurnRecord{g, 1, j::TurnRecordKind::Checkpoint, "p" + std::to_string(g)}); }
		}
		idle::commit(*host);
		for (auto ids : {damaged, other}) { for (std::size_t i = 0; i < 3; ++i) { host->hold(ids[i]); } }
		check(idle::park(*host) == 6, "half of each slot's groups park");
		for (auto ids : {damaged, other}) { for (std::size_t i = 3; i < 6; ++i) { turns.stage(j::TurnRecord{ids[i], 2, j::TurnRecordKind::Batch, "b" + std::to_string(ids[i])}); } }
		idle::commit(*host);
		host->next_artifact(other[5]);  // the open groups' entries are in their shards
		host->next_artifact(damaged[5]);
	}
	std::string shard;
	for (const auto& name : disk.list("host")) { if (name.starts_with("groups-" + hd::hex(5, 3) + "-")) { shard = name; } }
	check(!shard.empty(), "slot 5 has its shard");
	{
		auto file = host_io(disk)->open_existing(shard);
		file->write_at(40, std::string(1, '\xff'));
		file->sync();
	}
	for (int restart = 0; restart < 2; ++restart) {
		auto host = idle::open(disk, 1);
		for (std::size_t i = 0; i < 6; ++i) { check(host->status(damaged[i]) == j::GroupStatus::lost, "a group of the damaged shard is lost: " + std::to_string(i)); }
		const auto unborn = idle::slot_ids(5, damaged.back() + 1, 1).front();
		check(host->status(unborn) == j::GroupStatus::lost, "an id the damaged shard may have held is refused");
		bool refused = false;
		try { host->create_group(unborn); } catch (const std::logic_error&) { refused = true; }
		check(refused, "a create in the damaged shard is refused");
		check(!host->driver(0).has_group(damaged[4]), "the log forgets a group of the damaged shard");
		for (std::size_t i = 0; i < 3; ++i) { check(host->status(other[i]) == j::GroupStatus::closed && host->entry(other[i])->manifest == "p" + std::to_string(other[i]), "a parked group of another shard opens"); }
		for (std::size_t i = 3; i < 6; ++i) { check(host->driver(0).records(other[i]).size() == 2, "an open group of another shard recovers its records"); }
		check(host->entry(other[5])->high == hd::serial_block, "an open group of another shard keeps its serial block");
		const auto born = idle::slot_ids(6, other.back() + 1, static_cast<std::size_t>(restart) + 1).back();
		host->create_group(born);
		host->driver(0).stage(j::TurnRecord{born, 1, j::TurnRecordKind::Checkpoint, "born"});
		idle::commit(*host);
		check(host->status(born) == j::GroupStatus::stored, "another shard takes a create");
		host->hold(born);
		idle::park(*host);
	}
}

// Q2: Host::park called mid-turn, between a turn's begin() and completed(): the groups of the
// turn in flight stay, other idle groups park, and both survive a restart.
void park_mid_turn() {
	fl::filesystem disk;
	{
		auto host = idle::open(disk, 1);
		auto& turns = host->driver(0);
		for (std::uint64_t g = 1; g <= 3; ++g) { host->create_group(g); turns.stage(j::TurnRecord{g, 1, j::TurnRecordKind::Checkpoint, "p" + std::to_string(g)}); }
		idle::commit(*host);
		auto holder = host->hold(1);
		host->hold(2);  // held, then released
		turns.stage(j::TurnRecord{1, 2, j::TurnRecordKind::Batch, "b1"});
		const auto& write = turns.begin();
		holder.reset();  // released with its batch in flight
		check(host->park(0) == 1 && host->status(2) == j::GroupStatus::closed, "mid-turn, the released group outside the turn parks");
		check(host->status(1) == j::GroupStatus::stored, "the group in the turn in flight stays");
		write.file->write_at(write.offset, write.bytes); write.file->sync_data(); turns.completed();
		check(host->park(0) == 0, "after the turn, the group holds a batch: it does not park");
	}
	auto host = idle::open(disk, 1);
	check(host->status(2) == j::GroupStatus::closed && host->entry(2)->manifest == "p2", "the group parked mid-turn stays parked");
	check(host->driver(0).records(1).size() == 2 && host->driver(0).records(3).size() == 1, "the turn's group and the held one recover their records");
}

// Review F3: as idle_damage, with the second block's header damaged too, so no manifest names
// the damaged block's groups. Recovery reports the damage, and the woken group, whose table
// entry is open before its turn (D5), is lost rather than read back as parked.
void idle_unmanifested() {
	fl::filesystem disk;
	const std::string marker(3000, 'Z');
	{
		auto host = idle::open(disk, 1);
		auto& turns = host->driver(0);
		for (std::uint64_t g = 1; g <= 6; ++g) {
			host->create_group(g);
			turns.stage(j::TurnRecord{g, 1, j::TurnRecordKind::Checkpoint, "p" + std::to_string(g)});
		}
		idle::commit(*host);
		for (std::uint64_t g = 1; g <= 5; ++g) { host->hold(g); }
		check(idle::park(*host) == 5, "five groups park");
		host->reopen_group(1);
		const auto at = std::max(host->entry(1)->base, turns.retired(1)) + 1;
		turns.stage(j::TurnRecord{1, at, j::TurnRecordKind::Checkpoint, "p1", 1});
		turns.stage(j::TurnRecord{1, at + 1, j::TurnRecordKind::Batch, marker});
		idle::commit(*host);
		for (std::uint64_t s = 2; s < 6; ++s) { turns.stage(j::TurnRecord{6, s, j::TurnRecordKind::Batch, "later" + std::to_string(s)}); idle::commit(*host); }
	}
	std::string segment;
	for (const auto& name : disk.list("host")) { if (name.starts_with("l") && disk.view("host/" + name).find(marker.substr(0, 900)) != std::string_view::npos) { segment = name; } }
	const auto view = disk.view("host/" + segment);
	const auto first = (view.find(marker.substr(0, 900)) / 1024 + 1) * 1024;
	{
		auto file = host_io(disk)->open_existing(segment);
		for (std::uint64_t at : {first + 512, first + 1024, first + 1024 + 512}) { file->write_at(at, std::string(1, 'Y')); }
		file->sync();
	}
	auto host = idle::open(disk, 1);
	check(host->driver(0).recovered().damaged, "the log reports the damage");
	check(host->status(1) == j::GroupStatus::lost, "the woken group whose post-wake records the damage took is lost");
	for (std::uint64_t g = 2; g <= 5; ++g) { check(host->status(g) == j::GroupStatus::closed, "a parked group opens from its table record"); }
}

// journal-idle-groups A3, review F3: damage to two blocks of a woken group's batch costs no
// parked group, and the woken group, whose only post-wake records were there, is reported lost
// rather than read back as parked.
void idle_damage() {
	fl::filesystem disk;
	const std::string marker(3000, 'Z');
	{
		auto host = idle::open(disk, 1);
		auto& turns = host->driver(0);
		for (std::uint64_t g = 1; g <= 6; ++g) {
			host->create_group(g);
			turns.stage(j::TurnRecord{g, 1, j::TurnRecordKind::Checkpoint, "p" + std::to_string(g)});
		}
		idle::commit(*host);
		for (std::uint64_t g = 1; g <= 5; ++g) { host->hold(g); }
		check(idle::park(*host) == 5, "five groups park");
		host->reopen_group(1);
		const auto at = std::max(host->entry(1)->base, turns.retired(1)) + 1;
		turns.stage(j::TurnRecord{1, at, j::TurnRecordKind::Checkpoint, "p1", 1});
		turns.stage(j::TurnRecord{1, at + 1, j::TurnRecordKind::Batch, marker});
		idle::commit(*host);
		for (std::uint64_t s = 2; s < 6; ++s) { turns.stage(j::TurnRecord{6, s, j::TurnRecordKind::Batch, "later" + std::to_string(s)}); idle::commit(*host); }
	}
	// The batch's first block holds its fragment header; the two after it are the batch alone.
	std::string segment;
	for (const auto& name : disk.list("host")) { if (name.starts_with("l") && disk.view("host/" + name).find(marker.substr(0, 900)) != std::string_view::npos) { segment = name; } }
	check(!segment.empty(), "the woken group's batch is in the log");
	const auto view = disk.view("host/" + segment);
	const auto first = (view.find(marker.substr(0, 900)) / 1024 + 1) * 1024;
	{
		auto file = host_io(disk)->open_existing(segment);
		// A byte of each of two blocks' payloads; idle_unmanifested damages the second's header too.
		for (std::uint64_t at : {first + 512, first + 1024 + 512}) { file->write_at(at, std::string(1, 'Y')); }
		file->sync();
	}
	auto host = idle::open(disk, 1);
	check(host->driver(0).recovered().damaged, "the log reads the damage");
	check(host->status(1) == j::GroupStatus::lost, "the woken group whose post-wake records the damage took is lost");
	for (std::uint64_t g = 2; g <= 5; ++g) { check(host->status(g) == j::GroupStatus::closed && host->entry(g)->manifest == "p" + std::to_string(g), "a parked group opens from its table record: group " + std::to_string(g)); }
	// Group 6's later records share the block of the batch's last fragment, which the damage left
	// orphaned, so that block's manifest may name it: it keeps every record or is lost, never fewer.
	check(host->status(6) == j::GroupStatus::lost || host->driver(0).records(6).size() == 5, "the group that wrote after the damage keeps every record or is lost");
}

int main(int argc, char** argv) {
	const std::string only = argc > 1 ? argv[1] : "";
	const std::vector<std::pair<std::string, void (*)()>> cases{
		{"names", names}, {"reclaim_isolation", reclaim_isolation}, {"census", census},
		{"closed_reopen", closed_reopen}, {"recreate", recreate}, {"failed_close", failed_close}, {"failed_reopen", failed_reopen}, {"close_in_doubt", close_in_doubt}, {"failed_close_crash", failed_close_crash}, {"loop_slots", loop_slots}, {"close_behind", close_behind},
		{"sealer_close", sealer_close}, {"sealer_close_crash", sealer_close_crash}, {"second_recovery", second_recovery}, {"lock", lock}, {"view_sweep", view_sweep}, {"closed_view", closed_view},
		{"loop_views", loop_views}, {"idle_crash", idle_crash}, {"idle_damage", idle_damage},
		{"split_crash", split_crash}, {"table_damage", table_damage}, {"park_mid_turn", park_mid_turn}, {"split_stale_node", split_stale_node},
		{"idle_unmanifested", idle_unmanifested}};
	for (const auto& [name, run] : cases) {
		if (!only.empty() && only != name) { continue; }
		try { run(); } catch (const std::exception& error) { check(false, name + ": " + error.what()); }
	}
	std::cout << checks << " host checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
