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

// 64 sequential durable batches per case, through the journal. With --compare, the same batches
// through the plain loop a caller writes without the library: one write and one durable flush
// per batch (F_FULLFSYNC on macOS, whose fsync leaves data in the drive's cache; fdatasync on
// Linux), appended to one file, with no batch header, checksum, manifest or recovery. The
// comparison checks first that both made the same bytes durable: the journal's recovered
// batches, the plain file and the batches written are the same byte string.
//
//   journal_bench [--compare]      run it from the repository root; it writes under .scratch/
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

#ifdef __APPLE__
constexpr const char* plain_flush_name = "F_FULLFSYNC";
#elif defined(__linux__)
constexpr const char* plain_flush_name = "fdatasync";
#else
constexpr const char* plain_flush_name = "fsync";
#endif

[[noreturn]] void fail(const char* what) { throw std::system_error(errno, std::generic_category(), what); }

// The durable flush of the plain loop.
void plain_flush(int fd) {
	int result;
#ifdef __APPLE__
	do { result = ::fcntl(fd, F_FULLFSYNC); } while (result < 0 && errno == EINTR);
#elif defined(__linux__)
	do { result = ::fdatasync(fd); } while (result < 0 && errno == EINTR);
#else
	do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
#endif
	if (result < 0) { fail("plain flush"); }
}

void plain_write(int fd, std::string_view bytes) {
	while (!bytes.empty()) {
		auto written = ::write(fd, bytes.data(), bytes.size());
		if (written < 0) {
			if (errno == EINTR) { continue; }
			fail("plain write");
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
	try {
		bool compare = argc > 1 && std::string_view(argv[1]) == "--compare";
		const auto root = std::filesystem::current_path() / ".scratch";
		std::filesystem::create_directories(root);
		std::cout << "opaque operation bytes=256, groups=64; local filesystem microbenchmark\n";
		if (compare) { std::cout << "writer: journal, or plain = one write and one " << plain_flush_name << " per batch, no framing\n"; }
		std::cout << (compare ? "writer, " : "") << "ops/group, payload_bytes, wall_s, cpu_s, ops/s, p50_batch_ms, p99_batch_ms\n";
		for (std::size_t operations : {1u, 32u, 256u}) {
			const auto stem = "journal-bench-" + std::to_string(::getpid()) + "-" + std::to_string(operations);
			Scratch directory(root / stem);
			std::string batch(operations * 256, 'x');
			kronuz::journal::Identity identity{}; identity[0] = 'B';
			Result journaled{};
			{
				kronuz::journal::PosixIO io(directory.path);
				kronuz::journal::Journal journal(io, 1024 * 1024);
				journal.create(identity);
				journaled = measure(batch, [&](const std::string& bytes) { journal.append_batch(bytes); });
			}
			print(compare ? "journal" : nullptr, operations, batch.size(), journaled);
			if (!compare) { continue; }

			Scratch plain_directory(root / (stem + "-plain"));
			const auto plain_path = plain_directory.path / "log";
			Result plain{};
			{
				Descriptor file{::open(plain_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600)};
				if (file.fd < 0) { fail("plain open"); }
				plain_flush(file.fd);
				Descriptor dir{::open(plain_directory.path.c_str(), O_RDONLY)};
				if (dir.fd < 0 || ::fsync(dir.fd) < 0) { fail("plain directory"); }
				plain = measure(batch, [&](const std::string& bytes) { plain_write(file.fd, bytes); plain_flush(file.fd); });
			}
			print("plain", operations, batch.size(), plain);

			// The same bytes: what the journal recovers, what is in the plain file, what was written.
			std::string recovered;
			{
				kronuz::journal::PosixIO io(directory.path);
				kronuz::journal::Journal journal(io, 1024 * 1024);
				auto frontier = journal.recover([&](auto, std::string_view bytes) { recovered.append(bytes); });
				if (frontier.sequence != groups) { throw std::runtime_error("same-results check failed: the journal recovered the wrong number of batches"); }
			}
			std::ifstream stream(plain_path, std::ios::binary);
			std::string stored((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
			auto expected = expected_bytes(batch);
			if (recovered != expected || stored != expected) { throw std::runtime_error("same-results check failed: the bytes made durable differ"); }
			std::cout << "# same bytes: " << groups << " batches, " << expected.size() << " bytes in the journal's recovery, the plain file and the input\n";
		}
	} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
