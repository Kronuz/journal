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

// CRC-32C compatibility. test/fixtures/crc32c holds a journal directory (batches, manifest,
// checkpoint and dependency artifacts) and a turn log written by the byte-table CRC at
// journal dbf753f. They must still verify and recover, a flipped byte must still be refused,
// and crc32c()/Checksum must equal an independent bitwise CRC-32C for any fragmentation.
// `crc_fixture_test --generate DIRECTORY` writes the fixtures with the code it is built from.
#include "journal/journal.h"
#if __has_include("journal/turn_log.h")
#include "journal/turn_log.h"
#define JOURNAL_TURN_LOG 1  // journals before the turn log have no such fixture
#endif
#include "completion/posix.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <random>

namespace j = kronuz::journal;
namespace fs = std::filesystem;

namespace {
int checks = 0, failures = 0;
void check(bool value, const char* what) {
	++checks;
	if (!value) { ++failures; std::cerr << "FAIL: " << what << '\n'; }
}
template <class F> bool throws(F&& f) {
	try { f(); return false; } catch (const std::exception&) { return true; }
}

// Bitwise CRC-32C: reflected 0x1edc6f41, init and final XOR 0xffffffff, no table.
std::uint32_t bitwise(std::string_view bytes) {
	std::uint32_t crc = ~0u;
	for (unsigned char byte : bytes) {
		crc ^= byte;
		for (int bit = 0; bit < 8; ++bit) { crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78u : 0); }
	}
	return ~crc;
}

std::string pattern(std::size_t size, std::uint64_t seed) {
	std::mt19937_64 random(seed);
	std::string bytes(size, '\0');
	for (auto& byte : bytes) { byte = static_cast<char>(random()); }
	return bytes;
}

// The payloads the fixtures hold; sizes cross the hardware routine's 3 KiB stride and the 64 KiB artifact chunk.
const std::vector<std::string>& batches() {
	static const std::vector<std::string> values{"first", pattern(300, 1), pattern(20000, 2), "", pattern(7000, 3), "last"};
	return values;
}
const std::string& checkpoint_bytes() { static const std::string value = pattern(140000, 4); return value; }
const std::string& dependency_bytes() { static const std::string value = pattern(3100, 5); return value; }
constexpr std::size_t file_header_size = 28, file_header_v2_size = 60, batch_header_size = 24;  // Journal's private layout constants
const j::Identity journal_identity{'C', 'R', 'C', '3', '2', 'C', 'J'};

j::PreparedArtifact prepare(j::Journal& journal, std::string_view bytes) {
	auto builder = journal.prepare_artifact();
	while (!bytes.empty()) {
		auto count = std::min(bytes.size(), j::detail::artifact_chunk_size);
		builder.append_chunk(bytes.substr(0, count)); bytes.remove_prefix(count);
	}
	return builder.finish();
}
std::string read_artifact(j::ArtifactReader& reader) {
	std::string result(static_cast<std::size_t>(reader.descriptor().length), '\0');
	for (std::size_t offset = 0; offset < result.size();) {
		auto count = std::min(result.size() - offset, j::detail::artifact_chunk_size);
		auto read = reader.read_at(offset, std::span<char>(result.data() + offset, count));
		if (!read) { throw std::runtime_error("artifact stopped reading"); }
		offset += read;
	}
	return result;
}

// Three batches, a checkpoint with one dependency covering them, then the rest.
void generate_journal(const fs::path& directory) {
	j::PosixIO io(directory); j::Journal journal(io, 1 << 20);
	journal.create(journal_identity);
	for (std::size_t i = 0; i < 3; ++i) { journal.append_batch(batches()[i]); }
	auto dependency = prepare(journal, dependency_bytes()); auto bundle = prepare(journal, checkpoint_bytes());
	journal.publish_checkpoint(bundle, std::array{dependency}, 3);
	for (std::size_t i = 3; i < batches().size(); ++i) { journal.append_batch(batches()[i]); }
}

#ifdef JOURNAL_TURN_LOG
const j::TurnLogOptions turn_options{8 * 512, 512, 1};  // 512-byte blocks, 4 KiB segments
const j::Identity turn_identity{'C', 'R', 'C', 'T'};
using Groups = std::map<std::uint64_t, std::vector<j::TurnRecord>>;

// Deterministic turns of several groups; records cross blocks and turns cross segments.
Groups write_turns(j::TurnLog& log) {
	Groups model; std::mt19937_64 random(11);
	for (unsigned turn = 0; turn < 40; ++turn) {
		std::vector<j::TurnRecord> batch; std::map<std::uint64_t, std::uint64_t> next;
		for (unsigned n = random() % 3 + 1; n; --n) {
			auto group = random() % 4 + 1;
			auto& sequence = next.try_emplace(group, log.sequence(group)).first->second;
			std::string payload(random() % 900, '\0');
			for (std::size_t i = 0; i < payload.size(); ++i) { payload[i] = static_cast<char>('a' + (group * 7 + sequence * 3 + i) % 26); }
			batch.push_back({group, ++sequence, j::TurnRecordKind::Batch, payload});
		}
		log.append(batch);
		for (auto& record : batch) { model[record.group].push_back(record); }
	}
	return model;
}
void generate_turn_log(const fs::path& directory) {
	j::PosixIO io(directory); auto owner = io.acquire_owner(true);
	j::TurnRecovery recovered;
	j::TurnLog log(io, turn_identity, recovered, turn_options);
	write_turns(log);
}

#endif

fs::path scratch(const std::string& name) {
	auto path = fs::current_path() / ".scratch" / ("crc-fixture-" + name);
	fs::remove_all(path); fs::create_directories(path);
	fs::permissions(path, fs::perms::owner_all);
	return path;
}
fs::path copy_fixture(const std::string& name) {
	auto path = scratch(name);
	fs::copy(fs::path(JOURNAL_FIXTURE_DIR) / name, path, fs::copy_options::recursive);
	return path;
}
std::string slurp(const fs::path& path) {
	std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), {}};
}
void flip(const fs::path& path, std::uint64_t offset) {
	std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
	file.seekg(static_cast<std::streamoff>(offset)); char c; file.get(c);
	file.seekp(static_cast<std::streamoff>(offset)); file.put(static_cast<char>(c ^ 0x5a));
}

void primitive() {
	check(j::crc32c("123456789") == 0xe3069283u && j::crc32c("") == 0, "standard vectors");
	std::mt19937_64 random(7);
	auto bytes = pattern(200000, 6);
	// Every length around the hardware stride, at a few alignments.
	for (std::size_t offset : {0, 1, 3, 5}) {
		for (std::size_t size = 0; size <= 9300; size += (size < 300 ? 1 : 37)) {
			auto view = std::string_view(bytes).substr(offset, size);
			if (j::crc32c(view) != bitwise(view)) { check(false, "crc32c equals the bitwise CRC"); break; }
		}
	}
	check(j::crc32c(bytes) == bitwise(bytes), "crc32c over 200000 bytes equals the bitwise CRC");
	// Any fragmentation, with empty pieces, ends at the same value.
	for (int round = 0; round < 100; ++round) {
		auto size = random() % bytes.size(); auto whole = std::string_view(bytes).substr(0, size);
		j::Checksum checksum;
		for (std::size_t at = 0; at < size;) {
			auto piece = std::min<std::size_t>(size - at, random() % (round % 2 ? 6000 : 4));
			checksum.update(whole.substr(at, piece)); at += piece;
		}
		if (checksum.value() != j::crc32c(whole) || checksum.value() != bitwise(whole)) { check(false, "fragmented Checksum equals the whole CRC"); break; }
	}
	j::Checksum empty; empty.update(""); empty.update(std::string_view{});
	check(empty.value() == 0 && j::Checksum{}.value() == 0, "empty input has CRC 0");
}

void journal_fixture() {
	auto directory = copy_fixture("journal");
	std::vector<std::string> replayed;
	j::Frontier frontier;
	{
		j::PosixIO io(directory); j::Journal journal(io, 1 << 20);
		std::string checkpoint, dependency; std::size_t dependencies = 0;
		frontier = journal.recover([&](auto sequence, std::string_view batch) { check(sequence == replayed.size() + 4, "suffix replay is contiguous"); replayed.emplace_back(batch); },
			[&](const j::Frontier& selected, j::ArtifactReader& artifact, std::span<j::ArtifactReader> required) {
				checkpoint = read_artifact(artifact); dependencies = required.size();
				if (!required.empty()) { dependency = read_artifact(required[0]); }
				check(selected.sequence == 3 || selected.sequence == batches().size(), "checkpoint covers the first three batches");
			});
		check(checkpoint == checkpoint_bytes() && dependencies == 1 && dependency == dependency_bytes(), "checkpoint and dependency artifacts verify and read back");
	}
	check(frontier.sequence == batches().size() && frontier.base_sequence == 3 && frontier.version == 2, "recovered frontier");
	check(replayed == std::vector<std::string>(batches().begin() + 3, batches().end()), "batches after the checkpoint replay byte for byte");

	// The manifest, the generation file and every artifact header end in a CRC the bitwise reference reproduces.
	int manifests = 0, headers = 0;
	for (const auto& entry : fs::directory_iterator(directory)) {
		auto name = entry.path().filename().string(); auto bytes = slurp(entry.path());
		auto trailing = [&](std::size_t length) {
			std::string_view tail(bytes); tail = tail.substr(length - 4);
			return j::get32(tail);
		};
		if (name.starts_with("manifest") && !name.starts_with("manifest.pending-") && bytes.size() > 4) {
			++manifests; check(trailing(bytes.size()) == bitwise(std::string_view(bytes).substr(0, bytes.size() - 4)), "manifest CRC equals the bitwise CRC");
		}
		// The journal file (28-byte header) and the checkpoint generation (60): header CRC, then each batch header
		// (4 magic, 4 length, 8 sequence, 4 payload CRC, 4 header CRC) and payload.
		if (name.starts_with("journal-") || name.starts_with("generation-")) {
			auto header_size = name.starts_with("journal-") ? file_header_size : file_header_v2_size;
			++headers; check(trailing(header_size) == bitwise(std::string_view(bytes).substr(0, header_size - 4)), "file header CRC equals the bitwise CRC");
			std::size_t at = header_size; std::size_t batch = 0;
			while (at + batch_header_size <= bytes.size()) {
				std::string_view header(bytes.data() + at, batch_header_size); auto raw = header;
				j::get32(header); auto length = j::get32(header); j::get64(header); auto payload = j::get32(header);
				check(at + batch_header_size + length <= bytes.size(), "batch fits the file");
				check(j::get32(header) == bitwise(raw.substr(0, batch_header_size - 4)), "batch header CRC equals the bitwise CRC");
				check(payload == bitwise(std::string_view(bytes).substr(at + batch_header_size, length)), "batch payload CRC equals the bitwise CRC");
				at += batch_header_size + length; ++batch;
			}
			check(at == bytes.size() && batch == (name.starts_with("journal-") ? 3 : batches().size() - 3), "every batch in the file was checked");
		}
		if (name.starts_with("artifact-") && bytes.size() >= j::detail::artifact_ownership_size) {
			++headers; std::string_view prefix(bytes.data(), j::detail::artifact_ownership_size);
			check(trailing(j::detail::artifact_ownership_size) == bitwise(prefix.substr(0, j::detail::artifact_ownership_size - 4)), "artifact ownership CRC equals the bitwise CRC");
		}
	}
	check(manifests == 1 && headers == 4, "manifest, generation and artifact files were checked");

	// A flipped byte in the manifest, the generation or an artifact is still refused.
	for (const auto& entry : fs::directory_iterator(directory)) {
		auto name = entry.path().filename().string();
		if (!name.starts_with("manifest") && !name.starts_with("generation-") && !name.starts_with("artifact-")) { continue; }
		auto damaged = scratch("journal-damaged"); fs::copy(directory, damaged, fs::copy_options::recursive);
		// The generation's first batch is empty, so its second batch's payload starts two headers in.
		auto payload = file_header_v2_size + 2 * batch_header_size;
		// Each check is pinned by flipping the byte it covers and, separately, the CRC it compares against.
		auto last = fs::file_size(damaged / name) - 1;
		std::vector<std::uint64_t> offsets{20u, last};
		if (name.starts_with("generation-")) { offsets = {payload - 1, payload + 2, payload + 100, last}; }
		if (name.starts_with("artifact-")) { offsets = {100u, j::detail::artifact_ownership_size - 1, last}; }
		for (auto offset : offsets) {
			fs::remove_all(damaged); fs::copy(directory, damaged, fs::copy_options::recursive);
			flip(damaged / name, offset);
			check(throws([&] {
				j::PosixIO io(damaged); j::Journal journal(io, 1 << 20);
				journal.recover([](auto, auto) {}, [](const j::Frontier&, j::ArtifactReader& a, std::span<j::ArtifactReader> r) { read_artifact(a); for (auto& d : r) { read_artifact(d); } });
			}), "a flipped byte is refused");
		}
		fs::remove_all(damaged);
	}
	fs::remove_all(directory);
}

#ifdef JOURNAL_TURN_LOG
void turn_log_fixture() {
	auto directory = copy_fixture("turn-log");
	j::PosixIO io(directory); auto owner = io.acquire_owner(true);
	// The expected records come from the same deterministic workload, replayed into a scratch log.
	Groups expected;
	{
		auto reference = scratch("turn-log-reference"); j::PosixIO reference_io(reference); auto reference_owner = reference_io.acquire_owner(true);
		j::TurnRecovery ignored; j::TurnLog log(reference_io, turn_identity, ignored, turn_options);
		expected = write_turns(log);
	}
	j::TurnRecovery recovered;
	{ j::TurnLog log(io, turn_identity, recovered, turn_options); }
	check(!recovered.damaged && recovered.lost.empty() && recovered.epoch == 2, "turn log segments verify");
	check(recovered.groups == expected && !expected.empty(), "every turn-log record reads back");
	fs::remove_all(directory); fs::remove_all(scratch("turn-log-reference"));

	// A flipped byte anywhere in a segment, header or payload, is damage or a lost record, never a silent success.
	for (std::uint64_t offset : {40, 60, 100, 200, 300, 450, 511, 700, 1500, 3000}) {
		auto damaged = copy_fixture("turn-log");
		std::vector<fs::path> segments;
		for (const auto& entry : fs::directory_iterator(damaged)) { if (entry.path().filename().string().starts_with("turns-")) { segments.push_back(entry.path()); } }
		std::sort(segments.begin(), segments.end());
		check(!segments.empty(), "the fixture holds turn-log segments");
		if (!segments.empty() && fs::file_size(segments.front()) > offset) {
			flip(segments.front(), offset);
			j::PosixIO damaged_io(damaged); auto damaged_owner = damaged_io.acquire_owner(true);
			j::TurnRecovery after; j::TurnLog log(damaged_io, turn_identity, after, turn_options);
			check(after.damaged || !after.lost.empty() || after.groups != expected, "a damaged turn-log byte does not read back as intact");
		}
		fs::remove_all(damaged);
	}
}
#endif
}  // namespace

int main(int argc, char** argv) {
	try {
		if (argc == 3 && std::string_view(argv[1]) == "--generate") {
			fs::path root(argv[2]);
			for (auto name : {"journal", "turn-log"}) { fs::remove_all(root / name); fs::create_directories(root / name); fs::permissions(root / name, fs::perms::owner_all); }
			generate_journal(root / "journal");
#ifdef JOURNAL_TURN_LOG
			generate_turn_log(root / "turn-log");
			fs::remove(root / "turn-log" / "owner.lock");  // the test creates its own
#endif
			return 0;
		}
		primitive(); journal_fixture();
#ifdef JOURNAL_TURN_LOG
		turn_log_fixture();
#endif
	} catch (const std::exception& error) {
		std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
		return 1;
	}
	std::cout << checks << " checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
