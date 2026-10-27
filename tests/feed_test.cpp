// Unit tests for the desktop app's server side (docs/desktop-app.md): the signed release feed,
// backend packs and the routes only a local client may use. No network, no driver, no model.
//
//   * the feed: base64, the Ed25519 signature over the exact bytes (a signature made by another
//     implementation from the RFC 8032 test key, and one made here), and the feed's shape, every
//     URL, digest, size and name it hands on held to its rule;
//   * versions and the update answer;
//   * packs: which apply, given an injected library loader; which folders are installed packs;
//   * archives: entries that would leave the folder, links, hard links, devices and entries under a
//     link are refused from tar's listings, and links out of an unpacked folder after the fact;
//   * a whole install with the system's tar over a fake network: the archive lands in its folder
//     with its marker, a wrong digest installs nothing;
//   * the local-only routes: refused from the network whatever the key, answered on loopback;
//   * the backend modules the engine loads from the pack folders.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "access.h"
#include "backend_packs.h"
#include "event_bus.h"
#include "feed.h"
#include "http_fetch.h"
#include "sha256.h"
#include "update_service.h"

#include "bmoe/probe.h"

#include "monocypher-ed25519.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace bmoe;
using namespace bmoe::server;

static int failures = 0;

static void check(bool ok, const std::string & what) {
    std::printf("[%s] %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

// RFC 8032, section 7.1, TEST 1: a published test key, never a real one.
static const char * kTestSeedHex = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
static const char * kTestPublicKey = "11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=";
// The signature of `{"version":"0.39.0"}` by that key, made with another Ed25519 implementation
// (Python's cryptography), which is what `openssl pkeyutl -sign -rawin` writes too: the two agree
// on every input, Ed25519 being deterministic.
static const char * kForeignSignature =
    "PZDx/SxtS22n354br9Ni/LheVeYN7mon/VQ/DG/Ui32C/9Q8paNHeNAGFDoWVHcG6u/ylscHdWem6CHG8QvOCw==";

static std::string base64_encode(const uint8_t * p, size_t n) {
    static const char * a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v =
            (uint32_t) p[i] << 16 | (i + 1 < n ? (uint32_t) p[i + 1] << 8 : 0) | (i + 2 < n ? (uint32_t) p[i + 2] : 0);
        out += a[(v >> 18) & 63];
        out += a[(v >> 12) & 63];
        out += i + 1 < n ? a[(v >> 6) & 63] : '=';
        out += i + 2 < n ? a[v & 63] : '=';
    }
    return out;
}

static std::string sign_with_test_key(const std::string & message) {
    uint8_t seed[32];
    for (int i = 0; i < 32; ++i)
        seed[i] = (uint8_t) std::stoi(std::string(kTestSeedHex + 2 * i, 2), nullptr, 16);
    uint8_t sk[64], pk[32], sig[64];
    crypto_ed25519_key_pair(sk, pk, seed);
    crypto_ed25519_sign(sig, sk, reinterpret_cast<const uint8_t *>(message.data()), message.size());
    return base64_encode(sig, 64);
}

static const std::string kSha = std::string(64, 'a');

static std::string sample_feed(const std::string & pack_sha = kSha, uint64_t pack_size = 1234) {
    return std::string(R"({
  "version": "0.39.0",
  "published": "2026-10-28",
  "notes": "https://github.com/Helldez/BigMoeOnEdge/releases/tag/v0.39.0",
  "installers": { "test-target": { "url": "https://example.org/dl/BigMoeOnEdge-v0.39.0-test-setup.exe", "sha256": ")") +
           kSha + R"(", "size": 100 } },
  "archives": { "test-target": { "url": "https://example.org/dl/bmoe-cli-v0.39.0-test.tar.gz", "sha256": ")" +
           kSha +
           R"(", "size": 50 } },
  "packs": [
    { "id": "gpu", "title": "A graphics card", "target": "test-target", "needs_any": ["libdriver.so.1", "driver.dll"],
      "url": "https://example.org/dl/bmoe-gpu-v0.39.0-test.tar.gz", "sha256": ")" +
           pack_sha + R"(", "size": )" + std::to_string(pack_size) + R"( },
    { "id": "gpu", "title": "Elsewhere", "target": "other-target", "needs_any": ["libdriver.so.1"],
      "url": "https://example.org/dl/bmoe-gpu-other.tar.gz", "sha256": ")" +
           kSha + R"(", "size": 10 }
  ]
})";
}

static void feed_tests() {
    std::string raw;
    check(base64_decode("aGVsbG8=", raw) && raw == "hello", "base64: padded");
    check(base64_decode("aGVsbG8", raw) && raw == "hello", "base64: unpadded");
    check(base64_decode("aGVs\nbG8=\n", raw) && raw == "hello", "base64: line breaks ignored");
    check(!base64_decode("aGVsbG8=x", raw), "base64: data after padding refused");
    check(!base64_decode("aGV$bG8=", raw), "base64: a character outside the alphabet refused");
    check(!base64_decode("a", raw), "base64: a lone character refused");

    Ed25519PublicKey key{};
    check(parse_public_key(kTestPublicKey, key), "key: the RFC 8032 test public key parses");
    Ed25519PublicKey bad{};
    check(!parse_public_key("aGVsbG8=", bad), "key: five bytes are not a key");

    std::string err;
    const std::string msg = R"({"version":"0.39.0"})";
    check(verify_feed_signature(msg, kForeignSignature, key, err),
          "signature: one made by another Ed25519 implementation verifies: " + err);
    check(sign_with_test_key(msg) == kForeignSignature, "signature: Monocypher signs exactly as the other one does");
    check(!verify_feed_signature(msg + " ", kForeignSignature, key, err), "signature: one byte more and it fails");
    check(!verify_feed_signature(R"({"version":"0.39.1"})", kForeignSignature, key, err),
          "signature: a changed byte fails: " + err);
    Ed25519PublicKey other = key;
    other[0] ^= 1;
    check(!verify_feed_signature(msg, kForeignSignature, other, err), "signature: another key fails");
    check(!verify_feed_signature(msg, "not base64!", key, err), "signature: not base64 fails");
    check(!verify_feed_signature(msg, "aGVsbG8=", key, err), "signature: the wrong length fails");

    const std::string text = sample_feed();
    Feed feed;
    check(read_signed_feed(text, sign_with_test_key(text), key, feed, err), "feed: a signed feed reads: " + err);
    check(feed.version == "0.39.0" && feed.published == "2026-10-28" && feed.installers.count("test-target") &&
              feed.archives.count("test-target") && feed.packs.size() == 2,
          "feed: version, installers, archives and packs read");
    check(feed.packs[0].needs_any.size() == 2 && feed.packs[0].asset.size == 1234 &&
              feed.packs[0].asset.file_name() == "bmoe-gpu-v0.39.0-test.tar.gz",
          "feed: a pack's libraries, size and file name");
    check(!read_signed_feed(text, sign_with_test_key(text + "\n"), key, feed, err),
          "feed: a signature of other bytes refuses the feed before it is parsed");

    auto refused = [&](const std::string & from, const std::string & to, const std::string & what) {
        std::string t = sample_feed();
        const size_t at = t.find(from);
        if (at == std::string::npos) {
            check(false, "feed: test setup, no '" + from + "'");
            return;
        }
        t.replace(at, from.size(), to);
        Feed f;
        std::string e;
        check(!parse_feed(t, f, e), "feed: refused, " + what + ": " + e);
    };
    refused("https://example.org/dl/BigMoeOnEdge", "http://example.org/dl/BigMoeOnEdge", "a plain http URL");
    refused("https://example.org/dl/bmoe-gpu-v0", "file:///etc/bmoe-gpu-v0", "a file URL");
    refused(R"("size": 100)", R"("size": 0)", "a size of zero");
    refused(R"("size": 100)", R"("size": -1)", "a negative size");
    refused(R"("id": "gpu", "title": "A)", R"("id": "../gpu", "title": "A)", "a pack id that is a path");
    refused(R"(["libdriver.so.1", "driver.dll"])", R"(["/tmp/evil.so"])", "a library named by path");
    refused(R"(["libdriver.so.1", "driver.dll"])", R"([])", "a pack that needs nothing");
    refused(R"("target": "other-target")", R"("target": "test-target")", "the same pack twice for one target");
    refused(R"("version": "0.39.0")", R"("version": "latest")", "a version that is not X.Y.Z");
    {
        std::string t = sample_feed();
        t.replace(t.find(kSha), 64, std::string(64, 'A'));
        Feed f;
        check(!parse_feed(t, f, err), "feed: refused, an uppercase digest");
    }
    check(!parse_feed("[1,2]", feed, err), "feed: an array is not a feed");
    check(!parse_feed(std::string(kFeedMaxBytes + 1, ' '), feed, err), "feed: one larger than a feed can be");

    std::array<uint64_t, 3> a{}, b{};
    check(parse_version("0.39.0", a) && parse_version("v0.38.12", b) && compare_versions(a, b) > 0,
          "version: 0.39.0 is newer than v0.38.12");
    check(parse_version("1.2.3", a) && parse_version("1.10.0", b) && compare_versions(a, b) < 0,
          "version: compared by number, not by text");
    check(!parse_version("1.2", a) && !parse_version("1.2.3-rc1", a) && !parse_version("", a),
          "version: anything but three numbers is refused");

    parse_feed(sample_feed(), feed, err);
    json up = update_answer(feed, "0.38.0", "test-target");
    check(up["available"].get<bool>() && up["latest"] == "0.39.0" && up["installer"]["size"] == 100,
          "update: a newer release with an installer for this target is available");
    up = update_answer(feed, "0.39.0", "test-target");
    check(!up["available"].get<bool>(), "update: the same version is not");
    up = update_answer(feed, "0.38.0", "nowhere");
    check(!up["available"].get<bool>() && up["installer"].is_null(), "update: no installer for this target, none");
    check(is_pack_id("cuda") && is_pack_id("hip-7") && !is_pack_id("-x") && !is_pack_id("CUDA") && !is_pack_id(""),
          "pack id: lowercase, digits and '-'");
}

static void write(const fs::path & p, const std::string & data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << data;
}

static void pack_tests() {
    Feed feed;
    std::string err;
    parse_feed(sample_feed(), feed, err);
    const FeedPack & gpu = feed.packs[0];
    std::vector<std::string> asked;
    auto loads_driver = [&](const std::string & n) {
        asked.push_back(n);
        return n == "driver.dll";
    };
    check(pack_applies(gpu, loads_driver) && asked.size() == 2, "pack: applies when the second library loads");
    check(!pack_applies(gpu, [](const std::string &) { return false; }), "pack: not when none does");
    check(!pack_applies(gpu, nullptr), "pack: not when nothing can be asked");

    const fs::path root = fs::temp_directory_path() / "bmoe_feed_test_packs";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "gpu");
    fs::create_directories(root / "other");
    fs::create_directories(root / ".staging-gpu");
    write(root / "stray.txt", "x");
    const std::vector<std::string> dirs = backend_pack_dirs(root.u8string());
    check(dirs.size() == 2 && fs::u8path(dirs[0]).filename() == "gpu" && fs::u8path(dirs[1]).filename() == "other",
          "pack dirs: each folder, sorted; no file, nothing in flight");
    check(write_pack_marker((root / "gpu").u8string(), InstalledPack{"gpu", "0.39.0", kSha}, err),
          "pack marker: written");
    write(root / "other" / kPackMarker, R"({"id": "gpu"})");
    const std::vector<InstalledPack> inst = installed_packs(root.u8string());
    check(inst.size() == 1 && inst[0].id == "gpu" && inst[0].version == "0.39.0",
          "installed: a marker that names another pack than its folder is not believed");

    json answer = backends_answer({"CPU"}, "test-target", &feed, inst, loads_driver, false);
    check(answer["packs"].size() == 1 && answer["packs"][0]["applies"].get<bool>() &&
              answer["packs"][0]["installed"].get<bool>() && answer["packs"][0]["current"].get<bool>(),
          "backends: this target's pack only, applying, installed and current");
    answer = backends_answer({"CPU"}, "test-target", nullptr, {}, loads_driver, true);
    check(answer["feed"].is_null() && answer["packs"].empty() && answer["restart_required"].get<bool>(),
          "backends: no feed read, no pack offered");
    fs::remove_all(root, ec);
}

static void archive_tests() {
    check(archive_entry_refusal("libggml-cuda.so").empty(), "entry: a plain name");
    check(archive_entry_refusal("./lib/libx.so").empty(), "entry: a nested name with ./");
    check(archive_entry_refusal("./").empty(), "entry: the root itself");
    check(!archive_entry_refusal("/etc/passwd").empty(), "entry: an absolute path is refused");
    check(!archive_entry_refusal("../evil.so").empty(), "entry: a leading .. is refused");
    check(!archive_entry_refusal("lib/../../evil.so").empty(), "entry: a .. inside is refused");
    check(!archive_entry_refusal("lib/..").empty(), "entry: a trailing .. is refused");
    check(!archive_entry_refusal("C:/Windows/evil.dll").empty(), "entry: a drive is refused");
    check(!archive_entry_refusal("..\\evil.dll").empty(), "entry: a backslash is refused");
    check(!archive_entry_refusal("").empty(), "entry: an empty name is refused");
    check(archive_entry_refusal("lib..so").empty(), "entry: two dots inside a name are fine");

    // As GNU tar and bsdtar print them.
    const std::vector<std::string> names = {"./", "./libggml-cuda.so", "./libcudart.so.12", "./libcudart.so.12.8.90"};
    const std::vector<std::string> gnu = {
        "drwxr-xr-x runner/runner     0 2026-10-28 10:00 ./",
        "-rwxr-xr-x runner/runner 12345 2026-10-28 10:00 ./libggml-cuda.so",
        "lrwxrwxrwx runner/runner     0 2026-10-28 10:00 ./libcudart.so.12 -> libcudart.so.12.8.90",
        "-rwxr-xr-x runner/runner 99999 2026-10-28 10:00 ./libcudart.so.12.8.90"};
    check(archive_listing_refusal(names, gnu).empty(), "listing: files, a folder and a link inside are fine");
    const std::vector<std::string> bsd = {"-rw-r--r--  0 0      0      1234 Oct 28  2026 ggml-cuda.dll"};
    check(archive_listing_refusal({"ggml-cuda.dll"}, bsd).empty(), "listing: bsdtar's format of a zip");
    check(!archive_listing_refusal({"a", "a/x"}, {"lrwxrwxrwx 0 0 0 0 Oct 28 2026 a -> /etc", "-rw-r--r-- 0 a/x"})
               .empty(),
          "listing: an entry under a link is refused");
    check(!archive_listing_refusal({"./a", "a/x"}, {"lrwxrwxrwx u/g 0 d t ./a -> /etc", "-rw-r--r-- u/g 0 d t a/x"})
               .empty(),
          "listing: and so is one written with another spelling of the link's path");
    check(!archive_listing_refusal({"b"}, {"hrw-r--r-- u/g 0 d t b link to /etc/passwd"}).empty(),
          "listing: a hard link is refused");
    check(!archive_listing_refusal({"b"}, {"-rw-r--r-- u/g 0 d t b link to a"}).empty(),
          "listing: a hard link shown as a file is refused");
    check(!archive_listing_refusal({"dev"}, {"crw-r--r-- u/g 0 d t dev"}).empty(), "listing: a device is refused");
    check(!archive_listing_refusal({"../x"}, {"-rw-r--r-- u/g 0 d t ../x"}).empty(), "listing: a name that leaves");
    check(!archive_listing_refusal({"a", "b"}, {"-rw-r--r-- u/g 0 d t a"}).empty(), "listing: listings that disagree");
    check(!archive_listing_refusal({}, {}).empty(), "listing: an empty archive");

#if !defined(_WIN32)
    const fs::path root = fs::temp_directory_path() / "bmoe_feed_test_tree";
    std::error_code ec;
    fs::remove_all(root, ec);
    write(root / "libx.so.1.2", "x");
    fs::create_symlink("libx.so.1.2", root / "libx.so.1");
    fs::create_directories(root / "sub");
    fs::create_symlink("../libx.so.1.2", root / "sub" / "up");
    check(unpacked_tree_refusal(root.u8string()).empty(), "tree: links that stay inside are fine");
    fs::create_symlink("../../outside", root / "sub" / "out");
    check(!unpacked_tree_refusal(root.u8string()).empty(), "tree: a link out through .. is refused");
    fs::remove(root / "sub" / "out");
    fs::create_symlink("/etc/passwd", root / "abs");
    check(!unpacked_tree_refusal(root.u8string()).empty(), "tree: an absolute link is refused");
    fs::remove_all(root, ec);
#endif
}

// The network as the install sees it: the feed and its signature from memory, the pack copied
// from a file the test made.
class FakeNet final : public HttpFetch {
public:
    std::map<std::string, std::string> pages;
    std::map<std::string, std::string> files;
    bool get(const std::string & url, uint64_t, uint64_t, const std::string &, std::string & body, std::string & error)
        override {
        const auto it = pages.find(url);
        if (it == pages.end()) {
            error = "404";
            return false;
        }
        body = it->second;
        return true;
    }
    bool download(const std::string & url,
                  const std::string & dest,
                  const std::function<bool(uint64_t)> & progress,
                  std::string & error) override {
        const auto it = files.find(url);
        if (it == files.end()) {
            error = "404";
            return false;
        }
        std::error_code ec;
        fs::create_directories(fs::u8path(dest).parent_path(), ec);
        fs::copy_file(fs::u8path(it->second), fs::u8path(dest), fs::copy_options::overwrite_existing, ec);
        if (progress) progress(fs::file_size(fs::u8path(dest), ec));
        return !ec;
    }
};

// The state of the last `backend` event of `id`, once it is no longer running.
static std::string wait_for(EventBus::Queue & q, const std::string & id, json & last) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string frame;
        if (!EventBus::next(q, frame, std::chrono::milliseconds(200))) return "closed";
        // Pack progress is its own event, `backend`.
        if (frame.compare(0, 15, "event: backend\n") != 0) continue;
        const size_t at = frame.find("data: ");
        if (frame.empty() || at == std::string::npos) continue;
        const json e = json::parse(frame.substr(at + 6));
        if (e.value("id", "") != id) continue;
        last = e;
        const std::string state = e.value("state", "");
        if (state != "running") return state;
    }
    return "timeout";
}

static void install_tests() {
    const fs::path dir = fs::temp_directory_path() / "bmoe_feed_test_install";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const fs::path src = dir / "src";
    write(src / "libggml-gpu.so", "module");
    write(src / "LICENSE-GPU.txt", "licence");
    const fs::path archive = dir / "pack.tar.gz";
    std::string out;
    if (run_system_command({"tar", "-czf", archive.string(), "-C", src.string(), "."}, out) != 0) {
        check(false, "install: no tar to make the test archive with: " + out);
        return;
    }
    std::string sha, err;
    sha256_file(archive.u8string(), sha, err);
    const uint64_t size = fs::file_size(archive);
    const std::string feed_url = "https://example.org/feed.json";

    for (const bool tampered : {false, true}) {
        const fs::path data = dir / (tampered ? "data-bad" : "data");
        const std::string text = sample_feed(tampered ? std::string(64, 'b') : sha, size);
        FakeNet net;
        net.pages[feed_url] = text;
        net.pages[feed_url + ".sig"] = sign_with_test_key(text);
        net.files["https://example.org/dl/bmoe-gpu-v0.39.0-test.tar.gz"] = archive.u8string();
        EventBus bus;
        auto q = bus.subscribe();
        UpdateConfig cfg;
        cfg.data_dir = data.u8string();
        cfg.feed_url = feed_url;
        cfg.public_key = kTestPublicKey;
        cfg.target = "test-target";
        cfg.version = "0.38.0";
        cfg.library_loads = [](const std::string & n) { return n == "libdriver.so.1"; };
        UpdateService svc(bus, net, cfg);

        UpdateService::Reply r = svc.install_pack("gpu", false);
        check(r.status == 409, "install: refused before any feed was read");
        r = svc.backends(true);
        check(r.status == 200 && r.body["packs"].size() == 1 && r.body["packs"][0]["applies"].get<bool>(),
              "install: the feed is read and the pack applies");
        r = svc.install_pack("nope", false);
        check(r.status == 404, "install: a pack the feed does not offer");
        r = svc.install_pack("gpu", false);
        check(tampered ? r.status == 502 : (r.status == 200 && r.body["restart_required"].get<bool>()),
              "install: answered once finished, " + std::string(tampered ? "with the refusal" : "restart required"));
        json last;
        const std::string state = wait_for(*q, "gpu", last);
        const fs::path installed = data / "backends" / "gpu";
        if (!tampered) {
            check(state == "done" && last.value("restart_required", false), "install: done, restart required");
            check(fs::exists(installed / "libggml-gpu.so") && fs::exists(installed / "LICENSE-GPU.txt") &&
                      fs::exists(installed / kPackMarker),
                  "install: the files and the marker are in <data>/backends/gpu");
            r = svc.backends(false);
            check(r.body["packs"][0]["installed"].get<bool>() && r.body["packs"][0]["current"].get<bool>(),
                  "install: the pack reads as installed and current, from the kept feed");
        } else {
            check(state == "error" && last.value("error", "").find("SHA-256") != std::string::npos,
                  "install: a digest that does not match installs nothing: " + last.value("error", ""));
            check(!fs::exists(installed), "install: and no folder is left");
        }
        bus.close_all();
    }

    // A feed whose signature is not this key's is read by nothing.
    {
        FakeNet net;
        const std::string text = sample_feed();
        net.pages[feed_url] = text;
        net.pages[feed_url + ".sig"] = kForeignSignature;
        EventBus bus;
        UpdateConfig cfg;
        cfg.data_dir = (dir / "data-unsigned").u8string();
        cfg.feed_url = feed_url;
        cfg.public_key = kTestPublicKey;
        cfg.target = "test-target";
        UpdateService svc(bus, net, cfg);
        const UpdateService::Reply b = svc.backends(true);
        check(svc.update().status == 502 && b.status == 200 && b.body.contains("feed_error") &&
                  b.body["feed"].is_null(),
              "feed: one signed by nobody this build trusts is refused, and the backends say why");
        cfg.public_key = "";
        UpdateService keyless(bus, net, cfg);
        check(keyless.update().status == 503 && keyless.update().body["code"] == "no_feed_key",
              "feed: a build with no key reads no feed");
    }
    fs::remove_all(dir, ec);
}

static void pending_tests() {
    const fs::path root = fs::temp_directory_path() / "bmoe_feed_test_pending";
    std::error_code ec;
    fs::remove_all(root, ec);
    write(root / "gpu" / "old.so", "old");
    write(root / ".pending-gpu" / "new.so", "new");
    write(root / ".old-gpu" / "older.so", "older");
    check(finish_pending_packs(root.u8string()).empty(), "pending: moved without a problem");
    check(fs::exists(root / "gpu" / "new.so") && !fs::exists(root / "gpu" / "old.so") &&
              !fs::exists(root / ".pending-gpu") && !fs::exists(root / ".old-gpu"),
          "pending: the new copy replaces the old one at start, leftovers go");
    fs::remove_all(root, ec);
}

static void access_tests() {
    AccessPolicy policy;
    RequestFacts r;
    r.host = "127.0.0.1:8765";
    r.method = "POST";
    r.path = "/api/backends/cuda/install";
    r.content_type = "application/json";
    r.remote_addr = "127.0.0.1";
    check(check_access(policy, r).allowed(), "local only: a loopback client may install a pack");
    r.remote_addr = "::ffff:127.0.0.1";
    check(check_access(policy, r).allowed(), "local only: so may an IPv4-mapped loopback one");
    policy.loopback = false;
    policy.api_key = "k";
    r.host = "192.168.1.5:8765";
    r.authorization = "Bearer k";
    r.remote_addr = "192.168.1.9";
    AccessDecision d = check_access(policy, r);
    check(d.status == 403 && d.code == "local_only", "local only: a network client with the key may not");
    for (const auto & [m, p] : std::vector<std::pair<std::string, std::string>>{{"GET", "/api/backends"},
                                                                                {"HEAD", "/api/backends"},
                                                                                {"GET", "/api/update"},
                                                                                {"POST", "/api/update/download"},
                                                                                {"POST", "/api/models/url"},
                                                                                {"DELETE", "/api/models/local/a/b"}}) {
        r.method = m;
        r.path = p;
        d = check_access(policy, r);
        check(d.status == 403 && d.code == "local_only", "local only: " + m + " " + p + " from the network");
    }
    for (const auto & [m, p] : std::vector<std::pair<std::string, std::string>>{{"GET", "/api/backends/x"},
                                                                                {"POST", "/api/backends/a/b/install"},
                                                                                {"GET", "/api/models"},
                                                                                {"DELETE", "/api/models/local"},
                                                                                {"POST", "/api/update"}})
        check(local_only_route(m, p) == nullptr, "local only: " + m + " " + p + " is not on the list");
    r.method = "GET";
    r.path = "/api/models";
    check(check_access(policy, r).allowed(), "local only: the rest of the API still answers the network");
}

static void module_tests() {
#if defined(_WIN32)
    const std::string pre = "ggml-", ext = ".dll";
#else
    const std::string pre = "libggml-", ext = ".so";
#endif
    const fs::path root = fs::temp_directory_path() / "bmoe_feed_test_modules";
    std::error_code ec;
    fs::remove_all(root, ec);
    write(root / "exe" / (pre + "cpu-haswell" + ext), "");
    write(root / "exe" / (pre + "base" + ext), "");
    write(root / "exe" / (pre + "vulkan" + ext), "");
    write(root / "p1" / (pre + "cuda" + ext), "");
    write(root / "p1" / (pre + "base" + ext), "");
    write(root / "p1" / (pre + "cpu-zen4" + ext), "");
    write(root / "p1" / (pre + "vulkan" + ext), "");
    write(root / "p1" / "libcudart.so.12", "");
    write(root / "p2" / (pre + "cuda" + ext), "");
    write(root / "p2" / (pre + "hip" + ext), "");
    write(root / "p2" / (pre + "hip-gfx" + ext), "");
    const std::vector<std::string> mods = extra_backend_modules(
        (root / "exe").u8string(), {(root / "p1").u8string(), (root / "p2").u8string(), (root / "none").u8string()});
    std::vector<std::string> names;
    for (const std::string & m : mods)
        names.push_back(fs::u8path(m).parent_path().filename().u8string() + "/" + fs::u8path(m).filename().u8string());
    check(names == std::vector<std::string>{"p1/" + pre + "cuda" + ext, "p2/" + pre + "hip" + ext},
          "modules: one per backend nobody before had; not the base library, the CPU or a runtime library");
    fs::remove_all(root, ec);
}

int main() {
    feed_tests();
    pack_tests();
    archive_tests();
    install_tests();
    pending_tests();
    access_tests();
    module_tests();
    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall feed, pack and local-only checks passed\n");
    return 0;
}
