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
// `sync_seam_test [case]` runs one case.
#include "journal/host.h"
#include "journal/journal.h"
#include "journal/memory_io.h"
#include "journal/turn_log.h"
#include <iostream>

namespace j = kronuz::journal;
namespace jt = kronuz::journal::testing;

namespace {
int checks = 0, failures = 0;
void check(bool value, const std::string& what) {
	++checks;
	if (!value) { ++failures; std::cerr << "FAIL: " << what << '\n'; }
}
void expect(std::size_t actual, std::size_t wanted, const std::string& what) {
	check(actual == wanted, what + ": " + std::to_string(actual) + ", wanted " + std::to_string(wanted));
}

// `direct` counts File::sync; the turn flush's File::sync_data is the follow-up's.
struct Counts { std::size_t seam = 0, direct = 0, directory = 0; };

// A file that counts the syncs made on it directly; the seam unwraps it to the inner file.
class CountedFile final : public j::File {
public:
	CountedFile(std::unique_ptr<j::File> inner, Counts& counts) : inner(std::move(inner)), counts_(counts) {}
	std::uint64_t size() override { return inner->size(); }
	std::size_t read_at(std::uint64_t offset, std::span<char> bytes) override { return inner->read_at(offset, bytes); }
	std::size_t write_at(std::uint64_t offset, std::string_view bytes) override { return inner->write_at(offset, bytes); }
	void truncate(std::uint64_t size) override { inner->truncate(size); }
	void sync() override { ++counts_.direct; inner->sync(); }
	void sync_data() override { inner->sync_data(); }
	std::unique_ptr<j::File> inner;
private:
	Counts& counts_;
};

class CountingIO final : public j::IO {
public:
	explicit CountingIO(std::shared_ptr<j::IO> io) : io_(std::move(io)) {}
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
	void sync_directory() override { ++counts.directory; io_->sync_directory(); }
	void sync_files(std::span<j::File* const> files) override {
		for (auto* file : files) { ++counts.seam; static_cast<CountedFile&>(*file).inner->sync(); }
	}
protected:
	std::unique_ptr<j::OwnerLock> lock_owner(const j::OwnerLockOptions& options) override { return io_->acquire_owner(options); }
private:
	std::unique_ptr<j::File> wrap(std::unique_ptr<j::File> file) { return file ? std::make_unique<CountedFile>(std::move(file), counts) : nullptr; }
	std::shared_ptr<j::IO> io_;
};

const j::Identity host_identity{'H', 'O', 'S', 'T'};
const j::TurnLogOptions turn_options{16 * 1024, 1024, 1, 4, 4096};

// A host's table commit syncs its shard file once through the seam, then the directory once.
void host_table() {
	jt::Disk disk;
	auto io = std::make_shared<CountingIO>(std::make_shared<jt::MemoryIO>(disk, "host/"));
	j::Host host(io, host_identity, j::HostOptions{1, turn_options});
	const auto opened = io->counts;
	expect(opened.direct, 0, "opening the host syncs no file past the seam");
	check(opened.seam >= 1, "the layout record's sync reaches the seam");
	host.create_group(7);
	expect(io->counts.seam - opened.seam, 1, "a table commit makes one seam sync");
	expect(io->counts.direct - opened.direct, 0, "a table commit syncs no file past the seam");
	expect(io->counts.directory - opened.directory, 1, "a table commit syncs the directory once");
}

// Preparing a turn log segment syncs the preallocated file through the seam, then the directory.
void turn_segments() {
	jt::Disk disk;
	CountingIO io(std::make_shared<jt::MemoryIO>(disk));
	j::TurnRecovery recovered;
	j::TurnLog log(io, j::Identity{'T', 'U', 'R', 'N'}, recovered, turn_options);
	log.prepare();
	check(log.stats().segments_prepared >= 1, "the log prepared a segment");
	expect(io.counts.seam, log.stats().segments_prepared, "each prepared segment makes one seam sync");
	expect(io.counts.direct, 0, "a prepared segment syncs no file past the seam");
}

// A journal's creation syncs its header through the seam; recovering a torn tail truncates it
// and syncs the file through the seam once.
void journal_tail() {
	jt::Disk disk;
	{
		CountingIO io(std::make_shared<jt::MemoryIO>(disk));
		j::Journal journal(io, 1024);
		journal.create(j::Identity{'A'});
		check(io.counts.seam >= 1, "creation syncs the data file header through the seam");
		journal.append_batch("first");
	}
	std::string data;
	for (const auto& [name, id] : disk.names) { if (name.starts_with("journal-")) { data = name; } }
	check(!data.empty(), "the journal's data file exists");
	{ jt::MemoryIO raw(disk); auto file = raw.open_existing(data); file->write_at(file->size(), "torn tail"); file->sync(); }
	CountingIO io(std::make_shared<jt::MemoryIO>(disk));
	j::Journal journal(io, 1024);
	std::size_t batches = 0;
	journal.recover([&](auto, auto) { ++batches; });
	expect(batches, 1, "recovery replays the acknowledged batch");
	expect(io.counts.seam, 1, "truncating the torn tail syncs through the seam once");
	expect(io.counts.direct, 0, "recovery syncs no file past the seam");
}
}  // namespace

int main(int argc, char** argv) {
	const std::string only = argc > 1 ? argv[1] : "";
	const std::vector<std::pair<std::string, void (*)()>> cases{{"host_table", host_table}, {"turn_segments", turn_segments}, {"journal_tail", journal_tail}};
	for (const auto& [name, run] : cases) {
		if (!only.empty() && only != name) { continue; }
		try { run(); } catch (const std::exception& error) { check(false, name + ": " + error.what()); }
	}
	std::cout << checks << " sync seam checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
