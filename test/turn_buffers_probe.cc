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

// What a turn's stage() and completed() cost, through TurnLog's API alone, so the same source
// builds against main and against this branch: allocations and bytes allocated per warm turn,
// and the fastest of `rounds` turns' CPU time. Workload: turn_buffers_test's warm case, 8 groups
// of 4 records (100 B to 40 KiB, 384,800 body bytes a turn) and a checkpoint turn each round.
//   turn_buffers_probe [rounds]
#include "faultlab/faultfs.h"
#include "journal/turn_log.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <optional>

// TSan's runtime defines operator new, so under TSan nothing is counted and the probe refuses.
#if defined(__SANITIZE_THREAD__)
#define JOURNAL_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define JOURNAL_TEST_TSAN 1
#endif
#endif
namespace {
#ifdef JOURNAL_TEST_TSAN
constexpr bool heap_counted = false;
#else
constexpr bool heap_counted = true;
#endif
std::atomic<bool> counting{false};
std::atomic<std::uint64_t> allocations{0}, allocated{0};
}  // namespace

#ifndef JOURNAL_TEST_TSAN
void* operator new(std::size_t size) {
	if (counting.load(std::memory_order_relaxed)) { allocations.fetch_add(1, std::memory_order_relaxed); allocated.fetch_add(size, std::memory_order_relaxed); }
	if (void* p = std::malloc(size ? size : 1)) { return p; }
	throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif

namespace j = kronuz::journal;
namespace fl = kronuz::faultlab;

int main(int argc, char** argv) {
	if (!heap_counted) { std::cerr << "turn_buffers_probe: TSan's runtime defines operator new, so allocations are not counted\n"; return 2; }
	const unsigned rounds = argc > 1 ? static_cast<unsigned>(std::stoul(argv[1])) : 100;
	const j::TurnLogOptions options{std::uint64_t{48} << 20, 32 << 10, 1, 1000, 1 << 20};
	const j::Identity identity{'P', 'R', 'O', 'B', 'E'};
	fl::filesystem disk; fl::io io(disk);
	j::TurnRecovery recovered; j::TurnLog log(io, identity, recovered, options);
	const std::size_t sizes[] = {100, 1000, 40000, 7000};
	auto record = [](std::uint64_t group, std::uint64_t sequence, std::size_t size, j::TurnRecordKind kind) {
		return j::TurnRecord{group, sequence, kind, std::string(size, static_cast<char>('a' + (group + sequence) % 26)), sequence - 1};
	};
	std::uint64_t calls = 0, bytes = 0, turns = 0;
	double fastest = 1e30;
	for (unsigned round = 0; round < rounds; ++round) {
		std::vector<j::TurnRecord> batch, checkpoints;
		for (std::uint64_t group = 1; group <= 8; ++group) {
			for (std::uint64_t i = 0; i < 4; ++i) { batch.push_back(record(group, log.sequence(group) + 1 + i, sizes[i], j::TurnRecordKind::Batch)); }
			checkpoints.push_back(record(group, log.sequence(group) + 5, 5000, j::TurnRecordKind::Checkpoint));
		}
		for (auto* turn : {&batch, &checkpoints}) {
			allocations = 0; allocated = 0;
			std::optional<j::TurnLog::TurnWrite> write;
			counting = true;
			const auto start = std::chrono::steady_clock::now();
			write = log.stage(*turn);
			const auto staged = std::chrono::steady_clock::now();
			counting = false;
			write->file->write_at(write->offset, write->bytes);
			log.flush(*write);
			counting = true;
			const auto before = std::chrono::steady_clock::now();
			log.completed();
			const auto done = std::chrono::steady_clock::now();
			counting = false;
			if (turn == &batch && round >= 4) {
				calls += allocations; bytes += allocated; ++turns;
				fastest = std::min(fastest, std::chrono::duration<double, std::micro>((staged - start) + (done - before)).count());
			}
		}
		if (log.segment() != 1) { std::cerr << "the probe's turns left their segment\n"; return 1; }
	}
	std::cout << "warm batch turn (32 records, 384800 body bytes), mean of " << turns << ": "
		<< static_cast<double>(calls) / turns << " allocations, " << static_cast<double>(bytes) / turns << " bytes allocated; fastest stage()+completed() "
		<< fastest << " us\n";
	return 0;
}
