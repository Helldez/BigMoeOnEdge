// The release feed (docs/desktop-app.md, "The feed"): what the latest release offers, signed. Read
// and checked here with no network and no files, so the rules are tested on their own
// (tests/feed_test.cpp): the signature over the exact bytes, then the shape, then every URL, digest
// and name the feed hands on to a download or a path.
#pragma once

#include "json_util.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bmoe::server {

// Where the feed of the latest release is. The repository's own release assets: a fork that signs
// its own feed changes this with its key (feed_key.h), and --feed-url points a test elsewhere.
inline constexpr const char * kFeedUrl = "https://github.com/Helldez/BigMoeOnEdge/releases/latest/download/feed.json";

// The largest feed the server reads, and the largest signature: a feed is a few kilobytes, and a
// host that answers with more is not serving one.
inline constexpr size_t kFeedMaxBytes = 1u << 20;
inline constexpr size_t kFeedSignatureMaxBytes = 1024;

using Ed25519PublicKey = std::array<uint8_t, 32>;

// One file the feed names: where it is, its digest and its exact size.
struct FeedAsset {
    std::string url;
    std::string sha256;
    uint64_t size = 0;
    // The last path segment of the URL: the name the file is saved under.
    std::string file_name() const;
};

// A backend that needs a vendor's runtime, as an archive of its own. It applies to a machine where
// one of `needs_any` (driver libraries, by the name the system loader takes) loads.
struct FeedPack {
    std::string id;
    std::string title;
    std::string target;
    std::vector<std::string> needs_any;
    FeedAsset asset;
};

struct Feed {
    std::string version;
    std::string published;
    std::string notes;
    std::map<std::string, FeedAsset> installers; // by target
    std::map<std::string, FeedAsset> archives;   // by target
    std::vector<FeedPack> packs;
};

// Standard base64 (RFC 4648, padded or not; line breaks and blanks ignored). False on anything else.
bool base64_decode(const std::string & text, std::string & out);

// A public key as the build carries it: base64 of the 32 raw key bytes. False when it is not one.
bool parse_public_key(const std::string & base64, Ed25519PublicKey & key);

// Whether `signature` (base64 of the 64 raw bytes, as `openssl pkeyutl -sign -rawin` writes them
// and the release encodes them) is the key's Ed25519 signature of exactly `message`. False, with
// the reason, otherwise. Checked before a byte of the feed is parsed.
bool verify_feed_signature(const std::string & message,
                           const std::string & signature,
                           const Ed25519PublicKey & key,
                           std::string & err);

// The feed's JSON, held to its shape: a version, every asset an https URL with a SHA-256 and a
// size, every pack a plain id and plain library names. False, with the reason, for anything else;
// an entry that fails is never half-taken.
bool parse_feed(const std::string & text, Feed & feed, std::string & err);

// verify_feed_signature, then parse_feed.
bool read_signed_feed(const std::string & text,
                      const std::string & signature,
                      const Ed25519PublicKey & key,
                      Feed & feed,
                      std::string & err);

// "X.Y.Z" against "X.Y.Z", by number: <0, 0, >0. A leading "v" is ignored. False from
// parse_version for anything that is not three numbers.
bool parse_version(const std::string & text, std::array<uint64_t, 3> & out);
int compare_versions(const std::array<uint64_t, 3> & a, const std::array<uint64_t, 3> & b);

// A pack id as it becomes a folder name: lowercase letters, digits and '-', not starting with
// '-', at most 32 characters.
bool is_pack_id(const std::string & id);

// What GET /api/update answers: {current, latest, available, notes, target, installer}, where
// `installer` is this target's (null when the feed has none for it).
json update_answer(const Feed & feed, const std::string & current, const std::string & target);

} // namespace bmoe::server
