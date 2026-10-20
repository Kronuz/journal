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

#pragma once

#include <crc32c/crc32c.hh>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace kronuz::journal {

class Corruption : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

// CRC-32C lives in the crc32c library; these names forward to it.
using Checksum = ::kronuz::crc32c::Crc32c;
inline std::uint32_t crc32c(std::string_view bytes) noexcept { return ::kronuz::crc32c::crc32c(bytes); }

inline void put32(std::string& bytes, std::uint32_t value) {
	for (unsigned i = 0; i < 4; ++i) { bytes.push_back(static_cast<char>(value >> (i * 8))); }
}
inline void put64(std::string& bytes, std::uint64_t value) {
	for (unsigned i = 0; i < 8; ++i) { bytes.push_back(static_cast<char>(value >> (i * 8))); }
}
inline std::uint32_t get32(std::string_view& bytes) {
	if (bytes.size() < 4) { throw Corruption("truncated integer"); }
	std::uint32_t value = 0;
	for (unsigned i = 0; i < 4; ++i) { value |= std::uint32_t(static_cast<unsigned char>(bytes[i])) << (i * 8); }
	bytes.remove_prefix(4);
	return value;
}
inline std::uint64_t get64(std::string_view& bytes) {
	if (bytes.size() < 8) { throw Corruption("truncated integer"); }
	std::uint64_t value = 0;
	for (unsigned i = 0; i < 8; ++i) { value |= std::uint64_t(static_cast<unsigned char>(bytes[i])) << (i * 8); }
	bytes.remove_prefix(8);
	return value;
}

} // namespace kronuz::journal
