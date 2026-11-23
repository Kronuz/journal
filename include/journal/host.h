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
//   groups-<slot:3x>-<node:9x>      RJGT0001: the group table's entries under one node of a slot's
//                                   trie, at most leaf_bytes unless one entry; replaced whole
//   l<generation:16x>-<loop:4x>-turns-<segment:16x>   a loop's RFTW0001 segment
//   g<group:16x>-artifact-<incarnation:16x><serial:16x>   a group's artifact
//
// docs/design/host-layout.md has the layout and the re-partition protocol.

#pragma once

#include "turn_driver.h"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

namespace kronuz::journal {

struct HostOptions {
	std::uint32_t loops = 1;  // L, 1 to 4,096; another L than the stored one re-partitions at open
	TurnLogOptions turns{};
};

// A group's table entry. Serials [mark, high) of its incarnation are the only names its
// artifacts may have, so reclamation and admission probe them by name and never scan. A deleted
// entry (Host::delete_group) keeps its incarnation and floor and frees the id for the next one.
enum class GroupState : std::uint8_t { open = 1, closed = 2, lost = 3, deleted = 4 };
struct GroupEntry {
	std::uint64_t incarnation = 0;
	GroupState state = GroupState::open;
	std::uint64_t mark = 0, high = 0;
	// The checkpoint record the table holds once the group closed: its sequence and bytes. The
	// log's records of the group through `base` are superseded by it. A deleted entry, and the
	// incarnation created after it, keep `base` with no bytes: the deleted incarnation's floor.
	std::uint64_t base = 0;
	std::string manifest;
	std::uint64_t next = 0;  // in memory: the next serial, from a fresh block after every open
	// In memory: a failed close may have reached the disk, so nothing holds the group until the
	// host reopens and reads which state the disk kept.
	bool in_doubt = false;
	// An open group's loss bounded by damage to its log records (docs/mechanisms/lost-bound.md):
	// the highest sequence the damage could hold; 0 when none. Only a reader holding the group
	// through it continues the group; the log's records through it are superseded.
	std::uint64_t bound = 0;
	bool operator==(const GroupEntry& other) const {
		return incarnation == other.incarnation && state == other.state && mark == other.mark && high == other.high && base == other.base && manifest == other.manifest && bound == other.bound;
	}
};
// What a Store does with a group: create it (no entry, or one whose first turn never became
// durable), recover it (stored, or closed: recovery reopens it), or nothing (lost: it rejoins
// under a new id). Bounded: the group's log lost records through Host::bound(group); a reader
// holding the group through the bound recovers it from the log's records after it, and any other
// loses it (Host::lose).
enum class GroupStatus { none, stored, closed, lost, bounded };

namespace host_detail {
inline constexpr std::string_view layout_magic{"RJHL0001", 8}, table_magic{"RJGT0001", 8};
inline constexpr std::uint32_t slots = 4096;
inline constexpr std::uint64_t serial_block = 64;
inline constexpr std::size_t sweep_floor = 64;  // the range table's least sweep threshold
inline constexpr std::string_view layout_name = "layout";

inline constexpr std::size_t leaf_bytes = 4096;  // a shard past this splits, unless it holds one entry
inline constexpr unsigned max_depth = 32;

// A group's id mixed by splitmix64's finalizer: its top 12 bits are its slot, the next its path
// in the slot's trie of shards.
inline std::uint64_t mix(std::uint64_t group) noexcept {
	group ^= group >> 30; group *= 0xbf58476d1ce4e5b9ull; group ^= group >> 27; group *= 0x94d049bb133111ebull; group ^= group >> 31;
	return group;
}
// A group's slot: one of 4,096.
inline std::uint32_t slot(std::uint64_t group) noexcept { return static_cast<std::uint32_t>(mix(group) >> 52); }
// A slot's table is a binary trie whose leaves are its shard files: node 1 is the root, 2n and
// 2n + 1 are n's children, and a node's depth is its bit width less one.
inline unsigned depth(std::uint64_t node) noexcept { return static_cast<unsigned>(std::bit_width(node)) - 1; }
inline std::uint64_t node_at(std::uint64_t group, unsigned depth) noexcept {
	return std::uint64_t{1} << depth | (mix(group) >> (52 - depth) & ((std::uint64_t{1} << depth) - 1));
}
inline bool holds(std::uint64_t node, std::uint64_t group) noexcept { return node_at(group, depth(node)) == node; }
// Node `low` lies strictly under `node`.
inline bool below(std::uint64_t low, std::uint64_t node) noexcept {
	return depth(low) > depth(node) && low >> (depth(low) - depth(node)) == node;
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
// A shard is a slot and a node of its trie.
using Shard = std::pair<std::uint32_t, std::uint64_t>;
inline std::string shard_name(Shard shard) { return "groups-" + hex(shard.first, 3) + "-" + hex(shard.second, 9); }
inline std::optional<Shard> shard_of(std::string_view name) {
	if (name.size() != 20 || !name.starts_with("groups-") || name[10] != '-') { return std::nullopt; }
	auto slot = parse_hex(name.substr(7, 3)); auto node = parse_hex(name.substr(11));
	if (!slot || *slot >= slots || !node || !*node || depth(*node) > max_depth) { return std::nullopt; }
	return Shard{static_cast<std::uint32_t>(*slot), *node};
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
// mark, high, base, manifest length and bytes), then a CRC. A bounded entry's state byte carries
// bounded_state, and its bound follows its base. A deleted entry is state 4 with no manifest.
inline constexpr std::uint8_t bounded_state = 0x80;
inline std::size_t shard_bytes(const std::map<std::uint64_t, GroupEntry>& entries) {
	std::size_t bytes = 32;
	for (const auto& [_, entry] : entries) { bytes += 45 + (entry.bound ? 8 : 0) + entry.manifest.size(); }
	return bytes;
}
inline std::string encode_shard(const Identity& host, const std::map<std::uint64_t, GroupEntry>& entries) {
	std::string bytes(table_magic); bytes.append(host.data(), host.size()); put32(bytes, static_cast<std::uint32_t>(entries.size()));
	for (const auto& [id, entry] : entries) {
		put64(bytes, id); put64(bytes, entry.incarnation); bytes.push_back(static_cast<char>(static_cast<std::uint8_t>(entry.state) | (entry.bound ? bounded_state : 0)));
		put64(bytes, entry.mark); put64(bytes, entry.high); put64(bytes, entry.base);
		if (entry.bound) { put64(bytes, entry.bound); }
		put32(bytes, static_cast<std::uint32_t>(entry.manifest.size())); bytes += entry.manifest;
	}
	put32(bytes, crc32c(bytes));
	return bytes;
}
inline std::map<std::uint64_t, GroupEntry> decode_shard(std::string_view bytes, const Identity& host, Shard shard) {
	if (bytes.size() < 32 || crc32c(bytes.substr(0, bytes.size() - 4)) != detail::checksum_at_end(bytes)) { throw Corruption("damaged group table"); }
	if (bytes.substr(0, 8) != table_magic || bytes.substr(8, 16) != std::string_view(host.data(), host.size())) { throw Corruption("foreign group table"); }
	bytes.remove_prefix(24); bytes.remove_suffix(4);
	std::map<std::uint64_t, GroupEntry> entries;
	for (auto count = get32(bytes); count; --count) {
		const auto id = get64(bytes);
		GroupEntry entry; entry.incarnation = get64(bytes);
		if (bytes.empty()) { throw Corruption("truncated group entry"); }
		const auto byte = static_cast<std::uint8_t>(bytes[0]); bytes.remove_prefix(1);
		const auto state = static_cast<std::uint8_t>(byte & ~bounded_state);
		if (state < 1 || state > 4 || (byte & bounded_state && state != static_cast<std::uint8_t>(GroupState::open))) { throw Corruption("invalid group state"); }
		entry.state = static_cast<GroupState>(state);
		entry.mark = get64(bytes); entry.high = get64(bytes); entry.base = get64(bytes);
		if (byte & bounded_state) { entry.bound = get64(bytes); if (!entry.bound) { throw Corruption("invalid group bound"); } }
		const auto length = get32(bytes);
		if (length > bytes.size()) { throw Corruption("truncated group entry"); }
		entry.manifest.assign(bytes.substr(0, length)); bytes.remove_prefix(length);
		entry.next = entry.high;
		if (!id || host_detail::slot(id) != shard.first || !holds(shard.second, id) || entry.mark > entry.high || !entries.emplace(id, std::move(entry)).second) { throw Corruption("invalid group entry"); }
	}
	if (!bytes.empty()) { throw Corruption("trailing group table bytes"); }
	return entries;
}

// An entry only its shard file holds once committed: a closed or deleted one.
inline bool filed(const GroupEntry& entry) noexcept { return entry.state == GroupState::closed || entry.state == GroupState::deleted; }

// Splits a shard's entries by the next bit of their path until each part encodes in leaf_bytes,
// holds one entry, or sits at max_depth. Both children of a split are written, so a slot's
// shards always cover every path under it.
using Entries = std::map<std::uint64_t, GroupEntry>;
inline void partition(std::uint64_t node, Entries entries, std::vector<std::pair<std::uint64_t, Entries>>& parts) {
	if (entries.size() <= 1 || depth(node) == max_depth || shard_bytes(entries) <= leaf_bytes) { parts.emplace_back(node, std::move(entries)); return; }
	Entries left, right;
	for (auto& [id, entry] : entries) { (holds(2 * node, id) ? left : right).emplace(id, std::move(entry)); }
	partition(2 * node, std::move(left), parts);
	partition(2 * node + 1, std::move(right), parts);
}
// A node's first path, of 2^32, and the order that lists a node before the nodes under it.
inline std::uint64_t first_path(std::uint64_t node) noexcept { return (node ^ std::uint64_t{1} << depth(node)) << (max_depth - depth(node)); }
inline bool path_order(std::uint64_t a, std::uint64_t b) noexcept { return std::pair{first_path(a), depth(a)} < std::pair{first_path(b), depth(b)}; }
// `nodes`: a slot's stored nodes under `node`, in path_order. They cover it when it is stored
// or its children both are covered.
inline bool covered(std::span<const std::uint64_t> nodes, std::uint64_t node) {
	if (nodes.empty()) { return false; }
	if (nodes.front() == node) { return true; }
	const auto middle = std::partition_point(nodes.begin(), nodes.end(), [&](std::uint64_t low) { return first_path(low) < first_path(2 * node + 1); });
	const auto split = static_cast<std::size_t>(middle - nodes.begin());
	return covered(nodes.first(split), 2 * node) && covered(nodes.subspan(split), 2 * node + 1);
}
// Sorts a slot's stored nodes into those that stand, those an interrupted commit left (open()
// removes them), and the nodes no file covers (`missing`, also in `kept`). A node whose children
// are covered was split, and its removal is not yet durable; nodes under a stored node they do not
// cover are an unfinished split's, and the node stands. Only a stored node splits (Host::commit),
// so a path under no stored node and no cover lost its file: its node is missing, never guessed.
inline void resolve(std::span<const std::uint64_t> nodes, std::uint64_t node, std::vector<std::uint64_t>& kept, std::vector<std::uint64_t>& stale, std::vector<std::uint64_t>& missing) {
	const bool stored = !nodes.empty() && nodes.front() == node;
	const auto rest = stored ? nodes.subspan(1) : nodes;
	const auto middle = std::partition_point(rest.begin(), rest.end(), [&](std::uint64_t low) { return first_path(low) < first_path(2 * node + 1); });
	const auto split = static_cast<std::size_t>(middle - rest.begin());
	if (!rest.empty() && covered(rest.first(split), 2 * node) && covered(rest.subspan(split), 2 * node + 1)) {
		if (stored) { stale.push_back(node); }
		resolve(rest.first(split), 2 * node, kept, stale, missing);
		resolve(rest.subspan(split), 2 * node + 1, kept, stale, missing);
		return;
	}
	if (stored) { kept.push_back(node); stale.insert(stale.end(), rest.begin(), rest.end()); return; }
	if (rest.empty() || depth(node) == max_depth) { kept.push_back(node); missing.push_back(node); return; }
	resolve(rest.first(split), 2 * node, kept, stale, missing);
	resolve(rest.subspan(split), 2 * node + 1, kept, stale, missing);
}

// A group's serial range as its views' scans read it, on whichever thread runs the scan
// primitive; the host updates it on the loop's thread whenever the table entry changes. Live
// views of a resident group share one.
struct Range {
	std::mutex mutex;
	std::uint64_t incarnation = 0, mark = 0, high = 0;
};
// One loop's table of its groups' views' ranges. It drops dead ranges once it doubles since its
// last sweep: O(log ids) amortized.
struct Ranges {
	std::multimap<std::uint64_t, std::weak_ptr<Range>> table;
	std::size_t sweep_at = sweep_floor;  // the table's size that sweeps it: twice the live after a sweep
	std::thread::id owner;  // the loop's thread: the first to touch the table, or the last to adopt it; debug builds check it
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
	// Hands the loop's range table to the calling thread, for a caller that serializes the loop
	// itself (strata's turn mutex): that serialization orders this after the old owner's last table
	// call. Debug builds then check the loop's table calls against this thread.
	void adopt_loop(std::uint32_t loop) {
		[[maybe_unused]] auto& ranges = ranges_.at(loop);
#ifndef NDEBUG
		ranges.owner = std::this_thread::get_id();
#endif
	}
	// The group's table entry. Open and lost entries live in memory; a closed one only in its
	// shard file, read back here, so a closed id costs the live host nothing.
	std::optional<GroupEntry> entry(std::uint64_t group) const {
		if (const auto* found = resident(group)) { return *found; }
		return stored(group);
	}
	GroupStatus status(std::uint64_t group) const {
		std::optional<GroupEntry> closed;
		const auto* found = resident(group);
		if (!found) { closed = stored(group); found = closed ? &*closed : nullptr; }
		if (!found) { return GroupStatus::none; }
		if (found->state == GroupState::lost) { return GroupStatus::lost; }
		if (found->state == GroupState::closed) { return GroupStatus::closed; }
		if (found->state == GroupState::deleted) { return GroupStatus::none; }
		auto& turns = drivers_.at(loop(group));
		if (turns->lost(group)) { return GroupStatus::lost; }
		if (found->bound) { return GroupStatus::bounded; }
		return opened_[loop(group)].contains(group) || turns->has_group(group) || turns->holds(group) || !found->manifest.empty() ? GroupStatus::stored : GroupStatus::none;
	}
	// A bounded group's bound (GroupStatus::bounded), or 0.
	std::uint64_t bound(std::uint64_t group) const {
		const auto* found = resident(group);
		return found && found->state == GroupState::open ? found->bound : 0;
	}
	// A reader of a bounded group that does not hold it through its bound loses it: durably, with
	// its artifacts, as an open marks the ids recovery calls lost; the log forgets it. On its loop.
	void lose(std::uint64_t group) {
		if (!bound(group)) { throw std::logic_error("only a bounded group is lost by its reader"); }
		mark_lost({group});
		auto& turns = driver_of(group);
		turns.forget(group, std::numeric_limits<std::uint64_t>::max());
		turns.log().refuse(group);
		opened_[loop(group)].erase(group);
	}
	// The group's view of the directory: its own names, and a scan that probes its serials. On the
	// group's loop, which owns the loop's range table (ranges_).
	std::shared_ptr<IO> group_io(std::uint64_t group) {
		if (!group) { throw std::invalid_argument("group 0 is reserved"); }
		auto& owned = owned_ranges(group);
		auto& ranges = owned.table;
		if (ranges.size() >= owned.sweep_at) {
			std::erase_if(ranges, [](const auto& item) { return item.second.expired(); });
			owned.sweep_at = std::max(2 * ranges.size(), host_detail::sweep_floor);
		}
		// A resident group's live views share one range; a closed or unborn group's view shares only
		// an unpublished one, so it never shows a closed incarnation's artifacts.
		const auto* found = resident(group);
		std::shared_ptr<host_detail::Range> range;
		for (auto [at, last] = ranges.equal_range(group); at != last;) {
			auto live = at->second.lock();
			if (!live) { at = ranges.erase(at); continue; }
			if (!range) {
				std::lock_guard hold(live->mutex);
				if (found || !live->incarnation) { range = live; }
			}
			++at;
		}
		if (!range) {
			range = std::make_shared<host_detail::Range>(); ranges.emplace(group, range);
			if (found) { publish(group, *found); }
		}
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
	void attach(std::uint64_t group) { opened_[loop(group)].insert(group); }

	// A Store's hold on its group (journal.h), from its create or recover until its close or the
	// end of its Journal, whichever comes first: two live Stores never hold one id. Busy
	// (EWOULDBLOCK, as a held owner lock is) while another Store holds the group or while records
	// its last one staged are unsettled. EIO while a failed close leaves the group in doubt, until
	// the host reopens. On the group's loop; a hold ends on any thread.
	class Hold {
	public:
		Hold(const Hold&) = delete;
		Hold& operator=(const Hold&) = delete;
		~Hold() { host_.unhold(group_); }
	private:
		friend class Host;
		Hold(Host& host, std::uint64_t group) : host_(host), group_(group) {}
		Host& host_;
		std::uint64_t group_;
	};
	std::unique_ptr<Hold> hold(std::uint64_t group) {
		if (const auto* found = resident(group); found && found->in_doubt) {
			throw std::system_error(EIO, std::generic_category(), "the group's failed close may be on disk: it holds again once the host reopens");
		}
		const auto at = loop(group);
		settle(at);
		const bool staged = driver(at).staged(group);
		std::lock_guard lock(holds_mutex_);
		if (staged || holds_.contains(group)) {
			throw std::system_error(EWOULDBLOCK, std::generic_category(), "the group is held by a live Store or its last records are unsettled");
		}
		holds_.insert(group);
		return std::unique_ptr<Hold>(new Hold(*this, group));
	}
	bool holding(std::uint64_t group) const { std::lock_guard lock(holds_mutex_); return holds_.contains(group); }
	// Releases the loop's groups whose holds ended and whose records are settled: each leaves
	// memory but not the log (TurnDriver::evict), so retention keeps every durable record and
	// the next recover reads them back. On the loop; hold() runs it first.
	void settle(std::uint32_t at) {
		std::vector<std::uint64_t> ended;
		{ std::lock_guard lock(holds_mutex_); ended.swap(released_[at]); }
		std::vector<std::uint64_t> unsettled;
		auto& turns = driver(at);
		for (auto group : ended) {
			if (holding(group)) { continue; }
			if (turns.staged(group)) { unsettled.push_back(group); continue; }
			turns.evict(group); opened_[at].erase(group);
		}
		if (!unsettled.empty()) {
			std::lock_guard lock(holds_mutex_);
			released_[at].insert(released_[at].end(), unsettled.begin(), unsettled.end());
		}
	}
	// The shared Journal's table transitions (journal.h). Create and reopen change the entry in
	// memory and mark its slot: the slot commits before the next turn of the group's loop
	// (TurnDriver::before_turn), so each is durable before the turn carrying the group's first
	// record (docs/design/idle-groups.md D5). Close and park are durable when they return.
	// A new incarnation of the group: a group the table does not hold, one whose first turn never
	// became durable, or a deleted one; the earlier incarnation's artifacts go first. Its first
	// serial block is reserved by its first artifact (next_artifact), durably, so no artifact is
	// named by an undurable block. It keeps a deleted incarnation's floor (base): its records
	// follow that incarnation's in the log, from TurnDriver::retired on.
	void create_group(std::uint64_t group) {
		if (status(group) != GroupStatus::none) { throw std::logic_error("the group exists, is closed, or is lost"); }
		load(group);
		auto& entry = shards_[host_detail::slot(group)][group];
		if (remove_artifacts(group, entry)) { io_->sync_directory(); }
		entry = GroupEntry{entry.incarnation + 1, GroupState::open, 0, 0, entry.base, {}, 0};
		dirty_[loop(group)].insert(group);
		opened_[loop(group)].insert(group);
		publish(group, entry);  // its views name nothing until its first block
	}
	// A closed or parked group opens again; its table record is staged again by its holder.
	void reopen_group(std::uint64_t group) {
		load(group);
		auto& entry = held(group, GroupState::closed);
		entry.state = GroupState::open;
		dirty_[loop(group)].insert(group);
		publish(group, entry);  // its views list its incarnation's artifacts, as the census counts them
	}
	// The table takes the group's last record, a checkpoint covering every batch; once that is
	// durable the log forgets the group, so retention reclaims its records. As TurnLog's
	// checkpoint guard does, it refuses a sequence below the group's last acknowledged record,
	// in the log or the table, whose records the forget would drop. The forget fails the
	// group's durable() waiters with group_closed (TurnDriver::forget). A failed commit leaves the
	// group open in memory and in later commits of its slot, in doubt (GroupEntry::in_doubt).
	void close_group(std::uint64_t group, std::uint64_t sequence, std::string manifest) {
		const auto base = held(group, GroupState::open).base;
		auto& turns = driver_of(group);
		if (turns.staged(group)) { throw std::logic_error("the group has records staged or in flight"); }
		if (sequence < std::max(base, turns.sequence(group))) {
			throw std::logic_error("a close behind the group's last acknowledged record");
		}
		std::vector<TurnRecord> closing{{group, sequence, TurnRecordKind::Checkpoint, std::move(manifest)}};
		close_into_table(closing);
	}
	// Deletes a closed group no Store holds, so its next create_group starts it empty
	// (docs/mechanisms/host-delete.md). Its entry commits deleted first, without the checkpoint
	// record and keeping the floor that supersedes its records in the log; then its artifacts go
	// and a second commit moves its mark past them. A power cut keeps the whole closed group or
	// none of it: an open finishes a recorded delete. Refused with std::logic_error unless the
	// group is closed, and with std::system_error(EWOULDBLOCK) while a hold of it is live; the
	// first commit's failure leaves it closed, a later one deleted, whose rest the next open or
	// create removes. Synchronous, on the group's loop, right after close_group with no handoff.
	void delete_group(std::uint64_t group) {
		if (status(group) != GroupStatus::closed) { throw std::logic_error("only a closed group is deleted"); }
		if (holding(group)) { throw std::system_error(EWOULDBLOCK, std::generic_category(), "a Store holds the group"); }
		load(group);
		auto deleted = shards_[host_detail::slot(group)].at(group);
		deleted.state = GroupState::deleted; deleted.manifest = {};
		update(group, [&](GroupEntry& entry) { entry = deleted; });
		if (deleted.mark == deleted.high) { return; }
		auto& entry = shards_[host_detail::slot(group)].emplace(group, std::move(deleted)).first->second;
		if (remove_artifacts(group, entry)) { io_->sync_directory(); }
		update(group, [](GroupEntry&) {});
	}
	// Parks the loop's idle groups (docs/design/idle-groups.md D2): every group no consumer has
	// open (no Store holds it, no sealer is registered), with nothing staged and one live record, a
	// checkpoint, closes into
	// the table as close_group does, with one commit for them all (each touched shard, one file
	// sync, one directory sync). On the loop's thread; mid-turn it skips the turn's groups (D10).
	// Returns how many parked; a failed commit leaves each of them in doubt, as a failed close does.
	std::size_t park(std::uint32_t at) {
		settle(at);
		auto& turns = driver(at);
		std::vector<TurnRecord> parking;
		for (auto group : turns.log().idle()) {
			if (holding(group) || turns.staged(group) || turns.sealed(group)) { continue; }
			const auto* found = resident(group);
			if (!found || found->state != GroupState::open || found->in_doubt) { continue; }
			auto records = turns.log().peek(group);
			parking.push_back(std::move(records.front()));
		}
		if (!parking.empty()) { close_into_table(parking); }
		return parking.size();
	}
	// Commits the loop's groups that a create or reopen changed: the hook every driver runs before
	// a turn with records.
	void commit_transitions(std::uint32_t at) {
		if (dirty_.empty() || dirty_[at].empty()) { return; }
		commit(std::set<std::uint64_t>(dirty_[at]));
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
	const GroupEntry* resident(std::uint64_t group) const {
		const auto& shard = shards_[host_detail::slot(group)];
		auto found = shard.find(group); return found == shard.end() ? nullptr : &found->second;
	}
	// The shard the group's entry falls in: the stored node on its path, or its slot's root before
	// the slot's first commit. A slot's nodes are prefix-free and in path order.
	host_detail::Shard shard_of(std::uint64_t group) const {
		const auto slot = host_detail::slot(group);
		const auto& nodes = nodes_[slot];
		const auto path = host_detail::mix(group) >> 20 & 0xffffffffu;
		auto at = std::upper_bound(nodes.begin(), nodes.end(), path, [](std::uint64_t p, std::uint64_t node) { return p < host_detail::first_path(node); });
		if (at != nodes.begin() && host_detail::holds(*std::prev(at), group)) { return {slot, *std::prev(at)}; }
		return {slot, 1};
	}
	bool stored(host_detail::Shard shard) const { return std::binary_search(nodes_[shard.first].begin(), nodes_[shard.first].end(), shard.second, host_detail::path_order); }
	bool damaged(host_detail::Shard shard) const { const auto& nodes = damaged_[shard.first]; return std::find(nodes.begin(), nodes.end(), shard.second) != nodes.end(); }
	// The shard's file as stored, or nothing before its first commit.
	host_detail::Entries stored_shard(host_detail::Shard shard) const {
		if (!stored(shard)) { return {}; }
		return host_detail::decode_shard(host_detail::read_file(*io_, host_detail::shard_name(shard), std::uint64_t{1} << 32), identity_, shard);
	}
	// A group of a damaged shard is lost: what the shard held of it is unknown.
	std::optional<GroupEntry> stored(std::uint64_t group) const {
		const auto shard = shard_of(group);
		if (damaged(shard)) { return GroupEntry{0, GroupState::lost, 0, 0, 0, {}, 0}; }
		auto entries = stored_shard(shard);
		auto found = entries.find(group);
		if (found == entries.end()) { return std::nullopt; }
		return std::move(found->second);
	}
	// retired() for the loops' drivers: a group the table closed retires through its close's
	// sequence + 1 (TurnDriver::forget keeps any floor past it), and so does the incarnation
	// reopened after it, until its own close. An unreadable or damaged shard retires every sequence.
	std::uint64_t retired_floor(std::uint64_t group) const noexcept {
		try {
			if (const auto* found = resident(group)) { return found->base ? found->base + 1 : 0; }
			if (damaged(shard_of(group))) { return std::numeric_limits<std::uint64_t>::max(); }
			const auto closed = stored(group);
			return closed && closed->base ? closed->base + 1 : 0;
		} catch (...) { return std::numeric_limits<std::uint64_t>::max(); }
	}
	void unhold(std::uint64_t group) noexcept {
		std::lock_guard lock(holds_mutex_);
		holds_.erase(group);
		try { released_[loop(group)].push_back(group); } catch (...) {}  // unsettled: its records stay indexed
	}
	// Brings the group's entry into memory when only its shard file holds it (closed or deleted).
	void load(std::uint64_t group) {
		if (resident(group)) { return; }
		if (auto filed = stored(group)) { shards_[host_detail::slot(group)].emplace(group, std::move(*filed)); }
	}
	GroupEntry& held(std::uint64_t group, GroupState state) {
		auto& shard = shards_[host_detail::slot(group)];
		auto found = shard.find(group);
		if (found == shard.end() || found->second.state != state || found->second.in_doubt) { throw std::logic_error("the group is not in the table state this needs"); }
		return found->second;
	}
	// Serials below the first one that still names a file are gone for good: serials are never
	// reused, and none is being created (a group prepares one artifact at a time).
	void settle_mark(std::uint64_t group, GroupEntry& entry) {
		while (entry.mark < entry.next && !io_->entry_footprint(host_detail::artifact_name(group, entry.incarnation, entry.mark))) { ++entry.mark; }
	}
	// Each record, a group's last and a checkpoint covering every batch, becomes its table entry's
	// base; the slots commit once, then the log forgets each group, and the closed entries and
	// opened_ leave memory. A failed commit restores each entry open, in doubt.
	void close_into_table(std::vector<TurnRecord>& records) {
		std::set<std::uint64_t> groups; std::set<std::uint32_t> slots; std::vector<GroupEntry> before; before.reserve(records.size());
		for (auto& record : records) {
			auto& entry = shards_[host_detail::slot(record.group)].at(record.group);
			before.push_back(entry);
			settle_mark(record.group, entry); entry.state = GroupState::closed; entry.base = record.sequence; entry.manifest = std::move(record.payload);
			entry.bound = 0;  // the closing checkpoint covers every batch, so the base supersedes it
			groups.insert(record.group); slots.insert(host_detail::slot(record.group));
		}
		try { commit(groups); }
		catch (...) {
			for (std::size_t i = 0; i < records.size(); ++i) {
				auto& entry = shards_[host_detail::slot(records[i].group)].at(records[i].group);
				entry = std::move(before[i]); entry.in_doubt = true;
			}
			throw;
		}
		for (const auto& record : records) { driver_of(record.group).forget(record.group, record.sequence + 1); opened_[loop(record.group)].erase(record.group); }
		for (auto slot : slots) { std::erase_if(shards_[slot], [](const auto& item) { return host_detail::filed(item.second); }); }
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
	// A restored closed entry stays in memory, since the file may hold it open, until a commit
	// of its slot writes it; a committed slot keeps no closed entry in memory.
	template <class Change> void update(std::uint64_t group, Change&& change) {
		auto& shard = shards_[host_detail::slot(group)];
		std::optional<GroupEntry> before;
		if (auto found = shard.find(group); found != shard.end()) { before = found->second; }
		auto& entry = shard[group];
		try { change(entry); commit({group}); }
		catch (...) {
			if (before) { entry = std::move(*before); } else { shard.erase(group); }
			throw;
		}
		std::erase_if(shard, [](const auto& item) { return host_detail::filed(item.second); });
	}
	// Replaces the shards the groups' entries fall in: writes each `.pending`, syncs them all with
	// one call, renames them, then syncs the directory once; the groups' views see their ranges. A
	// shard holds the open and lost entries in memory under its node and the closed ones only its
	// file holds; one past leaf_bytes splits, and the split node's file goes once its children are
	// durable. A damaged shard is never rewritten. A committed group is no longer dirty.
	void commit(const std::set<std::uint64_t>& groups) {
		std::set<host_detail::Shard> shards; std::set<std::uint32_t> slots;
		for (auto group : groups) { shards.insert(shard_of(group)); slots.insert(host_detail::slot(group)); }
		remove_orphans(slots);
		struct Write { host_detail::Shard shard; std::string bytes; };
		std::vector<Write> writes; std::vector<host_detail::Shard> split;
		for (auto shard : shards) {
			if (damaged(shard)) { throw Corruption("damaged group table shard: its groups are lost"); }
			auto entries = stored_shard(shard);
			const auto& resident = shards_[shard.first];
			std::erase_if(entries, [&](const auto& item) { return !host_detail::filed(item.second) || resident.contains(item.first); });
			for (const auto& [id, entry] : resident) { if (host_detail::holds(shard.second, id)) { entries.insert_or_assign(id, entry); } }
			std::vector<std::pair<std::uint64_t, host_detail::Entries>> parts;
			// A slot's first file is its root, whole: only a stored node splits (host_detail::resolve).
			if (stored(shard)) { host_detail::partition(shard.second, std::move(entries), parts); }
			else { parts.emplace_back(shard.second, std::move(entries)); }
			if (parts.size() > 1) { split.push_back(shard); }
			for (const auto& [node, part] : parts) { writes.push_back({{shard.first, node}, host_detail::encode_shard(identity_, part)}); }
		}
		std::vector<std::unique_ptr<File>> files; files.reserve(writes.size());
		for (const auto& write : writes) {
			const auto pending = host_detail::shard_name(write.shard) + ".pending";
			io_->remove(pending);
			files.push_back(io_->create_exclusive(pending));
			detail::write_all(*files.back(), 0, write.bytes);
		}
		std::vector<File*> syncing; syncing.reserve(files.size());
		for (auto& file : files) { syncing.push_back(file.get()); }
		io_->sync_files(syncing);
		files.clear();
		// A split's children renamed before a failure are orphans: the next commit removes them
		// before it renames, so the split node's later versions are never shadowed.
		for (const auto& write : writes) {
			if (!stored(write.shard)) { orphans_[write.shard.first].push_back(write.shard.second); }
			io_->replace(host_detail::shard_name(write.shard) + ".pending", host_detail::shard_name(write.shard));
		}
		io_->sync_directory();
		for (auto slot : slots) { orphans_[slot].clear(); }
		for (auto shard : split) {
			auto& nodes = nodes_[shard.first];
			if (auto at = std::lower_bound(nodes.begin(), nodes.end(), shard.second, host_detail::path_order); at != nodes.end() && *at == shard.second) {
				nodes.erase(at);
				// Durable with a later directory sync; until then open() drops it, its children covering it.
				try { io_->remove(host_detail::shard_name(shard)); } catch (...) {}
			}
		}
		for (const auto& write : writes) {
			auto& nodes = nodes_[write.shard.first];
			auto at = std::lower_bound(nodes.begin(), nodes.end(), write.shard.second, host_detail::path_order);
			if (at == nodes.end() || *at != write.shard.second) { nodes.insert(at, write.shard.second); }
		}
		auto committed = [&](std::uint64_t group) {
			const auto slot = host_detail::slot(group);
			for (auto at = shards.lower_bound({slot, 0}); at != shards.end() && at->first == slot; ++at) { if (host_detail::holds(at->second, group)) { return true; } }
			return false;
		};
		for (auto slot : slots) {
			if (!dirty_.empty()) { std::erase_if(dirty_[host_detail::loop(slot, loops_)], committed); }
			for (const auto& [id, entry] : shards_[slot]) { publish(id, entry); }
		}
	}
	// Files a failed commit renamed that no node holds: gone, durably, before any later rename in
	// their slots, so no node's later version is shadowed by them (host_detail::resolve).
	void remove_orphans(const std::set<std::uint32_t>& slots) {
		bool removed = false;
		for (auto slot : slots) {
			for (auto node : orphans_[slot]) { if (!stored({slot, node})) { io_->remove(host_detail::shard_name({slot, node})); removed = true; } }
		}
		if (removed) { io_->sync_directory(); }
		for (auto slot : slots) { orphans_[slot].clear(); }
	}
	void publish(std::uint64_t group, const GroupEntry& entry) {
		if (ranges_.empty()) { return; }  // the open's commits: no view exists yet
		for (auto [at, last] = owned_ranges(group).table.equal_range(group); at != last; ++at) {
			if (auto range = at->second.lock()) { std::lock_guard hold(range->mutex); range->incarnation = entry.incarnation; range->mark = entry.mark; range->high = entry.high; }
		}
	}
	// The group's loop's range table. Debug builds check that the calling thread is the loop's: the
	// first that touched the table once the host opened, or the last that adopted it (adopt_loop).
	host_detail::Ranges& owned_ranges(std::uint64_t group) {
		auto& ranges = ranges_[loop(group)];
#ifndef NDEBUG
		if (ranges.owner == std::thread::id{}) { ranges.owner = std::this_thread::get_id(); }
		assert(ranges.owner == std::this_thread::get_id() && "group_io and the table commits run on the group's loop; adopt_loop hands it over");
#endif
		return ranges;
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
				if (entry.base) { loop.base[id] = entry.base; }
				if (entry.state == GroupState::open) { loop.open.push_back(id); }
				if (entry.bound) { loop.bound[id] = entry.bound; }
			}
		}
		return groups;
	}
	// The directory's names, read once per open: layout, table and log names. Artifacts are
	// never listed; a group's are probed by name.
	struct Names { bool layout = false; std::set<std::string> pending; std::map<std::uint32_t, std::vector<std::uint64_t>> shards; };
	Names scan() {
		Names names; logs_.clear();
		auto cursor = io_->scan_directory();
		while (auto name = cursor->next()) {
			if (*name == host_detail::layout_name) { names.layout = true; }
			else if (*name == "layout.pending" || (name->ends_with(".pending") && host_detail::shard_of(name->substr(0, name->size() - 8)))) { names.pending.insert(*name); }
			else if (auto shard = host_detail::shard_of(*name)) { names.shards[shard->first].push_back(shard->second); }
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
		load_table(names);
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
		opened_.resize(loops_); released_.resize(loops_); dirty_.resize(loops_);
		auto groups = known(loops_);
		for (std::uint32_t loop = 0; loop < loops_; ++loop) {
			auto view = loop_io(generation_, loop);
			loop_views_.push_back(view);
			drivers_.push_back(std::make_unique<TurnDriver>(*view, identity_, options_.turns, groups[loop]));
			drivers_.back()->retire_with([this](std::uint64_t group) { return retired_floor(group); });
			drivers_.back()->before_turn([this, loop] { commit_transitions(loop); });
		}
		std::set<std::uint64_t> lost; std::map<std::uint64_t, std::uint64_t> bounded;
		for (const auto& turns : drivers_) {
			lost.insert(turns->recovered().lost.begin(), turns->recovered().lost.end());
			for (const auto& [id, found] : turns->recovered().bounded) { bounded[id] = found.bound; }
		}
		// A bounded group's table entry stores its bound before anything serves, so the bound
		// outlives the damage; the log then keeps its records after the bound, and it is not lost.
		for (auto id : mark_bounds(bounded)) { driver_of(id).keep(id); lost.erase(id); }
		mark_lost(lost);
		// The log's groups of a damaged shard are lost with it: their incarnation and serials, which
		// name their artifacts, are unknown.
		for (auto& turns : drivers_) {
			std::vector<std::uint64_t> orphaned;
			for (const auto& [id, _] : turns->recovered().groups) { if (damaged(shard_of(id))) { orphaned.push_back(id); } }
			for (auto id : orphaned) { turns->forget(id, std::numeric_limits<std::uint64_t>::max()); }
		}
		for (auto& shard : shards_) { std::erase_if(shard, [](const auto& item) { return host_detail::filed(item.second); }); }
		ranges_.resize(loops_);  // last: the open's commits publish to no table
	}
	// Reads each slot's stored shards: an interrupted commit's files go first (host_detail::resolve),
	// and a damaged or missing shard fails only its own groups, which are lost.
	void load_table(Names& names) {
		bool removed = false;
		for (auto& [slot, found] : names.shards) {
			std::sort(found.begin(), found.end(), host_detail::path_order);
			std::vector<std::uint64_t> kept, stale, missing;
			host_detail::resolve(found, 1, kept, stale, missing);
			for (auto node : stale) { io_->remove(host_detail::shard_name({slot, node})); removed = true; }
			damaged_[slot] = missing;
			for (auto node : kept) {
				if (std::find(missing.begin(), missing.end(), node) != missing.end()) { continue; }
				try {
					auto entries = host_detail::decode_shard(host_detail::read_file(*io_, host_detail::shard_name({slot, node}), std::uint64_t{1} << 32), identity_, {slot, node});
					shards_[slot].merge(entries);
				} catch (const Corruption&) { damaged_[slot].push_back(node); }
			}
			nodes_[slot] = std::move(kept);
		}
		if (removed) { io_->sync_directory(); }
	}
	// Stores each bounded group's bound in its open entry, durably, and clears the bound of an
	// open group whose log kept a checkpoint after it (which supersedes what the damage held).
	// Returns the bounded ids stored; any other (no open entry, or a damaged shard) stays lost.
	std::set<std::uint64_t> mark_bounds(const std::map<std::uint64_t, std::uint64_t>& bounded) {
		std::set<std::uint64_t> stored, changed;
		for (const auto& [id, at] : bounded) {
			if (damaged(shard_of(id))) { continue; }
			auto& shard = shards_[host_detail::slot(id)];
			auto found = shard.find(id);
			if (found == shard.end() || found->second.state != GroupState::open) { continue; }
			if (found->second.bound != at) { found->second.bound = at; changed.insert(id); }
			stored.insert(id);
		}
		for (std::uint32_t slot = 0; slot < host_detail::slots && !drivers_.empty(); ++slot) {
			for (auto& [id, entry] : shards_[slot]) {
				if (!entry.bound || bounded.contains(id) || entry.state != GroupState::open) { continue; }
				const auto& records = driver_of(id).records(id);
				if (!records.empty() && records.front().kind == TurnRecordKind::Checkpoint && records.front().sequence > entry.bound) { entry.bound = 0; changed.insert(id); }
			}
		}
		if (!changed.empty()) { commit(changed); }
		return stored;
	}
	// Every open marks the ids recovery calls lost, durably, before it serves; then their
	// artifacts go, and the entries keep the ids refused for good. A recorded delete's artifacts
	// go with them (delete_group).
	void mark_lost(const std::set<std::uint64_t>& ids) {
		std::set<std::uint64_t> changed;
		for (auto id : ids) {
			if (damaged(shard_of(id))) { continue; }  // lost with its shard, which is never rewritten
			auto& entry = shards_[host_detail::slot(id)][id];
			if (entry.state != GroupState::lost) { entry.state = GroupState::lost; entry.bound = 0; changed.insert(id); }
		}
		if (!changed.empty()) { commit(changed); }
		changed.clear(); bool removed = false;
		for (std::uint32_t slot = 0; slot < host_detail::slots; ++slot) {
			for (auto& [id, entry] : shards_[slot]) {
				if ((entry.state == GroupState::lost || entry.state == GroupState::deleted) && entry.mark < entry.high) { removed |= remove_artifacts(id, entry); changed.insert(id); }
			}
		}
		if (removed) { io_->sync_directory(); }
		if (!changed.empty()) { commit(changed); }
	}
	// L changed: (1) stream each log of the stored generation in turn into the next generation's
	// L_new logs: its groups' records join their new loop's turn, written and synced once it holds
	// a quarter segment, so memory holds one old log's live records and L_new turns; (2) mark the
	// ids recovery calls lost, durably; (3) commit by replacing the layout, then remove the old
	// logs. Before (3) the old generation is authoritative and the next open starts again. A group
	// lives in one old log and reads back in sequence order, so none is lost or reordered.
	void repartition() {
		std::set<std::uint64_t> lost;
		auto groups = known(loops_);
		const auto next = generation_ + 1;
		// A turn never crosses a segment: a quarter of one, counting each record's framing.
		const auto budget = options_.turns.segment_bytes / 4;
		struct Target { std::shared_ptr<PrefixIO> view; std::unique_ptr<TurnLog> log; std::vector<TurnRecord> turn; std::uint64_t bytes = 0; };
		std::vector<Target> targets(options_.loops);
		for (std::uint32_t loop = 0; loop < options_.loops; ++loop) {
			targets[loop].view = std::make_shared<PrefixIO>(io_, host_detail::log_prefix(next, loop), [] {
				return std::make_unique<host_detail::NameCursor>([] { return std::optional<std::string>(); });
			});
			TurnRecovery fresh; targets[loop].log = std::make_unique<TurnLog>(*targets[loop].view, identity_, fresh, options_.turns);
		}
		auto flush = [](Target& target) { if (!target.turn.empty()) { target.log->append(target.turn); target.turn.clear(); target.bytes = 0; } };
		for (std::uint32_t loop = 0; loop < loops_; ++loop) {
			auto view = loop_io(generation_, loop);
			auto recovered = TurnLog::read(*view, identity_, options_.turns, groups[loop]);
			lost.insert(recovered.lost.begin(), recovered.lost.end());
			auto carry = [&](std::uint64_t id, std::vector<TurnRecord>& kept) {
				auto& target = targets[host_detail::loop(host_detail::slot(id), options_.loops)];
				for (auto& record : kept) {
					const auto size = record.payload.size() + 64;
					if (!target.turn.empty() && target.bytes + size > budget) { flush(target); }
					target.bytes += size; target.turn.push_back(std::move(record));
				}
			};
			for (auto& [id, kept] : recovered.groups) {
				// A group the table bounds, with no checkpoint since, continues in the new log after its bound.
				if (auto found = groups[loop].bound.find(id); found != groups[loop].bound.end() && kept.front().kind != TurnRecordKind::Checkpoint) { targets[host_detail::loop(host_detail::slot(id), options_.loops)].log->keep(id, found->second); }
				carry(id, kept);
			}
			// A bounded group's records after its bound, which its stored bound lets the new log start.
			for (auto& [id, found] : recovered.bounded) {
				if (mark_bounds({{id, found.bound}}).empty()) { continue; }
				lost.erase(id);
				targets[host_detail::loop(host_detail::slot(id), options_.loops)].log->keep(id, found.bound);
				carry(id, found.records);
			}
		}
		for (auto& target : targets) { flush(target); }
		targets.clear();
		mark_lost(lost);
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
	std::vector<std::map<std::uint64_t, GroupEntry>> shards_ = std::vector<std::map<std::uint64_t, GroupEntry>>(host_detail::slots);  // open and lost entries, and closed ones a failed commit restored
	// Per slot, each touched only by its loop: its stored shards' nodes, prefix-free and in path
	// order; the damaged ones among them, whose groups are lost; and a failed commit's orphans.
	std::vector<std::vector<std::uint64_t>> nodes_ = std::vector<std::vector<std::uint64_t>>(host_detail::slots);
	std::vector<std::vector<std::uint64_t>> damaged_ = std::vector<std::vector<std::uint64_t>>(host_detail::slots);
	std::vector<std::vector<std::uint64_t>> orphans_ = std::vector<std::vector<std::uint64_t>>(host_detail::slots);
	std::map<std::pair<std::uint64_t, std::uint32_t>, std::vector<std::string>> logs_;
	std::vector<std::shared_ptr<IO>> loop_views_;
	std::vector<std::set<std::uint64_t>> opened_;  // per loop: groups a consumer created or recovered, until released or closed
	mutable std::mutex holds_mutex_;
	std::set<std::uint64_t> holds_;  // groups a live Store holds
	std::vector<std::vector<std::uint64_t>> released_;  // per loop: groups whose holds ended, for settle()
	std::vector<std::set<std::uint64_t>> dirty_;  // per loop: groups a create or reopen changed, committed before its next turn
	// Per loop: its groups' views' ranges. Loop l's table belongs to loop l's thread, the one that
	// runs group_io and the table commits (publish) of l's groups, so no lock guards it; one shared
	// table would race across loops. adopt_loop moves it to another thread. docs/mechanisms/host-ranges.md
	// has the rule.
	std::vector<host_detail::Ranges> ranges_;
	std::vector<std::unique_ptr<TurnDriver>> drivers_;
};

}  // namespace kronuz::journal
