// A Hugging Face repository as a source of models: what a person typed, what the repository holds,
// and the catalog entry a chosen model becomes. Pure string and JSON rules, so they are tested
// without the network; the fetching is the caller's (http_fetch.h).
//
// The catalog stays the one place a download's URLs are written: a model picked here becomes a
// catalog entry built by the server from a repository id and file names it validated, so the
// download path (the server's curl, or a phone's own downloader reading `files[].url`) does not
// change and never takes a URL from a client.
#pragma once

#include "json_util.h"
#include "model_library.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bmoe::server {

// Where a person said to look: a repository, a revision, and possibly one file in it.
struct HfRef {
    std::string repo;              // "owner/name"
    std::string revision = "main"; // a branch, tag or commit
    std::string file;              // a path inside the repository, when the text named one
};

// Reads "owner/name", or a huggingface.co / hf.co URL to a repository, a tree, a blob or a
// resolve link. Every character of the repository id, the revision and the path is checked,
// because they become a URL the server fetches and a file name it writes. False with the reason.
bool parse_hf_ref(const std::string & text, HfRef & out, std::string & error);

// One file of a repository's listing.
struct HfFile {
    std::string path; // inside the repository, with its folders
    uint64_t bytes = 0;
};

// One model a repository offers: a gguf, or a split set listed by its first shard.
struct HfModel {
    std::string name;          // the first file's path: what the model is called here and in the catalog id
    std::string quant;         // as the file name spells it ("Q4_K_M", "UD-IQ2_M"), "" when it does not
    std::vector<HfFile> files; // in shard order, the ones the listing has
    int count = 1;             // shards the set declares
    uint64_t bytes = 0;        // of the files present
    bool complete() const { return (int) files.size() == count; }
};

// The models in a tree listing (the JSON array the `/api/models/<repo>/tree/<rev>` endpoint
// answers, recursive). Files that are not gguf are ignored; shards group as model_files.h groups
// a directory. Sorted by name. False with the reason when the JSON is not a listing.
bool parse_hf_tree(const json & tree, std::vector<HfModel> & out, std::string & error);

// The quantization a gguf file name spells, or "".
std::string quant_from_name(const std::string & name);

// The listing's URL, and a file's resolve URL, on `endpoint` ("https://huggingface.co", or a
// mirror). The path's segments are the validated ones, so nothing is escaped.
std::string hf_tree_url(const std::string & endpoint, const HfRef & ref);
std::string hf_resolve_url(const std::string & endpoint, const HfRef & ref, const std::string & path);

// The catalog entry a model of a repository becomes: id "hf:<repo>/<first file>", one file per
// shard named by its base name, each with its resolve URL and size.
CatalogEntry hf_catalog_entry(const std::string & endpoint, const HfRef & ref, const HfModel & model);

// Whether a catalog id names an entry made here.
bool is_hf_catalog_id(const std::string & id);

// One repository a search found.
struct HfRepoHit {
    std::string id; // "owner/name"
    uint64_t downloads = 0;
    uint64_t likes = 0;
    std::string modified; // the site's lastModified, as given
    bool gated = false;   // a token is needed to read it
};

// The search URL on `endpoint`: repositories with gguf files whose name matches `query`
// (percent-encoded here), the most downloaded first, at most `limit`.
std::string hf_search_url(const std::string & endpoint, const std::string & query, int limit);

// The repositories a search listing holds (the JSON array `/api/models?search=` answers). An
// entry without an id is skipped. False with the reason when the JSON is not a listing.
bool parse_hf_search(const json & hits, std::vector<HfRepoHit> & out, std::string & error);

// What the numbers mean on this machine, in one sentence each, from facts alone: no platform,
// no name. `budget` is what this process may hold resident (0 when unknown).
struct FitInputs {
    uint64_t file_bytes = 0;    // the whole model
    uint64_t present_bytes = 0; // already on disk, complete files only
    uint64_t disk_free = 0;     // in the models directory
    uint64_t dense_bytes = 0;   // what stays resident under streaming
    uint64_t token_cycle = 0;   // the expert bytes one token touches
    uint64_t budget = 0;        // residency budget, 0 unknown
    bool readable = false;      // the header parsed
    bool is_moe = false;
    bool streamable = false;
    std::string refusal; // why a MoE is not streamed, when it is not
};
struct FitVerdict {
    bool downloadable = true; // false only when the disk cannot take it
    std::string mode;         // "streams", "plain mmap", "not readable"
    std::string disk;         // the disk sentence
    std::string memory;       // the memory sentence
};
FitVerdict fit_verdict(const FitInputs & in);

} // namespace bmoe::server
