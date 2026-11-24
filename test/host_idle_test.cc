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

// host_test's idle-group cases (docs/design/idle-groups.md): groups parked, woken, created and
// re-partitioned under power loss at every step, and parked groups that log damage never costs.
#include "host_support.h"

struct OpenedTag { friend constexpr auto steal(OpenedTag); };
template struct Steal<OpenedTag, &j::Host::opened_>;

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
	std::vector<std::unique_ptr<j::Host::Hold>> open_groups;  // their consumer keeps them open
	for (std::uint64_t g : {20u, 21u, 22u, 23u}) { host->create_group(g); model.stage(*host, g, j::TurnRecordKind::Checkpoint, "c" + std::to_string(g)); }
	commit(*host); model.acknowledge();
	for (std::uint64_t g : {20u, 21u, 22u, 23u}) { open_groups.push_back(host->hold(g)); }
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
	open_groups.clear(); host.reset();
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
	std::vector<std::unique_ptr<j::Host::Hold>> open_groups;
	for (std::size_t i = 40; i < 160; ++i) { host->create_group(fresh[i]); model.stage(*host, fresh[i], j::TurnRecordKind::Checkpoint, "s" + std::to_string(fresh[i])); }
	commit(*host); model.acknowledge();
	for (std::size_t i = 41; i < 160; i += 2) { open_groups.push_back(host->hold(fresh[i])); }
	park(*host);
	for (std::size_t i = 0; i < 40; i += 3) {
		host->reopen_group(fresh[i]);
		const auto entry = host->entry(fresh[i]);
		model.groups[fresh[i]].sequence = entry->base;
		model.stage(*host, fresh[i], j::TurnRecordKind::Checkpoint, entry->manifest, std::max(entry->base, host->driver_of(fresh[i]).retired(fresh[i])) + 1);
		model.stage(*host, fresh[i], j::TurnRecordKind::Batch, "w" + std::to_string(fresh[i]));
	}
	commit(*host); model.acknowledge();
	open_groups.clear(); host.reset();
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
		std::vector<std::unique_ptr<j::Host::Hold>> open_groups;
		for (std::uint64_t g = 9; g <= 12; ++g) { open_groups.push_back(host->hold(g)); }
		check(idle::park(*host) == 8, "the groups no consumer has open park");
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
		std::vector<std::unique_ptr<j::Host::Hold>> open_groups;
		for (std::size_t i = 41; i < 160; i += 2) { open_groups.push_back(host->hold(ids[i])); }
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
		std::vector<std::unique_ptr<j::Host::Hold>> open_groups;
		for (auto ids : {damaged, other}) { for (std::size_t i = 3; i < 6; ++i) { open_groups.push_back(host->hold(ids[i])); } }
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
		auto holder = host->hold(1), third = host->hold(3);
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

// A turn-log segment's data: each sector's first 504 bytes, where a record's bytes run unbroken.
std::string sector_data(std::string_view file) {
	std::string bytes;
	for (std::size_t at = 0; at < file.size(); at += j::turn_log_detail::sector_bytes) { bytes.append(file.substr(at, j::turn_log_detail::sector_data)); }
	return bytes;
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
		auto sixth = host->hold(6);
		check(idle::park(*host) == 5, "five groups park");
		host->reopen_group(1);
		const auto at = std::max(host->entry(1)->base, turns.retired(1)) + 1;
		turns.stage(j::TurnRecord{1, at, j::TurnRecordKind::Checkpoint, "p1", 1});
		turns.stage(j::TurnRecord{1, at + 1, j::TurnRecordKind::Batch, marker});
		idle::commit(*host);
		for (std::uint64_t s = 2; s < 6; ++s) { turns.stage(j::TurnRecord{6, s, j::TurnRecordKind::Batch, "later" + std::to_string(s)}); idle::commit(*host); }
	}
	std::string segment;
	for (const auto& name : disk.list("host")) { if (name.starts_with("l") && sector_data(disk.view("host/" + name)).find(marker.substr(0, 900)) != std::string::npos) { segment = name; } }
	const auto first = (j::turn_log_detail::file_offset(sector_data(disk.view("host/" + segment)).find('Z')) / 1024 + 1) * 1024;
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
// parked group, and the woken group, whose only post-wake records were there, is reported bounded
// at that batch (docs/mechanisms/lost-bound.md) rather than read back as parked; its reader,
// holding it through its checkpoint record only, loses it.
void idle_damage() {
	fl::filesystem disk;
	const std::string marker(3000, 'Z');
	std::uint64_t woke = 0;  // group 1's restaged checkpoint record; its batch is woke + 1
	{
		auto host = idle::open(disk, 1);
		auto& turns = host->driver(0);
		for (std::uint64_t g = 1; g <= 6; ++g) {
			host->create_group(g);
			turns.stage(j::TurnRecord{g, 1, j::TurnRecordKind::Checkpoint, "p" + std::to_string(g)});
		}
		idle::commit(*host);
		auto sixth = host->hold(6);
		check(idle::park(*host) == 5, "five groups park");
		host->reopen_group(1);
		woke = std::max(host->entry(1)->base, turns.retired(1)) + 1;
		turns.stage(j::TurnRecord{1, woke, j::TurnRecordKind::Checkpoint, "p1", 1});
		turns.stage(j::TurnRecord{1, woke + 1, j::TurnRecordKind::Batch, marker});
		idle::commit(*host);
		for (std::uint64_t s = 2; s < 6; ++s) { turns.stage(j::TurnRecord{6, s, j::TurnRecordKind::Batch, "later" + std::to_string(s)}); idle::commit(*host); }
	}
	// The batch's first block holds its fragment header; the two after it are the batch alone.
	std::string segment;
	for (const auto& name : disk.list("host")) { if (name.starts_with("l") && sector_data(disk.view("host/" + name)).find(marker.substr(0, 900)) != std::string::npos) { segment = name; } }
	check(!segment.empty(), "the woken group's batch is in the log");
	const auto first = (j::turn_log_detail::file_offset(sector_data(disk.view("host/" + segment)).find('Z')) / 1024 + 1) * 1024;
	{
		auto file = host_io(disk)->open_existing(segment);
		// A byte of each of two blocks' payloads; idle_unmanifested damages the second's header too.
		for (std::uint64_t at : {first + 512, first + 1024 + 512}) { file->write_at(at, std::string(1, 'Y')); }
		file->sync();
	}
	auto host = idle::open(disk, 1);
	check(host->driver(0).recovered().damaged, "the log reads the damage");
	check(host->status(1) == j::GroupStatus::bounded && host->bound(1) == woke + 1 && host->driver(0).records(1).empty(), "the woken group is bounded at its damaged batch, with no record after it");
	for (std::uint64_t g = 2; g <= 5; ++g) { check(host->status(g) == j::GroupStatus::closed && host->entry(g)->manifest == "p" + std::to_string(g), "a parked group opens from its table record: group " + std::to_string(g)); }
	// Group 6's later records follow the batch's last fragment in the block after the damaged ones,
	// and no manifest of a damaged block names group 6: it keeps every record.
	const std::vector<j::TurnRecord> later{{6, 2, j::TurnRecordKind::Batch, "later2"}, {6, 3, j::TurnRecordKind::Batch, "later3"}, {6, 4, j::TurnRecordKind::Batch, "later4"}, {6, 5, j::TurnRecordKind::Batch, "later5"}};
	const auto& sixth = host->driver(0).records(6);
	check(host->status(6) == j::GroupStatus::stored && sixth.size() == 5 && sixth.front().sequence == 1 && std::vector<j::TurnRecord>(sixth.begin() + 1, sixth.end()) == later, "the group that wrote after the damage keeps every record");
	// Group 1's reader holds it through its checkpoint record, below the bound: it loses it.
	host->lose(1);
	check(host->status(1) == j::GroupStatus::lost, "a reader below the bound loses the group");
}

// Review F1, D9's orphan rule: a park whose split fails at its directory sync, after its renames,
// leaves its children beside their node. The next commit of the slot removes them first, so a
// create it acknowledges survives a power cut: no orphan covers the node at open.
void split_orphan() {
	fl::filesystem disk;
	const auto ids = idle::slot_ids(7, 1, 81);
	const auto fresh = ids[80];
	{
		auto host = idle::open(disk, 1);
		auto& turns = host->driver(0);
		for (std::size_t i = 0; i < 80; ++i) { host->create_group(ids[i]); turns.stage(j::TurnRecord{ids[i], 1, j::TurnRecordKind::Checkpoint, std::string(40, 'x') + std::to_string(ids[i])}); }
		idle::commit(*host);
		check(idle::shard_files(disk, 7) == 1, "split_orphan: eighty open entries fit one shard");
		disk.add_fault(fl::fault{fl::op::sync_directory, 1, EIO, true});
		bool threw = false;
		try { idle::park(*host); } catch (const std::exception&) { threw = true; }
		disk.clear_faults();
		check(threw && idle::shard_files(disk, 7) > 1, "split_orphan: the splitting park fails at its directory sync and leaves its children");
		host->create_group(fresh);
		turns.stage(j::TurnRecord{fresh, 1, j::TurnRecordKind::Checkpoint, "fresh"});
		turns.stage(j::TurnRecord{fresh, 2, j::TurnRecordKind::Batch, "acked"});
		idle::commit(*host);
		check(host->status(fresh) == j::GroupStatus::stored, "split_orphan: the create is acknowledged");
	}
	disk.crash(0, sector_cut);
	for (int restart = 0; restart < 2; ++restart) {
		auto host = idle::open(disk, 1);
		check(host->status(fresh) == j::GroupStatus::stored && host->driver(0).records(fresh).size() == 2, "split_orphan: the create acknowledged after the failed split survives: restart " + std::to_string(restart));
	}
}

// Review F2: a split slot loses one shard file (the name is gone, not a CRC failure). Only that
// file's groups are lost, and reported so; its siblings stay on disk and their parked groups open.
void missing_shard() {
	fl::filesystem disk;
	const auto ids = idle::slot_ids(7, 1, 160);
	{
		auto host = idle::open(disk, 1);
		auto& turns = host->driver(0);
		for (auto g : ids) { host->create_group(g); turns.stage(j::TurnRecord{g, 1, j::TurnRecordKind::Checkpoint, std::string(40, 'p') + std::to_string(g)}); }
		idle::commit(*host);
		check(idle::shard_files(disk, 7) == 1, "missing_shard: a slot's first commit writes its root whole");
		check(idle::park(*host) == ids.size(), "missing_shard: the slot's groups park");
	}
	std::vector<std::string> leaves;
	for (const auto& name : disk.list("host")) { if (name.starts_with("groups-" + hd::hex(7, 3) + "-")) { leaves.push_back(name); } }
	check(leaves.size() >= 3, "missing_shard: the slot split into at least three shards");
	const auto gone = hd::shard_of(leaves[leaves.size() / 2])->second;
	host_io(disk)->remove(leaves[leaves.size() / 2]);
	host_io(disk)->sync_directory();
	for (int restart = 0; restart < 2; ++restart) {
		auto host = idle::open(disk, 1);
		check(idle::shard_files(disk, 7) == leaves.size() - 1, "missing_shard: open keeps the other shards");
		std::size_t lost = 0;
		for (auto g : ids) {
			if (hd::holds(gone, g)) { lost += host->status(g) == j::GroupStatus::lost; continue; }
			check(host->status(g) == j::GroupStatus::closed && host->entry(g)->manifest == std::string(40, 'p') + std::to_string(g), "missing_shard: a parked group of another shard opens: " + std::to_string(g));
		}
		const auto in_gone = static_cast<std::size_t>(std::count_if(ids.begin(), ids.end(), [&](auto g) { return hd::holds(gone, g); }));
		check(in_gone > 0 && lost == in_gone, "missing_shard: every group of the missing shard is lost, never none");
		std::vector<std::uint64_t> inside, outside;
		for (auto g : idle::slot_ids(7, ids.back() + 1, 400)) { (hd::holds(gone, g) ? inside : outside).push_back(g); }
		check(!inside.empty() && outside.size() > 1, "missing_shard: fresh ids on both sides");
		bool refused = false;
		try { host->create_group(inside.front()); } catch (const std::logic_error&) { refused = true; }
		check(refused && host->status(inside.front()) == j::GroupStatus::lost, "missing_shard: an id on the missing shard's paths is lost, and a create there is refused");
		const auto other = outside[static_cast<std::size_t>(restart)];
		host->create_group(other);
		host->driver(0).stage(j::TurnRecord{other, 1, j::TurnRecordKind::Checkpoint, "born"});
		idle::commit(*host);
		check(host->status(other) == j::GroupStatus::stored, "missing_shard: the slot's other shards take a create");
	}
}

namespace idle {
// A slot's first commit past leaf_bytes, then a park that splits it, the power failing anywhere.
void first_split_run(fl::filesystem& disk, Model& model) {
	auto host = open(disk, 1);
	for (auto g : slot_ids(7, 1, 160)) { host->create_group(g); model.stage(*host, g, j::TurnRecordKind::Checkpoint, std::string(40, 'f') + std::to_string(g)); }
	commit(*host); model.acknowledge();
	park(*host);
	host.reset();
	check(shard_files(disk, 7) > 1, "the parked slot split");
}
}  // namespace idle

// Review F2's premise: only a stored node splits, so a power cut never leaves an unstored node's
// files short of covering it, and open may call such a node's paths lost.
void first_split_crash() {
	fl::filesystem base;
	idle::Model model;
	std::uint64_t cut = 1;
	while (!idle_crash_run(base, model, cut, idle::first_split_run)) { ++cut; }
	std::cout << "first_split_crash: " << cut - 1 << " power cuts, every one of them recovered\n";
}

// Review F3, the consumer contract (D2): Host::park takes every idle group no consumer has open,
// held by no Store and sealed by no sealer, as strata's groups are once their index closes; a
// closed or parked group leaves opened_, so it costs the host no heap.
void park_unheld() {
	fl::filesystem disk;
	{
		auto host = idle::open(disk, 1);
		auto& turns = host->driver(0);
		for (std::uint64_t g = 1; g <= 10; ++g) { host->create_group(g); turns.stage(j::TurnRecord{g, 1, j::TurnRecordKind::Checkpoint, "p" + std::to_string(g)}); }
		idle::commit(*host);
		const int owner = 0;
		for (std::uint64_t g = 6; g <= 10; ++g) { turns.seal(g, [](std::uint64_t, bool) { return std::optional<j::TurnRecord>(); }, &owner); }
		const auto& opened = (*host).*steal(OpenedTag{});
		check(opened[0].size() == 10, "park_unheld: created groups are open");
		check(host->park(0) == 5, "park_unheld: the idle groups with no hold and no sealer park");
		for (std::uint64_t g = 1; g <= 10; ++g) { check(host->status(g) == (g <= 5 ? j::GroupStatus::closed : j::GroupStatus::stored), "park_unheld: a sealed group stays open: " + std::to_string(g)); }
		for (std::uint64_t g = 6; g <= 10; ++g) { host->close_group(g, 1, "c" + std::to_string(g)); }
		check(opened[0].empty(), "park_unheld: a parked or closed group leaves opened_");
	}
	auto host = idle::open(disk, 1);
	for (std::uint64_t g = 1; g <= 10; ++g) { check(host->status(g) == j::GroupStatus::closed && host->entry(g)->manifest == (g <= 5 ? "p" : "c") + std::to_string(g), "park_unheld: every group reads back closed: " + std::to_string(g)); }
}
