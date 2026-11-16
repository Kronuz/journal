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

// Idle cost per group on one host (A3, Q3 (b)): the process's open descriptors with N groups
// open and idle, each holding a published checkpoint (two artifacts) and a batch, then with all
// N closed, on PosixIO. `journal_idle_cost DIR [N...]` (default 0 100 1000); DIR is scratch.
#include "completion/posix.h"
#include "journal/store.h"
#include <cstdio>
#include <filesystem>
#include <sys/stat.h>

namespace j = kronuz::journal;

namespace {
std::size_t descriptors() {
	std::size_t count = 0;
#ifdef __linux__
	for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) { (void)entry; ++count; }
#else
	for (const auto& entry : std::filesystem::directory_iterator("/dev/fd")) { (void)entry; ++count; }
#endif
	return count;  // the listing's own descriptor included, in every count alike
}
void commit(j::Host& host) { for (std::uint32_t l = 0; l < host.loops(); ++l) { while (!host.driver(l).idle()) { host.driver(l).commit(); } } }
}  // namespace

int main(int argc, char** argv) {
	if (argc < 2) { std::fprintf(stderr, "usage: journal_idle_cost DIR [N...]\n"); return 2; }
	const std::filesystem::path base(argv[1]);
	std::vector<std::uint64_t> counts;
	for (int i = 2; i < argc; ++i) { counts.push_back(std::stoull(argv[i])); }
	if (counts.empty()) { counts = {0, 100, 1000}; }
	const j::AdmissionLimits limits{{64u << 20, 4096}, {1u << 20, 8}, {8u << 20, 8}, 64, 3};
	const j::HostOptions options{2, {1u << 20, 32u << 10, 1, 4, 256u << 10}};
	std::printf("%8s %10s %10s %10s %10s %14s %16s\n", "groups", "process", "host", "open idle", "closed", "per open group", "per closed group");
	bool zero = true;
	for (auto count : counts) {
		const auto directory = base / ("host-" + std::to_string(count));
		std::filesystem::remove_all(directory); std::filesystem::create_directories(directory); ::chmod(directory.c_str(), 0700);
		const auto process = descriptors();
		std::size_t host_only = 0, open_idle = 0, closed = 0;
		{
			j::Host host(std::make_shared<j::PosixIO>(directory), {'I', 'D', 'L', 'E'}, options);
			host_only = descriptors();
			std::vector<std::unique_ptr<j::Store>> stores;
			for (std::uint64_t g = 1; g <= count; ++g) {
				auto& store = *stores.emplace_back(std::make_unique<j::Store>(host, g, limits));
				store.create({'G', static_cast<char>(g), static_cast<char>(g >> 8)});
				while (!store.inventory_step(64).complete) {}
			}
			commit(host);
			std::vector<std::shared_ptr<j::StorePublication>> publications;
			for (auto& store : stores) {
				auto id = store->reserve_replacement(1024, 1u << 20);
				store->begin_artifact(*id, j::ArtifactPart::Application); store->write_chunk(*id, "application"); store->finish_artifact(*id);
				store->begin_artifact(*id, j::ArtifactPart::Bundle); store->write_chunk(*id, "bundle"); store->finish_artifact(*id);
				publications.push_back(store->begin_publication(*id, store->frontier().sequence));
			}
			commit(host);
			for (auto& publication : publications) { publication->result(); }
			publications.clear();
			std::vector<std::shared_ptr<j::StoreAppend>> appends;
			for (auto& store : stores) { appends.push_back(store->begin_append(std::move(*store->reserve_append(j::AdmissionClass::Normal, 5)), "batch")); }
			commit(host);
			for (auto& append : appends) { append->result(); }
			appends.clear();
			open_idle = descriptors();
			// Closing needs a checkpoint covering the batch: one more publication each.
			for (auto& store : stores) {
				auto id = store->reserve_replacement(1024, 1u << 20);
				store->begin_artifact(*id, j::ArtifactPart::Application); store->write_chunk(*id, "application"); store->finish_artifact(*id);
				store->begin_artifact(*id, j::ArtifactPart::Bundle); store->write_chunk(*id, "bundle"); store->finish_artifact(*id);
				publications.push_back(store->begin_publication(*id, store->frontier().sequence));
			}
			commit(host);
			for (auto& publication : publications) { publication->result(); }
			for (auto& store : stores) { store->close(); }
			stores.clear();
			closed = descriptors();
		}
		const double per_open = count ? double(open_idle) - double(host_only) : 0, per_closed = count ? double(closed) - double(host_only) : 0;
		std::printf("%8llu %10zu %10zu %10zu %10zu %14.4f %16.4f\n", static_cast<unsigned long long>(count), process, host_only, open_idle, closed,
			count ? per_open / double(count) : 0.0, count ? per_closed / double(count) : 0.0);
		zero &= open_idle == host_only && closed == host_only;
		std::filesystem::remove_all(directory);
	}
	std::printf("%s\n", zero ? "idle groups hold no descriptor" : "IDLE GROUPS HOLD DESCRIPTORS");
	return zero ? 0 : 1;
}
