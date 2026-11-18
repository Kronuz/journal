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

// Cost of one Host::group_io call by the number of live ids (goal5 O4): group_io erases the
// expired entries of its range table on every call, a sweep of every live id's entry. Builds a
// Host over MemoryIO (a few files, so its directory sync costs nothing), holds N live ranges
// (what N open group views keep), then times calls on fresh ids: ns per call, median of 5 runs.
// `journal_group_io_bench [N...]` (default 1000 10000 100000 1000000; 0 is the empty baseline).
#include "journal/host.h"
#include "journal/memory_io.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

namespace j = kronuz::journal;
using Ranges = std::multimap<std::uint64_t, std::weak_ptr<j::host_detail::Range>>;

// Reaches Host::ranges_ with no change to Host: explicit instantiation ignores access.
template <class Tag, auto Member> struct Steal { friend constexpr auto steal(Tag) { return Member; } };
struct RangesTag { friend constexpr auto steal(RangesTag); };
template struct Steal<RangesTag, &j::Host::ranges_>;

namespace {
using Clock = std::chrono::steady_clock;
double seconds_since(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }
std::uint64_t mix(std::uint64_t& state) {
	std::uint64_t z = (state += 0x9e3779b97f4a7c15ull);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull; z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

// N live ranges, as N open group views hold: a Range each, its entry in the host's table.
// `shuffled` takes ids in random order (a host's real order); otherwise 1..N ascending.
std::vector<std::shared_ptr<j::host_detail::Range>> inject(j::Host& host, std::uint64_t count, bool shuffled) {
	auto& ranges = host.*steal(RangesTag{});
	std::vector<std::shared_ptr<j::host_detail::Range>> held; held.reserve(count);
	std::uint64_t state = 1;
	for (std::uint64_t i = 1; i <= count; ++i) {
		held.push_back(std::make_shared<j::host_detail::Range>());
		ranges.emplace(shuffled ? mix(state) >> 1 | 1 : i, held.back());
	}
	return held;
}

struct Result { double median, low, high; std::uint64_t calls; };
// Time group_io on fresh ids (or one resident id): 5 runs, each sized to about 0.3 s.
Result time_calls(j::Host& host, std::uint64_t resident) {
	std::uint64_t next = std::uint64_t{1} << 62;
	auto call = [&] { auto io = host.group_io(resident ? resident : next++); asm volatile("" : : "r"(io.get()) : "memory"); };
	call();
	auto start = Clock::now(); call(); const double once = seconds_since(start);
	const std::uint64_t calls = std::clamp<std::uint64_t>(static_cast<std::uint64_t>(0.3 / std::max(once, 1e-9)), 3, 2000000);
	std::vector<double> runs;
	for (int run = 0; run < 5; ++run) {
		start = Clock::now();
		for (std::uint64_t i = 0; i < calls; ++i) { call(); }
		runs.push_back(seconds_since(start) * 1e9 / static_cast<double>(calls));
	}
	std::sort(runs.begin(), runs.end());
	return {runs[2], runs.front(), runs.back(), calls};
}

}  // namespace

int main(int argc, char** argv) {
	std::vector<std::uint64_t> counts;
	for (int i = 1; i < argc; ++i) { counts.push_back(std::stoull(argv[i])); }
	if (counts.empty()) { counts = {1000, 10000, 100000, 1000000}; }
	constexpr std::uint64_t resident_id = 0xdeadbeef;
	std::printf("%9s  %-16s %14s %14s %14s %9s %10s\n", "ids", "population", "median ns/call", "min", "max", "calls/run", "ns per id");
	std::vector<std::pair<double, double>> fit;
	for (auto count : counts) {
		struct Case { const char* name; bool shuffled; bool resident; bool built; };
		for (auto [name, shuffled, resident, built] : {Case{"random ids", true, false, false}, Case{"ascending ids", false, false, false},
		                                                Case{"random, resident", true, true, false}, Case{"built by group_io", true, false, true}}) {
			if (built && count > 10000) { continue; }  // group_io's own sweep makes building quadratic
			j::testing::Disk disk;
			j::Host host(std::make_shared<j::testing::MemoryIO>(disk), {'G', 'I', 'O', 'C'}, j::HostOptions{1, {}});
			if (resident) { host.create_group(resident_id); }
			std::vector<std::shared_ptr<j::host_detail::Range>> held;
			std::vector<std::shared_ptr<j::IO>> views;
			if (built) { std::uint64_t state = 1; for (std::uint64_t i = 0; i < count; ++i) { views.push_back(host.group_io(mix(state) >> 1 | 1)); } }
			else { held = inject(host, count, shuffled); }
			const auto result = time_calls(host, resident ? resident_id : 0);
			std::printf("%9llu  %-16s %14.0f %14.0f %14.0f %9llu %10.3f\n", static_cast<unsigned long long>(count), name, result.median, result.low, result.high,
			            static_cast<unsigned long long>(result.calls), count ? result.median / static_cast<double>(count) : 0.0);
			std::fflush(stdout);
			if (!resident && !built && shuffled) { fit.emplace_back(static_cast<double>(count), result.median); }
		}
	}
	if (fit.size() >= 2) {
		double n = static_cast<double>(fit.size()), sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
		for (auto [x, y] : fit) { sx += x; sy += y; sxx += x * x; sxy += x * y; syy += y * y; }
		const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx), intercept = (sy - slope * sx) / n;
		const double r = (n * sxy - sx * sy) / std::sqrt((n * sxx - sx * sx) * (n * syy - sy * sy));
		std::printf("fit (random ids, medians): ns/call = %.1f + %.4f * ids, r^2 = %.5f\n", intercept, slope, r * r);
		std::printf("endpoints: %.4f ns per id between %.0f and %.0f ids\n", (fit.back().second - fit.front().second) / (fit.back().first - fit.front().first), fit.front().first, fit.back().first);
	}
}
