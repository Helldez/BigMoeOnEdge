// How a model download survives a flaky line and a full disk, decided apart from curl and threads
// so the rules are tested on their own (tests/server_test.cpp): which failures are worth another
// attempt and how long to wait before it, how much disk a resume still needs, and which file a
// partial download is - a name the catalog gave, inside the models folder and nowhere else.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace bmoe::server {

// Attempts at one file before the download ends in `error` and waits for the user. Every attempt
// resumes from the `.part`, so a retry costs a reconnect, never the bytes already received.
inline constexpr int kDownloadMaxAttempts = 6;
// The wait after the first failed attempt; each later one doubles it, up to the cap. A Wi-Fi drop
// usually heals within seconds, and a minute is long enough to outlast a router's restart without
// leaving the user looking at a stalled bar.
inline constexpr std::chrono::seconds kDownloadRetryFirstDelay{2};
inline constexpr std::chrono::seconds kDownloadRetryMaxDelay{60};

// True for a curl exit status that says the line failed rather than the request: the host could
// not be resolved or reached, the transfer stalled or was cut short (18), the connection was reset
// or answered nothing (52, 55, 56), the TLS handshake or an HTTP/2 stream broke. curl's own
// --retry covers timeouts and 408/429/5xx answers but not these, which are what a long download
// over a flaky line actually dies of. An HTTP error, a missing file or a full disk is not transient.
bool is_transient_curl_exit(int rc);

// The wait before attempt `failed + 1`, after `failed` attempts have failed (1 or more).
std::chrono::seconds download_retry_delay(int failed);

// Bytes a file still needs on disk: what the catalog says it holds, less what is already there
// (its `.part`). Never below zero.
uint64_t bytes_still_needed(uint64_t expected, uint64_t on_disk);

// Why a download that needs `needed` more bytes cannot start on a disk with `available` free, or ""
// when it fits. `available` 0 means the free space could not be read: the download is let start
// rather than refused on a guess.
std::string free_space_refusal(uint64_t needed, uint64_t available);

// Whether a catalog file name is a bare file name: not empty, not "." or "..", no separator, no
// drive colon, no NUL. The catalog, the added entries and every path built from a name hold to
// it, so no row, hand-edited or not, names a file outside the models folder.
bool is_plain_file_name(const std::string & name);

// Whether `text` is a SHA-256 digest as the catalog spells one: 64 lowercase hex digits.
bool is_sha256_hex(const std::string & text);

// Why a finished file whose digest is `actual` is not the file the catalog names by `expected`, or
// "" when it is (or the catalog names no digest).
std::string digest_refusal(const std::string & expected, const std::string & actual);

// The protocols curl may use for `url`, as its --proto and --proto-redir value: "=https" for an
// https URL, so no redirect downgrades it. Plain http is for one place only: the mirror the
// operator named with --hf-endpoint, passed as `endpoint` (the server's endpoint, with no trailing
// slash). An http URL under it, when it is itself http, gets "=http,https"; any other http URL, and
// anything that is not http(s), gets "" and is not fetched.
std::string curl_protocols(const std::string & url, const std::string & endpoint);

// Why a catalog file cannot be offered, or "" when it can: its name becomes a path in the models
// folder (is_plain_file_name), its URL a curl argument (curl_protocols, against `endpoint`), and
// its digest, when it names one, must be one (is_sha256_hex). Checked as the catalog and the added
// entries load and as an entry is added, so nothing downstream sees a file that fails it.
std::string catalog_file_refusal(const std::string & name,
                                 const std::string & url,
                                 const std::string & sha256,
                                 const std::string & endpoint);

// The `.part` a download of catalog file `name` writes into `models_dir`. False when the name is
// not a plain file name (is_plain_file_name), so no catalog row, hand-edited or not, makes the
// server write or delete anywhere but directly inside the models folder.
bool partial_file_path(const std::string & models_dir, const std::string & name, std::string & out);

// Delete the `.part` of each of `names` in `models_dir`, as partial_file_path() names them. A
// missing one is not an error. Returns the bytes freed; false, with the reason, when a name is not
// plain or a file could not be removed.
bool delete_partial_files(const std::string & models_dir,
                          const std::vector<std::string> & names,
                          uint64_t & freed,
                          std::string & err);

// "14.2 GB": decimal gigabytes with one decimal, as the model repositories quote sizes and as the
// Android app writes them, so both front-ends name the same file with the same number.
std::string gb_label(uint64_t bytes);

} // namespace bmoe::server
