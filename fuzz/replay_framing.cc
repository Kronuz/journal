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

// Runs the framing fuzz target without libFuzzer, as a test: the framing fixtures (strata's tail
// model's logs), their truncations at every sector, their single-byte corruptions, and junk.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

static void run(char mode, const std::string& bytes) {
	std::string input(1, mode); input += bytes;
	LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
}

int main() {
	std::mt19937_64 random(7);
	std::size_t inputs = 0;
	for (auto name : {"log-a", "log-b", "log-c", "log-d"}) {
		std::ifstream file(std::filesystem::path(JOURNAL_FIXTURE_DIR) / name, std::ios::binary);
		const std::string log{std::istreambuf_iterator<char>(file), {}};
		run(0, log); run(1, log); inputs += 2;
		for (std::size_t size = 0; size < log.size(); size += 512) { run(0, log.substr(0, size)); run(0, log.substr(0, size + random() % 512)); inputs += 2; }
		for (int k = 0; k < 2000; ++k) { auto bad = log; bad[random() % bad.size()] ^= char(1 + random() % 255); run(0, bad); ++inputs; }
		for (int k = 0; k < 200; ++k) { auto zeroed = log; auto at = random() % zeroed.size(); std::fill(zeroed.begin() + static_cast<std::ptrdiff_t>(at), zeroed.end(), '\0'); run(0, zeroed); ++inputs; }
	}
	for (int k = 0; k < 200; ++k) { std::string junk(random() % 2000, '\0'); for (auto& c : junk) { c = char(random()); } run(0, junk); run(1, junk); inputs += 2; }
	std::printf("framing fuzz replay: %zu inputs\n", inputs);
	return 0;
}
