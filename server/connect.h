// Connecting other apps to this server (docs/agent.md, "Connecting other apps"): the clients the
// catalog lists (catalog/clients.json, data, not code), their settings filled in for this server,
// and writing a client's settings file with a backup of the one it replaces. JSON and files, no
// HTTP; the home folder is passed in, so a test writes into a folder of its own.
#pragma once

#include "json_util.h"

#include <string>

namespace bmoe::server {

struct ConnectFacts {
    std::string origin; // http://127.0.0.1:8765
    bool has_key = false;
    std::string model; // the loaded model's id, "" when none
    std::string home;  // the user's home folder, "" when there is none (a phone app)
};

// The catalog's entries as GET /api/connect shows them: {id, name, how, snippet, file, writable}.
// `catalog` is the parsed clients.json; an entry missing its id, name or snippet is left out.
json connect_list(const json & catalog, const ConnectFacts & facts);

// `text` with the catalog's {{placeholders}} replaced. `quoted`: each value is escaped for a
// double-quoted string (TOML and JSON read `\"` and `\\` alike).
std::string fill_template(const std::string & text, const ConnectFacts & facts, bool quoted);

// The new content of a settings file of format `write` (toml_block | json_merge) that holds
// `snippet`, given its current content (`existing`, "" when there is none). toml_block replaces the
// block between its own markers, or appends one; json_merge merges the snippet's object into the
// file's, key by key. False with the reason (a JSON file that does not parse is not overwritten).
bool merge_settings(const std::string & write,
                    const std::string & existing,
                    const std::string & snippet,
                    std::string & out,
                    std::string & err);

// Write client `id`'s settings: `file` and `backup` (the copy of the file it replaced, "" when there
// was none). False with an HTTP status and the reason.
bool connect_write(const json & catalog,
                   const std::string & id,
                   const ConnectFacts & facts,
                   json & result,
                   int & status,
                   std::string & err);

} // namespace bmoe::server
