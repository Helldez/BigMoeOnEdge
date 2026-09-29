// Unit tests for decide()'s policy (core/src/engine/decide/): where the prompt splits, how choices
// are checked and scored, when the kept prefix state is restored, stored or left alone, and which
// failures leave the session usable.
//
// The policy runs over IDecideBackend, so here it runs over a scripted fake: tokens are the bytes of
// the text, the "model state" is the list of tokens fed so far, and the logits are a pure function of
// that state. A restored state that differs from a recomputed one would therefore show up as
// different logits, exactly as it would on a real model — which the gates check separately (G18).
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "bmoe/config.h"
#include "bmoe/decide.h"

#include "choice_scorer.h"
#include "decider.h"
#include "prefix_cache.h"
#include "prompt_split.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace bmoe;
using namespace bmoe::detail;

static int failures = 0;

static void expect(const char * name, bool ok) {
    if (ok) return;
    std::printf("[FAIL] %s\n", name);
    ++failures;
}

namespace {

constexpr Token kTurnEnd = 300; // what the fake "template" closes a user turn with
constexpr int kVocab = 512;

std::vector<Token> bytes_of(const std::string & s) {
    std::vector<Token> t;
    for (unsigned char c : s)
        t.push_back((Token) c);
    return t;
}

struct Prefill {
    int from, to;
};

class FakeBackend final : public IDecideBackend {
public:
    int ctx = 4096;
    bool scales = true;
    bool fail_prefill = false;
    bool refuse_load = false;
    std::vector<Token> seq; // the "model state": every token fed, in order
    std::vector<Prefill> prefills;
    int clears = 0;
    std::vector<float> logits_;

    bool render(const std::string & content, std::vector<Token> & out, std::string &) override {
        out = bytes_of(content);
        out.push_back(kTurnEnd);
        return true;
    }
    std::vector<Token> tokenize_plain(const std::string & text) override { return bytes_of(text); }
    int n_ctx() const override { return ctx; }
    int n_vocab() const override { return kVocab; }
    bool prefill_cost_scales_with_tokens() const override { return scales; }
    void clear() override {
        seq.clear();
        logits_.clear();
        ++clears;
    }
    bool prefill(const std::vector<Token> & tokens, int from, int to) override {
        prefills.push_back({from, to});
        // A backend fed at a position it does not hold is what a wrong restore would look like.
        if (fail_prefill || from != (int) seq.size()) return false;
        seq.insert(seq.end(), tokens.begin() + from, tokens.begin() + to);
        logits_.clear();
        if (to == (int) tokens.size()) {
            // A pure function of the whole state: equal states give equal rows, bit for bit.
            logits_.assign(kVocab, 0.0f);
            unsigned h = 2166136261u;
            for (Token t : seq)
                h = (h ^ (unsigned) t) * 16777619u;
            for (int v = 0; v < kVocab; ++v)
                logits_[v] = (float) ((h >> (v % 24)) & 0xff) / 32.0f;
        }
        return true;
    }
    const float * last_logits() override { return logits_.empty() ? nullptr : logits_.data(); }
    bool save_state(std::vector<uint8_t> & out) override {
        out.clear();
        for (Token t : seq)
            for (int b = 0; b < 4; ++b)
                out.push_back((uint8_t) ((uint32_t) t >> (8 * b)));
        return true;
    }
    bool load_state(const std::vector<uint8_t> & in) override {
        if (refuse_load) return false;
        seq.clear();
        for (size_t i = 0; i + 3 < in.size(); i += 4)
            seq.push_back((Token) (in[i] | in[i + 1] << 8 | in[i + 2] << 16 | (uint32_t) in[i + 3] << 24));
        return true;
    }
    void begin_prefill_measure() override {}
    void end_prefill_measure(PrefillStats &) override {}
};

DecideRequest req(const std::string & prefix, const std::string & suffix) {
    DecideRequest r;
    r.prefix = prefix;
    r.suffix = suffix;
    r.choices = {"A", "B", "C"};
    return r;
}

// The same request on a fresh backend with no cache: the reference a restored run must equal.
DecideResult reference(const DecideRequest & r) {
    FakeBackend fresh;
    return run_decide(fresh, nullptr, r);
}

} // namespace

static void test_prefix_split() {
    expect("split: gives the last shared token back", prefix_split({1, 2, 3, 4, 9}, {1, 2, 3, 7}) == 2);
    expect("split: nothing shared", prefix_split({5, 6}, {1, 2}) == 0);
    expect("split: empty prefix", prefix_split({5, 6}, {}) == 0);
    expect("split: always leaves a token to prefill", prefix_split({1, 2, 3}, {1, 2, 3, 4}) == 2);
    expect("split: one-token prompt", prefix_split({1}, {1}) == 0);
    expect("split: empty prompt", prefix_split({}, {1}) == 0);
}

static void test_choices() {
    std::vector<Token> ids;
    std::string err;
    expect("choices: distinct first tokens accepted",
           choice_first_tokens({"A", "B"}, {{65}, {66, 1}}, kVocab, ids, err) && ids == std::vector<Token>{65, 66});
    expect("choices: a shared first token is refused",
           !choice_first_tokens({"Apple", "Avocado"}, {{65, 1}, {65, 2}}, kVocab, ids, err) &&
               err.find("cannot be told apart") != std::string::npos);
    expect("choices: an empty choice is refused", !choice_first_tokens({"A", ""}, {{65}, {}}, kVocab, ids, err));
    expect("choices: none is refused", !choice_first_tokens({}, {}, kVocab, ids, err));
    expect("choices: out of vocabulary is refused", !choice_first_tokens({"A"}, {{kVocab}}, kVocab, ids, err));

    // Normalised over the WHOLE vocabulary, not over the choices.
    std::vector<float> lg(8, 0.0f);
    lg[3] = 2.0f;
    std::vector<double> lp;
    int best = -1;
    score_choices(lg.data(), 8, {1, 3}, lp, best);
    const double z = std::log(7.0 + std::exp(2.0));
    expect("score: best is the higher logit", best == 1);
    expect("score: log-prob over the whole vocabulary",
           lp.size() == 2 && std::fabs(lp[0] - (0.0 - z)) < 1e-9 && std::fabs(lp[1] - (2.0 - z)) < 1e-9);
}

static void test_no_cache() {
    FakeBackend b;
    const DecideResult r = run_decide(b, nullptr, req("task: open settings. ", "screen 1"));
    expect("no cache: ok", r.ok && r.best >= 0 && r.choice_logp.size() == 3);
    expect("no cache: one whole prefill",
           b.prefills.size() == 1 && b.prefills[0].from == 0 && b.prefills[0].to == r.n_tokens);
    expect("no cache: nothing reused", r.n_reused == 0 && r.n_prefilled == r.n_tokens && r.prefix_state_bytes == 0);
    expect("no cache: sequence emptied on the way out", b.seq.empty());
}

static void test_cache_reuse() {
    FakeBackend b;
    LastPrefixCache cache;
    const std::string p = "task: open settings. history: none. ";

    const DecideRequest r1 = req(p, "screen 1");
    const DecideResult d1 = run_decide(b, &cache, r1);
    const int split = (int) p.size() - 1;
    expect("cold: ok and nothing reused", d1.ok && d1.n_reused == 0);
    expect("cold: prefilled in two pieces at the split",
           b.prefills.size() == 2 && b.prefills[0].to == split && b.prefills[1].from == split);
    expect("cold: equals the uncached reference", d1.choice_logp == reference(r1).choice_logp);
    expect("cold: kept the prefix state", d1.prefix_state_bytes > 0);

    // Same prefix, new screen: the prefix is restored, only the suffix is prefilled.
    b.prefills.clear();
    const DecideRequest r2 = req(p, "screen 2, a longer one");
    const DecideResult d2 = run_decide(b, &cache, r2);
    expect("warm: reused the prefix", d2.ok && d2.n_reused == split && d2.n_prefilled == d2.n_tokens - split);
    expect("warm: prefilled only the suffix", b.prefills.size() == 1 && b.prefills[0].from == split);
    expect("warm: equals the uncached reference", d2.choice_logp == reference(r2).choice_logp);

    // The history grew: the old prefix is restored, the new part prefilled and kept.
    b.prefills.clear();
    const std::string p3 = p + "step 1: tapped B. ";
    const DecideRequest r3 = req(p3, "screen 3");
    const DecideResult d3 = run_decide(b, &cache, r3);
    const int split3 = (int) p3.size() - 1;
    expect("extended: restored the old prefix", d3.ok && d3.n_reused == split);
    expect("extended: prefilled the growth, then the suffix", b.prefills.size() == 2 && b.prefills[0].from == split &&
                                                                  b.prefills[0].to == split3 &&
                                                                  b.prefills[1].from == split3);
    expect("extended: equals the uncached reference", d3.choice_logp == reference(r3).choice_logp);

    // reuse_prefix off: whole prefill, and the kept state is not touched.
    b.prefills.clear();
    DecideRequest r4 = req(p3, "screen 4");
    r4.reuse_prefix = false;
    const size_t kept = cache.bytes();
    const DecideResult d4 = run_decide(b, &cache, r4);
    expect("reuse off: one whole prefill",
           d4.ok && d4.n_reused == 0 && b.prefills.size() == 1 && b.prefills[0].from == 0);
    expect("reuse off: kept state untouched", cache.bytes() == kept);
    b.prefills.clear();
    const DecideResult d5 = run_decide(b, &cache, req(p3, "screen 5"));
    expect("reuse off: the next call still restores", d5.ok && d5.n_reused == split3);

    // A different task: a miss, and the new prefix replaces the old one.
    b.prefills.clear();
    const DecideResult d6 = run_decide(b, &cache, req("task: send a message. ", "screen 1"));
    expect("diverged: a miss", d6.ok && d6.n_reused == 0);
}

static void test_refusals_and_failures() {
    FakeBackend b;
    LastPrefixCache cache;
    DecideRequest collide = req("p ", "s");
    collide.choices = {"Yes", "Yeah"};
    b.seq = {7, 8, 9}; // a conversation generate() left in the sequence
    DecideResult r = run_decide(b, &cache, collide);
    expect("collision: refused, not fatal, nothing prefilled", !r.ok && !r.fatal && b.prefills.empty());
    expect("collision: the old conversation is not left behind", b.seq.empty());

    b.ctx = 4;
    r = run_decide(b, &cache, req("prefix ", "suffix"));
    expect("n_ctx: refused, not fatal, nothing prefilled", !r.ok && !r.fatal && b.prefills.empty());
    b.ctx = 4096;

    b.fail_prefill = true;
    r = run_decide(b, &cache, req("prefix ", "suffix"));
    expect("prefill failure: fatal, sequence emptied", !r.ok && r.fatal && b.seq.empty());
    b.fail_prefill = false;

    // A state the backend will not load back is a miss, and is dropped.
    run_decide(b, &cache, req("prefix ", "suffix"));
    b.refuse_load = true;
    b.prefills.clear();
    r = run_decide(b, &cache, req("prefix ", "other"));
    expect("refused load: a miss that still answers", r.ok && r.n_reused == 0 && b.prefills[0].from == 0);
}

static void test_policy() {
    expect("auto on a token-scaled prefill keeps state", make_prefix_cache(PrefixCacheMode::Auto, true) != nullptr);
    expect("auto on a flat-cost prefill keeps none", make_prefix_cache(PrefixCacheMode::Auto, false) == nullptr);
    expect("on keeps state anywhere", make_prefix_cache(PrefixCacheMode::On, false) != nullptr);
    expect("off keeps none", make_prefix_cache(PrefixCacheMode::Off, true) == nullptr);

    for (PrefixCacheMode m : {PrefixCacheMode::Auto, PrefixCacheMode::On, PrefixCacheMode::Off}) {
        PrefixCacheMode back = PrefixCacheMode::Auto;
        expect("mode name round-trips", parse_prefix_cache_mode(prefix_cache_mode_name(m), back) && back == m);
    }
    PrefixCacheMode untouched = PrefixCacheMode::On;
    expect("unknown mode is rejected", !parse_prefix_cache_mode("yes", untouched) && untouched == PrefixCacheMode::On);
}

int main() {
    test_prefix_split();
    test_choices();
    test_no_cache();
    test_cache_reuse();
    test_refusals_and_failures();
    test_policy();
    if (failures == 0) {
        std::printf("all decide checks passed\n");
        return 0;
    }
    std::printf("%d decide check(s) failed\n", failures);
    return 1;
}
