#include "feed.h"

#include "download_policy.h"

#include "monocypher-ed25519.h"

#include <cctype>
#include <set>

namespace bmoe::server {

namespace {

int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// What a feed hands on to curl: https, on every redirect too (download_policy.h). No mirror is
// allowed plain http here: the digest would still catch a changed file, but nothing needs it.
bool is_https_url(const std::string & url) {
    return curl_protocols(url, "") == "=https";
}

bool read_asset(const json & j, const std::string & where, FeedAsset & a, std::string & err) {
    if (!j.is_object()) {
        err = where + " is not an object";
        return false;
    }
    const json & url = j.contains("url") ? j["url"] : json();
    const json & sha = j.contains("sha256") ? j["sha256"] : json();
    const json & size = j.contains("size") ? j["size"] : json();
    if (!url.is_string() || !is_https_url(url.get<std::string>())) {
        err = where + ".url is not an https URL";
        return false;
    }
    if (!sha.is_string() || !is_sha256_hex(sha.get<std::string>())) {
        err = where + ".sha256 is not a SHA-256 digest (64 lowercase hex digits)";
        return false;
    }
    if (!size.is_number_unsigned() || size.get<uint64_t>() == 0) {
        err = where + ".size is not a positive byte count";
        return false;
    }
    a.url = url.get<std::string>();
    a.sha256 = sha.get<std::string>();
    a.size = size.get<uint64_t>();
    if (!is_plain_file_name(a.file_name())) {
        err = where + ".url does not end in a file name";
        return false;
    }
    return true;
}

bool read_asset_map(const json & j, const char * key, std::map<std::string, FeedAsset> & out, std::string & err) {
    if (!j.contains(key)) return true;
    const json & m = j[key];
    if (!m.is_object()) {
        err = std::string(key) + " is not an object";
        return false;
    }
    for (auto it = m.begin(); it != m.end(); ++it) {
        FeedAsset a;
        if (!read_asset(it.value(), std::string(key) + "." + it.key(), a, err)) return false;
        out[it.key()] = a;
    }
    return true;
}

// A library name as the system loader takes it: a bare file name, which the loader searches for in
// its own places. A path would make the server load a file of the feed's choosing.
bool is_library_name(const std::string & name) {
    if (!is_plain_file_name(name)) return false;
    for (const char c : name)
        if (c == '\\' || c == '/' || c == ' ') return false;
    return true;
}

json asset_json(const FeedAsset & a) {
    return json{{"url", a.url}, {"sha256", a.sha256}, {"size", a.size}, {"file", a.file_name()}};
}

} // namespace

std::string FeedAsset::file_name() const {
    std::string path = url;
    const size_t q = path.find_first_of("?#");
    if (q != std::string::npos) path.resize(q);
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(slash + 1);
}

bool base64_decode(const std::string & text, std::string & out) {
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    size_t pad = 0;
    for (const char ch : text) {
        const unsigned char c = (unsigned char) ch;
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        if (c == '=') {
            ++pad;
            continue;
        }
        if (pad) return false; // data after padding
        const int v = base64_value(c);
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t) v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((char) ((acc >> bits) & 0xff));
        }
    }
    // Leftover bits must be zero padding of the last group, and at most two '='.
    if (pad > 2 || bits >= 6 || (acc & ((1u << bits) - 1)) != 0) return false;
    return true;
}

bool parse_public_key(const std::string & base64, Ed25519PublicKey & key) {
    std::string raw;
    if (!base64_decode(base64, raw) || raw.size() != key.size()) return false;
    for (size_t i = 0; i < key.size(); ++i)
        key[i] = (uint8_t) raw[i];
    return true;
}

bool verify_feed_signature(const std::string & message,
                           const std::string & signature,
                           const Ed25519PublicKey & key,
                           std::string & err) {
    std::string raw;
    if (signature.size() > kFeedSignatureMaxBytes || !base64_decode(signature, raw) || raw.size() != 64) {
        err = "the feed's signature is not a base64 Ed25519 signature";
        return false;
    }
    if (crypto_ed25519_check(reinterpret_cast<const uint8_t *>(raw.data()), key.data(),
                             reinterpret_cast<const uint8_t *>(message.data()), message.size()) != 0) {
        err = "the feed's signature does not match this build's key: the feed is not from this project's releases";
        return false;
    }
    return true;
}

bool parse_feed(const std::string & text, Feed & feed, std::string & err) {
    feed = Feed{};
    if (text.size() > kFeedMaxBytes) {
        err = "the feed is larger than a feed can be";
        return false;
    }
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception & e) {
        err = std::string("the feed is not JSON: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        err = "the feed is not a JSON object";
        return false;
    }
    std::array<uint64_t, 3> v{};
    if (!j.contains("version") || !j["version"].is_string() || !parse_version(j["version"].get<std::string>(), v)) {
        err = "the feed's version is not X.Y.Z";
        return false;
    }
    feed.version = j["version"].get<std::string>();
    if (j.contains("published") && j["published"].is_string()) feed.published = j["published"].get<std::string>();
    if (j.contains("notes") && j["notes"].is_string()) {
        feed.notes = j["notes"].get<std::string>();
        // A link a UI shows and the app opens in the browser: https or nothing.
        if (!is_https_url(feed.notes)) feed.notes.clear();
    }
    if (!read_asset_map(j, "installers", feed.installers, err)) return false;
    if (!read_asset_map(j, "archives", feed.archives, err)) return false;
    if (j.contains("packs")) {
        if (!j["packs"].is_array()) {
            err = "packs is not an array";
            return false;
        }
        std::set<std::string> seen;
        for (size_t i = 0; i < j["packs"].size(); ++i) {
            const json & p = j["packs"][i];
            const std::string where = "packs[" + std::to_string(i) + "]";
            FeedPack pack;
            if (!p.is_object() || !p.contains("id") || !p["id"].is_string() ||
                !is_pack_id(p["id"].get<std::string>())) {
                err = where + ".id is not a pack id (lowercase letters, digits and '-')";
                return false;
            }
            pack.id = p["id"].get<std::string>();
            if (!p.contains("target") || !p["target"].is_string() || p["target"].get<std::string>().empty()) {
                err = where + ".target is missing";
                return false;
            }
            pack.target = p["target"].get<std::string>();
            // One pack of an id per target: the id is the folder it installs into.
            if (!seen.insert(pack.target + "/" + pack.id).second) {
                err = where + " repeats pack '" + pack.id + "' for " + pack.target;
                return false;
            }
            pack.title = p.contains("title") && p["title"].is_string() ? p["title"].get<std::string>() : pack.id;
            if (!p.contains("needs_any") || !p["needs_any"].is_array() || p["needs_any"].empty()) {
                err = where + ".needs_any must list at least one library";
                return false;
            }
            for (const json & n : p["needs_any"]) {
                if (!n.is_string() || !is_library_name(n.get<std::string>())) {
                    err = where + ".needs_any holds something that is not a library file name";
                    return false;
                }
                pack.needs_any.push_back(n.get<std::string>());
            }
            if (!read_asset(p, where, pack.asset, err)) return false;
            feed.packs.push_back(std::move(pack));
        }
    }
    return true;
}

bool read_signed_feed(const std::string & text,
                      const std::string & signature,
                      const Ed25519PublicKey & key,
                      Feed & feed,
                      std::string & err) {
    return verify_feed_signature(text, signature, key, err) && parse_feed(text, feed, err);
}

bool parse_version(const std::string & text, std::array<uint64_t, 3> & out) {
    size_t i = (!text.empty() && (text[0] == 'v' || text[0] == 'V')) ? 1 : 0;
    for (int part = 0; part < 3; ++part) {
        if (i >= text.size() || !std::isdigit((unsigned char) text[i])) return false;
        uint64_t n = 0;
        size_t digits = 0;
        while (i < text.size() && std::isdigit((unsigned char) text[i])) {
            if (++digits > 9) return false;
            n = n * 10 + (uint64_t) (text[i] - '0');
            ++i;
        }
        out[part] = n;
        if (part < 2) {
            if (i >= text.size() || text[i] != '.') return false;
            ++i;
        }
    }
    return i == text.size();
}

int compare_versions(const std::array<uint64_t, 3> & a, const std::array<uint64_t, 3> & b) {
    for (int i = 0; i < 3; ++i)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

bool is_pack_id(const std::string & id) {
    if (id.empty() || id.size() > 32 || id[0] == '-') return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    return true;
}

json update_answer(const Feed & feed, const std::string & current, const std::string & target) {
    std::array<uint64_t, 3> cur{}, latest{};
    const bool newer =
        parse_version(current, cur) && parse_version(feed.version, latest) && compare_versions(latest, cur) > 0;
    const auto it = feed.installers.find(target);
    const bool has_installer = it != feed.installers.end();
    return json{{"current", current},
                {"latest", feed.version},
                {"available", newer && has_installer},
                {"notes", feed.notes},
                {"published", feed.published},
                {"target", target},
                {"installer", has_installer ? asset_json(it->second) : json()}};
}

} // namespace bmoe::server
