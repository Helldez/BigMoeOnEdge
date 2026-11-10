// The draft half of speculative decoding, where the draft is made by weights: the model's own head
// or a draft model in a file of its own (SpecConfig::uses_driver).
//
// llama.cpp's speculative driver (common/speculative.h) owns every kind of draft: it reads a draft
// file's header to say what the file is, moves the target's hidden states to the draft context, runs
// the draft graph and keeps the draft state. This module is the one place the engine turns the
// driver's answer into what it has to do itself: how the draft weights are loaded, what context they
// draft in, and the driver built over the pair. Nothing outside it names a kind of drafter, so a kind
// the driver learns with a submodule bump needs no change here unless it needs a new fact.
//
// Shared by the session, which opens the contexts, and by the placement probe, which has to fit the
// same contexts beside the target before the session exists.
#pragma once

#include "bmoe/config.h"

#include "common.h"
#include "llama.h"
#include "speculative.h"

#include <string>

namespace bmoe::detail {

// What drafts, in the driver's own terms.
struct DraftKind {
    common_speculative_type type = COMMON_SPECULATIVE_TYPE_NONE;

    bool known() const { return type != COMMON_SPECULATIVE_TYPE_NONE; }
    // The nextn block: a graph llama.cpp builds only in a context of the MTP type, over weights it
    // loads only when asked to. Every other kind is a model of its own in an ordinary context.
    bool is_head() const { return type == COMMON_SPECULATIVE_TYPE_DRAFT_MTP; }
    std::string name() const;
};

// The kind `spec` drafts with: the model's head for the head's source, and for a drafter whatever
// its file's header says the file is. Not known() for a drafter file the driver has no draft for,
// and for a source without weights.
DraftKind draft_kind(const SpecConfig & spec);

// A draft file's model parameters: whole and resident, on the devices in `devices` (the target's own
// list, so the file cannot land on a device the plan left out) or on the host.
llama_model_params
draft_file_model_params(const DraftKind & kind, const SpecConfig & spec, ggml_backend_dev_t * devices);

// The draft context's parameters, from the target's: its own narrow graph and KV, no rollback
// snapshots (the target is the one that speculates) and no device expert cache.
llama_context_params
draft_context_params(const DraftKind & kind, const SpecConfig & spec, const llama_context_params & target);

// The driver over the two contexts. Null when llama.cpp cannot build one for this pair.
common_speculative *
make_draft_driver(const DraftKind & kind, const SpecConfig & spec, llama_context * ctx_tgt, llama_context * ctx_dft);

} // namespace bmoe::detail
