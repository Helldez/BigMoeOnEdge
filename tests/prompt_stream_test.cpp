// The prompt stream (core/src/engine/prompt_stream.h): the KV bookkeeping's view of a prompt with
// images in it. Pure data, so these checks need no model: an image is a span of placeholders over
// fewer positions than tokens, a prefix is shared whole-image or not at all, and a cut never lands
// inside one.
#include "prompt_stream.h"

#include <cstdio>
#include <string>

using bmoe::detail::MediaSpan;
using bmoe::detail::PromptStream;

static int failures = 0;
static void check(bool ok, const std::string & what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

// "a b <img:3 tokens, 2 positions> c d"
static PromptStream sample(const std::string & id = "img1") {
    PromptStream s;
    s.push_token(10);
    s.push_token(11);
    s.push_media(3, 2, id);
    s.push_token(12);
    s.push_token(13);
    return s;
}

int main() {
    {
        PromptStream s = sample();
        check(s.size() == 7 && s.media.size() == 1 && s.media[0].begin == 2, "a span is pushed as placeholders");
        check(s.tokens[2] == LLAMA_TOKEN_NULL && s.tokens[4] == LLAMA_TOKEN_NULL && s.tokens[5] == 12,
              "placeholders are the null token, the text its ids");
        check(s.pos_at(0) == 0 && s.pos_at(2) == 2 && s.pos_at(5) == 4 && s.pos_at(7) == 6,
              "positions equal indices before a span and fall behind by what it saved after it");
        check(s.media_at(1) == nullptr && s.media_at(2) && s.media_at(4) && s.media_at(5) == nullptr,
              "media_at knows which indices an image holds");
        PromptStream text;
        for (llama_token t : {1, 2, 3}) text.push_token(t);
        check(text.pos_at(3) == 3 && !text.has_media(), "a text-only stream maps every index to itself");
    }
    {
        // Prefix reuse: the same image at the same place is shared; a different one is not.
        PromptStream kv = sample();
        PromptStream next = sample();
        next.push_token(14);
        check(kv.common_prefix(next, next.size() - 1) == 7, "a conversation extended after its image shares it all");
        PromptStream other = sample("img2");
        check(kv.common_prefix(other, 10) == 2, "a different image at the same place ends the prefix before it");
        PromptStream shifted;
        shifted.push_token(10);
        shifted.push_media(3, 2, "img1");
        check(kv.common_prefix(shifted, 10) == 1, "the same image at another place is not shared");
        check(kv.common_prefix(next, 3) == 2, "a limit inside the image stops before it, never inside");
        PromptStream text;
        for (llama_token t : {10, 11, 12}) text.push_token(t);
        check(kv.common_prefix(text, 10) == 2, "text where the image was ends the prefix at the image");
    }
    {
        PromptStream s = sample();
        check(s.truncate(4) == 2 && s.size() == 2 && s.media.empty(), "a cut inside an image moves to its start");
        PromptStream t = sample();
        check(t.truncate(5) == 5 && t.media.size() == 1, "a cut right after an image keeps it");
        PromptStream u = sample();
        check(u.truncate(99) == 7, "a cut past the end keeps everything");
    }
    {
        PromptStream kv;
        kv.push_token(10);
        PromptStream s = sample();
        check(kv.append(s, 1, 7) && kv.size() == 7 && kv.media.size() == 1 && kv.media[0].begin == 2 &&
                  kv.media[0].id == "img1",
              "append carries the spans over, shifted to their place in the destination");
        PromptStream bad;
        check(!bad.append(s, 0, 3) && bad.empty(), "a range that cuts an image is refused whole");
        check(kv.pos_at(7) == 6, "the appended stream maps positions as the source did");
    }
    if (failures) {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("all prompt stream checks passed\n");
    return 0;
}
