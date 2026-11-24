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

// The flush mode a host's PosixIO is opened with decides every sync the journal makes, recorded
// through completion's sync seam (PosixIO::replace_sync_calls). `sync_mode_test [case]`:
//   host     a host in each mode: the turn barrier (commit() and run() on the native queue) and a
//            checkpoint's publication make the calls the mode names, and nothing weaker
//   refusal  full mode: the call the mode names failing fails the turn, with no weaker call after
//   table    completion's calls per sync site and mode, as recorded, against faultlab's
//            call_for/persists, and the two tables on every platform
#include "completion/native_completion.h"
#include "faultlab/faultfs.h"
#include "journal/host.h"
#include "journal/store.h"
#include "completion/posix.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <string>
#include <system_error>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace j = kronuz::journal;
namespace fl = kronuz::faultlab;

namespace {
int checks = 0, failures = 0;
void check(bool value, const std::string& what) {
	++checks;
	if (!value) { ++failures; std::cerr << "FAIL: " << what << '\n'; }
}

// The calls the seam records, in order; `writeback` is Linux's hint before a batch.
enum class Call { fsync, fdatasync, writeback, full_fsync, barrier_fsync };
const char* name(Call call) {
	switch (call) {
	case Call::fsync: return "fsync";
	case Call::fdatasync: return "fdatasync";
	case Call::writeback: return "writeback";
	case Call::full_fsync: return "F_FULLFSYNC";
	case Call::barrier_fsync: return "F_BARRIERFSYNC";
	}
	return "?";
}
std::string listing(const std::vector<Call>& calls) {
	std::string out = "[";
	for (std::size_t i = 0; i < calls.size(); ++i) { out += (i ? " " : "") + std::string(name(calls[i])); }
	return out + "]";
}
// The call completion's sync_call names, as the seam records it.
Call recorded(j::SyncCall call) {
	switch (call) {
	case j::SyncCall::fsync: return Call::fsync;
	case j::SyncCall::fdatasync: return Call::fdatasync;
	case j::SyncCall::full_fsync: return Call::full_fsync;
	case j::SyncCall::barrier_fsync: return Call::barrier_fsync;
	}
	return Call::fsync;
}

// Every call the seam sees, from any thread; `refuse`, when set, fails that call with ENOTSUP.
struct Recorder {
	std::mutex mutex;
	std::vector<Call> calls;
	std::optional<Call> refuse;
	bool note(Call call) {
		std::lock_guard hold(mutex);
		calls.push_back(call);
		if (refuse == call) { errno = ENOTSUP; return false; }
		return true;
	}
	std::vector<Call> take() { std::lock_guard hold(mutex); return std::exchange(calls, {}); }
	void arm(std::optional<Call> call) { std::lock_guard hold(mutex); refuse = call; }
};
Recorder recorder;
int record_fsync(int fd) { return recorder.note(Call::fsync) ? ::fsync(fd) : -1; }
#ifdef __APPLE__
int record_fcntl(int fd, int command) {
	if (command == F_FULLFSYNC && !recorder.note(Call::full_fsync)) { return -1; }
	if (command == F_BARRIERFSYNC && !recorder.note(Call::barrier_fsync)) { return -1; }
	return ::fcntl(fd, command);
}
#endif
#ifdef __linux__
int record_fdatasync(int fd) { return recorder.note(Call::fdatasync) ? ::fdatasync(fd) : -1; }
int record_writeback(int fd) { return recorder.note(Call::writeback) ? ::sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WRITE) : -1; }
#endif
// A PosixIO in `mode` whose syncs the recorder sees: installed before any file opens.
std::shared_ptr<j::PosixIO> open_io(const std::filesystem::path& directory, j::FlushMode mode) {
	auto io = std::make_shared<j::PosixIO>(directory, j::PosixIOOptions{mode});
#ifdef __APPLE__
	io->replace_sync_calls(&record_fsync, &record_fcntl);
#elif defined(__linux__)
	io->replace_sync_calls(&record_fsync, &record_fdatasync, &record_writeback);
#else
	io->replace_sync_calls(&record_fsync);
#endif
	return io;
}

std::filesystem::path fresh(const std::string& stem) {
	auto directory = std::filesystem::current_path() / ".scratch" / (stem + "-" + std::to_string(::getpid()));
	std::filesystem::remove_all(directory); std::filesystem::create_directories(directory); ::chmod(directory.c_str(), 0700);
	return directory;
}
const char* mode_name(j::FlushMode mode) { return mode == j::FlushMode::full ? "full" : "plain"; }
bool has(const std::vector<Call>& calls, Call call) { return std::find(calls.begin(), calls.end(), call) != calls.end(); }

// A completion driver over the native queue: each primitive runs on the queue's backend (on
// macOS the fallback worker's File::sync_data, straight on the file), polled to its end.
struct NativeDriver {
	j::NativeCompletionQueue queue;
	task::task<void> run(std::shared_ptr<j::IOOperation> operation, task::cancel_token = {}) {
		while (!operation->done()) {
			if (!queue.submit(operation)) { throw std::logic_error("native queue busy"); }
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			for (;;) {
				if (auto result = queue.poll()) {
					if (result->operation != operation || !operation->complete(std::move(result->completion))) { throw std::logic_error("completion ownership"); }
					break;
				}
				if (std::chrono::steady_clock::now() > deadline) { throw std::runtime_error("native queue deadline"); }
				pollfd notification{queue.notification_descriptor(), POLLIN, 0};
				::poll(&notification, 1, 1);
			}
		}
		co_return;
	}
};

const j::Identity host_identity{'S', 'Y', 'N', 'C'};
const j::TurnLogOptions turn_options{64 * 1024, 1024, 1, 2, 4096};
const j::AdmissionLimits limits{{64u << 20, 4096}, {1u << 20, 8}, {8u << 20, 8}, 64, 3};

// The calls a host in `mode` makes for a turn committed inline, a turn run on the native queue,
// and a checkpoint's publication.
struct Windows { std::vector<Call> commit, run, checkpoint; };
Windows host_windows(j::FlushMode mode) {
	const auto directory = fresh(std::string("sync-mode-host-") + mode_name(mode));
	Windows windows;
	{
		j::Host host(open_io(directory, mode), host_identity, j::HostOptions{1, turn_options});
		auto& turns = host.driver(0);
		host.create_group(7);
		turns.stage({7, 1, j::TurnRecordKind::Checkpoint, "m1", 0}); turns.commit();
		j::Store store(host, 42, limits);
		store.create({'G'});
		while (!turns.idle()) { turns.commit(); }  // the group's first checkpoint record
		while (!store.inventory_step(64).complete) {}
		recorder.take();

		turns.stage({7, 2, j::TurnRecordKind::Batch, "inline", 0}); turns.commit();
		windows.commit = recorder.take();

		NativeDriver native;
		task::inline_scheduler loop;
		task::cancel_source stop;
		bool ran = false; std::exception_ptr error;
		auto runner = [&]() -> task::task<void> {
			try { co_await turns.run(j::completion_ref::of(native), stop.token()); } catch (...) { error = std::current_exception(); }
			ran = true;
		};
		task::spawn(task::scheduler_ref::of(loop), runner());
		loop.run();
		turns.stage({7, 3, j::TurnRecordKind::Batch, "driven", 0});
		loop.run();
		windows.run = recorder.take();
		stop.request(); loop.run();
		check(ran && !error && turns.sequence(7) == 3, std::string(mode_name(mode)) + ": run() writes the staged turn and stops");

		auto id = store.reserve_replacement(1024, 1u << 20);
		store.begin_artifact(*id, j::ArtifactPart::Application); store.write_chunk(*id, "app"); store.finish_artifact(*id);
		store.begin_artifact(*id, j::ArtifactPart::Bundle); store.write_chunk(*id, "bundle"); store.finish_artifact(*id);
		auto publication = store.begin_publication(*id, store.frontier().sequence);
		turns.commit(); publication->result();
		windows.checkpoint = recorder.take();
	}
	std::filesystem::remove_all(directory);
	return windows;
}

// macOS: full makes F_FULLFSYNC and never a bare barrier, ending on a full flush; plain never
// makes F_FULLFSYNC. A turn's data barrier is exactly the call sync_call names for File::sync_data.
void follows(j::FlushMode mode, const std::vector<Call>& calls, const std::string& what, bool turn) {
	const auto at = std::string(mode_name(mode)) + " " + what + " " + listing(calls);
	if (turn && j::host_sync_platform != j::SyncPlatform::linux_kernel) {
		check(calls == std::vector<Call>{recorded(j::sync_call(j::SyncSite::data, mode))}, "a turn barrier is one call, the one the mode names: " + at);
	}
	if (j::host_sync_platform != j::SyncPlatform::macos) { return; }
	if (mode == j::FlushMode::full) {
		// Each fsync only stages: the next call that is not an fsync is the drive flush.
		bool staged = true;
		for (std::size_t i = 0; i < calls.size(); ++i) {
			if (calls[i] != Call::fsync) { continue; }
			auto next = i; while (next < calls.size() && calls[next] == Call::fsync) { ++next; }
			staged &= next < calls.size() && calls[next] == Call::full_fsync;
		}
		check(has(calls, Call::full_fsync) && !has(calls, Call::barrier_fsync) && staged, "full mode flushes the drive after every fsync: " + at);
	} else {
		check(!has(calls, Call::full_fsync) && has(calls, Call::fsync), "plain mode never flushes the drive: " + at);
	}
}

void host() {
	const auto full = host_windows(j::FlushMode::full), plain = host_windows(j::FlushMode::plain);
	for (const auto& [mode, windows] : {std::pair{j::FlushMode::full, full}, std::pair{j::FlushMode::plain, plain}}) {
		std::cout << mode_name(mode) << ": commit " << listing(windows.commit) << ", run " << listing(windows.run) << ", checkpoint " << listing(windows.checkpoint) << '\n';
		follows(mode, windows.commit, "turn barrier (commit)", true);
		follows(mode, windows.checkpoint, "checkpoint publication", false);
		// Linux's io_uring and FreeBSD's AIO issue the run's barrier themselves, past the seam.
		if (j::host_sync_platform == j::SyncPlatform::macos) { follows(mode, windows.run, "turn barrier (run, native queue)", true); }
	}
	if (j::host_sync_platform != j::SyncPlatform::macos) {
		check(full.commit == plain.commit && full.run == plain.run && full.checkpoint == plain.checkpoint,
			"off macOS both modes make the same calls: commit " + listing(full.commit) + " vs " + listing(plain.commit)
			+ ", checkpoint " + listing(full.checkpoint) + " vs " + listing(plain.checkpoint));
	}
}

// Full mode: the call the mode names for the turn barrier fails, and the turn fails with its
// errno, with no other sync after it; plain mode on macOS never makes that call.
void refusal() {
	const Call refused = recorded(j::sync_call(j::SyncSite::data, j::FlushMode::full));
	for (auto mode : {j::FlushMode::full, j::FlushMode::plain}) {
		const auto directory = fresh(std::string("sync-mode-refusal-") + mode_name(mode));
		{
			j::Host host(open_io(directory, mode), host_identity, j::HostOptions{1, turn_options});
			auto& turns = host.driver(0);
			host.create_group(7);
			turns.stage({7, 1, j::TurnRecordKind::Checkpoint, "m1", 0}); turns.commit();
			recorder.take();
			recorder.arm(refused);
			turns.stage({7, 2, j::TurnRecordKind::Batch, "refused", 0});
			int error = 0;
			try { turns.commit(); } catch (const std::system_error& e) { error = e.code().value(); }
			recorder.arm(std::nullopt);
			const auto calls = recorder.take();
			const bool reaches = mode == j::FlushMode::full || j::host_sync_platform != j::SyncPlatform::macos;
			const auto at = std::string(mode_name(mode)) + " " + listing(calls);
			if (reaches) {
				check(error == ENOTSUP, "the turn fails with the refused call's errno: " + at);
				check(calls == std::vector<Call>{refused}, "nothing weaker follows a refused call: " + at);
				check(turns.failed(), "the refused turn fails the log: " + at);
			} else {
				check(error == 0 && turns.sequence(7) == 2 && !has(calls, Call::full_fsync), "plain mode never makes the refused call: " + at);
			}
		}
		std::filesystem::remove_all(directory);
	}
}

fl::flush_mode model(j::FlushMode mode) { return mode == j::FlushMode::full ? fl::flush_mode::full : fl::flush_mode::plain; }
fl::sync_site model(j::SyncSite site) {
	switch (site) {
	case j::SyncSite::file: return fl::sync_site::file;
	case j::SyncSite::data: return fl::sync_site::data;
	case j::SyncSite::directory: return fl::sync_site::directory;
	case j::SyncSite::order: return fl::sync_site::order;
	}
	return fl::sync_site::file;
}
fl::sync_platform model(j::SyncPlatform platform) {
	switch (platform) {
	case j::SyncPlatform::macos: return fl::sync_platform::macos;
	case j::SyncPlatform::linux_kernel: return fl::sync_platform::linux_kernel;
	case j::SyncPlatform::other: return fl::sync_platform::other;
	}
	return fl::sync_platform::other;
}
fl::sync_call model(Call call) {
	switch (call) {
	case Call::fsync: case Call::writeback: return fl::sync_call::fsync;
	case Call::fdatasync: return fl::sync_call::fdatasync;
	case Call::full_fsync: return fl::sync_call::full_fsync;
	case Call::barrier_fsync: return fl::sync_call::barrier_fsync;
	}
	return fl::sync_call::fsync;
}
const char* model_name(fl::sync_call call) {
	switch (call) {
	case fl::sync_call::fsync: return "fsync";
	case fl::sync_call::fdatasync: return "fdatasync";
	case fl::sync_call::full_fsync: return "F_FULLFSYNC";
	case fl::sync_call::barrier_fsync: return "F_BARRIERFSYNC";
	}
	return "?";
}

// The calls completion's PosixIO makes at each sync site, recorded on this platform, against
// faultlab's model: a site's last call (the one after its staged fsyncs) is call_for's, so a call
// completion weakens cannot stay "persists" in the model. Then the two tables, every platform.
void table() {
	const j::SyncSite sites[] = {j::SyncSite::file, j::SyncSite::data, j::SyncSite::directory, j::SyncSite::order};
	for (auto mode : {j::FlushMode::full, j::FlushMode::plain}) {
		const auto directory = fresh(std::string("sync-mode-table-") + mode_name(mode));
		{
			auto io = open_io(directory, mode);
			auto owner = io->acquire_owner(true);
			auto file = io->create_exclusive("file");
			file->write_at(0, "bytes");
			recorder.take();
			for (auto site : sites) {
				switch (site) {
				case j::SyncSite::file: file->sync(); break;
				case j::SyncSite::data: file->sync_data(); break;
				case j::SyncSite::directory: io->sync_directory(); break;
				case j::SyncSite::order: { j::File* files[1]{file.get()}; io->order_files_and_directory(files); break; }
				}
				const auto calls = recorder.take();
				const auto expected = fl::call_for(model(site), model(mode), fl::host_platform);
				const auto at = std::string(mode_name(mode)) + " site " + std::to_string(static_cast<int>(site)) + ": recorded " + listing(calls) + ", model " + model_name(expected);
				check(!calls.empty() && model(calls.back()) == expected, "the site's call is the model's: " + at);
				check(!calls.empty() && fl::persists(model(calls.back()), fl::host_platform) == fl::persists(expected, fl::host_platform), "the model's persists matches the call made: " + at);
			}
		}
		std::filesystem::remove_all(directory);
	}
	for (auto platform : {j::SyncPlatform::macos, j::SyncPlatform::linux_kernel, j::SyncPlatform::other}) {
		for (auto mode : {j::FlushMode::full, j::FlushMode::plain}) {
			for (auto site : sites) {
				const auto completion = model(recorded(j::sync_call(site, mode, platform)));
				check(completion == fl::call_for(model(site), model(mode), model(platform)),
					std::string("completion's sync_call and faultlab's call_for agree: platform ") + std::to_string(static_cast<int>(platform)) + " " + mode_name(mode) + " site " + std::to_string(static_cast<int>(site)));
			}
		}
	}
}
}  // namespace

int main(int argc, char** argv) {
	const std::string only = argc > 1 ? argv[1] : "";
	const std::pair<const char*, void (*)()> cases[] = {{"host", host}, {"refusal", refusal}, {"table", table}};
	bool ran = false;
	for (const auto& [name, run] : cases) {
		if (!only.empty() && only != name) { continue; }
		ran = true;
		try { run(); } catch (const std::exception& error) { check(false, std::string(name) + ": " + error.what()); }
	}
	if (!ran) { std::cerr << "unknown case " << only << '\n'; return 2; }
	std::cout << checks << " checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
