// Writes the legacy-format fixtures: stores whose appends publish the manifest per batch.
// Built against journal 0b8a5f3 (the last revision that writes them):
//   generate DIRECTORY        creates DIRECTORY/v1 and DIRECTORY/v2
// v1: create, then the batch "first" (generation 1, manifest version 1).
// v2: v1, then a checkpoint covering it (bundle "first", dependency "application"), then the
//     batch "second" (generation 2, manifest version 2); generation 1 is left unreclaimed.
#include "journal/journal.h"
#include "completion/posix.h"
#include <array>
#include <filesystem>
#include <iostream>
namespace j = kronuz::journal;
namespace {
j::PreparedArtifact prepare(j::Journal& journal, std::string_view bytes) {
	auto builder = journal.prepare_artifact(); builder.append_chunk(bytes); return builder.finish();
}
}  // namespace
int main(int argc, char** argv) {
	if (argc != 2) { std::cerr << "generate DIRECTORY\n"; return 2; }
	j::Identity identity{}; identity[0] = 'A';
	for (auto version : {"v1", "v2"}) {
		auto directory = std::filesystem::path(argv[1]) / version;
		std::filesystem::create_directories(directory); ::chmod(directory.c_str(), 0700);
		j::PosixIO io(directory); j::Journal journal(io, 1024);
		journal.create(identity); journal.append_batch("first");
		if (std::string_view(version) == "v2") {
			auto bundle = prepare(journal, "first"); std::array dependencies{prepare(journal, "application")};
			journal.publish_checkpoint(bundle, dependencies, 1);
			journal.append_batch("second");
		}
	}
}
