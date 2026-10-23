// Deleting a model from the models folder, decided apart from HTTP and the library's locks so the
// rules are tested on their own (tests/server_test.cpp). A client names a model the way the
// listing does (`folder/name`, or `name` at the top of the folder), never a path: the files deleted
// are the ones the folder's own scan groups under that name (model_files.h), so a split set goes as
// a unit and nothing outside the models folder can be reached. A scan directory is never written:
// its models are loaded in place and are not this server's to remove.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bmoe::server {

// What deleting `rel` from `models_dir` would remove. `rel` is `name` or `folder/name`, each part a
// plain name, and must be a model the scan of `models_dir` lists by that first file.
struct ModelDeletion {
    std::string folder;             // "" at the top of the models folder
    std::vector<std::string> names; // the files of the model, as the scan found them
    std::vector<std::string> paths; // the same, joined to the folder
};

// False, with the reason, when `rel` is not such a model. `not_found` tells a model that is not
// there (404) from a name that could never be one (400).
bool find_model_to_delete(
    const std::string & models_dir, const std::string & rel, ModelDeletion & out, bool & not_found, std::string & err);

// Whether any of `paths` is the file `loaded` (the model a session has open), by identity: a
// link or a second spelling of the same file is the same file.
bool deletion_touches(const ModelDeletion & d, const std::string & loaded);

// Remove the files, and the model's own folder once it is empty. A link is removed as the link,
// its target kept. `freed` is the bytes of the regular files removed. False, with the reason, at
// the first file that could not be removed (those before it are gone).
bool delete_model_files(const std::string & models_dir, const ModelDeletion & d, uint64_t & freed, std::string & err);

} // namespace bmoe::server
