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

// 64 sequential durable batches per case, through the journal in the flush mode named. With
// --compare, the same batches through the raw loop a caller writes without the library: one
// write and one flush per batch, the call the mode's File::sync_data makes (macOS: F_FULLFSYNC in
// full, fsync in plain, which leaves data in the drive's cache; Linux: fdatasync in both),
// appended to one file, with no batch header, checksum, manifest or recovery. The comparison
// checks first that both made the same bytes durable: the journal's recovered batches, the raw
// file and the batches written are the same byte string.
//
//   journal_bench plain|full [--compare]   run it from the repository root; it writes under .scratch/
#include "journal/journal.h"
#include "completion/posix.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <system_error>
#include <unistd.h>
#include <vector>

static double cpu_seconds() {
	rusage usage{};
	if (::getrusage(RUSAGE_SELF, &usage)) { throw std::runtime_error("getrusage failed"); }
	return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}

namespace {
constexpr unsigned groups = 64;

namespace j = kronuz::journal;

[[noreturn]] void fail(const char* what) { throw std::system_error(errno, std::generic_category(), what); }

// The raw loop's flush: the call `mode` makes for File::sync_data, as completion's sync_call names it.
const char* raw_flush_name(j::FlushMode mode) {
	switch (j::sync_call(j::SyncSite::data, mode)) {
	case j::SyncCall::full_fsync: return "F_FULLFSYNC";
	case j::SyncCall::fdatasync: return "fdatasync";
	default: return "fsync";
	}
}
void raw_flush(int fd, j::FlushMode mode) {
	int result;
	switch (j::sync_call(j::SyncSite::data, mode)) {
#ifdef __APPLE__
	case j::SyncCall::full_fsync: do { result = ::fcntl(fd, F_FULLFSYNC); } while (result < 0 && errno == EINTR); break;
#endif
#ifdef __linux__
	case j::SyncCall::fdatasync: do { result = ::fdatasync(fd); } while (result < 0 && errno == EINTR); break;
#endif
	default: do { result = ::fsync(fd); } while (result < 0 && errno == EINTR); break;
	}
	if (result < 0) { fail("raw flush"); }
}

void raw_write(int fd, std::string_view bytes) {
	while (!bytes.empty()) {
		auto written = ::write(fd, bytes.data(), bytes.size());
		if (written < 0) {
			if (errno == EINTR) { continue; }
			fail("raw write");
		}
		bytes.remove_prefix(static_cast<std::size_t>(written));
	}
}

struct Descriptor {
	int fd;
	~Descriptor() { if (fd >= 0) { ::close(fd); } }
};

// Batch `group` of a case: the same bytes whichever writer appends it.
void stamp(std::string& batch, unsigned group) { std::memcpy(batch.data(), &group, sizeof group); }

std::string expected_bytes(std::string batch) {
	std::string all;
	for (unsigned group = 0; group < groups; ++group) { stamp(batch, group); all += batch; }
	return all;
}

struct Result { double wall, cpu; std::vector<double> samples; };

// Times `append(batch)` for each of the groups, one sample per batch.
template <class Append>
Result measure(std::string& batch, Append&& append) {
	std::vector<double> samples; samples.reserve(groups);
	auto cpu_begin = cpu_seconds();
	auto begin = std::chrono::steady_clock::now();
	for (unsigned group = 0; group < groups; ++group) {
		stamp(batch, group);
		auto start = std::chrono::steady_clock::now();
		append(batch);
		samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
	}
	auto wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
	auto cpu = cpu_seconds() - cpu_begin;
	std::sort(samples.begin(), samples.end());
	return {wall, cpu, std::move(samples)};
}

void print(const char* writer, std::size_t operations, std::size_t bytes, const Result& r) {
	if (writer) { std::cout << writer << ','; }
	std::cout << std::fixed << std::setprecision(6) << operations << ',' << bytes << ',' << r.wall << ',' << r.cpu << ','
		<< groups * operations / r.wall << ',' << r.samples[groups / 2 - 1] << ',' << r.samples[groups - 1] << '\n';
}

// A fresh directory under .scratch, removed when the guard goes.
struct Scratch {
	std::filesystem::path path;
	explicit Scratch(std::filesystem::path p) : path(std::move(p)) {
		if (!std::filesystem::create_directory(path)) { throw std::runtime_error("benchmark directory already exists"); }
		::chmod(path.c_str(), 0700);
	}
	~Scratch() { std::filesystem::remove_all(path); }
};
}  // namespace

int main(int argc, char** argv) {
	const std::string_view mode_name = argc > 1 ? argv[1] : "";
	if ((mode_name != "plain" && mode_name != "full") || argc > 3 || (argc == 3 && std::string_view(argv[2]) != "--compare")) {
		std::cerr << "usage: journal_bench plain|full [--compare]\n";
		return 2;
	}
	const j::FlushMode mode = mode_name == "full" ? j::FlushMode::full : j::FlushMode::plain;
	try {
		bool compare = argc == 3;
		const auto root = std::filesystem::current_path() / ".scratch";
		std::filesystem::create_directories(root);
		std::cout << "opaque operation bytes=256, groups=64, flush mode " << mode_name << "; local filesystem microbenchmark\n";
		if (compare) { std::cout << "writer: journal, or raw = one write and one " << raw_flush_name(mode) << " per batch, no framing\n"; }
		std::cout << (compare ? "writer, " : "") << "ops/group, payload_bytes, wall_s, cpu_s, ops/s, p50_batch_ms, p99_batch_ms\n";
		for (std::size_t operations : {1u, 32u, 256u}) {
			const auto stem = "journal-bench-" + std::to_string(::getpid()) + "-" + std::to_string(operations);
			Scratch directory(root / stem);
			std::string batch(operations * 256, 'x');
			kronuz::journal::Identity identity{}; identity[0] = 'B';
			Result journaled{};
			{
				j::PosixIO io(directory.path, j::PosixIOOptions{mode});
				kronuz::journal::Journal journal(io, 1024 * 1024);
				journal.create(identity);
				journaled = measure(batch, [&](const std::string& bytes) { journal.append_batch(bytes); });
			}
			print(compare ? "journal" : nullptr, operations, batch.size(), journaled);
			if (!compare) { continue; }

			Scratch raw_directory(root / (stem + "-raw"));
			const auto raw_path = raw_directory.path / "log";
			Result raw{};
			{
				Descriptor file{::open(raw_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600)};
				if (file.fd < 0) { fail("raw open"); }
				raw_flush(file.fd, mode);
				Descriptor dir{::open(raw_directory.path.c_str(), O_RDONLY)};
				if (dir.fd < 0 || ::fsync(dir.fd) < 0) { fail("raw directory"); }
				raw = measure(batch, [&](const std::string& bytes) { raw_write(file.fd, bytes); raw_flush(file.fd, mode); });
			}
			print("raw", operations, batch.size(), raw);

			// The same bytes: what the journal recovers, what is in the raw file, what was written.
			std::string recovered;
			{
				j::PosixIO io(directory.path, j::PosixIOOptions{mode});
				kronuz::journal::Journal journal(io, 1024 * 1024);
				auto frontier = journal.recover([&](auto, std::string_view bytes) { recovered.append(bytes); });
				if (frontier.sequence != groups) { throw std::runtime_error("same-results check failed: the journal recovered the wrong number of batches"); }
			}
			std::ifstream stream(raw_path, std::ios::binary);
			std::string stored((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
			auto expected = expected_bytes(batch);
			if (recovered != expected || stored != expected) { throw std::runtime_error("same-results check failed: the bytes made durable differ"); }
			std::cout << "# same bytes: " << groups << " batches, " << expected.size() << " bytes in the journal's recovery, the raw file and the input\n";
		}
	} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
