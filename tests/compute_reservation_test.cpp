// Gate: request_compute_reservation (core/src/engine/compute_reservation.h) makes the context's next
// decode reserve its compute buffers again, once, and leaves the context computing what it did.
//
// What it guards is a llama.cpp behaviour, not an API: which setter's change llama.cpp answers with a
// new scheduler. That is only visible in its log, where every reservation opens with the line
// `sched_reserve: reserving ...`, so the gate counts those lines. If upstream stops reserving on the
// setter, the count stays put and this fails; if it renames the line, this fails too, which is the
// moment to find the new evidence rather than to lose the check.
#include "compute_reservation.h"

#include "llama.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static int g_reservations = 0;

// The line that opens a reservation, and only it: the same function also logs "reserving full memory
// module" inside it.
static const char k_reservation_line[] = "sched_reserve: reserving ...";

static void count_reservations(ggml_log_level, const char * text, void *) {
    if (text && std::strstr(text, k_reservation_line)) ++g_reservations;
}

static bool decode_one(llama_context * ctx, llama_token tok, llama_pos pos) {
    llama_batch b = llama_batch_init(1, 0, 1);
    b.n_tokens = 1;
    b.token[0] = tok;
    b.pos[0] = pos;
    b.n_seq_id[0] = 1;
    b.seq_id[0][0] = 0;
    b.logits[0] = true;
    const bool ok = llama_decode(ctx, b) == 0;
    llama_batch_free(b);
    return ok;
}

int main(int argc, char ** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s model.gguf\n", argv[0]);
        return 2;
    }
    llama_log_set(&count_reservations, nullptr);
    llama_backend_init();

    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    llama_model * model = llama_model_load_from_file(argv[1], mp);
    if (!model) {
        std::fprintf(stderr, "could not load %s\n", argv[1]);
        return 1;
    }
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 256;
    cp.n_batch = 64;
    cp.n_ubatch = 64;
    cp.embeddings = false; // the precondition the request restores
    llama_context * ctx = llama_init_from_model(model, cp);
    CHECK(ctx != nullptr);
    if (!ctx) return 1;

    const int n_vocab = llama_vocab_n_tokens(llama_model_get_vocab(model));
    const llama_token tok = n_vocab > 1 ? 1 : 0;
    llama_pos pos = 0;

    CHECK(decode_one(ctx, tok, pos++));
    const int settled = g_reservations;
    CHECK(settled >= 1); // the context reserved at creation, and the evidence is readable

    // No request, no reservation: a decode alone does not reserve again.
    CHECK(decode_one(ctx, tok, pos++));
    CHECK(g_reservations == settled);

    // A request: exactly one reservation, at the next decode, and none after it.
    bmoe::detail::request_compute_reservation(ctx);
    CHECK(decode_one(ctx, tok, pos++));
    CHECK(g_reservations == settled + 1);
    CHECK(decode_one(ctx, tok, pos++));
    CHECK(g_reservations == settled + 1);

    // The context still computes what it did: logits, and no embeddings left switched on.
    CHECK(llama_get_logits_ith(ctx, -1) != nullptr);
    CHECK(llama_get_embeddings_ith(ctx, -1) == nullptr);

    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    if (failures)
        std::fprintf(stderr, "%d check(s) failed\n", failures);
    else
        std::printf("compute_reservation: ok\n");
    return failures ? 1 : 0;
}
