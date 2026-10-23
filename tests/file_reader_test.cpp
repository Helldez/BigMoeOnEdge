// FileReader (core/src/io/file_reader.h): a read either delivers every byte asked for, or fails.
// A range that runs past the end of the file is a gguf header pointing outside it (a truncated
// download, a damaged shard), and must come back as -1, never as a short read or a buffer padded
// with whatever the bounce held before (#348). Checked buffered and with the direct mode the platform
// has (O_DIRECT's aligned windows, NO_BUFFERING, F_NOCACHE), at the end of the file and in the middle.
// No model: a scratch file of a known byte pattern is enough.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.
#include "file_reader.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;

static void check(bool ok, const std::string & what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

static unsigned char pattern(uint64_t i) {
    return (unsigned char) ((i * 131u + 7u) & 0xffu);
}

static bool matches(const std::vector<unsigned char> & buf, uint64_t off) {
    for (size_t i = 0; i < buf.size(); ++i)
        if (buf[i] != pattern(off + i)) return false;
    return true;
}

static void run(const std::string & path, uint64_t size, bool direct) {
    const std::string mode = direct ? "direct" : "buffered";
    bmoe::FileReader r;
    check(r.open(path, 1, direct, 1 << 16), mode + ": opens");
    if (!r.is_open()) return;
    check(r.file_size() == size, mode + ": knows the file's size");

    std::vector<unsigned char> buf(5000, 0xEE);
    check(r.read(0, buf.data(), 1000, buf.size()) >= (long long) buf.size() && matches(buf, 1000),
          mode + ": a read inside the file delivers every byte");

    const uint64_t tail = 3000; // the last bytes, ending exactly at EOF and not on a granule
    buf.assign(tail, 0xEE);
    check(r.read(0, buf.data(), size - tail, tail) >= (long long) tail && matches(buf, size - tail),
          mode + ": a read that ends exactly at the end of the file delivers it");

    buf.assign(tail + 1, 0xEE);
    check(r.read(0, buf.data(), size - tail, tail + 1) == -1, mode + ": one byte past the end is an error");
    buf.assign(64, 0xEE);
    check(r.read(0, buf.data(), size + 4096, buf.size()) == -1, mode + ": a range wholly past the end is an error");
    check(r.read(0, buf.data(), UINT64_MAX - 8, 64) == -1, mode + ": an offset that would overflow is an error");

    buf.assign(4096, 0xEE);
    check(r.read(0, buf.data(), 8192, buf.size()) >= (long long) buf.size() && matches(buf, 8192),
          mode + ": the reader still reads after refusing");
}

int main() {
    const std::string path = "bmoe_file_reader_test.bin";
    const uint64_t size = 3 * 65536 + 777; // not a multiple of any direct-read granule
    {
        FILE * f = std::fopen(path.c_str(), "wb");
        if (!f) {
            std::printf("[FAIL] cannot write the scratch file\n");
            return 1;
        }
        for (uint64_t i = 0; i < size; ++i)
            std::fputc(pattern(i), f);
        std::fclose(f);
    }
    run(path, size, false);
    run(path, size, true);
    std::remove(path.c_str());
    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall file reader checks passed\n");
    return 0;
}
