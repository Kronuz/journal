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

// The growing-log reader and its tail rule (docs/mechanisms/framing.md), over strata's tail
// model's four logs (test/fixtures/framing, exported by export.py): the model's checks 1, 2,
// 4, 5 and 6 with the model's counts, then one case per row of the rule.

#include "journal/framing.h"
#include "journal/memory_io.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

namespace j = kronuz::journal;
namespace f = kronuz::journal::framing;

namespace {
int checks = 0, failures = 0;
void check(bool value, const std::string& what) {
	++checks;
	if (!value) { ++failures; std::cerr << "FAIL: " << what << '\n'; }
}

const j::Identity identity{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const f::Stream stream{"STRLOG01", identity, 512, 7, 1, false};

struct Log { std::string name, bytes; std::vector<std::size_t> ends; };
std::string slurp(const std::filesystem::path& path) { std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), {}}; }
std::vector<Log> logs() {
	std::vector<Log> result;
	for (auto name : {"log-a", "log-b", "log-c", "log-d"}) {
		Log log{name, slurp(std::filesystem::path(JOURNAL_FIXTURE_DIR) / name), {}};
		std::istringstream ends(slurp(std::filesystem::path(JOURNAL_FIXTURE_DIR) / (std::string(name) + ".ends")));
		for (std::size_t end; ends >> end;) { log.ends.push_back(end); }
		result.push_back(std::move(log));
	}
	return result;
}

// The manifest log's reading: its first record is a synced snapshot, so a problem there is damage.
struct Verdict { f::Tail tail; std::size_t records; };
Verdict classify(std::string_view bytes) {
	auto scan = f::scan(bytes, stream);
	if (scan.tail != f::Tail::clean && scan.records.empty()) { return {f::Tail::damage, 0}; }
	return {scan.tail, scan.records.size()};
}

// Every image a crash can leave of one append written in one write: each sector of it persisted
// or not (unwritten bytes read as zeros), the size at E, at E+n, or at a sector multiple between.
template <typename Visit>
void honest_images(std::string_view previous, std::string_view next, Visit visit) {
	const std::size_t E = previous.size(), N = next.size(), sector = f::sector_bytes;
	std::vector<std::size_t> sectors;
	for (auto s = E / sector; s <= (N - 1) / sector; ++s) { sectors.push_back(s); }
	std::set<std::size_t> sizes{E, N};
	for (auto size = (E / sector + 1) * sector; size < N; size += sector) { sizes.insert(size); }
	for (std::uint64_t mask = 0; mask < (std::uint64_t{1} << sectors.size()); ++mask) {
		std::string image(previous); image.resize(N, '\0');
		for (std::size_t i = 0; i < sectors.size(); ++i) {
			if (!(mask >> i & 1)) { continue; }
			const auto from = std::max(sectors[i] * sector, E), to = std::min((sectors[i] + 1) * sector, N);
			image.replace(from, to - from, next.substr(from, to - from));
		}
		for (auto size : sizes) { visit(std::string_view(image).substr(0, size), size == N && image == next); }
	}
}

// Bytes no reader reads: block tails too short for a fragment.
std::set<std::size_t> padding(std::string_view log) {
	std::set<std::size_t> result; const std::size_t block = stream.block_bytes;
	for (std::size_t position = 0; position < log.size();) {
		if (position % block == 0) { position += f::header_bytes(f::no_manifest); }
		const auto room = block - position % block;
		if (room < f::fragment_header + 1) { for (auto i = position; i < std::min(position + room, log.size()); ++i) { result.insert(i); } position += room; continue; }
		if (position >= log.size()) { break; }
		position += f::fragment_header + f::get16(log.substr(position + 4));
	}
	return result;
}

void model_logs() {
	const auto all = logs();
	const std::size_t records[] = {10, 12, 8, 31};
	for (std::size_t i = 0; i < all.size(); ++i) {
		const auto& log = all[i];
		auto scan = f::scan(log.bytes, stream);
		check(scan.tail == f::Tail::clean && scan.records.size() == records[i] && log.ends.size() == records[i], log.name + " reads clean, every record");
		f::Emitter emitter(stream, 0);
		bool ends = true;
		for (std::size_t r = 0; r < scan.records.size(); ++r) {
			emitter.record(scan.records[r].bytes);
			ends = ends && scan.records[r].end == log.ends[r] && emitter.position() == log.ends[r] && (r == 0 ? scan.records[r].offset == f::header_bytes(f::no_manifest) : true);
		}
		check(ends, log.name + ": each record ends where the model's does");
		check(emitter.bytes() == log.bytes, log.name + ": the emitter writes the model's bytes");
	}

	// Check 1: honest crash images of every append open at the last whole record, never as damage.
	std::size_t images = 0, torn = 0, misread = 0;
	for (const auto& log : all) {
		for (std::size_t k = 1; k < log.ends.size(); ++k) {
			honest_images(std::string_view(log.bytes).substr(0, log.ends[k - 1]), std::string_view(log.bytes).substr(0, log.ends[k]), [&](std::string_view image, bool whole) {
				++images; auto verdict = classify(image);
				torn += verdict.tail == f::Tail::torn;
				misread += verdict.tail == f::Tail::damage || verdict.records != (whole ? k + 1 : k);
			});
		}
	}
	check(images == 1020 && torn == 672 && misread == 0, "honest crash images: " + std::to_string(images) + ", " + std::to_string(torn) + " torn, " + std::to_string(misread) + " misread");

	// Check 2: no single-bit flip anywhere reads as torn or clean with fewer records.
	std::size_t flips = 0, damage = 0, silent = 0, unread = 0;
	for (const auto& log : all) {
		const auto pad = padding(log.bytes);
		std::string image(log.bytes);
		for (std::size_t offset = 0; offset < image.size(); ++offset) {
			for (unsigned bit = 0; bit < 8; ++bit) {
				image[offset] = static_cast<char>(image[offset] ^ (1 << bit));
				auto verdict = classify(image);
				image[offset] = static_cast<char>(image[offset] ^ (1 << bit));
				++flips;
				if (verdict.tail == f::Tail::damage) { ++damage; }
				else if (verdict.records == log.ends.size() && pad.contains(offset)) { ++unread; }
				else { ++silent; }
			}
		}
	}
	check(flips == 219672 && damage == 219088 && silent == 0 && unread == 584,
		"single-bit flips: " + std::to_string(flips) + ", " + std::to_string(damage) + " damage, " + std::to_string(silent) + " silent, " + std::to_string(unread) + " in padding");

	// Check 4: a torn last append and a flip in the record before it: never silent.
	std::size_t cases = 0; silent = 0;
	for (const auto& log : all) {
		const auto n = log.ends.size(), low = log.ends[n - 3], high = std::min(log.ends[n - 2], low + 256);
		std::set<std::string> seen;
		honest_images(std::string_view(log.bytes).substr(0, log.ends[n - 2]), log.bytes, [&](std::string_view image, bool whole) {
			if (whole || !seen.emplace(image).second || classify(image).tail != f::Tail::torn) { return; }
			std::string damaged(image);
			for (auto offset = low; offset < high; ++offset) {
				for (unsigned bit = 0; bit < 8; ++bit) {
					damaged[offset] = static_cast<char>(damaged[offset] ^ (1 << bit));
					auto verdict = classify(damaged);
					damaged[offset] = static_cast<char>(damaged[offset] ^ (1 << bit));
					++cases; silent += verdict.tail != f::Tail::damage && verdict.records < n - 1;
				}
			}
		});
	}
	check(cases == 7504 && silent == 0, "two faults: " + std::to_string(cases) + " images, " + std::to_string(silent) + " silent");

	// Check 5: two appends in flight leave images that read as damage: the writer keeps one.
	std::size_t total = 0; damage = 0;
	for (const auto& log : all) {
		const auto n = log.ends.size(), E0 = log.ends[n - 3], E2 = log.ends[n - 1];
		const auto first = E0 / f::sector_bytes, last = (E2 - 1) / f::sector_bytes;
		if (last - first + 1 > 10) { continue; }
		for (std::uint64_t mask = 0; mask < (std::uint64_t{1} << (last - first + 1)); ++mask) {
			std::string image = log.bytes.substr(0, E0); image.resize(E2, '\0');
			for (auto s = first; s <= last; ++s) {
				if (!(mask >> (s - first) & 1)) { continue; }
				const auto from = s * f::sector_bytes, to = std::min(from + f::sector_bytes, E2);
				image.replace(from, to - from, log.bytes.substr(from, to - from));
			}
			++total; damage += classify(image).tail == f::Tail::damage;
		}
	}
	check(total == 76 && damage == 33, "two appends in flight: " + std::to_string(damage) + " of " + std::to_string(total) + " damage");

	// Check 6: a short write left in place mostly reads as damage, so a failed append is truncated back.
	std::size_t sizes = 0, closed = 0;
	for (const auto& log : all) {
		for (std::size_t k = 1; k < log.ends.size(); ++k) {
			for (auto size = log.ends[k - 1] + 1; size < log.ends[k]; ++size) { ++sizes; closed += classify(std::string_view(log.bytes).substr(0, size)).tail == f::Tail::damage; }
		}
	}
	check(sizes == 21962 && closed == 21846, "short writes: " + std::to_string(closed) + " of " + std::to_string(sizes) + " damage");
}

// One case per row of the tail rule, on a log of three records.
void rule_rows() {
	f::Emitter emitter(stream, 0);
	std::vector<std::uint64_t> ends;
	for (auto size : {300, 200, 700}) { emitter.record(std::string(size, 'x')); ends.push_back(emitter.position()); }
	const std::string log = emitter.bytes();
	auto at = [&](std::string_view bytes) { return f::scan(bytes, stream); };
	check(log.substr(56, 2) == std::string("\xff\xff", 2), "a stream without manifests writes no_manifest");

	auto clean = at(log);
	check(clean.tail == f::Tail::clean && clean.records.size() == 3 && clean.at == log.size() && clean.records[2].bytes == std::string(700, 'x'), "clean: the last record ends at the end of file");
	check(at({}).tail == f::Tail::clean && at({}).records.empty(), "an empty file is clean and empty");

	// The third record spans blocks 1 and 2; a crash at the block boundary leaves it without its last fragment.
	const auto boundary = (ends[1] / 512 + 1) * 512;
	check(boundary < ends[2], "the third record crosses a block");
	auto cut_at_sector = at(std::string_view(log).substr(0, boundary));
	check(cut_at_sector.tail == f::Tail::torn && cut_at_sector.records.size() == 2, "torn: a record without its last fragment, the file ending at a sector multiple");
	auto cut_inside = at(std::string_view(log).substr(0, boundary + 100));
	check(cut_inside.tail == f::Tail::damage && cut_inside.records.size() == 2, "damage: a fragment the end of file cuts");
	auto header_cut = at(std::string_view(log).substr(0, boundary + 30));
	check(header_cut.tail == f::Tail::damage && header_cut.at == boundary, "damage: a block header the end of file cuts");
	auto partial_sector = at(std::string_view(log).substr(0, ends[1] + 10));
	check(partial_sector.tail == f::Tail::damage, "damage: a fragment header the end of file cuts");

	std::string unwritten(log); std::fill(unwritten.begin() + static_cast<std::ptrdiff_t>(boundary), unwritten.end(), '\0');
	auto zeros = at(unwritten);
	check(zeros.tail == f::Tail::torn && zeros.records.size() == 2 && zeros.at == boundary, "torn: a block header whose piece was never written");
	std::string unwritten_fragment(log); std::fill(unwritten_fragment.begin() + static_cast<std::ptrdiff_t>(ends[1]), unwritten_fragment.begin() + static_cast<std::ptrdiff_t>(boundary), '\0');
	unwritten_fragment.resize(boundary);
	auto zero_fragment = at(unwritten_fragment);
	check(zero_fragment.tail == f::Tail::torn && zero_fragment.records.size() == 2 && zero_fragment.at == ends[1], "torn: a fragment whose bytes are zeros to its block's end");

	std::string flipped(log); flipped[ends[1] + 30] ^= 1;
	auto bad = at(flipped);
	check(bad.tail == f::Tail::damage && bad.records.size() == 2, "damage: bytes there and a check fails");

	// Zeros from a fragment to its block's end, with a whole record in a later block: damage.
	f::Emitter four(stream, 0); std::vector<std::uint64_t> four_ends;
	for (auto size : {100, 100, 600, 100}) { four.record(std::string(size, 'v')); four_ends.push_back(four.position()); }
	std::string followed = four.bytes(); std::fill(followed.begin() + static_cast<std::ptrdiff_t>(four_ends[0]), followed.begin() + 512, '\0');
	check(four_ends[3] > 1024 && at(followed).tail == f::Tail::damage, "damage: zeros with a whole record after them");
	followed.resize(four_ends[2]);
	check(at(followed).tail == f::Tail::torn, "torn: zeros with only a record's last fragment after them");

	std::string last_zeroed(log); std::fill(last_zeroed.begin() + static_cast<std::ptrdiff_t>(ends[1] + 4), last_zeroed.begin() + static_cast<std::ptrdiff_t>(ends[1] + 30), '\0');
	check(at(last_zeroed).tail == f::Tail::damage, "damage: zeros inside a fragment with bytes after them in its block");

	// A header that verifies under another epoch, and one carrying a manifest on a stream without them.
	f::Stream other = stream; other.epoch = 2;
	f::Emitter second(other, ends[2]); second.record("next");
	check(at(log + second.bytes()).tail == f::Tail::damage, "damage: a fragment bound to another epoch");
	f::Stream manifests = stream; manifests.manifests = true;
	f::Emitter with(manifests, 0, f::Manifest{true, 7, 0, {{1, 1}}}); with.record("x");
	check(at(with.bytes()).tail == f::Tail::damage, "damage: a header with a manifest on a stream without them");
	check(f::scan(with.bytes(), manifests).tail == f::Tail::clean, "a stream with manifests reads its headers");
	f::Stream foreign = stream; foreign.segment = 8;
	check(at(f::Emitter(foreign, 0).bytes()).tail == f::Tail::clean, "an emitter that wrote nothing wrote no header");
	f::Emitter other_segment(foreign, 0); other_segment.record("x");
	check(at(other_segment.bytes()).tail == f::Tail::damage, "damage: a header bound to another segment");

	// A Last fragment with no record open, and a First while one is open.
	f::Emitter split(stream, 0); split.record(std::string(600, 'y'));
	f::Emitter filled(stream, 0); filled.record(std::string(512 - f::header_bytes(f::no_manifest) - f::fragment_header, 'z'));
	check(filled.position() == 512, "a record fills block 0 exactly");
	check(at(filled.bytes() + split.bytes().substr(512)).tail == f::Tail::damage, "damage: a Last fragment with no record open");
	f::Emitter after(stream, 512); after.record("w");
	check(at(split.bytes().substr(0, 512) + after.bytes()).tail == f::Tail::damage, "damage: a First fragment while a record is open");

	bool threw = false;
	try { f::Stream tiny = stream; tiny.block_bytes = 64; f::scan(log, tiny); } catch (const std::invalid_argument&) { threw = true; }
	check(threw, "a block too small for a header and a fragment is refused");

	// read_file reads a growing file, its partial last block included.
	kronuz::journal::testing::Disk disk; kronuz::journal::testing::MemoryIO io(disk, "framing/");
	auto file = io.create_exclusive("log"); file->write_at(0, log);
	check(f::read_file(*file) == log, "read_file reads the whole file");
	check(f::read_record(*file, stream.block_bytes, clean.records[2].offset) == std::string(700, 'x'), "read_record reads a record across blocks");
}
}  // namespace

int main() {
	try {
		model_logs();
		rule_rows();
	} catch (const std::exception& error) {
		std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
		return 1;
	}
	std::cout << checks << " checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
