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

// The block framing of journal's durable logs: the turn log (RFTW0001) and strata's
// manifest log. A stream is blocks of block_bytes, each opening with a header (magic,
// identity, segment, block number, epoch, an optional manifest of the block before it,
// CRC-32C); records split into fragments that never cross a block, each under its own
// CRC-32C and bound to the stream's segment and epoch. docs/mechanisms/framing.md.

#pragma once

#include "primitives.h"
#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kronuz::journal::framing {

// A block header: magic (8), identity (16), segment (8), block (4), epoch (8), the manifest's
// segment (8), block (4) and count (2), 16 bytes per manifest entry, CRC-32C (4).
inline constexpr std::size_t header_fixed = 58, fragment_header = 24;
inline constexpr std::uint16_t no_manifest = 0xffff;
// The unit a crash leaves old or new, and every page size's divisor.
inline constexpr std::size_t sector_bytes = 512;
inline constexpr std::size_t header_bytes(std::uint16_t count) { return header_fixed + (count == no_manifest ? 0 : 16 * std::size_t{count}) + 4; }
// A fragment's length is 16 bits; a block holds its header and at least one fragment byte.
inline constexpr bool valid_block_bytes(std::uint32_t bytes) { return bytes >= 256 && bytes <= 65535 + fragment_header; }

enum Fragment : std::uint8_t { Full = 1, First, Middle, Last };
// The groups with fragments in a block and their highest sequences, named by the next block's header.
struct Manifest { bool known = false; std::uint64_t segment = 0; std::uint32_t block = 0; std::map<std::uint64_t, std::uint64_t> groups; };

inline void put16(std::string& bytes, std::uint16_t value) { bytes.push_back(static_cast<char>(value)); bytes.push_back(static_cast<char>(value >> 8)); }
inline std::uint16_t get16(std::string_view bytes) { return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[0]) | static_cast<unsigned char>(bytes[1]) << 8); }
inline std::uint32_t peek32(std::string_view bytes) { return get32(bytes); }
inline std::uint64_t peek64(std::string_view bytes) { return get64(bytes); }

// What every block header and fragment of a stream is bound to. Without manifests, every
// header's count is no_manifest.
struct Stream {
	std::string_view magic;  // 8 bytes
	Identity identity{};
	std::uint32_t block_bytes = 0;
	std::uint64_t segment = 0, epoch = 0;
	bool manifests = true;
};

// Writes records as fragments into blocks from a stream offset, starting a block's header as
// the stream enters it and padding a block's tail too short for a fragment.
class Emitter {
public:
	// `filling` holds the groups with fragments in the block being filled, which the next
	// block's header names.
	Emitter(const Stream& stream, std::uint64_t position, Manifest filling = {})
		: stream_(stream), position_(position), filling_(std::move(filling)) {}
	// Returns the offset of the record's first fragment. A nonzero group is noted in the
	// manifest of each block the record touches.
	std::uint64_t record(std::string_view logical, std::uint64_t group = 0, std::uint64_t sequence = 0) {
		std::string_view rest(logical); bool first = true; std::uint64_t start = 0;
		const std::uint64_t block = stream_.block_bytes;
		while (!rest.empty()) {
			enter_block();
			auto room = block - position_ % block;
			if (room < fragment_header + 1) { pad(room); continue; }
			auto count = std::min<std::size_t>(rest.size(), room - fragment_header);
			bool last = count == rest.size();
			if (first) { start = position_; }
			if (group) { auto& highest = filling_.groups[group]; highest = std::max(highest, sequence); }
			fragment(first ? (last ? Full : First) : (last ? Last : Middle), rest.substr(0, count));
			rest.remove_prefix(count); first = false;
		}
		return start;
	}
	const std::string& bytes() const noexcept { return bytes_; }
	std::uint64_t position() const noexcept { return position_; }
	const Manifest& filling() const noexcept { return filling_; }
private:
	void enter_block() {
		const std::uint64_t block = stream_.block_bytes;
		if (position_ % block) { return; }
		auto start = bytes_.size();
		bytes_.append(stream_.magic); bytes_.append(stream_.identity.data(), stream_.identity.size());
		put64(bytes_, stream_.segment); put32(bytes_, static_cast<std::uint32_t>(position_ / block)); put64(bytes_, stream_.epoch);
		// A manifest larger than half a block is not kept: damage there costs conservatively.
		auto count = stream_.manifests && filling_.known && header_bytes(static_cast<std::uint16_t>(std::min<std::size_t>(filling_.groups.size(), 4096))) <= block / 2 ?
			static_cast<std::uint16_t>(filling_.groups.size()) : no_manifest;
		put64(bytes_, count == no_manifest ? 0 : filling_.segment); put32(bytes_, count == no_manifest ? 0 : filling_.block); put16(bytes_, count);
		if (count != no_manifest) { for (const auto& [group, sequence] : filling_.groups) { put64(bytes_, group); put64(bytes_, sequence); } }
		put32(bytes_, crc32c(std::string_view(bytes_).substr(start)));
		position_ += header_bytes(count);
		filling_ = Manifest{true, stream_.segment, static_cast<std::uint32_t>(position_ / block), {}};
	}
	void pad(std::uint64_t count) { bytes_.append(count, '\0'); position_ += count; }
	void fragment(Fragment type, std::string_view payload) {
		std::string header; put16(header, static_cast<std::uint16_t>(payload.size())); header.push_back(static_cast<char>(type)); header.push_back('\0');
		put64(header, stream_.segment); put64(header, stream_.epoch);
		Checksum checksum; checksum.update(header); checksum.update(payload);
		put32(bytes_, checksum.value()); bytes_.append(header); bytes_.append(payload);
		position_ += fragment_header + payload.size();
	}
	Stream stream_; std::uint64_t position_; std::string bytes_; Manifest filling_;
};

// Where Emitter::record leaves a stream without manifests after a `size`-byte record written
// from `position`: block headers, padding and fragment headers included, in constant time.
inline std::uint64_t record_end(const Stream& stream, std::uint64_t position, std::uint64_t size) {
	if (stream.manifests) { throw std::invalid_argument("framing: record_end needs a stream without manifests"); }
	if (!size) { return position; }
	const std::uint64_t block = stream.block_bytes, header = header_bytes(no_manifest), carried = block - header - fragment_header;
	if (position % block == 0) { position += header; }
	if (auto room = block - position % block; room < fragment_header + 1) { position += room + header; }  // padded
	const std::uint64_t first = block - position % block - fragment_header;
	if (size <= first) { return position + fragment_header + size; }
	size -= first;
	position += first + fragment_header;  // a block start: whole blocks of `carried` bytes, then the last
	const std::uint64_t whole = (size - 1) / carried;
	return position + whole * block + header + fragment_header + (size - whole * carried);
}

// A check's outcome: the bytes verify, or they fail, or the end of file cuts them.
enum class Check : std::uint8_t { ok, bad, cut };

// The header opening `block`, the bytes from a block start to its end or the end of file.
// Verifies everything but the epoch, which the header carries.
struct Header { Check check = Check::bad; std::uint64_t epoch = 0; std::uint16_t count = 0; std::size_t size = 0; };
inline Header parse_header(std::string_view block, const Stream& stream, std::uint64_t index) {
	Header header;
	if (block.size() < (stream.manifests ? header_fixed : header_bytes(no_manifest))) { header.check = Check::cut; return header; }
	header.count = get16(block.substr(56)); header.size = header_bytes(header.count);
	if (!stream.manifests && header.count != no_manifest) { return header; }
	if (header.size > stream.block_bytes) { return header; }
	if (header.size > block.size()) { header.check = Check::cut; return header; }
	if (block.substr(0, 8) != stream.magic || block.substr(8, 16) != std::string_view(stream.identity.data(), 16) || peek64(block.substr(24)) != stream.segment ||
		peek32(block.substr(32)) != index || peek32(block.substr(header.size - 4)) != crc32c(block.substr(0, header.size - 4))) { return header; }
	header.check = Check::ok; header.epoch = peek64(block.substr(36));
	return header;
}

// The fragment at the start of `at`, the bytes from it to its block's end or the end of file,
// `room` the bytes to its block's end. Its structure is checked before its extent, so a length
// that runs past its block is bad; one that runs past the end of file is cut.
struct FragmentView { Check check = Check::bad; std::uint8_t type = 0; std::string_view payload; };
inline FragmentView parse_fragment(std::string_view at, std::size_t room, std::uint64_t segment, std::uint64_t epoch) {
	FragmentView view;
	if (at.size() < fragment_header) { view.check = Check::cut; return view; }
	const auto length = get16(at.substr(4)); view.type = static_cast<std::uint8_t>(at[6]);
	if (!(length > 0 && fragment_header + length <= room && view.type >= Full && view.type <= Last && at[7] == '\0' &&
		peek64(at.substr(8)) == segment && peek64(at.substr(16)) == epoch)) { return view; }
	if (fragment_header + length > at.size()) { view.check = Check::cut; return view; }
	Checksum checksum; checksum.update(at.substr(4, fragment_header - 4)); checksum.update(at.substr(fragment_header, length));
	if (checksum.value() == peek32(at)) { view.check = Check::ok; view.payload = at.substr(fragment_header, length); }
	return view;
}

// Reads back the record whose first fragment starts at offset in a written stream.
inline std::string read_record(File& file, std::uint32_t block_bytes, std::uint64_t offset) {
	std::string logical, header(fragment_header, '\0');
	for (std::uint64_t position = offset;;) {
		if (position % block_bytes == 0) {
			std::string count(2, '\0');
			if (file.read_at(position + 56, count) != 2) { throw Corruption("short log read"); }
			position += header_bytes(get16(count));
		}
		auto room = block_bytes - position % block_bytes;
		if (room < fragment_header + 1) { position += room; continue; }
		if (file.read_at(position, header) != header.size()) { throw Corruption("short log read"); }
		auto length = get16(std::string_view(header).substr(4)); auto type = static_cast<std::uint8_t>(header[6]);
		std::string payload(length, '\0');
		if (!length || length > room - fragment_header || file.read_at(position + fragment_header, payload) != length) { throw Corruption("invalid log fragment"); }
		Checksum checksum; checksum.update(std::string_view(header).substr(4)); checksum.update(payload);
		if (checksum.value() != peek32(header) || (logical.empty() ? type != Full && type != First : type != Middle && type != Last)) { throw Corruption("log record changed since it was written"); }
		logical.append(payload); position += fragment_header + length;
		if (type == Full || type == Last) { return logical; }
	}
}

// Reads a whole file, a growing log's last block included.
inline std::string read_file(File& file) {
	std::string bytes(file.size(), '\0');
	std::size_t done = 0;
	while (done < bytes.size()) {
		auto count = file.read_at(done, std::span<char>(bytes.data() + done, bytes.size() - done));
		if (count == 0 || count > bytes.size() - done) { throw Corruption("short log read"); }
		done += count;
	}
	return bytes;
}

// How a growing log ends (docs/mechanisms/framing.md). Clean: the last record ends at the end
// of file. Torn: an append a crash left unfinished, which the caller drops. Damage: bytes
// that changed after they were written, or a size no crash leaves.
enum class Tail : std::uint8_t { clean, torn, damage };
struct Record { std::uint64_t offset = 0, end = 0; std::string bytes; };  // first fragment; past the last
struct Scan {
	std::vector<Record> records;  // the whole records before the stop, in order
	Tail tail = Tail::clean;
	std::uint64_t at = 0;  // where the read stopped: the end of file, or the failing header or fragment
};

namespace detail {
// Whether a whole record starts in a block after the one holding `at`, resynchronizing at block
// starts as a scan of whole blocks does after a gap.
inline bool whole_record_after(std::string_view bytes, std::uint64_t at, const Stream& stream) {
	const std::uint64_t block = stream.block_bytes, eof = bytes.size();
	bool assembling = false;
	for (std::uint64_t start = (at / block + 1) * block; start < eof; start += block) {
		auto view = bytes.substr(start, block);
		auto header = parse_header(view, stream, start / block);
		if (header.check != Check::ok || header.epoch != stream.epoch) { assembling = false; continue; }
		for (std::uint64_t position = header.size; block - position >= fragment_header + 1 && start + position < eof;) {
			auto fragment = parse_fragment(view.substr(position), block - position, stream.segment, stream.epoch);
			if (fragment.check != Check::ok) { assembling = false; break; }
			if (fragment.type == Full || (fragment.type == Last && assembling)) { return true; }
			assembling = fragment.type == First || (fragment.type == Middle && assembling);
			position += fragment_header + fragment.payload.size();
		}
	}
	return false;
}
}  // namespace detail

// Reads a growing log from its start. At the first header or fragment that does not verify,
// the tail rule decides from that point alone; the first row that matches decides:
//   a record left without its last fragment, the file ending at a sector multiple: torn;
//   a header or fragment the end of file cuts: damage (one write per append never cuts one);
//   one whose bytes are zeros to its block's end or the end of file: torn (never written);
//   anything else: damage.
// A torn verdict with a whole record after it is damage: one record is in flight at a time.
inline Scan scan(std::string_view bytes, const Stream& stream) {
	if (!valid_block_bytes(stream.block_bytes) || stream.magic.size() != 8) { throw std::invalid_argument("invalid log stream"); }
	Scan result;
	const std::uint64_t block = stream.block_bytes, eof = bytes.size();
	std::string assembling; bool partial = false; std::uint64_t first = 0;
	auto stop = [&](Check check, std::uint64_t at) {
		result.at = at;
		bool torn = false;
		if (check == Check::bad) {
			const auto end = std::min((at / block + 1) * block, eof);
			torn = std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(end), [](char c) { return c == '\0'; });
		}
		result.tail = torn && !detail::whole_record_after(bytes, at, stream) ? Tail::torn : Tail::damage;
		return result;
	};
	std::uint64_t position = 0;
	while (position < eof) {
		if (position % block == 0) {
			auto header = parse_header(bytes.substr(position, block), stream, position / block);
			if (header.check == Check::ok && header.epoch != stream.epoch) { header.check = Check::bad; }
			if (header.check != Check::ok) { return stop(header.check, position); }
			position += header.size;
		}
		const auto room = block - position % block;
		if (room < fragment_header + 1) { position += room; continue; }  // padding, never read
		if (position >= eof) { break; }
		auto fragment = parse_fragment(bytes.substr(position, room), room, stream.segment, stream.epoch);
		if (fragment.check != Check::ok) { return stop(fragment.check, position); }
		if (fragment.type == Full || fragment.type == First) {
			if (partial) { return stop(Check::bad, position); }
			assembling.assign(fragment.payload); partial = true; first = position;
		} else {
			if (!partial) { return stop(Check::bad, position); }
			assembling.append(fragment.payload);
		}
		position += fragment_header + fragment.payload.size();
		if (fragment.type == Full || fragment.type == Last) {
			result.records.push_back({first, position, std::move(assembling)});
			assembling.clear(); partial = false;
		}
	}
	result.at = eof;
	if (partial) {
		result.tail = eof % sector_bytes == 0 && !detail::whole_record_after(bytes, eof, stream) ? Tail::torn : Tail::damage;
	}
	return result;
}

}  // namespace kronuz::journal::framing
