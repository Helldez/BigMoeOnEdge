// Backend packs (docs/desktop-app.md, "Backend packs"): which of the feed's packs apply to this
// machine, where installed ones live, and which archives may be unpacked at all. Policy only: the
// library loader is a parameter and the archive is read from tar's own listing, so every rule is
// tested without a driver, a download or a tar (tests/feed_test.cpp).
#pragma once

#include "feed.h"
#include "json_util.h"

#include <functional>
#include <string>
#include <vector>

namespace bmoe::server {

// Whether the library named (a bare file name, as needs_any lists it) loads on this machine.
using LibraryProbe = std::function<bool(const std::string & name)>;

// A pack applies when one of the driver libraries it names loads here: the vendor's runtime is
// there to drive, so the backend has a device to offer. No library named, no answer: false.
bool pack_applies(const FeedPack & pack, const LibraryProbe & loads);

// The folders under `backends_root` (<data>/backends) the engine loads backend modules from: one
// per installed pack, by name, sorted. A name starting with '.' is an install in flight or one
// being replaced, never loaded.
std::vector<std::string> backend_pack_dirs(const std::string & backends_root);

// What an installed pack says of itself, kept beside its files as pack.json.
struct InstalledPack {
    std::string id;
    std::string version; // the release it came from
    std::string sha256;  // of the archive it was unpacked from
};

inline constexpr const char * kPackMarker = "pack.json";

bool read_pack_marker(const std::string & pack_dir, InstalledPack & out);
bool write_pack_marker(const std::string & pack_dir, const InstalledPack & pack, std::string & err);

// Every installed pack under `backends_root`, by its marker (a folder without one is not counted).
std::vector<InstalledPack> installed_packs(const std::string & backends_root);

// Why an archive entry named `name` may not be unpacked, or "" when it may: it must stay inside
// the folder it is unpacked into. Refused: an empty name, an absolute one (a leading separator, a
// drive or any ':'), a backslash (a separator on Windows, where tar would follow it), a NUL, and a
// `..` segment anywhere.
std::string archive_entry_refusal(const std::string & name);

// Why an archive may not be unpacked, from tar's two listings of it: `names` (tar -t, one entry a
// line) and `verbose` (tar -tv, the same entries in the same order). Every name passes
// archive_entry_refusal; every entry is a file, a folder or a symbolic link (no hard link, no
// device); and no entry lies under a link, which would write wherever the link points before
// anything could check it. "" when it may.
std::string archive_listing_refusal(const std::vector<std::string> & names, const std::vector<std::string> & verbose);

// Why a folder an archive was unpacked into may not be kept, or "" when it may: every symbolic link
// in it points inside it (relative, no way out through `..`), and nothing in it is anything but a
// file, a folder or such a link.
std::string unpacked_tree_refusal(const std::string & dir);

// GET /api/backends: {registered, target, feed, packs, installed, restart_required}. `feed` is
// null when no verified feed has been read; each pack of this target says whether it applies here
// and whether it is installed (and at which release).
json backends_answer(const std::vector<std::string> & registered,
                     const std::string & target,
                     const Feed * feed,
                     const std::vector<InstalledPack> & installed,
                     const LibraryProbe & loads,
                     bool restart_required);

} // namespace bmoe::server
