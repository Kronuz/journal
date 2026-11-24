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

// Shared by host_test.cc and host_idle_test.cc: the host's recording IO, a node (host, stores and
// the state recovery gave each group) and the checks both files use. The helpers sit in an
// anonymous namespace, one copy per file; the check counters are the one shared state.
#pragma once
#include "completion/posix.h"
#include "faultlab/faultfs.h"
#include "journal/store.h"
#include <cerrno>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <iostream>
#include <set>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace j = kronuz::journal;
namespace fl = kronuz::faultlab;
namespace hd = kronuz::journal::host_detail;

// Reaches Host::ranges_ with no change to Host: explicit instantiation ignores access.
template <class Tag, auto Member> struct Steal { friend constexpr auto steal(Tag) { return Member; } };

extern int checks, failures;  // host_test.cc counts every file's checks

namespace {
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

// The comma-joined batches a test checkpoint names, and back.
std::string join(const std::vector<std::string>& batches) {
	std::string text;
	for (const auto& batch : batches) { text += (text.empty() ? "" : ",") + batch; }
	return text;
}
}  // namespace

// The idle-group cases (host_idle_test.cc), run by host_test.cc's main.
void idle_crash();
void idle_damage();
void split_crash();
void table_damage();
void park_mid_turn();
void split_stale_node();
void idle_unmanifested();
void split_orphan();
void missing_shard();
void first_split_crash();
void park_unheld();
