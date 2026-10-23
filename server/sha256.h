// SHA-256 (FIPS 180-4), for checking a finished download against the digest its catalog entry
// names. Written out here rather than linked: the server carries no TLS or crypto library (curl
// does the transfer), and this is the one digest it needs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace bmoe::server {

class Sha256 {
public:
    Sha256();
    void update(const void * data, size_t len);
    // The digest as 64 lowercase hex digits. The object is spent afterwards.
    std::string hex_digest();

private:
    void block(const uint8_t * p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t buf_len_ = 0;
    uint64_t total_ = 0;
};

// The digest of the file at `path` (UTF-8). `progress`, when set, is told the bytes read so far
// every few hundred megabytes and stops the read by returning false. False, with the reason, when
// the file cannot be read or the read was stopped.
bool sha256_file(const std::string & path,
                 std::string & hex,
                 std::string & error,
                 const std::function<bool(uint64_t)> & progress = nullptr);

} // namespace bmoe::server
