#include "sha256.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace bmoe::server {

namespace {

constexpr uint32_t k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

// Bytes read per call: large enough that a multi-gigabyte file is a few thousand reads.
constexpr size_t kReadChunk = 4u << 20;
// How often a long hash reports, in bytes.
constexpr uint64_t kProgressEvery = 256ull << 20;

} // namespace

Sha256::Sha256() : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::block(const uint8_t * p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t) p[4 * i] << 24 | (uint32_t) p[4 * i + 1] << 16 | (uint32_t) p[4 * i + 2] << 8 |
               (uint32_t) p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(const void * data, size_t len) {
    const uint8_t * p = static_cast<const uint8_t *>(data);
    total_ += len;
    if (buf_len_) {
        const size_t take = len < 64 - buf_len_ ? len : 64 - buf_len_;
        std::memcpy(buf_ + buf_len_, p, take);
        buf_len_ += take;
        p += take;
        len -= take;
        if (buf_len_ < 64) return;
        block(buf_);
        buf_len_ = 0;
    }
    for (; len >= 64; p += 64, len -= 64)
        block(p);
    std::memcpy(buf_, p, len);
    buf_len_ = len;
}

std::string Sha256::hex_digest() {
    const uint64_t bits = total_ * 8;
    const uint8_t one = 0x80, zero = 0;
    update(&one, 1);
    while (buf_len_ != 56)
        update(&zero, 1);
    uint8_t len_be[8];
    for (int i = 0; i < 8; ++i)
        len_be[i] = (uint8_t) (bits >> (56 - 8 * i));
    update(len_be, 8);
    static const char * hex = "0123456789abcdef";
    std::string out;
    for (uint32_t v : h_)
        for (int s = 28; s >= 0; s -= 4)
            out += hex[(v >> s) & 0xf];
    return out;
}

bool sha256_file(const std::string & path,
                 std::string & hex,
                 std::string & error,
                 const std::function<bool(uint64_t)> & progress) {
    std::FILE * f = nullptr;
#ifdef _WIN32
    f = _wfopen(std::filesystem::u8path(path).wstring().c_str(), L"rb");
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) {
        error = "could not open " + std::filesystem::u8path(path).filename().u8string() + " to check its digest";
        return false;
    }
    Sha256 h;
    std::vector<uint8_t> buf(kReadChunk);
    uint64_t done = 0, next_report = kProgressEvery;
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
        h.update(buf.data(), n);
        done += n;
        if (progress && done >= next_report) {
            next_report += kProgressEvery;
            if (!progress(done)) {
                std::fclose(f);
                error = "stopped";
                return false;
            }
        }
    }
    const bool failed = std::ferror(f) != 0;
    std::fclose(f);
    if (failed) {
        error = "could not read " + std::filesystem::u8path(path).filename().u8string() + " to check its digest";
        return false;
    }
    hex = h.hex_digest();
    return true;
}

} // namespace bmoe::server
