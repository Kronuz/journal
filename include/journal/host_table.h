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

// The host's group table: its options and entries, and host_detail's layout and shard codecs, slot
// trie, and the range tables of a loop's group views.
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

}  // namespace kronuz::journal
