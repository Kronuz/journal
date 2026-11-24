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

// A Host's group ids and heap on a memory disk: a Store's id across close, release and reopen, the
// heap per parked group, and the directory barriers of reclamation. Run by journal_test.cc.
#include "journal_support.h"


// Live heap bytes, less those a Metered IO or file call makes for the memory disk (reopen_*):
// a difference of two readings is the host's state alone. TSan's runtime defines operator new,
// so under TSan nothing is counted and the heap cases skip their heap checks, saying so.
#if defined(__SANITIZE_THREAD__)
#define JOURNAL_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define JOURNAL_TEST_TSAN 1
#endif
#endif
namespace heap {
#ifdef JOURNAL_TEST_TSAN
constexpr bool counted = false;
#else
constexpr bool counted = true;
#endif
struct alignas(16) Header { std::size_t size; bool metered; };
std::atomic<std::int64_t> bytes{0};
thread_local int off = 0;
struct Off { Off() { ++off; } ~Off() { --off; } };
// Whether the case's heap check runs; under TSan it prints why not.
bool checked(std::string_view name) {
	if (!counted) { std::cout << name << ": heap check skipped: TSan's runtime defines operator new, so the heap is not counted\n"; }
	return counted;
}
}  // namespace heap
#ifndef JOURNAL_TEST_TSAN
void* operator new(std::size_t size) {
	auto* header = static_cast<heap::Header*>(std::malloc(size + sizeof(heap::Header)));
	if (!header) { throw std::bad_alloc(); }
	header->size = size; header->metered = heap::off == 0;
	if (header->metered) { heap::bytes.fetch_add(static_cast<std::int64_t>(size), std::memory_order_relaxed); }
	return header + 1;
}
void operator delete(void* pointer) noexcept {
	if (!pointer) { return; }
	auto* header = static_cast<heap::Header*>(pointer) - 1;
	if (header->metered) { heap::bytes.fetch_sub(static_cast<std::int64_t>(header->size), std::memory_order_relaxed); }
	std::free(header);
}
void operator delete(void* pointer, std::size_t) noexcept { operator delete(pointer); }
#endif

namespace {
// A Store's id in a live Host (docs/mechanisms/group-lifetime.md): a close into the table and a
// release that leaves the records in the log both let the id recover in the same Host; two live
// Stores never hold it.
namespace fl = kronuz::faultlab;
const Identity reopen_host{'R', 'H'};
const TurnLogOptions reopen_turns{16 * 1024, 1024, 1, 4, 4096};
const AdmissionLimits reopen_limits{{64u << 20, 4096}, {1u << 20, 8}, {8u << 20, 8}, 64, 3};
Identity group_identity(std::uint64_t group) { Identity id{'G'}; for (unsigned i = 0; i < 8; ++i) { id[1 + i] = static_cast<char>(group >> (8 * i)); } return id; }
std::string join(const std::vector<std::string>& batches) {
	std::string text;
	for (const auto& batch : batches) { text += (text.empty() ? "" : ",") + batch; }
	return text;
}
std::vector<std::string> split(std::string_view text) {
	std::vector<std::string> batches;
	while (!text.empty()) { const auto comma = text.find(','); batches.emplace_back(text.substr(0, comma)); text.remove_prefix(comma == text.npos ? text.size() : comma + 1); }
	return batches;
}
bool prefix(const std::vector<std::string>& shorter, const std::vector<std::string>& longer) {
	return shorter.size() <= longer.size() && std::equal(shorter.begin(), shorter.end(), longer.begin());
}
std::string text(const std::vector<std::string>& batches) { return "[" + join(batches) + "]"; }

// Bytes and files the host writes to its group table, for park_count's cost per transition.
std::uint64_t table_bytes = 0, table_files = 0;
// The memory disk's files and calls, kept off the heap reading.
class MeteredFile final : public File {
public:
	explicit MeteredFile(std::unique_ptr<File> file, bool table = false) : file_(std::move(file)), table_(table) { table_files += table; }
	~MeteredFile() override { heap::Off off; file_.reset(); }
	std::uint64_t size() override { heap::Off off; return file_->size(); }
	std::size_t read_at(std::uint64_t offset, std::span<char> bytes) override { heap::Off off; return file_->read_at(offset, bytes); }
	std::size_t write_at(std::uint64_t offset, std::string_view bytes) override { heap::Off off; if (table_) { table_bytes += bytes.size(); } return file_->write_at(offset, bytes); }
	void truncate(std::uint64_t size) override { heap::Off off; file_->truncate(size); }
	void sync() override { heap::Off off; file_->sync(); }
private:
	std::unique_ptr<File> file_;
	bool table_;
};
class MeteredIO final : public IO {
public:
	explicit MeteredIO(std::shared_ptr<IO> io) : io_(std::move(io)) {}
	std::unique_ptr<OwnerLock> acquire_owner(bool create) override { heap::Off off; return io_->acquire_owner(create); }
	void open_existing_read_only_into(std::string_view name, File& file) override { heap::Off off; io_->open_existing_read_only_into(name, file); }
	bool open_reclaim_candidate_read_only_into(std::string_view name, File& file) override { heap::Off off; return io_->open_reclaim_candidate_read_only_into(name, file); }
	std::unique_ptr<File> open_reclaim_candidate_read_only(std::string_view name) override { return wrap([&] { return io_->open_reclaim_candidate_read_only(name); }); }
	std::unique_ptr<File> open_existing(std::string_view name) override { return wrap([&] { return io_->open_existing(name); }); }
	std::unique_ptr<File> open_existing_read_only(std::string_view name) override { return wrap([&] { return io_->open_existing_read_only(name); }); }
	std::unique_ptr<File> create_exclusive(std::string_view name) override { return wrap([&] { return io_->create_exclusive(name); }, name.starts_with("groups-")); }
	void replace(std::string_view source, std::string_view destination) override { heap::Off off; unordered_ = true; io_->replace(source, destination); }
	void remove(std::string_view name) override { heap::Off off; unordered_ = true; io_->remove(name); }
	std::unique_ptr<DirectoryCursor> scan_directory() override { heap::Off off; return io_->scan_directory(); }
	std::optional<EntryFootprint> entry_footprint(std::string_view name) override { heap::Off off; return io_->entry_footprint(name); }
	void sync_directory() override { heap::Off off; namespace_barriers += std::exchange(unordered_, false); io_->sync_directory(); }
	// Directory syncs that follow a removal or replacement; an ordering point is one here.
	std::size_t namespace_barriers = 0;
protected:
	std::unique_ptr<OwnerLock> lock_owner(const OwnerLockOptions& options) override { heap::Off off; return io_->acquire_owner(options); }
private:
	template <class Open> std::unique_ptr<File> wrap(Open&& open, bool table = false) {
		std::unique_ptr<File> file;
		{ heap::Off off; file = open(); }
		return file ? std::make_unique<MeteredFile>(std::move(file), table) : nullptr;
	}
	std::shared_ptr<IO> io_;
	bool unordered_ = false;
};

// The host's directory on `disk`, made on first use.
std::shared_ptr<IO> host_io(fl::filesystem& disk) {
	const auto names = disk.list();
	if (std::find(names.begin(), names.end(), "host") == names.end()) { disk.make_directory("host"); }
	return std::make_shared<fl::io>(disk, "host");
}
// One host on a memory disk.
struct Shared {
	std::unique_ptr<Host> host;
	explicit Shared(fl::filesystem& disk, TurnLogOptions turns = reopen_turns)
		: host(std::make_unique<Host>(std::make_shared<MeteredIO>(host_io(disk)), reopen_host, HostOptions{1, turns})) {}
	TurnDriver& turns() { return host->driver(0); }
	void commit() { while (!turns().idle()) { turns().commit(); } }
	// Creates the group or recovers it; `state` is its checkpoint's batches, then those replayed.
	std::unique_ptr<Store> open(std::uint64_t group, std::vector<std::string>* state = nullptr) {
		auto store = std::make_unique<Store>(*host, group, reopen_limits);
		std::vector<std::string> recovered;
		if (host->status(group) == GroupStatus::none) { store->create(group_identity(group)); }
		else {
			std::vector<std::string> batches;
			store->recover([&](std::uint64_t, std::string_view bytes) { batches.emplace_back(bytes); },
				[&](const Frontier&, ArtifactReader& bundle, std::span<ArtifactReader>) { recovered = split(read_artifact(bundle)); });
			recovered.insert(recovered.end(), batches.begin(), batches.end());
		}
		if (state) { *state = std::move(recovered); }
		commit();
		while (!store->inventory_step(64).complete) {}
		return store;
	}
	std::shared_ptr<StoreAppend> stage(Store& store, const std::string& batch) {
		return store.begin_append(std::move(*store.reserve_append(AdmissionClass::Normal, batch.size())), batch);
	}
	void append(Store& store, const std::string& batch) { auto operation = stage(store, batch); commit(); operation->result(); }
	// A checkpoint whose bundle names the batches it covers.
	void checkpoint(Store& store, const std::vector<std::string>& batches) {
		auto id = store.reserve_replacement(1024, 1u << 20);
		store.begin_artifact(*id, ArtifactPart::Application); store.write_chunk(*id, "application"); store.finish_artifact(*id);
		store.begin_artifact(*id, ArtifactPart::Bundle); store.write_chunk(*id, join(batches)); store.finish_artifact(*id);
		auto publication = store.begin_publication(*id, store.frontier().sequence);
		commit(); publication->result();
	}
};
// Host::settle and TurnDriver::evicted where they exist, so the cases also build at main.
template <class H> void settle(H& host) { if constexpr (requires { host.settle(0u); }) { host.settle(0u); } }
template <class D> bool evicted(D& turns, std::uint64_t group) {
	if constexpr (requires { turns.evicted(group); }) { return turns.evicted(group); } else { return false; }
}
// A recover of the group refused as busy, as a held owner lock is.
bool busy(Shared& node, std::uint64_t group) {
	Store store(*node.host, group, reopen_limits);
	try { store.recover([](std::uint64_t, std::string_view) {}, [](const Frontier&, ArtifactReader&, std::span<ArtifactReader>) {}); }
	catch (const std::system_error& error) { return error.code() == std::errc::operation_would_block; }
	catch (const std::exception&) { return false; }
	return false;
}

}  // namespace
// A1, clean: a close needs its records settled and a checkpoint covering them; once the table
// holds it, the group recovers in the live host, even while the closed Store still exists.
void reopen_clean_close() {
	fl::filesystem disk;
	std::vector<std::string> state;
	{
		Shared node(disk);
		auto a = node.open(7);
		node.append(*a, "a1"); node.append(*a, "a2");
		auto staged = node.stage(*a, "a3");
		check(throws([&] { a->close(); }) && !a->fenced(), "a close with a record staged is refused");
		node.commit(); staged->result(); staged.reset();
		check(throws([&] { a->close(); }) && !a->fenced(), "a close with batches no checkpoint covers is refused");
		node.checkpoint(*a, {"a1", "a2", "a3"});
		a->close();
		check(node.host->status(7) == GroupStatus::closed, "the table holds the closed group");
		auto b = node.open(7, &state);
		check(state == std::vector<std::string>{"a1", "a2", "a3"}, "the closed group recovers in the live host: " + text(state));
		node.append(*b, "b1");
		b.reset(); a.reset();
		auto c = node.open(7, &state);
		check(state == std::vector<std::string>{"a1", "a2", "a3", "b1"}, "the reopened group's later Store releases it and it recovers again: " + text(state));
	}
	Shared again(disk);
	again.open(7, &state);
	check(state == std::vector<std::string>{"a1", "a2", "a3", "b1"}, "the reopened host recovers the same state: " + text(state));
}

// A1, fenced or uncovered: a Store that cannot close into the table releases the group when it
// ends, with a record still staged: a fenced one drops its append, so the record is left in the
// driver alone; an unfenced one's append keeps its Journal, and so its hold, until the record
// settles. The recover is busy until then, and the group then recovers in the live host from
// the log's records, read back by position after retention forwarded them, exactly as a
// reopened host recovers it.
void reopen_released(bool fenced) {
	fl::filesystem disk;
	const std::string kind = fenced ? "fenced" : "unfenced";
	std::vector<std::string> state, expected{"a1", "a2", "a3"};
	{
		Shared node(disk);
		auto a = node.open(7), neighbor = node.open(9);
		node.append(*a, "a1"); node.checkpoint(*a, {"a1"}); node.append(*a, "a2");
		auto staged = node.stage(*a, "a3");
		if (fenced) { a->fence_storage(); staged.reset(); }
		a.reset();
		check(busy(node, 7), "a recover while the ended Store's record is staged is busy: " + kind);
		auto beside = node.stage(*neighbor, "n1");
		node.commit();
		check(!throws([&] { beside->result(); }), "a neighbor's append in the ended Store's turn completes: " + kind);
		beside.reset(); neighbor.reset();
		if (!fenced) { staged->result(); staged.reset(); }
		auto filler = node.open(8);
		for (int b = 0; b < 200; ++b) { node.append(*filler, std::string(900, 'x')); }
		check(node.turns().log().stats().forwarded_records > 0, "retention forwarded live records: " + kind);
		auto b = node.open(7, &state);
		check(state == expected, "the released group recovers every committed record in the live host: " + kind + " " + text(state));
		node.append(*b, "b1"); expected.push_back("b1");
		b.reset();
		auto c = node.open(7, &state);
		check(state == expected, "a second release recovers again: " + kind + " " + text(state));
	}
	Shared again(disk);
	again.open(7, &state);
	check(state == expected, "the reopened host recovers the same state: " + kind + " " + text(state));
}

// A Store whose append was refused (a record no segment holds) ends; the next Store recovers
// the group in the live host and appends, and the reopened host recovers what it appended.
void reopen_refused() {
	fl::filesystem disk;
	std::vector<std::string> state;
	{
		Shared node(disk);
		auto a = node.open(7);
		node.append(*a, "a1"); node.checkpoint(*a, {"a1"});
		check(throws([&] { node.append(*a, std::string(20000, 'x')); }), "a record no segment holds is refused");
		a.reset();
		auto b = node.open(7, &state);
		check(state == std::vector<std::string>{"a1"}, "the refused Store's group recovers in the live host: " + text(state));
		check(!throws([&] { node.append(*b, "b1"); }), "the next Store appends after the refused one");
	}
	Shared again(disk);
	again.open(7, &state);
	check(state == std::vector<std::string>{"a1", "b1"}, "the reopened host recovers the next Store's append: " + text(state));
}

// A2: a live Store's group is busy to a recover and a create, and so is a group whose first record
// its ended Store left staged; the holder carries on unharmed.
void reopen_two_stores() {
	fl::filesystem disk;
	Shared node(disk);
	std::vector<std::string> state;
	auto a = node.open(7);
	node.append(*a, "a1");
	check(busy(node, 7), "a recover of a group a live Store holds is busy");
	{
		Store other(*node.host, 7, reopen_limits); bool refused = false;
		try { other.create(group_identity(7)); } catch (const std::system_error& error) { refused = error.code() == std::errc::operation_would_block; }
		check(refused, "a create of a group a live Store holds is busy");
	}
	node.append(*a, "a2");
	check(!a->fenced(), "the holder carries on");
	auto created = std::make_unique<Store>(*node.host, 9, reopen_limits);
	created->create(group_identity(9)); created.reset();
	check(busy(node, 9), "a group whose first record its ended Store left staged is busy");
	node.commit();
	auto b = node.open(9, &state);
	check(state.empty() && node.host->status(9) == GroupStatus::stored, "it recovers once the record settled");
	a.reset();
	auto c = node.open(7, &state);
	check(state == std::vector<std::string>{"a1", "a2"}, "the ended holder's group recovers: " + text(state));
}

// A2 under TSan: a Store ends on another thread while the loop keeps trying to recover its group;
// the first recover that holds it follows the end's start, and replays every record.
void reopen_race() {
	fl::filesystem disk;
	Shared node(disk);
	auto a = node.open(7);
	node.append(*a, "a1"); node.append(*a, "a2");
	std::atomic<bool> ending{false};
	std::thread closer([&] { ending.store(true); a.reset(); });
	std::unique_ptr<Store> b; std::vector<std::string> state; unsigned refused = 0;
	while (!b) {
		auto store = std::make_unique<Store>(*node.host, 7, reopen_limits);
		std::vector<std::string> batches;
		try { store->recover([&](std::uint64_t, std::string_view bytes) { batches.emplace_back(bytes); }, [](const Frontier&, ArtifactReader&, std::span<ArtifactReader>) {}); }
		catch (const std::system_error& error) { check(error.code() == std::errc::operation_would_block, "a refused recover is busy"); ++refused; continue; }
		check(ending.load(), "a recover holds the group only once its Store's end began");
		b = std::move(store); state = std::move(batches);
	}
	closer.join();
	check(state == std::vector<std::string>{"a1", "a2"}, "the recover after the race replays every record: " + text(state));
	std::cout << "reopen_race: " << refused << " busy recovers\n";
}

namespace {
// A3: power cut at every I/O operation of a seeded run of close-then-recover cycles (a fenced
// release, a release, a clean close, or nothing, each with a record staged). The reopened host
// holds every record a recover or an append acknowledged, and nothing past what was attempted.
bool reopen_crash_run(std::uint64_t cut, std::uint64_t seed) {
	fl::filesystem disk;
	std::optional<Shared> node;
	std::unique_ptr<Store> store;
	std::vector<std::string> acknowledged, attempted;
	std::mt19937_64 random(seed);
	bool reached = false;
	const auto at = "seed " + std::to_string(seed) + " cut " + std::to_string(cut);
	try {
		host_io(disk);
		disk.power_off_after(cut - 1);
		node.emplace(disk);
		store = node->open(7);
		for (unsigned round = 0; round < 8; ++round) {
			const auto batch = "r" + std::to_string(round);
			attempted.push_back(batch);
			auto staged = node->stage(*store, batch);
			switch (random() % 4) {
			case 0: store->fence_storage(); [[fallthrough]];
			case 1: staged.reset(); store.reset(); node->commit(); break;
			case 2: node->commit(); staged->result(); acknowledged = attempted; node->checkpoint(*store, acknowledged); store->close(); store.reset(); break;
			default: node->commit(); staged->result(); acknowledged = attempted; break;
			}
			if (!store) {
				std::vector<std::string> state;
				store = node->open(7, &state);
				check(state == attempted, "a recover in the live host holds every attempted record: " + at + " " + text(state));
				acknowledged = state;
			}
		}
		reached = true;
	} catch (...) {
		if (!disk.powered_off()) { check(false, "the run failed before its power cut: " + at); }
	}
	if (node && node->turns().in_flight()) { node->turns().failed(std::make_exception_ptr(std::runtime_error("power lost"))); }
	store.reset(); node.reset();
	disk.crash(seed * 7919 + cut, fl::crash_policy{.size = fl::size_pick::sector});
	try {
		Shared again(disk);
		std::vector<std::string> state;
		if (again.host->status(7) != GroupStatus::none) { again.open(7, &state); }
		check(prefix(acknowledged, state) && prefix(state, attempted), "recovery keeps every acknowledged record and nothing unattempted: " + at + " recovered " + text(state) + ", acknowledged " + text(acknowledged));
	} catch (const std::exception& error) { check(false, "recovery threw " + std::string(error.what()) + ": " + at); }
	return reached;
}
}  // namespace
void reopen_crash() {
	std::uint64_t runs = 0;
	for (std::uint64_t seed = 0; seed < 16; ++seed) {
		std::uint64_t cut = 1;
		while (!reopen_crash_run(cut, seed)) { ++cut; ++runs; }
	}
	std::cout << "reopen_crash: 16 seeds, " << runs << " power cuts\n";
}

// A4: 10,000 cycles over 1,000 ids in one host, each recovering its group, appending, and then
// closing it into the table (even ids) or checkpointing and releasing it with one batch
// uncovered (odd ids). The host's heap after the first pass and after the last is reported.
void reopen_churn() {
	fl::filesystem disk;
	Shared node(disk, TurnLogOptions{64 * 1024, 1024, 1, 8, 4096});
	constexpr std::uint64_t ids = 1000, cycles = 10000;
	std::vector<std::vector<std::string>> expected(ids + 1);
	for (auto& batches : expected) { batches.reserve(cycles / ids + 1); }
	std::vector<std::string> state; state.reserve(cycles / ids + 1);
	std::int64_t warm = 0; std::uint64_t wrong = 0;
	for (std::uint64_t c = 0; c < cycles; ++c) {
		const auto group = 1 + c % ids;
		auto store = node.open(group, &state);
		wrong += state != expected[group];
		auto& batches = expected[group];
		if (group % 2) {
			if (!batches.empty()) { node.checkpoint(*store, batches); }
			batches.push_back("c" + std::to_string(c)); node.append(*store, batches.back());
		} else {
			batches.push_back("c" + std::to_string(c)); node.append(*store, batches.back());
			node.checkpoint(*store, batches); store->close();
		}
		store.reset();
		if (c + 1 == ids) { settle(*node.host); warm = heap::bytes.load(); }
	}
	settle(*node.host);
	const auto last = heap::bytes.load();
	check(wrong == 0, "every recover holds its group's batches: " + std::to_string(wrong) + " wrong");
	std::cout << "reopen_churn: " << cycles << " cycles of " << ids << " ids, host heap " << warm << " B after the first pass, " << last << " B after the last (" << (last - warm) << " B)\n";
	if (heap::checked("reopen_churn")) { check(last - warm <= std::int64_t(ids) * 64, "the host's heap stays flat across the churn"); }
}

// A8: 10,000 distinct groups, each created, written, checkpointed and closed once in one host:
// the host's heap after 1,000, 5,000 and 10,000, flat by its slope from 1,000 on.
void reopen_distinct() {
	fl::filesystem disk;
	Shared node(disk, TurnLogOptions{64 * 1024, 1024, 1, 4, 4096});
	const char* scale = std::getenv("JOURNAL_DISTINCT_IDS");  // another count, for timing
	const std::uint64_t ids = scale ? std::stoull(scale) : 10000, early = ids / 10, middle = ids / 2;
	std::int64_t first = 0, half = 0;
	for (std::uint64_t group = 1; group <= ids; ++group) {
		auto store = node.open(group);
		node.append(*store, "b");
		node.checkpoint(*store, {"b"});
		store->close(); store.reset();
		if (group == early || group == middle) { settle(*node.host); (group == early ? first : half) = heap::bytes.load(); }
	}
	settle(*node.host);
	const auto last = heap::bytes.load();
	const double slope = double(last - first) / double(ids - early);
	std::cout << "reopen_distinct: host heap " << first << " B after " << early << " closed groups, " << half << " B after " << middle << ", " << last << " B after " << ids
		<< "; slope " << slope << " B per group\n";
	if (heap::checked("reopen_distinct")) { check(slope < 1, "closing distinct groups leaves the host's heap flat: " + std::to_string(slope) + " B per group"); }
	std::vector<std::string> state;
	for (std::uint64_t group : {std::uint64_t{1}, ids}) {
		auto store = node.open(group, &state);
		check(state == std::vector<std::string>{"b"}, "a closed group recovers from the table: " + text(state));
	}
}

// journal-idle-groups A2: N groups, each created with its first checkpoint record, released, and
// parked into the table by Host::park, in batches of 10,000. Reports the host's heap per parked
// group (the slope from the first batch on), the table's bytes on disk per group, and checks the
// log holds none of them and is back to one segment; a sample wakes with its checkpoint, and so
// after a restart. JOURNAL_PARKED_GROUPS sets N (1,000,000 in the job; 20,000 in the suite).
void park_count() {
	fl::filesystem disk;
	const char* scale = std::getenv("JOURNAL_PARKED_GROUPS");
	const std::uint64_t ids = scale ? std::stoull(scale) : 20000, batch = std::min<std::uint64_t>(ids, 10000);
	const TurnLogOptions turns_options{4u << 20, 4096, 1, 4, 1u << 18};
	auto payload = [](std::uint64_t group) { return "checkpoint-" + std::to_string(group); };
	const auto start = std::chrono::steady_clock::now();
	std::int64_t first = 0;
	std::uint64_t parked = 0;
	{
		Shared node(disk, turns_options);
		auto& host = *node.host;
		auto& turns = node.turns();
		// A held group whose record starts in the first segment: the forward scan runs whenever its
		// segment is the oldest past retention.
		const std::uint64_t anchor = ids + 2;
		host.create_group(anchor);
		turns.stage(TurnRecord{anchor, 1, TurnRecordKind::Checkpoint, "anchor"});
		node.commit();
		const auto anchor_hold = host.hold(anchor);  // open, so park leaves it
		for (std::uint64_t low = 1; low <= ids; low += batch) {
			const auto high = std::min(ids + 1, low + batch);
			for (auto group = low; group < high; ++group) {
				host.create_group(group);
				turns.stage(TurnRecord{group, 1, TurnRecordKind::Checkpoint, payload(group)});
			}
			node.commit();
			for (auto group = low; group < high; ++group) { host.hold(group); }  // held, then released at once
			parked += host.park(0);
			if (low == 1) { first = heap::bytes.load(); }
		}
		// A busy group's turns past the retention window reclaim the parked groups' segments.
		const std::uint64_t busy = ids + 1;
		host.create_group(busy);
		turns.stage(TurnRecord{busy, 1, TurnRecordKind::Checkpoint, "busy"});
		node.commit();
		const auto scanned = turns.log().stats().forward_scanned, forwarding = turns.log().stats().turns, forwarded = turns.log().stats().forwarded_records;
		for (std::uint64_t s = 2; s < 2 + 192; ++s) { turns.stage(TurnRecord{busy, s, TurnRecordKind::Checkpoint, std::string(128u << 10, 'b'), s - 1}); node.commit(); }
		// The forward scan walks the log's live records each turn: none of a parked group's.
		const double per_turn = double(turns.log().stats().forward_scanned - scanned) / double(turns.log().stats().turns - forwarding);
		std::cout << "park_count: the forward scan visits " << per_turn << " live records per turn over " << ids << " parked groups (" << turns.log().stats().forwarded_records - forwarded << " forwarded)\n";
		check(turns.log().stats().forward_scanned > scanned && per_turn <= 2, "the forward scan runs and skips parked groups: " + std::to_string(per_turn) + " records per turn");
		const auto last = heap::bytes.load();
		const double slope = ids > batch ? double(last - first) / double(ids - batch) : 0;
		std::uint64_t table = 0;
		for (const auto& name : disk.list("host")) { if (name.starts_with("groups-")) { table += disk.view("host/" + name).size(); } }
		const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		std::cout << "park_count: " << parked << " of " << ids << " groups parked in " << seconds << " s; host heap " << first << " B after the first " << batch
			<< ", " << last << " B after all; slope " << slope << " B per parked group; table " << table << " B on disk, " << double(table) / double(ids)
			<< " B per group; log segments " << turns.log().data_segments().size() << "\n";
		check(parked == ids, "every idle released group parks");
		std::uint64_t in_log = 0, open = 0;
		for (std::uint64_t group = 1; group <= ids; ++group) { in_log += turns.holds(group) || turns.evicted(group); }
		for (std::uint64_t group = 1; group <= ids; group += ids / 97 + 1) { open += host.status(group) != GroupStatus::closed; }
		check(in_log == 0 && open == 0, "a parked group is closed in the table and the log holds nothing of it");
		check(turns.log().data_segments().size() <= turns_options.retain_segments, "the log keeps only the busy group's window: " + std::to_string(turns.log().data_segments().size()) + " segments");
		if (heap::checked("park_count") && ids > batch) { check(slope <= 1, "a parked group costs the host no heap: " + std::to_string(slope) + " B per group"); }
		// One transition at a time at full size: a sample woken (one turn each), then parked again
		// (one park each). The table bytes each transition writes, and its time; with
		// JOURNAL_DISK_DIR, again over a copy of the host's directory on that real filesystem.
		auto transitions = [&](Host& on, const std::string& where, std::uint64_t most) {
			auto& driver = on.driver(0);
			on.park(0);  // a reopened copy's idle busy group, off the clock
			const std::uint64_t sample = std::max<std::uint64_t>(1, std::min<std::uint64_t>(ids / 4, most)), step = ids / sample;
			const auto bytes = table_bytes, files = table_files;
			const auto began = std::chrono::steady_clock::now();
			std::uint64_t woken = 0, again = 0;
			for (std::uint64_t i = 0; i < sample; ++i) {
				const auto group = 2 + i * step;  // never 1, ids / 2 or ids: the groups checked below
				if (group == ids / 2 || group >= ids) { continue; }
				const auto entry = on.entry(group);
				on.reopen_group(group);
				const auto at = std::max(entry->base, driver.retired(group)) + 1;
				driver.stage(TurnRecord{group, at, TurnRecordKind::Checkpoint, entry->manifest, entry->base});
				while (!driver.idle()) { driver.commit(); }
				on.hold(group);
				again += on.park(0); ++woken;
			}
			const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
			const auto count = double(2 * std::max<std::uint64_t>(woken, 1));
			std::cout << "park_count: " << 2 * woken << " single transitions (wake, park) at " << ids << " groups" << where << ": " << double(table_bytes - bytes) / count
				<< " table B and " << double(table_files - files) / count << " table files written per transition, " << 1e6 * elapsed / count << " us each\n";
			check(again == woken, "every woken sample parks again" + where);
		};
		transitions(host, "", 1000);
		if (const char* real = std::getenv("JOURNAL_DISK_DIR")) {
			const std::filesystem::path directory = std::filesystem::path(real) / "park_count";
			std::filesystem::remove_all(directory); std::filesystem::create_directories(directory);
			for (const auto& name : disk.list("host")) {
				const auto bytes = disk.view("host/" + name);
				std::ofstream(directory / name, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
			}
			// Opens over the copy, its files in the page cache: the table's reads and decodes, the log's
			// recovery, and the open's syncs.
			std::size_t files = 0;
			for (const auto& name : disk.list("host")) { files += name.starts_with("groups-"); }
			std::cout << "park_count: the host opens over " << ids << " parked groups in " << files << " table files on " << real << " in";
			for (int open = 0; open < 3; ++open) {
				const auto opening = std::chrono::steady_clock::now();
				Host copy(std::make_shared<MeteredIO>(std::make_shared<PosixIO>(directory, PosixIOOptions{FlushMode::full})), reopen_host, HostOptions{1, turns_options});
				std::cout << " " << std::chrono::duration<double>(std::chrono::steady_clock::now() - opening).count() << " s";
			}
			std::cout << "\n";
			{
				Host copy(std::make_shared<MeteredIO>(std::make_shared<PosixIO>(directory, PosixIOOptions{FlushMode::full})), reopen_host, HostOptions{1, turns_options});
				transitions(copy, " on " + std::string(real), 100);  // each syncs for real
			}
			std::filesystem::remove_all(directory);
		}
		// Waking a sample: the table's record staged again, then a batch, in one turn.
		for (std::uint64_t group : {std::uint64_t{1}, ids / 2, ids}) {
			const auto entry = host.entry(group);
			check(entry && entry->manifest == payload(group) && entry->base == 1, "the table holds the parked group's checkpoint: group " + std::to_string(group));
			host.reopen_group(group);
			const auto at = std::max(entry->base, turns.retired(group)) + 1;
			turns.stage(TurnRecord{group, at, TurnRecordKind::Checkpoint, entry->manifest, entry->base});
			turns.stage(TurnRecord{group, at + 1, TurnRecordKind::Batch, "woken"});
		}
		node.commit();
	}
	const auto opening = std::chrono::steady_clock::now();
	Shared again(disk, turns_options);
	std::cout << "park_count: the host opens over " << ids << " parked groups in " << std::chrono::duration<double>(std::chrono::steady_clock::now() - opening).count() << " s\n";
	for (std::uint64_t group : {std::uint64_t{1}, ids / 2, ids}) {
		const auto& records = again.turns().records(group);
		check(records.size() == 2 && records[0].payload == payload(group) && records[1].payload == "woken", "a woken group recovers its checkpoint and its batch");
	}
	check(again.host->status(2) == GroupStatus::closed && again.host->entry(2)->manifest == payload(2), "a parked group stays parked across a restart");
}

// A7: 10,000 groups, each created, given a batch and released by its Store's end: the bytes per
// group the host keeps before settle() parks them and after, and a sample that recovers exactly.
void reopen_heap() {
	fl::filesystem disk;
	Shared node(disk, TurnLogOptions{64 * 1024, 1024, 1, 1000, 4096});
	constexpr std::uint64_t ids = 10000;
	settle(*node.host);
	const auto empty = heap::bytes.load();
	for (std::uint64_t group = 1; group <= ids; ++group) {
		auto store = node.open(group);
		node.append(*store, "b" + std::to_string(group));
	}
	const auto held = heap::bytes.load();
	settle(*node.host);
	const auto settled = heap::bytes.load();
	bool all = true;
	for (std::uint64_t group = 1; group <= ids; ++group) { all &= evicted(node.turns(), group); }
	check(all, "settle evicts every released group");
	const double before = double(held - empty) / ids, after = double(settled - empty) / ids;
	std::cout << "reopen_heap: " << ids << " released groups, " << before << " B per group before settle, " << after << " B after\n";
	if (heap::checked("reopen_heap")) { check(after < before && after < 400, "a released group keeps little of the host's heap"); }
	std::vector<std::string> state;
	for (std::uint64_t group : {std::uint64_t{1}, ids / 2, ids}) {
		auto store = node.open(group, &state);
		check(state == std::vector<std::string>{"b" + std::to_string(group)}, "a parked group recovers exactly: " + text(state));
	}
}

// A9: the directory barriers reclaim spends per reclaimed segment, over turn_log_power's
// workload (one to three of six groups a turn, records up to 700 bytes, one in five a checkpoint).
void reclaim_syncs() {
	std::size_t segments = 0, barriers = 0;
	for (std::uint64_t seed = 1; seed <= 200; ++seed) {
		fl::filesystem disk; MeteredIO io(std::make_shared<fl::io>(disk));
		TurnRecovery recovered; TurnLog log(io, reopen_host, recovered, TurnLogOptions{8 * 512, 512, 1, 3, 600});
		std::mt19937_64 random(seed); std::map<std::uint64_t, std::uint64_t> next;
		for (int t = 0; t < 60; ++t) {
			std::vector<TurnRecord> batch;
			for (auto n = random() % 3 + 1; n; --n) {
				const auto group = random() % 6 + 1, sequence = ++next[group], bytes = random() % 700 + 1;
				const auto kind = random() % 5 ? TurnRecordKind::Batch : TurnRecordKind::Checkpoint;
				batch.push_back({group, sequence, kind, std::string(bytes, 'r'), sequence - 1});
			}
			log.append(batch); log.prepare();
		}
		segments += log.stats().segments_recycled + log.stats().segments_removed; barriers += io.namespace_barriers;
	}
	std::cout << "reclaim_syncs: " << segments << " segments reclaimed, " << barriers << " directory barriers, " << double(barriers) / double(segments) << " per segment\n";
	check(segments > 0 && barriers == segments, "reclaim orders each reclaimed segment's removal");
}
