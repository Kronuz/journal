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

// Durable syncs go through the IO seam (IO::sync_files), so a host that skips or counts them
// sees every one: the host table's commit, a turn log's segment preparation and a journal's
// torn-tail truncation. A CountingIO counts the seam's calls and each File::sync made past it.
// `sync_seam_test [case]` runs one case; `sync_seam_test open_cost [groups]` times group opens
// with every seam sync skipped (docs/MEASUREMENTS.md).
#include "faultlab/faultfs.h"
#include "journal/host.h"
#include "journal/journal.h"
#include "journal/turn_log.h"
#include "completion/posix.h"
#include <chrono>
#include <filesystem>
#include <iostream>

namespace j = kronuz::journal;
namespace fl = kronuz::faultlab;

namespace {
int checks = 0, failures = 0;
void check(bool value, const std::string& what) {
	++checks;
	if (!value) { ++failures; std::cerr << "FAIL: " << what << '\n'; }
}
void expect(std::size_t actual, std::size_t wanted, const std::string& what) {
	check(actual == wanted, what + ": " + std::to_string(actual) + ", wanted " + std::to_string(wanted));
}

// seam: IO::sync_files and sync_data_files calls, one per file; direct: File::sync and sync_data made past them.
struct Counts { std::size_t seam = 0, seam_data = 0, direct = 0, direct_data = 0, directory = 0; };

// A file that counts the syncs made on it directly; the seam unwraps it to the inner file.
class CountedFile final : public j::File {
public:
	CountedFile(std::unique_ptr<j::File> inner, Counts& counts) : inner(std::move(inner)), counts_(counts) {}
	std::uint64_t size() override { return inner->size(); }
	std::size_t read_at(std::uint64_t offset, std::span<char> bytes) override { return inner->read_at(offset, bytes); }
	std::size_t write_at(std::uint64_t offset, std::string_view bytes) override { return inner->write_at(offset, bytes); }
	void truncate(std::uint64_t size) override { inner->truncate(size); }
	void sync() override { ++counts_.direct; inner->sync(); }
	void sync_data() override { ++counts_.direct_data; inner->sync_data(); }
	std::unique_ptr<j::File> inner;
private:
	Counts& counts_;
};

class CountingIO final : public j::IO {
public:
	explicit CountingIO(std::shared_ptr<j::IO> io, bool skip = false) : io_(std::move(io)), skip_(skip) {}
	Counts counts;
	std::unique_ptr<j::OwnerLock> acquire_owner(bool create) override { return io_->acquire_owner(create); }
	void open_existing_read_only_into(std::string_view name, j::File& file) override { io_->open_existing_read_only_into(name, file); }
	bool open_reclaim_candidate_read_only_into(std::string_view name, j::File& file) override { return io_->open_reclaim_candidate_read_only_into(name, file); }
	std::unique_ptr<j::File> open_reclaim_candidate_read_only(std::string_view name) override { return wrap(io_->open_reclaim_candidate_read_only(name)); }
	std::unique_ptr<j::File> open_existing(std::string_view name) override { return wrap(io_->open_existing(name)); }
	std::unique_ptr<j::File> open_existing_read_only(std::string_view name) override { return wrap(io_->open_existing_read_only(name)); }
	std::unique_ptr<j::File> create_exclusive(std::string_view name) override { return wrap(io_->create_exclusive(name)); }
	void replace(std::string_view source, std::string_view destination) override { io_->replace(source, destination); }
	void remove(std::string_view name) override { io_->remove(name); }
	std::unique_ptr<j::DirectoryCursor> scan_directory() override { return io_->scan_directory(); }
	std::optional<j::EntryFootprint> entry_footprint(std::string_view name) override { return io_->entry_footprint(name); }
	std::unique_ptr<j::File> open_reclaim_candidate(std::string_view name) override { return wrap(io_->open_reclaim_candidate(name)); }
	void sync_directory() override { ++counts.directory; if (!skip_) { io_->sync_directory(); } }
	void sync_files(std::span<j::File* const> files) override {
		for (auto* file : files) { ++counts.seam; if (!skip_) { static_cast<CountedFile&>(*file).inner->sync(); } }
	}
	void sync_data_files(std::span<j::File* const> files) override {
		for (auto* file : files) { ++counts.seam_data; if (!skip_) { static_cast<CountedFile&>(*file).inner->sync_data(); } }
	}
protected:
	std::unique_ptr<j::OwnerLock> lock_owner(const j::OwnerLockOptions& options) override { return io_->acquire_owner(options); }
private:
	std::unique_ptr<j::File> wrap(std::unique_ptr<j::File> file) { return file ? std::make_unique<CountedFile>(std::move(file), counts) : nullptr; }
	std::shared_ptr<j::IO> io_;
	bool skip_;
};

const j::Identity host_identity{'H', 'O', 'S', 'T'};
const j::TurnLogOptions turn_options{16 * 1024, 1024, 1, 4, 4096};

// A host's table commit syncs its shard file once through the seam, then the directory once.
void host_table() {
	fl::filesystem disk; disk.make_directory("host");
	auto io = std::make_shared<CountingIO>(std::make_shared<fl::io>(disk, "host"));
	j::Host host(io, host_identity, j::HostOptions{1, turn_options});
	const auto opened = io->counts;
	expect(opened.direct + opened.direct_data, 0, "opening the host syncs no file past the seam");
	check(opened.seam_data >= 1, "the turn log's open record flushes through the seam");
	check(opened.seam >= 1, "the layout record's sync reaches the seam");
	host.create_group(7); host.commit_transitions(0);
	expect(io->counts.seam - opened.seam, 1, "a table commit makes one seam sync");
	expect(io->counts.direct - opened.direct, 0, "a table commit syncs no file past the seam");
	expect(io->counts.directory - opened.directory, 1, "a table commit syncs the directory once");
}

// Preparing a turn log segment syncs the preallocated file through the seam, then the directory.
void turn_segments() {
	fl::filesystem disk;
	CountingIO io(std::make_shared<fl::io>(disk));
	j::TurnRecovery recovered;
	j::TurnLog log(io, j::Identity{'T', 'U', 'R', 'N'}, recovered, turn_options);
	log.prepare();
	check(log.stats().segments_prepared >= 1, "the log prepared a segment");
	expect(io.counts.seam, log.stats().segments_prepared, "each prepared segment makes one seam sync");
	expect(io.counts.direct + io.counts.direct_data, 0, "a prepared segment and the open record sync no file past the seam");
}

// Each turn's data barrier goes through the seam: one sync_data_files call per turn, and the
// open record's.
void turn_flush() {
	fl::filesystem disk;
	CountingIO io(std::make_shared<fl::io>(disk));
	j::TurnRecovery recovered;
	j::TurnLog log(io, j::Identity{'T', 'U', 'R', 'N'}, recovered, turn_options);
	for (std::uint64_t turn = 1; turn <= 5; ++turn) {
		std::vector<j::TurnRecord> records{{1, turn, j::TurnRecordKind::Batch, "payload", turn - 1}};
		log.append(records);
	}
	expect(io.counts.seam_data, log.stats().barriers, "each turn's barrier makes one seam data sync");
	check(log.stats().barriers >= 5, "five turns were written");
	expect(io.counts.direct_data, 0, "a turn's barrier syncs no file past the seam");
}

// A journal's creation syncs its header through the seam; recovering a torn tail truncates it
// and syncs the file through the seam once.
void journal_tail() {
	fl::filesystem disk;
	{
		CountingIO io(std::make_shared<fl::io>(disk));
		j::Journal journal(io, 1024);
		journal.create(j::Identity{'A'});
		check(io.counts.seam >= 1, "creation syncs the data file header through the seam");
		journal.append_batch("first");
		expect(io.counts.direct + io.counts.direct_data, 0, "create and append sync no file past the seam");
	}
	std::string data;
	for (const auto& name : disk.list()) { if (name.starts_with("journal-")) { data = name; } }
	check(!data.empty(), "the journal's data file exists");
	{ fl::io raw(disk); auto file = raw.open_existing(data); file->write_at(file->size(), "torn tail"); file->sync(); }
	CountingIO io(std::make_shared<fl::io>(disk));
	j::Journal journal(io, 1024);
	std::size_t batches = 0;
	journal.recover([&](auto, auto) { ++batches; });
	expect(batches, 1, "recovery replays the acknowledged batch");
	expect(io.counts.seam, 1, "truncating the torn tail syncs through the seam once");
	expect(io.counts.direct, 0, "recovery syncs no file past the seam");
}

// Opens `groups` groups on a real directory with every seam sync skipped (a host's test mode):
// each is a table commit and a first turn. The time left is the work that does not go through
// the seam, so a sync made past it shows as time.
void open_cost(std::size_t groups) {
	const auto path = std::filesystem::current_path() / ".scratch" / "sync-seam-open-cost";
	std::filesystem::remove_all(path); std::filesystem::create_directories(path);
	auto io = std::make_shared<CountingIO>(std::make_shared<j::PosixIO>(path), true);
	const auto start = std::chrono::steady_clock::now();
	{
		j::Host host(io, host_identity, j::HostOptions{1, turn_options});
		for (std::uint64_t group = 1; group <= groups; ++group) {
			host.create_group(group);
			std::vector<j::TurnRecord> records{{group, 1, j::TurnRecordKind::Batch, "first", 0}};
			host.driver_of(group).log().append(records);
		}
	}
	const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	const auto& c = io->counts;
	std::cout << "open_cost: " << groups << " groups, " << elapsed << " ms total, " << elapsed * 1000 / static_cast<double>(groups)
		<< " us per group; seam " << c.seam << " files, " << c.seam_data << " data, directory " << c.directory
		<< "; direct " << c.direct << " files, " << c.direct_data << " data\n";
	std::filesystem::remove_all(path);
}
}  // namespace

int main(int argc, char** argv) {
	const std::string only = argc > 1 ? argv[1] : "";
	if (only == "open_cost") { open_cost(argc > 2 ? std::stoul(argv[2]) : 200); return 0; }
	const std::vector<std::pair<std::string, void (*)()>> cases{{"host_table", host_table}, {"turn_segments", turn_segments}, {"turn_flush", turn_flush}, {"journal_tail", journal_tail}};
	for (const auto& [name, run] : cases) {
		if (!only.empty() && only != name) { continue; }
		try { run(); } catch (const std::exception& error) { check(false, name + ": " + error.what()); }
	}
	std::cout << checks << " sync seam checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
