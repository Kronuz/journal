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

// libFuzzer target for the shared journal's decoders (Track C, T8): data[0] picks one.
//   0: the input is a turn-log segment (block headers, manifests, fragments); recovery
//      either throws Corruption or recovers, and a recovered log accepts a turn.
//   1: the input is a group's checkpoint record; a shared Store's recovery either throws
//      Corruption or recovers a frontier whose sequence is its base.
#include "faultlab/faultfs.h"
#include "journal/store.h"

#include <cstdlib>
#include <string>

namespace j = kronuz::journal;
namespace fl = kronuz::faultlab;

namespace {
const j::Identity identity{'F', 'U', 'Z', 'Z'};
const j::TurnLogOptions options{8 * 512, 512, 1, 4, 1024};
const j::AdmissionLimits limits{{64u << 20, 4096}, {1u << 20, 8}, {8u << 20, 8}, 64, 3};

void segment(std::string_view bytes) {
	fl::filesystem disk; disk.make_directory("log"); fl::io io(disk, "log");
	{ auto file = io.create_exclusive(j::turn_log_detail::segment_name(1)); file->write_at(0, bytes); file->sync(); }
	io.sync_directory();
	try {
		j::TurnRecovery recovered;
		j::TurnLog log(io, identity, recovered, options);
		std::vector<j::TurnRecord> turn;
		for (const auto& [group, records] : recovered.groups) { if (!recovered.lost.contains(group)) { turn.push_back({group, records.back().sequence + 1, j::TurnRecordKind::Batch, "x"}); } }
		if (turn.empty()) { turn.push_back({1u << 20, 1, j::TurnRecordKind::Batch, "x"}); }
		log.append(turn);
	} catch (const j::Corruption&) {}
}

void checkpoint_record(std::string_view bytes) {
	if (bytes.size() > 1024) { bytes = bytes.substr(0, 1024); }  // within one turn of the fuzz geometry
	fl::filesystem disk; disk.make_directory("host"); auto io = std::make_shared<fl::io>(disk, "host");
	{
		j::Host host(io, identity, j::HostOptions{1, options});
		j::Store store(host, 1, limits);
		store.create(identity);
		auto& turns = host.driver(0);
		turns.commit();
		turns.stage({1, 2, j::TurnRecordKind::Checkpoint, std::string(bytes), 1});
		turns.stage({1, 3, j::TurnRecordKind::Batch, "after"});
		turns.commit();
	}
	j::Host host(io, identity, j::HostOptions{1, options});
	j::Store store(host, 1, limits);
	try {
		auto frontier = store.recover([](std::uint64_t, std::string_view) {}, [](const j::Frontier&, j::ArtifactReader&, std::span<j::ArtifactReader>) {});
		if (frontier.sequence < frontier.base_sequence) { std::abort(); }
	} catch (const j::Corruption&) {
	} catch (const std::runtime_error&) {  // an artifact the record names does not exist
	}
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	if (!size) { return 0; }
	const std::string_view bytes(reinterpret_cast<const char*>(data + 1), size - 1);
#ifdef JOURNAL_FUZZ_DECODER
	const int decoder = JOURNAL_FUZZ_DECODER;  // pinned at build time; data[0] is still consumed
#else
	const int decoder = data[0] % 2;
#endif
	if (decoder == 0) { segment(bytes); } else { checkpoint_record(bytes); }
	return 0;
}
