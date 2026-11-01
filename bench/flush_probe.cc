// Times the sync sequences an append could use, with raw system calls, interleaved.
//   probe DIR ROUNDS BYTES
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
static void die(const char* what) { std::perror(what); std::exit(1); }
static void full(int fd) {
#ifdef __APPLE__
	if (::fcntl(fd, F_FULLFSYNC) < 0) die("F_FULLFSYNC");
#else
	if (::fdatasync(fd) < 0) die("fdatasync");
#endif
}
static void barrier(int fd) {
#ifdef __APPLE__
	if (::fcntl(fd, F_BARRIERFSYNC) < 0) die("F_BARRIERFSYNC");
#else
	if (::fdatasync(fd) < 0) die("fdatasync");
#endif
}
static void dsync(int fd) { // the second flush of the two-flush shape: what a Sync primitive does
#ifdef __APPLE__
	full(fd);
#else
	if (::fsync(fd) < 0) die("fsync");
#endif
}
static void pw(int fd, const std::string& b, off_t at) { if (::pwrite(fd, b.data(), b.size(), at) != (ssize_t)b.size()) die("pwrite"); }
int main(int argc, char** argv) {
	if (argc < 4) { std::fprintf(stderr, "probe DIR ROUNDS BYTES\n"); return 2; }
	std::string dir = argv[1]; int rounds = std::atoi(argv[2]); size_t bytes = std::strtoul(argv[3], nullptr, 10);
	std::string record(bytes + 24, 'x'), slot(28, 's'), manifest(52, 'm');
	int d = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY); if (d < 0) die("open dir");
	int lock = ::open((dir + "/lock").c_str(), O_RDWR | O_CREAT, 0600); if (lock < 0) die("lock");
	struct Shape { const char* name; std::function<void(int, off_t&)> append; std::vector<double> ms; int fd = -1; off_t tail = 12288; };
	int serial = 0;
	std::vector<Shape> shapes;
	shapes.push_back({"plain: write, full", [&](int fd, off_t& tail) { pw(fd, record, tail); tail += record.size(); full(fd); }});
	shapes.push_back({"two flushes: write, full, write slot, full", [&](int fd, off_t& tail) { pw(fd, record, tail); tail += record.size(); dsync(fd); pw(fd, slot, 4096 * (1 + (tail / record.size()) % 2)); dsync(fd); }});
	shapes.push_back({"barrier: write, barrier, write slot, full", [&](int fd, off_t& tail) { pw(fd, record, tail); tail += record.size(); barrier(fd); pw(fd, slot, 4096 * (1 + (tail / record.size()) % 2)); full(fd); }});
	shapes.push_back({"current: write x2, full, create, write, full, rename, fsync dir, full", [&](int fd, off_t& tail) {
		pw(fd, record.substr(0, 24), tail); pw(fd, record.substr(24), tail + 24); tail += record.size(); dsync(fd);
		auto temp = dir + "/manifest.pending-" + std::to_string(++serial);
		int m = ::open(temp.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600); if (m < 0) die("create");
		pw(m, manifest, 0); dsync(m); if (::rename(temp.c_str(), (dir + "/manifest").c_str()) < 0) die("rename");
		if (::fsync(d) < 0) die("fsync dir");
#ifdef __APPLE__
		full(lock);
#endif
		::close(m); }});
	for (size_t i = 0; i < shapes.size(); ++i) {
		auto name = dir + "/data-" + std::to_string(i);
		shapes[i].fd = ::open(name.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600); if (shapes[i].fd < 0) die("data");
		pw(shapes[i].fd, std::string(12288, '\0'), 0); full(shapes[i].fd);
	}
	::fsync(d); full(lock);
	for (int r = 0; r < rounds; ++r) {
		for (auto& s : shapes) {
			auto t0 = std::chrono::steady_clock::now();
			s.append(s.fd, s.tail);
			s.ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
		}
	}
	std::printf("bytes=%zu rounds=%d (median ms, p10, p90)\n", bytes, rounds);
	for (auto& s : shapes) {
		std::sort(s.ms.begin(), s.ms.end());
		std::printf("%8.3f %8.3f %8.3f  %s\n", s.ms[s.ms.size() / 2], s.ms[s.ms.size() / 10], s.ms[s.ms.size() * 9 / 10], s.name);
		::close(s.fd);
	}
}
