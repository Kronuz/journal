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

#include <allocators/resources.h>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace kronuz::journal {
struct StorageResources {
	std::uint64_t logical_bytes = 0, entries = 0;
	bool operator==(const StorageResources&) const = default;
};
namespace detail {
inline allocators::ResourceVector<2> resource_vector(StorageResources value) noexcept {
	return {{value.logical_bytes, value.entries}};
}
inline StorageResources storage_resources(allocators::ResourceVector<2> value) noexcept {
	return {value.values[0], value.values[1]};
}
inline bool resources_fit(StorageResources value, StorageResources limit) noexcept {
	return resource_vector(value).fits(resource_vector(limit));
}
inline StorageResources resources_add(StorageResources a, StorageResources b) {
	return storage_resources(resource_vector(a).added(resource_vector(b)));
}
inline StorageResources resources_subtract(StorageResources a, StorageResources b) {
	return storage_resources(resource_vector(a).subtracted(resource_vector(b)));
}
} // namespace detail
} // namespace kronuz::journal
