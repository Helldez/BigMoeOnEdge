// How the gguf files of a directory become models: a split set (llama.cpp's gguf-split naming,
// "<stem>-00002-of-00005.gguf") is one model, listed by its first shard, which is the file the
// engine opens and resolves the rest from. Pure string rules, so they are tested without a disk.
#pragma once

#include <set>
#include <string>
#include <vector>

namespace bmoe::server {

// True for a file name ending in ".gguf", in any case.
bool is_gguf_name(const std::string & name);

struct ShardName {
    std::string stem; // the name before "-NNNNN-of-NNNNN.gguf"
    int index = 0;    // 1-based
    int count = 0;
};

// Parses "<stem>-<5 digits>-of-<5 digits>.gguf" (any case for "of" and the extension). False for
// any other name, and for an index or a count that cannot be a shard (0, or an index past the
// count), so a file that only looks like one is listed as a model of its own.
bool parse_shard_name(const std::string & name, ShardName & out);

// The name of shard `index` of `count` of `stem`, as gguf-split writes it.
std::string shard_file_name(const std::string & stem, int index, int count);

struct ModelFiles {
    std::string first;              // the file a load opens: the name itself, or shard 1 of the set
    int count = 1;                  // files the model is made of
    std::vector<std::string> files; // the ones present, in shard order
    bool complete() const { return (int) files.size() == count; }
};

// The models a directory's file names make, sorted by first file. Names that are not gguf files
// are ignored; a set missing shards is still one model, with fewer files than its count.
std::vector<ModelFiles> group_model_files(const std::vector<std::string> & names);

} // namespace bmoe::server
