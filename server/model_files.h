// How the gguf files of a directory become models: a split set (llama.cpp's gguf-split naming,
// "<stem>-00002-of-00005.gguf") is one model, listed by its first shard, which is the file the
// engine opens and resolves the rest from; and what a file is for (a model, or a file that only
// goes with one). Pure rules over names and header facts, so they are tested without a model;
// the one function that reads a disk, scan_model_files, only lists names.
#pragma once

#include "bmoe/probe.h"

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
    std::string folder;             // the subfolder of the scanned directory it is in, "" for the directory itself
    std::string first;              // the file a load opens: the name itself, or shard 1 of the set
    int count = 1;                  // files the model is made of
    std::vector<std::string> files; // the ones present, in shard order
    bool complete() const { return (int) files.size() == count; }
};

// The models a directory's file names make, sorted by first file. Names that are not gguf files
// are ignored; a set missing shards is still one model, with fewer files than its count.
std::vector<ModelFiles> group_model_files(const std::vector<std::string> & names);

// The models in `dir` and in each folder directly inside it, which is where a split set downloaded
// as a repository's folder lands ("<dir>/<model>/<model>-00001-of-00003.gguf"). Exactly one level:
// a folder inside a subfolder is not entered, and neither is a link to a folder, so a link cannot
// make the scan loop or reach outside `dir` through a name inside it. A model found in a subfolder
// carries its name in `folder`; every name is grouped within its own folder. Opening one is still
// confine_model_path's decision (access.h), which resolves links and refuses anything outside the
// roots whatever the listing showed.
std::vector<ModelFiles> scan_model_files(const std::string & dir);

// What a gguf file is for, from its header: a model to chat with, or a file that only goes with
// one. Names (`mmproj-`, `mtp-`) are a convention and not the format, so they never decide this.
enum class FileRole { Unknown, Model, Projector, Draft, Other };

// "model", "projector", "draft", "other"; "" for Unknown (the header could not be read).
const char * file_role_name(FileRole r);

// The role the header facts give. A projector says so (`general.type` "mmproj", or llama.cpp's
// projector architecture); a draft reads another model's layers (`<arch>.target_layers`) or is
// nothing but prediction heads (every block counted in `<arch>.nextn_predict_layers`); a file
// that declares another type, or no transformer blocks at all, is no causal language model. A
// model that carries its own prediction head beside its blocks is a model.
FileRole file_role(const GgufHeaderFacts & h);

// The role a file NAME suggests, for ordering and grouping a listing whose headers have not been
// read (a repository's): "mmproj" anywhere, or a draft prefix. A hint only; file_role decides.
FileRole file_role_hint(const std::string & name);

} // namespace bmoe::server
