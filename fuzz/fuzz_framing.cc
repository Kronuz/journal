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

// libFuzzer target for the growing-log reader and its tail rule (journal/framing.h). data[0]
// picks the stream: even, the manifest log's (512-byte blocks, no manifests); odd, 1 KiB blocks
// with manifests. Beyond memory safety it checks the reader's claims: records in order inside
// the file; a clean read ends at the end of file; a torn one stops at zeros or at a sector
// multiple; the records re-emit to the input's bytes outside padding; the prefix through the
// last whole record reads clean; read_record reads each record back.
#include "journal/framing.h"
#include "journal/memory_io.h"

#include <cstdlib>
#include <string>

namespace j = kronuz::journal;
namespace f = kronuz::journal::framing;

namespace {
const j::Identity identity{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const f::Stream manifest_log{"STRLOG01", identity, 512, 7, 1, false};
const f::Stream with_manifests{"RFTW0001", identity, 1024, 7, 1, true};

void require(bool value) { if (!value) { std::abort(); } }

// Offsets in an emitted stream that no reader reads.
bool padding_at(std::string_view bytes, const f::Stream& stream, std::uint64_t offset) {
	const std::uint64_t block = stream.block_bytes;
	for (std::uint64_t position = 0; position < bytes.size();) {
		if (position % block == 0) { position += f::header_bytes(f::get16(bytes.substr(position + 56))); }
		const auto room = block - position % block;
		if (room < f::fragment_header + 1) { if (offset >= position && offset < position + room) { return true; } position += room; continue; }
		if (position >= bytes.size()) { break; }
		position += f::fragment_header + f::get16(bytes.substr(position + 4));
	}
	return false;
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	if (!size) { return 0; }
	const auto& stream = data[0] % 2 ? with_manifests : manifest_log;
	const std::string_view bytes(reinterpret_cast<const char*>(data + 1), size - 1);
	const auto scan = f::scan(bytes, stream);

	std::uint64_t end = 0;
	for (const auto& record : scan.records) { require(record.offset >= end && record.end > record.offset && record.end <= bytes.size() && !record.bytes.empty()); end = record.end; }
	require(scan.at <= bytes.size() && scan.at >= end);
	if (scan.tail == f::Tail::clean) { require(scan.at == bytes.size()); }
	if (scan.tail == f::Tail::torn) {
		const auto block_end = std::min<std::uint64_t>((scan.at / stream.block_bytes + 1) * stream.block_bytes, bytes.size());
		const bool zeros = bytes.substr(scan.at, block_end - scan.at).find_first_not_of('\0') == std::string_view::npos;
		require(zeros || (scan.at == bytes.size() && bytes.size() % f::sector_bytes == 0));
	}

	// The prefix through the last whole record reads clean, with the same records.
	const auto prefix = f::scan(bytes.substr(0, end), stream);
	require(prefix.tail == f::Tail::clean && prefix.records.size() == scan.records.size());

	if (!stream.manifests) {
		// Without manifests the headers are fixed, so re-emitting the records rebuilds the input.
		f::Emitter emitter(stream, 0);
		for (const auto& record : scan.records) { emitter.record(record.bytes); }
		require(emitter.position() == end);
		for (std::uint64_t i = 0; i < end; ++i) { require(emitter.bytes()[i] == bytes[i] || padding_at(emitter.bytes(), stream, i)); }
	}

	if (!scan.records.empty()) {
		j::testing::Disk disk; j::testing::MemoryIO io(disk, "fuzz/");
		auto file = io.create_exclusive("log"); file->write_at(0, bytes);
		require(f::read_file(*file) == bytes);
		for (const auto& record : scan.records) { require(f::read_record(*file, stream, record.offset) == record.bytes); }
	}
	return 0;
}
