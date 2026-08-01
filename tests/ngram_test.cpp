// Unit tests for ngram_draft() (core/src/engine/ngram_draft.cpp) — the prompt-lookup draft source.
//
// The matcher is pure policy over token ids: no model, no llama.cpp, fully deterministic, so it runs
// unconditionally in ctest. That is the point of keeping it on this side of the seam — the drafting
// decision is testable exhaustively, while what it costs is a bench question.
//
// Nothing here can be a byte-identity gate: what the loop does with a draft is verified by a batched
// decode, which is not bit-identical to single-token decodes (see docs/mtp.md). These tests cover the
// decision, not the arithmetic.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "bmoe/ngram_draft.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace bmoe;

static int failures = 0;

static std::string show(const std::vector<int32_t> & v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ", ";
        s += std::to_string(v[i]);
    }
    return s + "]";
}

// Draft against `corpus` where the last element plays the part of the just-confirmed token, which is
// how the session calls it: ctx holds everything already emitted, `last` is not yet appended.
static void expect_draft(const char * name,
                         const std::vector<int32_t> & corpus,
                         int n_max,
                         int min_match,
                         int max_match,
                         const std::vector<int32_t> & want) {
    std::vector<int32_t> ctx(corpus.begin(), corpus.end() - 1);
    const int32_t last = corpus.back();

    std::vector<int32_t> got;
    const int n = ngram_draft(ctx, last, n_max, min_match, max_match, got);

    if (n == (int) got.size() && got == want) {
        std::printf("[PASS] %s -> %s\n", name, show(got).c_str());
    } else {
        std::printf("[FAIL] %s\n  want %s, got %s (returned %d)\n", name, show(want).c_str(), show(got).c_str(), n);
        ++failures;
    }
}

int main() {
    // A corpus with nothing to go on cannot draft. These are the steps that must cost exactly a
    // plain decode — the floor the whole source rests on.
    expect_draft("an empty corpus drafts nothing", {7}, 3, 3, 12, {});
    expect_draft("a corpus shorter than the minimum match drafts nothing", {1, 2}, 3, 3, 12, {});
    expect_draft("a corpus with no repetition drafts nothing", {1, 2, 3, 4, 5, 6}, 3, 3, 12, {});
    // A repetition shorter than the gate is not evidence: 2 tokens match, the floor is 3.
    expect_draft("a match shorter than the minimum drafts nothing", {1, 2, 3, 9, 9, 2, 3}, 3, 3, 12, {});

    // The base case: the trigram (1,2,3) occurred before and was followed by 4,5,6.
    expect_draft("a repeated trigram drafts its continuation", {1, 2, 3, 4, 5, 6, 0, 1, 2, 3}, 3, 3, 12, {4, 5, 6});
    // The draft width caps what is proposed, not what is matched.
    expect_draft("the draft width caps the proposal", {1, 2, 3, 4, 5, 6, 0, 1, 2, 3}, 2, 3, 12, {4, 5});
    // Only what actually followed exists. Here the best match ends two tokens from the corpus end,
    // so a width of 3 is clipped to the 2 tokens that are actually evidence.
    expect_draft("the continuation is clipped at the corpus end", {1, 2, 1, 2, 1, 2}, 3, 3, 12, {1, 2});

    // Selection is by match length first. The pattern (8,1,2,3) matches 4 tokens at the earlier
    // occurrence and only 3 at the later one, so the longer — and older — match must win.
    expect_draft("the longest match wins over a more recent shorter one", {8, 1, 2, 3, 40, 9, 1, 2, 3, 50, 8, 1, 2, 3},
                 1, 3, 12, {40});
    // With the lengths genuinely equal — each occurrence of (1,2,3) is preceded by a different
    // token, so both match exactly 3 — recency breaks the tie and the later continuation wins.
    expect_draft("the most recent wins among equal-length matches", {7, 1, 2, 3, 40, 8, 1, 2, 3, 50, 9, 1, 2, 3}, 1, 3,
                 12, {50});

    // max_match is a scan bound. Capping it at 3 makes the two candidates below tie at 3 instead of
    // ranking 5 against 3, which hands the pick to recency — the observable effect of the cap.
    expect_draft("capping the match length lets recency decide", {7, 7, 1, 2, 3, 40, 0, 0, 1, 2, 3, 50, 7, 7, 1, 2, 3},
                 1, 3, 3, {50});

    // The just-confirmed token is part of the pattern, not a spectator: without it, the suffix
    // (1,2) matches in two places and the trigram gate would reject. With it the match is (1,2,3).
    expect_draft("the confirmed token participates in the pattern", {1, 2, 3, 77, 5, 1, 2, 3}, 1, 3, 12, {77});

    // A match may overlap the pattern, which is how a run of one token predicts itself: the corpus
    // is four 9s, the pattern the last three, the match the three before it. Only one token of
    // evidence follows that match, so a width of 2 still drafts one.
    expect_draft("a repeated token predicts itself", {9, 9, 9, 9}, 2, 3, 12, {9});

    // The pattern itself is never its own match — otherwise every step would "match" at the end and
    // draft nothing meaningful. Here the only occurrence of (1,2,3) is the pattern.
    expect_draft("the pattern is not matched against itself", {0, 0, 0, 1, 2, 3}, 3, 3, 12, {});

    // Boundary of the gate: the same corpus drafts at min_match 2 and not at 3.
    expect_draft("a two-token match is rejected at a floor of three", {5, 6, 42, 0, 5, 6}, 2, 3, 12, {});
    expect_draft("the same two-token match is accepted at a floor of two", {5, 6, 42, 0, 5, 6}, 2, 2, 12, {42, 0});

    // Degenerate arguments are answered, not asserted on: the caller's validation already rejects
    // them, and a draft source that throws would be a decode-loop crash.
    expect_draft("a draft width of zero drafts nothing", {1, 2, 3, 4, 1, 2, 3}, 0, 3, 12, {});
    expect_draft("a maximum match below the minimum drafts nothing", {1, 2, 3, 4, 1, 2, 3}, 3, 3, 2, {});

    if (failures == 0) {
        std::printf("all n-gram draft checks passed\n");
        return 0;
    }
    std::printf("%d n-gram draft check(s) failed\n", failures);
    return 1;
}
