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

// Shared by the journal_test files: the memory-disk Journal model (MemoryIO, Model), the helpers that
// build and replay on it, and the check counters. The helpers sit in an anonymous namespace, one
// copy per file; the counters are the one shared state.
#pragma once
#include "journal/journal.h"
#include "completion/posix.h"
#include "journal/store.h"
#include <set>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <vector>
#include "faultlab/faultfs.h"
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <random>
#include <thread>

using namespace kronuz::journal;

extern int failures, checks;  // journal_test.cc counts every file's checks

namespace {
void check(bool result, std::string_view message) {
	++checks;
	if (!result) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
template <class Function> bool throws(Function&& function) {
	try { function(); return false; } catch (const std::exception&) { return true; }
}

struct Inode { std::string visible, durable; };
struct Model {
	using Names = std::map<std::string, std::shared_ptr<Inode>>;
	Names visible, durable;
	bool locked = false;
	std::size_t operations = 0, fail_operation = 0;
	bool fail_after = false, zero_write = false;
	std::function<void()> observe;
	std::function<void(const Inode*, std::uint64_t, std::size_t)> observe_read;
	bool oversized_read = false;
	std::size_t chunk = std::numeric_limits<std::size_t>::max();
	void before() {
		++operations;
		if (operations == fail_operation && !fail_after) { throw std::runtime_error("injected I/O failure before effect"); }
	}
	void after() {
		if (observe) { observe(); }
		if (operations == fail_operation && fail_after) { throw std::runtime_error("injected I/O failure after effect"); }
	}
	Model clone() const {
		Model copy;
		copy.chunk = chunk;
		std::map<const Inode*, std::shared_ptr<Inode>> nodes;
		for (const auto* names : {&visible, &durable}) {
			auto& destination = names == &visible ? copy.visible : copy.durable;
			for (const auto& [name, inode] : *names) {
				auto& fresh = nodes[inode.get()];
				if (!fresh) { fresh = std::make_shared<Inode>(*inode); }
				destination.emplace(name, fresh);
			}
		}
		return copy;
	}
	void power_loss(bool keep_visible_namespace) {
		if (!keep_visible_namespace) { visible = durable; }
		for (auto& [name, inode] : visible) { inode->visible = inode->durable; }
		durable = visible;
		locked = false;
		operations = fail_operation = 0;
		zero_write = false;
	}
};

class MemoryFile final : public File {
public:
	MemoryFile(Model& model, std::shared_ptr<Inode> inode) : model_(model), inode_(std::move(inode)) {}
	std::uint64_t size() override { model_.before(); auto size = inode_->visible.size(); model_.after(); return size; }
	std::size_t read_at(std::uint64_t offset, std::span<char> bytes) override {
		if (model_.observe_read) { model_.observe_read(inode_.get(), offset, bytes.size()); }
		model_.before();
		auto available = offset >= inode_->visible.size() ? 0 : inode_->visible.size() - static_cast<std::size_t>(offset);
		auto length = std::min({bytes.size(), available, model_.chunk});
		if (length) { std::copy_n(inode_->visible.data() + offset, length, bytes.data()); }
		model_.after(); return model_.oversized_read ? bytes.size() + 1 : length;
	}
	std::size_t write_at(std::uint64_t offset, std::string_view bytes) override {
		model_.before();
		auto length = model_.zero_write ? 0 : std::min(bytes.size(), model_.chunk);
		if (length) {
			inode_->visible.resize(std::max(inode_->visible.size(), static_cast<std::size_t>(offset) + length));
			std::copy_n(bytes.data(), length, inode_->visible.data() + offset);
		}
		model_.after(); return length;
	}
	void truncate(std::uint64_t size) override { model_.before(); inode_->visible.resize(size); model_.after(); }
	void sync() override { model_.before(); inode_->durable = inode_->visible; model_.after(); }
private:
	Model& model_;
	std::shared_ptr<Inode> inode_;
};

class MemoryIO final : public IO {
public:
	explicit MemoryIO(Model& model) : model_(model) {}
	std::unique_ptr<OwnerLock> acquire_owner(bool create) override {
		model_.before();
		if (model_.locked) { throw std::runtime_error("second owner"); }
		if (create) {
			if (model_.visible.contains("owner.lock")) { throw std::runtime_error("already initialized"); }
			model_.visible["owner.lock"] = std::make_shared<Inode>();
		} else if (!model_.visible.contains("owner.lock")) { throw std::runtime_error("missing owner lock"); }
		model_.locked = true;
		auto owner = std::make_unique<Lock>(model_);
		model_.after(); return owner;
	}
	std::unique_ptr<File> open_existing(std::string_view name) override {
		model_.before();
		auto found = model_.visible.find(std::string(name));
		if (found == model_.visible.end()) { throw std::runtime_error("missing file"); }
		auto file = std::make_unique<MemoryFile>(model_, found->second);
		model_.after(); return file;
	}
	std::unique_ptr<File> create_exclusive(std::string_view name) override {
		model_.before();
		auto [found, inserted] = model_.visible.emplace(std::string(name), std::make_shared<Inode>());
		if (!inserted) { throw std::runtime_error("existing file"); }
		auto file = std::make_unique<MemoryFile>(model_, found->second);
		model_.after(); return file;
	}
	void replace(std::string_view source, std::string_view destination) override {
		model_.before();
		auto node = model_.visible.extract(std::string(source));
		if (node.empty()) { throw std::runtime_error("missing replacement"); }
		model_.visible[std::string(destination)] = std::move(node.mapped());
		model_.after();
	}
	void remove(std::string_view name) override { model_.before(); model_.visible.erase(std::string(name)); model_.after(); }
	std::unique_ptr<DirectoryCursor> scan_directory() override {
		model_.before(); auto cursor = std::make_unique<Cursor>(model_); model_.after(); return cursor;
	}
	std::unique_ptr<File> open_reclaim_candidate(std::string_view name) override {
		model_.before(); auto found = model_.visible.find(std::string(name));
		auto file = found == model_.visible.end() ? nullptr : std::make_unique<MemoryFile>(model_, found->second);
		model_.after(); return file;
	}
	std::unique_ptr<File> open_existing_read_only(std::string_view name) override { return open_existing(name); }
	std::unique_ptr<File> open_reclaim_candidate_read_only(std::string_view name) override { return open_reclaim_candidate(name); }
	void open_existing_read_only_into(std::string_view, File&) override { throw std::logic_error("reusable file open unsupported"); }
	bool open_reclaim_candidate_read_only_into(std::string_view, File&) override { throw std::logic_error("reusable candidate open unsupported"); }
	std::optional<EntryFootprint> entry_footprint(std::string_view name) override {
		model_.before(); auto found = model_.visible.find(std::string(name)); std::optional<EntryFootprint> result;
		if (found != model_.visible.end()) { result = EntryFootprint{EntryKind::Regular, found->second->visible.size(), std::nullopt}; }
		model_.after(); return result;
	}
	void sync_directory() override { model_.before(); model_.durable = model_.visible; model_.after(); }
private:
	struct Cursor final : DirectoryCursor {
		explicit Cursor(Model& model) : model_(model) {}
		std::optional<std::string> next() override {
			model_.before(); auto next = model_.visible.upper_bound(last_); std::optional<std::string> result;
			if (next != model_.visible.end()) { last_ = next->first; result = last_; }
			model_.after(); return result;
		}
		Model& model_; std::string last_;
	};
	struct Lock final : OwnerLock {
		explicit Lock(Model& model) : model_(model) {}
		~Lock() override { model_.locked = false; }
		Model& model_;
	};
	Model& model_;
};

Identity identity() { Identity value{}; value[0] = 'A'; return value; }
constexpr std::string_view data_name = "journal-0000000000000001";
Model initialized(std::size_t chunk = std::numeric_limits<std::size_t>::max()) {
	Model model; model.chunk = chunk;
	MemoryIO io(model); Journal journal(io, 1024);
	journal.create(identity()); journal.append_batch("first");
	return model.clone();
}
std::vector<std::string> replay(Model& model) {
	MemoryIO io(model); Journal journal(io, 1024);
	std::vector<std::string> batches;
	auto frontier = journal.recover([&](auto sequence, std::string_view batch) {
		check(sequence == batches.size() + 1, "replay is contiguous"); batches.emplace_back(batch);
	});
	check(frontier.sequence == batches.size(), "recovered frontier matches replay");
	return batches;
}

PreparedArtifact prepare(Journal& journal, std::string_view bytes) {
	auto builder = journal.prepare_artifact();
	while (!bytes.empty()) {
		auto count = std::min(bytes.size(), detail::artifact_chunk_size);
		builder.append_chunk(bytes.substr(0, count)); bytes.remove_prefix(count);
	}
	return builder.finish();
}
std::string read_artifact(ArtifactReader& reader) {
	std::string result(static_cast<std::size_t>(reader.descriptor().length), '\0');
	std::size_t offset = 0;
	while (offset < result.size()) {
		auto count = std::min(result.size() - offset, detail::artifact_chunk_size);
		auto read = reader.read_at(offset, std::span<char>(result.data() + offset, count));
		if (!read) { throw std::runtime_error("artifact stopped reading"); }
		offset += read;
	}
	return result;
}
std::pair<Frontier, std::vector<std::string>> replay_checkpoint(Model& model) {
	MemoryIO io(model); Journal journal(io, 1024);
	std::vector<std::string> batches;
	auto frontier = journal.recover([&](auto, std::string_view batch) { batches.emplace_back(batch); },
		[&](const Frontier& selected, ArtifactReader& checkpoint, std::span<ArtifactReader> dependencies) {
			check(selected.checkpoint && dependencies.size() == selected.dependencies.size(), "manifest exposes all required dependencies");
			batches.push_back(read_artifact(checkpoint));
			for (auto& dependency : dependencies) { check(read_artifact(dependency) == "application", "application dependency restores after integrity validation"); }
		});
	return {frontier, batches};
}

Model reclaim_fixture() {
	auto model = initialized();
	{
		MemoryIO io(model); Journal journal(io, 1024); journal.recover([](auto, auto) {});
		auto old_bundle = prepare(journal, "first"); auto old_app = prepare(journal, "application");
		journal.publish_checkpoint(old_bundle, std::array{old_app}, 1);
		auto bundle = prepare(journal, "first"); auto app = prepare(journal, "application");
		journal.publish_checkpoint(bundle, std::array{app}, 1);
	}
	return model.clone();
}
StorageResources visible_resources(const Model& model) {
	StorageResources resources{0, model.visible.size()};
	for (const auto& [name, inode] : model.visible) { resources.logical_bytes += inode->visible.size(); }
	return resources;
}
}  // namespace

// The cases journal_test.cc runs from the other files.
// checkpoints and artifacts (journal_artifact_test.cc)
void checkpoint_basics();
void artifact_verification();
void checkpoint_successive_generations();
void preparation_ownership_and_failures();
void reclamation_roots_and_unknowns();
void durable_staging_ownership();
void mixed_artifact_formats();
void footprint_plans();
void checkpoint_publication_retirement();
void suspended_artifact_open();
void suspended_artifact_reads();
void incremental_artifact_verification();
void published_artifact_selection();
void checkpoint_corruption();
// the Store (journal_store_test.cc)
void store_publication_completions();
void detached_old_artifact_lease();
void store_append_completions();
void store_reclaim_before_inventory();
void store_replacement_growth();
void store_basics();
void store_failures();
void store_append_and_cleanup_failures();
void store_limits_and_pins();
void store_incremental_verification();
void store_published_selection();
void store_posix();
// a Host's group ids and heap (journal_reopen_test.cc)
void reopen_clean_close();
void reopen_released(bool fenced);
void reopen_refused();
void reopen_two_stores();
void reopen_race();
void reopen_crash();
void reopen_churn();
void reopen_distinct();
void park_count();
void reopen_heap();
void reclaim_syncs();
