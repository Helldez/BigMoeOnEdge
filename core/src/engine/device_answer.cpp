#include "device_answer.h"

#include "prefill_support.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

namespace bmoe::detail {

namespace {

constexpr const char * k_probe_text =
    "The river runs past the old mill and under the stone bridge before it reaches the town. In the "
    "morning the market opens on the square, and people come from the villages to sell bread, cheese "
    "and vegetables. Children walk to school along the canal, and the baker has been awake since four. "
    "By noon the square is full, the church bell rings twelve times, and the cafes put their chairs "
    "out in the sun. In the afternoon the farmers drive home, the stalls are folded away, and the town "
    "becomes quiet again until the evening, when the lights come on along the water.";

double since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// Decode `probe` on `c` from an empty memory as the pieces `widths` name, in order, and copy out the
// rows compared: of each piece `rows_of(width)` positions spread over it, its last included. False
// where a decode failed.
bool decode_probe(llama_context * c,
                  const std::vector<llama_token> & probe,
                  const std::vector<int> & widths,
                  int n_vocab,
                  std::vector<float> & rows) {
    rows.clear();
    int widest = 1;
    for (int w : widths)
        widest = std::max(widest, w);
    llama_memory_clear(llama_get_memory(c), true);
    llama_batch b = llama_batch_init(widest, 0, 1);
    bool ok = true;
    int pos = 0;
    for (size_t p = 0; ok && p < widths.size(); ++p) {
        const int w = widths[p];
        const int n_rows = answer_rows_of(w);
        batch_fill(b, probe.data() + pos, w, pos, /*all_logits*/ false);
        b.logits[w - 1] = 0;
        std::vector<int> at((size_t) n_rows);
        for (int r = 0; r < n_rows; ++r) {
            at[(size_t) r] = (int) ((int64_t) (r + 1) * w / n_rows) - 1;
            b.logits[at[(size_t) r]] = 1;
        }
        ok = llama_decode(c, b) == 0;
        for (size_t r = 0; ok && r < at.size(); ++r) {
            const float * row = llama_get_logits_ith(c, at[r]);
            ok = row != nullptr;
            if (ok) rows.insert(rows.end(), row, row + n_vocab);
        }
        pos += w;
    }
    llama_synchronize(c);
    llama_batch_free(b);
    llama_memory_clear(llama_get_memory(c), true);
    return ok;
}

// The median seconds `c` takes to decode one token alone, over k_answer_timed_tokens of `probe` after
// a short prefix: an answer's own graph, on a memory that already holds a context. 0 where the context
// has no room for them or a decode failed. Leaves the memory empty.
double time_tokens(llama_context * c, const std::vector<llama_token> & probe) {
    const int prefix = k_answer_timed_prefix, n = k_answer_timed_tokens;
    if ((int) probe.size() < prefix + n || (int) llama_n_ctx(c) < prefix + n || (int) llama_n_batch(c) < prefix)
        return 0.0;
    llama_memory_clear(llama_get_memory(c), true);
    llama_batch b = llama_batch_init(prefix, 0, 1);
    batch_fill(b, probe.data(), prefix, 0, /*all_logits*/ false);
    bool ok = llama_decode(c, b) == 0;
    llama_synchronize(c);
    std::vector<double> step;
    for (int i = 0; ok && i < n; ++i) {
        batch_fill(b, probe.data() + prefix + i, 1, prefix + i, /*all_logits*/ true);
        const auto t0 = std::chrono::steady_clock::now();
        ok = llama_decode(c, b) == 0;
        llama_synchronize(c);
        if (ok) step.push_back(since(t0));
    }
    llama_batch_free(b);
    llama_memory_clear(llama_get_memory(c), true);
    if (!ok || step.empty()) return 0.0;
    std::nth_element(step.begin(), step.begin() + (std::ptrdiff_t)(step.size() / 2), step.end());
    return step[step.size() / 2];
}

// The seconds `c` takes to compute one graph `width` tokens of `probe` wide from an empty memory, as a
// prompt's graph is computed: only its last row's logits asked for. A device's decode returns once the
// graph is queued, so what was queued before it is waited for first, and the graph itself to its end.
// 0 where the context cannot take that width or the decode failed. Leaves the memory empty.
double time_prompt(llama_context * c, const std::vector<llama_token> & probe, int width) {
    if (width <= 0 || (int) probe.size() < width || (int) llama_n_ctx(c) < width || (int) llama_n_batch(c) < width ||
        (int) llama_n_ubatch(c) < width)
        return 0.0;
    llama_memory_clear(llama_get_memory(c), true);
    llama_batch b = llama_batch_init(width, 0, 1);
    batch_fill(b, probe.data(), width, 0, /*all_logits*/ false);
    llama_synchronize(c);
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = llama_decode(c, b) == 0;
    llama_synchronize(c);
    const double s = since(t0);
    llama_batch_free(b);
    llama_memory_clear(llama_get_memory(c), true);
    return ok ? s : 0.0;
}

SideRates rates_of(double token_s, double prompt_s, int prompt_width) {
    SideRates r;
    if (token_s > 0.0) r.decode_tps = 1.0 / token_s;
    if (prompt_s > 0.0 && prompt_width > 0) r.prompt_tps = (double) prompt_width / prompt_s;
    return r;
}

} // namespace

SideRates answer_device_rates(const DeviceAnswer & a) {
    return rates_of(a.device_token_s, a.device_prompt_s, a.prompt_width);
}

SideRates answer_host_rates(const DeviceAnswer & a) {
    return rates_of(a.host_token_s, a.host_prompt_s, a.prompt_width);
}

AnswerWeighing weigh_answer_sides(const DeviceAnswer & a, double prompt_per_token, double margin) {
    const SideWeighing s = weigh_sides(answer_device_rates(a), answer_host_rates(a), prompt_per_token, margin);
    AnswerWeighing w;
    if (!s.weighed) return w;
    w.device_request_s = s.device_s;
    w.cores_request_s = s.cores_s;
    w.side = s.cores_sooner ? AnswerSide::Cores : AnswerSide::Device;
    return w;
}

int answer_prompt_width(int n_ctx, int n_ubatch) {
    const int w = std::min({n_ctx, n_ubatch, k_answer_prompt_tokens_max});
    return w > k_answer_narrow_max ? w : 0;
}

std::vector<llama_token> answer_probe_tokens(const llama_vocab * vocab, int want) {
    // Sized as llama.cpp's own common_tokenize sizes it: a guess, and once more at the size a
    // buffer too small is told it needs (returned negated).
    const int text_len = (int) std::strlen(k_probe_text);
    std::vector<llama_token> toks((size_t) text_len + 2);
    int nt = llama_tokenize(vocab, k_probe_text, text_len, toks.data(), (int) toks.size(), /*add_special*/ true,
                            /*parse_special*/ false);
    if (nt < 0 && nt != std::numeric_limits<int32_t>::min()) { // INT32_MIN: the count overflowed
        toks.resize((size_t) -nt);
        nt = llama_tokenize(vocab, k_probe_text, text_len, toks.data(), (int) toks.size(), true, false);
    }
    if (nt <= 1 || want < 1) return {};
    // Repeat the text (without its leading special token) until the probe is as wide as asked.
    const int body = toks[0] == llama_vocab_bos(vocab) ? 1 : 0;
    std::vector<llama_token> probe(toks.begin(), toks.begin() + nt);
    while ((int) probe.size() < want)
        probe.insert(probe.end(), toks.begin() + body, toks.begin() + nt);
    probe.resize((size_t) want);
    return probe;
}

int answer_rows_of(int width) {
    return width <= k_answer_narrow_max ? 1 : std::min(width, k_answer_rows);
}

std::vector<int> answer_piece_widths(int n_ctx, int n_batch) {
    std::vector<int> widths;
    int used = 0;
    for (int w = 1; w <= k_answer_narrow_max && used + w <= n_ctx && w <= n_batch; ++w) {
        widths.push_back(w);
        used += w;
    }
    const int wide = std::min({k_answer_wide_tokens, n_ctx - used, n_batch});
    if (wide > k_answer_narrow_max) widths.push_back(wide);
    return widths;
}

DeviceAnswer check_device_answer(llama_context * placed, const AnswerReference & ref, bool test_differs, bool timed) {
    DeviceAnswer a;
    const llama_model * model = llama_get_model(placed);
    const llama_vocab * vocab = llama_model_get_vocab(model);
    const int n_vocab = llama_vocab_n_tokens(vocab);

    const std::vector<int> widths = answer_piece_widths((int) llama_n_ctx(placed), (int) llama_n_batch(placed));
    int n = 0;
    for (int w : widths)
        n += w;
    // Without the wide piece the way a prompt runs was not compared, and that is half the question.
    if (widths.empty() || widths.back() <= k_answer_narrow_max) {
        a.unchecked = "the context is too narrow for a sequence to compare";
        return a;
    }
    // The prompt side is timed as wide as the session computes a prompt; the reference is sized for it.
    const int prompt_width = timed ? answer_prompt_width((int) llama_n_ctx(placed), (int) llama_n_ubatch(placed)) : 0;
    const std::vector<llama_token> probe =
        answer_probe_tokens(vocab, std::max({n, prompt_width, k_answer_timed_prefix + k_answer_timed_tokens}));
    if (probe.empty()) {
        a.unchecked = "the sequence it is compared on does not tokenize";
        return a;
    }
    a.tokens = n;
    a.pieces = (int) widths.size();
    a.wide = widths.back();

    // The placement first: where it cannot even decode, no reference is opened to say so.
    std::vector<float> dev_rows, host_rows;
    auto t0 = std::chrono::steady_clock::now();
    const bool dev_ok = decode_probe(placed, probe, widths, n_vocab, dev_rows);
    a.device_s = since(t0);
    if (!dev_ok) {
        a.unchecked = "the sequence could not be decoded on the device";
        return a;
    }

    t0 = std::chrono::steady_clock::now();
    llama_model * host = ref.load_model ? ref.load_model() : nullptr;
    if (!host) {
        a.unchecked = "the model could not be opened a second time on the cores for the reference";
        return a;
    }
    llama_context_params cp = llama_context_default_params();
    // No wider than the sequence and the prompt timed: the reference is a few graphs and is gone before
    // the session serves.
    cp.n_ctx = (uint32_t) std::max({n, k_answer_timed_prefix + k_answer_timed_tokens, prompt_width});
    cp.n_batch = (uint32_t) std::max(a.wide, prompt_width);
    cp.n_ubatch = cp.n_batch;
    cp.type_k = ref.type_k;
    cp.type_v = ref.type_v;
    cp.swa_full = false;
    cp.op_offload = false;
    cp.n_outputs_max = (uint32_t) k_answer_rows;
    llama_context * hc = llama_init_from_model(host, cp);
    const bool host_ctx = hc != nullptr;
    bool host_ok = host_ctx;
    if (hc) {
        llama_set_n_threads(hc, ref.n_threads, ref.n_threads_batch);
        host_ok = decode_probe(hc, probe, widths, n_vocab, host_rows);
    }
    a.host_s = since(t0);
    const auto done = [&] {
        if (hc) llama_free(hc);
        llama_model_free(host);
    };
    if (!host_ok || host_rows.size() != dev_rows.size()) {
        done();
        a.unchecked = host_ctx ? "the sequence could not be decoded on the cores for the reference"
                               : "no context could be created on the cores for the reference";
        return a;
    }

    const int rows = (int) (dev_rows.size() / (size_t) n_vocab);
    for (int r = 0; r < rows; ++r) {
        float * d = dev_rows.data() + (size_t) r * (size_t) n_vocab;
        if (test_differs && n_vocab > 1) std::rotate(d, d + 1, d + n_vocab);
        a.agreement.add(host_rows.data() + (size_t) r * (size_t) n_vocab, d, n_vocab);
    }
    a.verdict = a.agreement.matches() ? DeviceAnswer::Verdict::Confirmed : DeviceAnswer::Verdict::Differs;
    // Timed once the answers matched, while the reference is still open: a device that does not
    // compute the model is not a side, and its speed is nobody's question. The cores first, so the
    // device's turn is the one nearer the moment the session starts serving from it.
    // The pieces before it already ran every narrow width and a wide one on both sides, so the
    // kernels are loaded and the weights read when the clock starts.
    if (timed && a.verdict == DeviceAnswer::Verdict::Confirmed) {
        a.host_token_s = time_tokens(hc, probe);
        a.device_token_s = time_tokens(placed, probe);
        a.prompt_width = prompt_width;
        a.host_prompt_s = time_prompt(hc, probe, prompt_width);
        a.device_prompt_s = time_prompt(placed, probe, prompt_width);
    }
    done();
    return a;
}

SessionConfig on_cores(SessionConfig cfg) {
    cfg.n_gpu_layers = 0;
    cfg.device_split.clear();
    cfg.device_list.clear();
    // Not "auto": a caller who asked for every device in the room would keep them there with no layer
    // on them, and the scheduler hands such a device whatever nodes it can run.
    cfg.device_use = DeviceUse::CpuOnly;
    cfg.spec.mtp_head_on_devices = false;
    cfg.trial.answer_device.clear();
    // The load now serves the cores, and what it times is theirs.
    if (cfg.trial.shared_device_side == 1) cfg.trial.shared_device_side = 0;
    return cfg;
}

} // namespace bmoe::detail
