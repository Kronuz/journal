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

// One directory per host (Detent async-drivers Q3 (b)): every group of the host and every loop's
// turn log in one flat directory under one owner lock, the directory's own lock, which strata's
// cold tier takes too. A group holds a table entry and no descriptor. Names:
//
//   layout                          RJHL0001: L and the log generation; replaced whole
//   groups-<slot:3x>                RJGT0001: the group table's entries of one slot; replaced whole
//   l<generation:16x>-<loop:4x>-turns-<segment:16x>   a loop's RFTW0001 segment
//   g<group:16x>-artifact-<incarnation:16x><serial:16x>   a group's artifact
//
// docs/design/host-layout.md has the layout and the re-partition protocol.

#pragma once

#include "turn_driver.h"
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace kronuz::journal {

struct HostOptions {
	std::uint32_t loops = 1;  // L, 1 to 4,096; another L than the stored one re-partitions at open
	TurnLogOptions turns{};
};

// A group's table entry. Serials [mark, high) of its incarnation are the only names its
// artifacts may have, so reclamation and admission probe them by name and never scan.
enum class GroupState : std::uint8_t { open = 1, closed = 2, lost = 3 };
struct GroupEntry {
	std::uint64_t incarnation = 0;
	GroupState state = GroupState::open;
	std::uint64_t mark = 0, high = 0;
	// The checkpoint record the table holds once the group closed: its sequence and bytes. The
	// log's records of the group through `base` are superseded by it.
	std::uint64_t base = 0;
	std::string manifest;
	std::uint64_t next = 0;  // in memory: the next serial, from a fresh block after every open
	bool operator==(const GroupEntry& other) const {
		return incarnation == other.incarnation && state == other.state && mark == other.mark && high == other.high && base == other.base && manifest == other.manifest;
	}
};
// What a Store does with a group: create it (no entry, or one whose first turn never became
// durable), recover it (stored, or closed: recovery reopens it), or nothing (lost: it rejoins
// under a new id).
enum class GroupStatus { none, stored, closed, lost };

namespace host_detail {
inline constexpr std::string_view layout_magic{"RJHL0001", 8}, table_magic{"RJGT0001", 8};
inline constexpr std::uint32_t slots = 4096;
inline constexpr std::uint64_t serial_block = 64;
inline constexpr std::string_view layout_name = "layout";

// A group's slot: its id mixed (splitmix64's finalizer) into 4,096 slots.
inline std::uint32_t slot(std::uint64_t group) noexcept {
	group ^= group >> 30; group *= 0xbf58476d1ce4e5b9ull; group ^= group >> 27; group *= 0x94d049bb133111ebull; group ^= group >> 31;
	return static_cast<std::uint32_t>(group >> 52);
}
// Each of L loops owns a contiguous range of floor or ceil(4,096 / L) slots.
inline std::uint32_t loop(std::uint32_t slot, std::uint32_t loops) noexcept { return static_cast<std::uint32_t>((std::uint64_t{slot} * loops) >> 12); }

inline std::string hex(std::uint64_t value, int digits) {
	std::string text(static_cast<std::size_t>(digits), '0'); constexpr char symbols[] = "0123456789abcdef";
	for (int i = digits - 1; i >= 0; --i, value >>= 4) { text[static_cast<std::size_t>(i)] = symbols[value & 15]; }
	return text;
}
inline std::optional<std::uint64_t> parse_hex(std::string_view text) {
	if (text.empty() || text.size() > 16) { return std::nullopt; }
	std::uint64_t value = 0;
	for (char c : text) {
		int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
		if (digit < 0) { return std::nullopt; }
		value = value << 4 | static_cast<unsigned>(digit);
	}
	return value;
}
inline std::string log_prefix(std::uint64_t generation, std::uint32_t loop) { return "l" + hex(generation, 16) + "-" + hex(loop, 4) + "-"; }
// A log segment's (generation, loop) and its name inside the loop, from its name in the host.
struct LogName { std::uint64_t generation; std::uint32_t loop; std::string inner; };
inline std::optional<LogName> log_name(std::string_view name) {
	if (name.size() < 23 || name[0] != 'l' || name[17] != '-' || name[22] != '-') { return std::nullopt; }
	auto generation = parse_hex(name.substr(1, 16)); auto loop = parse_hex(name.substr(18, 4));
	if (!generation || !loop || name.substr(1, 16).size() != 16) { return std::nullopt; }
	return LogName{*generation, static_cast<std::uint32_t>(*loop), std::string(name.substr(23))};
}
inline std::string group_prefix(std::uint64_t group) { return "g" + hex(group, 16) + "-"; }
// An artifact's identity is its name: the group's incarnation, then its serial, big-endian.
inline Identity artifact_identity(std::uint64_t incarnation, std::uint64_t serial) {
	Identity identity{};
	for (unsigned i = 0; i < 8; ++i) {
		identity[i] = static_cast<char>(incarnation >> (56 - 8 * i)); identity[8 + i] = static_cast<char>(serial >> (56 - 8 * i));
	}
	return identity;
}
inline std::string artifact_name(std::uint64_t group, std::uint64_t incarnation, std::uint64_t serial) {
	return group_prefix(group) + "artifact-" + hex(incarnation, 16) + hex(serial, 16);
}
inline std::string shard_name(std::uint32_t slot) { return "groups-" + hex(slot, 3); }
inline std::optional<std::uint32_t> shard_slot(std::string_view name) {
	if (name.size() != 10 || !name.starts_with("groups-")) { return std::nullopt; }
	auto slot = parse_hex(name.substr(7));
	return slot && *slot < slots ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(*slot)) : std::nullopt;
}

inline std::string read_file(IO& io, std::string_view name, std::uint64_t maximum) {
	auto file = io.open_existing(name);
	const auto size = file->size();
	if (size > maximum) { throw Corruption("host metadata exceeds its bound"); }
	std::string bytes(static_cast<std::size_t>(size), '\0');
	detail::read_all(*file, 0, bytes);
	return bytes;
}
// Writes `name.pending`, syncs it and renames it over `name`; the caller syncs the directory.
inline void replace_file(IO& io, const std::string& name, std::string_view bytes) {
	const auto pending = name + ".pending";
	io.remove(pending);
	auto file = io.create_exclusive(pending);
	detail::write_all(*file, 0, bytes); detail::sync_file(io, *file); file.reset();
	io.replace(pending, name);
}

// The layout record: magic, the host's identity, L and the log generation, then a CRC.
inline std::string encode_layout(const Identity& host, std::uint32_t loops, std::uint64_t generation) {
	std::string bytes(layout_magic); bytes.append(host.data(), host.size());
	put32(bytes, loops); put64(bytes, generation); put32(bytes, crc32c(bytes));
	return bytes;
}
inline std::pair<std::uint32_t, std::uint64_t> decode_layout(std::string_view bytes, const Identity& host) {
	if (bytes.size() != 40 || crc32c(bytes.substr(0, 36)) != detail::checksum_at_end(bytes)) { throw Corruption("damaged host layout"); }
	if (bytes.substr(0, 8) != layout_magic || bytes.substr(8, 16) != std::string_view(host.data(), host.size())) { throw Corruption("foreign host layout"); }
	bytes.remove_prefix(24);
	const auto loops = get32(bytes); const auto generation = get64(bytes);
	if (!loops || loops > slots || !generation) { throw Corruption("invalid host layout"); }
	return {loops, generation};
}
// A shard: magic, the host's identity, the entry count, the entries (id, incarnation, state,
// mark, high, base, manifest length and bytes), then a CRC.
inline std::string encode_shard(const Identity& host, const std::map<std::uint64_t, GroupEntry>& entries) {
	std::string bytes(table_magic); bytes.append(host.data(), host.size()); put32(bytes, static_cast<std::uint32_t>(entries.size()));
	for (const auto& [id, entry] : entries) {
		put64(bytes, id); put64(bytes, entry.incarnation); bytes.push_back(static_cast<char>(entry.state));
		put64(bytes, entry.mark); put64(bytes, entry.high); put64(bytes, entry.base);
		put32(bytes, static_cast<std::uint32_t>(entry.manifest.size())); bytes += entry.manifest;
	}
	put32(bytes, crc32c(bytes));
	return bytes;
}
inline std::map<std::uint64_t, GroupEntry> decode_shard(std::string_view bytes, const Identity& host, std::uint32_t slot) {
	if (bytes.size() < 32 || crc32c(bytes.substr(0, bytes.size() - 4)) != detail::checksum_at_end(bytes)) { throw Corruption("damaged group table"); }
	if (bytes.substr(0, 8) != table_magic || bytes.substr(8, 16) != std::string_view(host.data(), host.size())) { throw Corruption("foreign group table"); }
	bytes.remove_prefix(24); bytes.remove_suffix(4);
	std::map<std::uint64_t, GroupEntry> entries;
	for (auto count = get32(bytes); count; --count) {
		const auto id = get64(bytes);
		GroupEntry entry; entry.incarnation = get64(bytes);
		if (bytes.empty()) { throw Corruption("truncated group entry"); }
		const auto state = static_cast<std::uint8_t>(bytes[0]); bytes.remove_prefix(1);
		if (state < 1 || state > 3) { throw Corruption("invalid group state"); }
		entry.state = static_cast<GroupState>(state);
		entry.mark = get64(bytes); entry.high = get64(bytes); entry.base = get64(bytes);
		const auto length = get32(bytes);
		if (length > bytes.size()) { throw Corruption("truncated group entry"); }
		entry.manifest.assign(bytes.substr(0, length)); bytes.remove_prefix(length);
		entry.next = entry.high;
		if (!id || host_detail::slot(id) != slot || entry.mark > entry.high || !entries.emplace(id, std::move(entry)).second) { throw Corruption("invalid group entry"); }
	}
	if (!bytes.empty()) { throw Corruption("trailing group table bytes"); }
	return entries;
}

// A group's serial range as its view's scan reads it, on whichever thread runs the scan
// primitive; the host updates it on the loop's thread whenever the table entry changes.
struct Range {
	std::mutex mutex;
	std::uint64_t incarnation = 0, mark = 0, high = 0;
};
// The names `scan` yields, in order.
class NameCursor final : public DirectoryCursor {
public:
	explicit NameCursor(std::function<std::optional<std::string>()> next) : next_(std::move(next)) {}
	std::optional<std::string> next() override { return next_(); }
private:
	std::function<std::optional<std::string>()> next_;
};
}  // namespace host_detail

// A view of the host's directory under a name prefix: a loop's turn log, or a group's
// artifacts. Names pass through with the prefix; scan_directory yields what `scan` lists; the
// owner lock is the host's, so the view's is a token that holds nothing.
class PrefixIO final : public IO {
public:
	using Scan = std::function<std::unique_ptr<DirectoryCursor>()>;
	PrefixIO(std::shared_ptr<IO> io, std::string prefix, Scan scan) : io_(std::move(io)), prefix_(std::move(prefix)), scan_(std::move(scan)) {}
	FilesystemClass directory_filesystem() const override { return io_->directory_filesystem(); }
	DirectoryTrust directory_trust() const override { return io_->directory_trust(); }
	std::unique_ptr<OwnerLock> acquire_owner(bool) override { return std::make_unique<OwnerLock>(); }
	void open_existing_read_only_into(std::string_view name, File& file) override { io_->open_existing_read_only_into(path(name), file); }
	bool open_reclaim_candidate_read_only_into(std::string_view name, File& file) override { return io_->open_reclaim_candidate_read_only_into(path(name), file); }
	std::unique_ptr<File> open_reclaim_candidate_read_only(std::string_view name) override { return io_->open_reclaim_candidate_read_only(path(name)); }
	std::unique_ptr<File> open_existing(std::string_view name) override { return io_->open_existing(path(name)); }
	std::unique_ptr<File> open_existing_read_only(std::string_view name) override { return io_->open_existing_read_only(path(name)); }
	std::unique_ptr<File> create_exclusive(std::string_view name) override { return io_->create_exclusive(path(name)); }
	void replace(std::string_view source, std::string_view destination) override { io_->replace(path(source), path(destination)); }
	void remove(std::string_view name) override { io_->remove(path(name)); }
	std::unique_ptr<DirectoryCursor> scan_directory() override { return scan_(); }
	std::optional<EntryFootprint> entry_footprint(std::string_view name) override { return io_->entry_footprint(path(name)); }
	std::optional<EntryInspection> inspect_entry(std::string_view name) override { return io_->inspect_entry(path(name)); }
	std::unique_ptr<File> open_reclaim_candidate(std::string_view name) override { return io_->open_reclaim_candidate(path(name)); }
	void sync_directory() override { io_->sync_directory(); }
	void sync_files(std::span<File* const> files) override { io_->sync_files(files); }
	void sync_data_files(std::span<File* const> files) override { io_->sync_data_files(files); }
	void sync_files_and_directory(std::span<File* const> files) override { io_->sync_files_and_directory(files); }
	void order_files_and_directory(std::span<File* const> files) override { io_->order_files_and_directory(files); }
private:
	std::string path(std::string_view name) const { return prefix_ + std::string(name); }
	std::shared_ptr<IO> io_;
	std::string prefix_;
	Scan scan_;
};

// The host: its directory's lock, layout and group table, and one turn driver per loop. Opening
// recovers every loop's log of the stored generation and marks the ids recovery calls lost in
// the table before anything else; another L re-partitions first. Stores (store.h) open on it
// and must not outlive it.
class Host {
public:
	Host(std::shared_ptr<IO> io, Identity identity, HostOptions options = {}) : io_(std::move(io)), identity_(identity), options_(options) {
		if (!io_) { throw std::invalid_argument("null host IO"); }
		if (!options_.loops || options_.loops > host_detail::slots) { throw std::invalid_argument("a host runs 1 to 4,096 loops"); }
		lock_ = io_->acquire_owner(OwnerLockOptions{OwnerLockMode::directory, OwnerFile::open_existing});
		open();
	}
	Host(const Host&) = delete;
	Host& operator=(const Host&) = delete;

	std::uint32_t loops() const noexcept { return loops_; }
	std::uint64_t generation() const noexcept { return generation_; }
	std::uint32_t loop(std::uint64_t group) const noexcept { return host_detail::loop(host_detail::slot(group), loops_); }
	TurnDriver& driver(std::uint32_t loop) { return *drivers_.at(loop); }
	TurnDriver& driver_of(std::uint64_t group) { return driver(loop(group)); }
	IO& io() noexcept { return *io_; }
	const GroupEntry* entry(std::uint64_t group) const {
		const auto& shard = shards_[host_detail::slot(group)];
		auto found = shard.find(group); return found == shard.end() ? nullptr : &found->second;
	}
	GroupStatus status(std::uint64_t group) const {
		const auto* found = entry(group);
		if (!found) { return GroupStatus::none; }
		if (found->state == GroupState::lost) { return GroupStatus::lost; }
		if (found->state == GroupState::closed) { return GroupStatus::closed; }
		auto& turns = drivers_.at(loop(group));
		if (turns->lost(group)) { return GroupStatus::lost; }
		return opened_.contains(group) || turns->has_group(group) || !found->manifest.empty() ? GroupStatus::stored : GroupStatus::none;
	}
	// The group's view of the directory: its own names, and a scan that probes its serials.
	std::shared_ptr<IO> group_io(std::uint64_t group) {
		if (!group) { throw std::invalid_argument("group 0 is reserved"); }
		std::erase_if(ranges_, [](const auto& item) { return item.second.expired(); });
		auto range = std::make_shared<host_detail::Range>();
		ranges_.emplace(group, range);
		if (const auto* found = entry(group)) { publish(group, *found); }
		return std::make_shared<PrefixIO>(io_, host_detail::group_prefix(group), [io = io_, group, range]() -> std::unique_ptr<DirectoryCursor> {
			std::uint64_t incarnation, serial, high;
			{ std::lock_guard hold(range->mutex); incarnation = range->incarnation; serial = range->mark; high = range->high; }
			return std::make_unique<host_detail::NameCursor>([io, group, incarnation, serial, high]() mutable -> std::optional<std::string> {
				for (; serial < high; ++serial) {
					if (io->entry_footprint(host_detail::artifact_name(group, incarnation, serial))) {
						return "artifact-" + host_detail::hex(incarnation, 16) + host_detail::hex(serial++, 16);
					}
				}
				return std::nullopt;
			});
		});
	}

	// A Store recovered the group: it is stored whatever its turn log still holds in memory.
	void attach(std::uint64_t group) { opened_.insert(group); }
	// The shared Journal's table transitions (journal.h); each is durable when it returns.
	// A new incarnation of the group, with its first serial block: a group the table does not
	// hold, or one whose first turn never became durable, whose artifacts go first.
	void create_group(std::uint64_t group) {
		if (status(group) != GroupStatus::none) { throw std::logic_error("the group exists, is closed, or is lost"); }
		update(group, [&](GroupEntry& entry) {
			if (remove_artifacts(group, entry)) { io_->sync_directory(); }
			entry = GroupEntry{entry.incarnation + 1, GroupState::open, 0, host_detail::serial_block, 0, {}, 0};
		});
		opened_.insert(group);
	}
	void reopen_group(std::uint64_t group) {
		held(group, GroupState::closed);
		update(group, [](GroupEntry& entry) { entry.state = GroupState::open; });
	}
	// The table takes the group's last record, a checkpoint covering every batch; once that is
	// durable the log forgets the group, so retention reclaims its records. As TurnLog's
	// checkpoint guard does, it refuses a sequence below the group's last acknowledged record,
	// in the log or the table, whose records the forget would drop. The forget fails the
	// group's durable() waiters with group_closed (TurnDriver::forget).
	void close_group(std::uint64_t group, std::uint64_t sequence, std::string manifest) {
		const auto base = held(group, GroupState::open).base;
		auto& turns = driver_of(group);
		if (turns.staged(group)) { throw std::logic_error("the group has records staged or in flight"); }
		if (sequence < std::max(base, turns.sequence(group))) {
			throw std::logic_error("a close behind the group's last acknowledged record");
		}
		update(group, [&](GroupEntry& entry) {
			settle_mark(group, entry); entry.state = GroupState::closed; entry.base = sequence; entry.manifest = std::move(manifest);
		});
		turns.forget(group);
	}
	// The group's next artifact identity; a used-up block reserves the next one in the table.
	Identity next_artifact(std::uint64_t group) {
		auto& entry = held(group, GroupState::open);
		if (entry.next == entry.high) {
			update(group, [&](GroupEntry& reserved) { settle_mark(group, reserved); reserved.high += host_detail::serial_block; });
		}
		return host_detail::artifact_identity(entry.incarnation, entry.next++);
	}

private:
	GroupEntry& held(std::uint64_t group, GroupState state) {
		auto& shard = shards_[host_detail::slot(group)];
		auto found = shard.find(group);
		if (found == shard.end() || found->second.state != state) { throw std::logic_error("the group is not in the table state this needs"); }
		return found->second;
	}
	// Serials below the first one that still names a file are gone for good: serials are never
	// reused, and none is being created (a group prepares one artifact at a time).
	void settle_mark(std::uint64_t group, GroupEntry& entry) {
		while (entry.mark < entry.next && !io_->entry_footprint(host_detail::artifact_name(group, entry.incarnation, entry.mark))) { ++entry.mark; }
	}
	bool remove_artifacts(std::uint64_t group, GroupEntry& entry) {
		bool removed = false;
		for (auto serial = entry.mark; serial < entry.high; ++serial) {
			const auto name = host_detail::artifact_name(group, entry.incarnation, serial);
			if (io_->entry_footprint(name)) { io_->remove(name); removed = true; }
		}
		entry.mark = entry.high;
		return removed;
	}
	// Changes the group's entry and commits its shard. A failed write restores the entry, so
	// the table in memory never runs ahead of the disk and nothing acts on an undurable change.
	template <class Change> void update(std::uint64_t group, Change&& change) {
		auto& shard = shards_[host_detail::slot(group)];
		std::optional<GroupEntry> before;
		if (auto found = shard.find(group); found != shard.end()) { before = found->second; }
		auto& entry = shard[group];
		try { change(entry); commit({host_detail::slot(group)}); }
		catch (...) { if (before) { entry = std::move(*before); } else { shard.erase(group); } throw; }
	}
	// Replaces the shards, then syncs the directory once; the groups' views see their ranges.
	void commit(const std::set<std::uint32_t>& slots) {
		for (auto slot : slots) { host_detail::replace_file(*io_, host_detail::shard_name(slot), host_detail::encode_shard(identity_, shards_[slot])); }
		io_->sync_directory();
		for (auto slot : slots) { for (const auto& [id, entry] : shards_[slot]) { publish(id, entry); } }
	}
	void publish(std::uint64_t group, const GroupEntry& entry) {
		auto [first, last] = ranges_.equal_range(group);
		for (; first != last; ++first) {
			if (auto range = first->second.lock()) { std::lock_guard hold(range->mutex); range->incarnation = entry.incarnation; range->mark = entry.mark; range->high = entry.high; }
		}
	}
	std::shared_ptr<IO> loop_io(std::uint64_t generation, std::uint32_t loop) {
		auto names = logs_[{generation, loop}];
		return std::make_shared<PrefixIO>(io_, host_detail::log_prefix(generation, loop), [names = std::move(names)]() -> std::unique_ptr<DirectoryCursor> {
			return std::make_unique<host_detail::NameCursor>([names, at = std::size_t{0}]() mutable -> std::optional<std::string> {
				if (at == names.size()) { return std::nullopt; }
				return names[at++];
			});
		});
	}
	// What the table tells each of L loops before it recovers.
	std::vector<TurnLogGroups> known(std::uint32_t loops) const {
		std::vector<TurnLogGroups> groups(loops);
		for (std::uint32_t slot = 0; slot < host_detail::slots; ++slot) {
			auto& loop = groups[host_detail::loop(slot, loops)];
			for (const auto& [id, entry] : shards_[slot]) {
				if (entry.state == GroupState::lost) { loop.lost.insert(id); continue; }
				if (!entry.manifest.empty()) { loop.base[id] = entry.base; }
				if (entry.state == GroupState::open) { loop.open.push_back(id); }
			}
		}
		return groups;
	}
	// The directory's names, read once per open: layout, table and log names. Artifacts are
	// never listed; a group's are probed by name.
	struct Names { bool layout = false; std::set<std::string> pending; std::set<std::uint32_t> shards; };
	Names scan() {
		Names names; logs_.clear();
		auto cursor = io_->scan_directory();
		while (auto name = cursor->next()) {
			if (*name == host_detail::layout_name) { names.layout = true; }
			else if (*name == "layout.pending" || (name->ends_with(".pending") && host_detail::shard_slot(name->substr(0, name->size() - 8)))) { names.pending.insert(*name); }
			else if (auto slot = host_detail::shard_slot(*name)) { names.shards.insert(*slot); }
			else if (auto log = host_detail::log_name(*name)) { logs_[{log->generation, log->loop}].push_back(log->inner); }
		}
		return names;
	}
	void open() {
		auto names = scan();
		for (const auto& name : names.pending) { io_->remove(name); }
		if (!names.pending.empty()) { io_->sync_directory(); }
		std::uint32_t stored = options_.loops;
		if (!names.layout) {
			if (!names.shards.empty() || !logs_.empty()) { throw Corruption("host directory without its layout"); }
			generation_ = 1;
			host_detail::replace_file(*io_, std::string(host_detail::layout_name), host_detail::encode_layout(identity_, stored, generation_));
			io_->sync_directory();
		} else {
			std::tie(stored, generation_) = host_detail::decode_layout(host_detail::read_file(*io_, host_detail::layout_name, 64), identity_);
		}
		for (auto slot : names.shards) { shards_[slot] = host_detail::decode_shard(host_detail::read_file(*io_, host_detail::shard_name(slot), std::uint64_t{1} << 32), identity_, slot); }
		// Logs of another generation are leftovers: an unfinished re-partition's, or the old
		// generation's that its commit had not yet removed.
		bool removed = false;
		for (auto it = logs_.begin(); it != logs_.end();) {
			if (it->first.first == generation_) { ++it; continue; }
			for (const auto& inner : it->second) { io_->remove(host_detail::log_prefix(it->first.first, it->first.second) + inner); }
			removed = true; it = logs_.erase(it);
		}
		if (removed) { io_->sync_directory(); }
		loops_ = stored;
		if (stored != options_.loops) { repartition(); }
		auto groups = known(loops_);
		for (std::uint32_t loop = 0; loop < loops_; ++loop) {
			auto view = loop_io(generation_, loop);
			loop_views_.push_back(view);
			drivers_.push_back(std::make_unique<TurnDriver>(*view, identity_, options_.turns, groups[loop]));
		}
		std::set<std::uint64_t> lost;
		for (const auto& turns : drivers_) { lost.insert(turns->recovered().lost.begin(), turns->recovered().lost.end()); }
		mark_lost(lost);
	}
	// Every open marks the ids recovery calls lost, durably, before it serves; then their
	// artifacts go, and the entries keep the ids refused for good.
	void mark_lost(const std::set<std::uint64_t>& ids) {
		std::set<std::uint32_t> changed;
		for (auto id : ids) {
			auto& entry = shards_[host_detail::slot(id)][id];
			if (entry.state != GroupState::lost) { entry.state = GroupState::lost; changed.insert(host_detail::slot(id)); }
		}
		if (!changed.empty()) { commit(changed); }
		changed.clear(); bool removed = false;
		for (std::uint32_t slot = 0; slot < host_detail::slots; ++slot) {
			for (auto& [id, entry] : shards_[slot]) {
				if (entry.state == GroupState::lost && entry.mark < entry.high) { removed |= remove_artifacts(id, entry); changed.insert(slot); }
			}
		}
		if (removed) { io_->sync_directory(); }
		if (!changed.empty()) { commit(changed); }
	}
	// L changed: (1) recover every log of the stored generation and mark its lost ids; (2)
	// write each group's live records into its new loop's log of the next generation, each log
	// synced; (3) commit by replacing the layout, then remove the old logs. Before (3) the old
	// generation is authoritative and the next open starts again.
	void repartition() {
		std::map<std::uint64_t, std::vector<TurnRecord>> records; std::set<std::uint64_t> lost;
		auto groups = known(loops_);
		for (std::uint32_t loop = 0; loop < loops_; ++loop) {
			auto view = loop_io(generation_, loop);
			auto recovered = TurnLog::read(*view, identity_, options_.turns, groups[loop]);
			lost.insert(recovered.lost.begin(), recovered.lost.end());
			for (auto& [id, kept] : recovered.groups) { if (!kept.empty()) { records[id] = std::move(kept); } }
		}
		mark_lost(lost);
		const auto next = generation_ + 1;
		// A turn never crosses a segment: a quarter of one, counting each record's framing.
		const auto budget = options_.turns.segment_bytes / 4;
		for (std::uint32_t loop = 0; loop < options_.loops; ++loop) {
			auto view = std::make_shared<PrefixIO>(io_, host_detail::log_prefix(next, loop), [] {
				return std::make_unique<host_detail::NameCursor>([] { return std::optional<std::string>(); });
			});
			TurnRecovery fresh; TurnLog log(*view, identity_, fresh, options_.turns);
			std::vector<TurnRecord> turn; std::uint64_t bytes = 0;
			for (const auto& [id, kept] : records) {
				if (host_detail::loop(host_detail::slot(id), options_.loops) != loop) { continue; }
				for (const auto& record : kept) {
					const auto size = record.payload.size() + 64;
					if (!turn.empty() && bytes + size > budget) { log.append(turn); turn.clear(); bytes = 0; }
					turn.push_back(record); bytes += size;
				}
			}
			if (!turn.empty()) { log.append(turn); }
		}
		host_detail::replace_file(*io_, std::string(host_detail::layout_name), host_detail::encode_layout(identity_, options_.loops, next));
		io_->sync_directory();
		const auto old = generation_;
		generation_ = next; loops_ = options_.loops;
		scan();
		for (auto it = logs_.begin(); it != logs_.end();) {
			if (it->first.first != old) { ++it; continue; }
			for (const auto& inner : it->second) { io_->remove(host_detail::log_prefix(old, it->first.second) + inner); }
			it = logs_.erase(it);
		}
		io_->sync_directory();
	}

	std::shared_ptr<IO> io_;
	Identity identity_;
	HostOptions options_;
	std::unique_ptr<OwnerLock> lock_;  // released after the drivers' files close
	std::uint32_t loops_ = 0;
	std::uint64_t generation_ = 0;
	std::vector<std::map<std::uint64_t, GroupEntry>> shards_ = std::vector<std::map<std::uint64_t, GroupEntry>>(host_detail::slots);
	std::map<std::pair<std::uint64_t, std::uint32_t>, std::vector<std::string>> logs_;
	std::vector<std::shared_ptr<IO>> loop_views_;
	std::set<std::uint64_t> opened_;  // groups a Store created or recovered this open
	std::multimap<std::uint64_t, std::weak_ptr<host_detail::Range>> ranges_;  // the groups' views
	std::vector<std::unique_ptr<TurnDriver>> drivers_;
};

}  // namespace kronuz::journal
