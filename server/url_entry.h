// A model from a direct link: a gguf URL a person pasted, made into a catalog entry the same way a
// repository's model is (hf_repo.h), so its download is the catalog's, with the same resume,
// cancel and progress. Pure string rules, tested without the network (tests/server_test.cpp); the
// size the entry carries is the caller's to ask for (http_fetch.h: HttpFetch::size).
//
// A link is a URL a client chose, which the server then fetches: only https, no credentials in it,
// and a file name the models folder can hold as it is. The name is the URL's last path segment and
// nothing else, checked to the same narrow alphabet a repository's file names are.
#pragma once

#include "model_library.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bmoe::server {

struct UrlModel {
    std::string host; // where the files come from, for the entry's blurb
    // Each file the model is made of, in shard order: its name in the models folder and its URL.
    // A link to any shard of a split set ("<stem>-00002-of-00003.gguf") names the whole set.
    std::vector<CatalogFile> files;
};

// Reads a direct https link to a gguf file. False, with the reason, for anything else: another
// scheme, credentials or a missing host, characters a URL must not carry raw, a last segment that
// is not a gguf file name made of letters, digits and `._-+` (percent-escapes decoded first).
bool parse_model_url(const std::string & text, UrlModel & out, std::string & error);

// The catalog entry the model becomes: id "url:<first file>", titled by that name, with each
// file's size from `bytes` (same order as `model.files`; 0 where unknown).
CatalogEntry url_catalog_entry(const UrlModel & model, const std::vector<uint64_t> & bytes);

// Whether a catalog id names an entry made from a link.
bool is_url_catalog_id(const std::string & id);

// What a HEAD request's answer says, from the headers of every response curl saw (the redirects
// it followed, then the last): the last response's status and Content-Length, 0 for each it lacks.
// The status is read from the headers and not from curl's exit code, which over HTTP/2 reports a
// refused HEAD as a transfer error rather than as the HTTP error it is.
struct HeadAnswer {
    int status = 0;
    uint64_t length = 0;
};
HeadAnswer head_answer(const std::string & headers);

} // namespace bmoe::server
