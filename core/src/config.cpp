#include "bmoe/config.h"

#include <utility>

namespace bmoe {

namespace {

struct PrefixCacheModeName {
    PrefixCacheMode mode;
    const char * name;
};
// The one place the spellings live: both directions read this table.
constexpr PrefixCacheModeName kPrefixCacheModes[] = {
    {PrefixCacheMode::Auto, "auto"},
    {PrefixCacheMode::On, "on"},
    {PrefixCacheMode::Off, "off"},
};

struct DenseWeightsModeName {
    DenseWeightsMode mode;
    const char * name;
};
constexpr DenseWeightsModeName kDenseWeightsModes[] = {
    {DenseWeightsMode::Mmap, "mmap"},
    {DenseWeightsMode::Warmed, "warm"},
    {DenseWeightsMode::Anonymous, "anon"},
    {DenseWeightsMode::Pinned, "pinned"},
};

} // namespace

const char * dense_weights_mode_name(DenseWeightsMode m) {
    for (const auto & e : kDenseWeightsModes)
        if (e.mode == m) return e.name;
    return kDenseWeightsModes[0].name;
}

const char * prefix_cache_mode_name(PrefixCacheMode m) {
    for (const auto & e : kPrefixCacheModes)
        if (e.mode == m) return e.name;
    return kPrefixCacheModes[0].name;
}

bool parse_prefix_cache_mode(const std::string & s, PrefixCacheMode & out) {
    for (const auto & e : kPrefixCacheModes)
        if (s == e.name) {
            out = e.mode;
            return true;
        }
    return false;
}

const char * kv_cache_type_name(KvCacheType t) {
    switch (t) {
    case KvCacheType::F16:
        return "f16";
    case KvCacheType::BF16:
        return "bf16";
    case KvCacheType::F32:
        return "f32";
    case KvCacheType::Q8_0:
        return "q8_0";
    case KvCacheType::Q5_1:
        return "q5_1";
    case KvCacheType::Q5_0:
        return "q5_0";
    case KvCacheType::Q4_1:
        return "q4_1";
    case KvCacheType::Q4_0:
        return "q4_0";
    case KvCacheType::IQ4_NL:
        return "iq4_nl";
    }
    return "f16";
}

ValidationResult validate(const RunConfig & cfg) {
    ValidationResult r;
    auto fail = [&](std::string msg) {
        r.ok = false;
        r.error = std::move(msg);
        return r;
    };

    if (cfg.model_path.empty()) {
        return fail("{model} is required: choose the model file to load");
    }
    if (cfg.n_predict <= 0) {
        return fail("{n-predict} must be at least 1");
    }
    if (cfg.n_threads <= 0) {
        return fail("{threads} must be at least 1");
    }
    if (cfg.n_ctx <= 0) {
        return fail("{ctx-size} must be at least 1 token");
    }
    // 0 means "as wide as the context"; anything larger than the context would be reserved for a
    // batch that can never arrive, which is the opposite of what this knob is for.
    if (cfg.ctx_checkpoints < 0) return fail("{ctx-checkpoints} cannot be negative (0 keeps none)");
    if (cfg.checkpoint_min_step < 0)
        return fail("{checkpoint-min-step} cannot be negative (0 sets no minimum spacing)");
    if (cfg.n_ubatch < 0) {
        return fail("{ubatch} cannot be negative (0 makes it as wide as the context)");
    }
    if (cfg.n_ubatch > cfg.n_ctx) {
        return fail("{ubatch} (" + std::to_string(cfg.n_ubatch) + ") is wider than {ctx-size} (" +
                    std::to_string(cfg.n_ctx) +
                    "): memory would be reserved for a batch that can never arrive. Lower it, or use 0 for as wide "
                    "as the context.");
    }
    // Lower bound only: 0 means "use the model default". The upper bound (<= the model's
    // real expert count) needs the loaded gguf, so it is deferred to run() where the model
    // is available — same rationale as the streaming checks that stay out of this pure path.
    if (cfg.n_expert_used < 0) {
        return fail("{n-expert-used} cannot be negative (0 keeps the model's own)");
    }
    if (!cfg.moe.expert_host_buffer.empty() && (cfg.moe.device_cache_mb <= 0 || cfg.moe.enabled))
        return fail("{expert-host-buffer} is where a device cache uploads resident experts from: it needs "
                    "{device-cache-mb} and {moe-stream} off");

    // Sampling ranges are enforced only when sampling is actually on (temp > 0). With temp <= 0
    // the engine takes the argmax path and the other knobs are inert, so a caller that leaves them
    // at any value still describes a valid greedy run — today's default stays valid untouched.
    if (cfg.sampling.temp > 0.0f) {
        if (cfg.sampling.top_k < 0) {
            return fail("{top-k} cannot be negative (0 turns the stage off)");
        }
        if (!(cfg.sampling.top_p > 0.0f && cfg.sampling.top_p <= SamplingConfig::top_p_max)) {
            return fail("{top-p} must be above 0 and at most 1");
        }
    }

    // Verification is exact only because greedy acceptance compares argmax against argmax. Under a
    // sampling chain the accepted prefix would depend on which draws happened to agree, which is a
    // different distribution from the one the caller asked for. Rejected rather than silently
    // ignored: a caller who asked for both was promised something the engine cannot give.
    if (!cfg.spec.mtp_head.empty() && !cfg.spec.is_mtp())
        return fail("{mtp-head} is the head MTP drafts use: it needs {spec=mtp}");
    if (cfg.spec.enabled() && cfg.sampling.temp > 0.0f) {
        return fail("{spec} needs greedy decoding ({temp} at 0): a draft is checked against the single most likely "
                    "token, which is only the answer when nothing is sampled.");
    }
    // The floor is 1 (draft one token, verify two positions); at 0 there is nothing to verify and
    // the loop degenerates into plain decode with the speculative scaffolding attached. The ceiling
    // is an evidence boundary, not a physical one — see SpecConfig::draft_max.
    if (cfg.spec.enabled() && (cfg.spec.draft_max < 1 || cfg.spec.draft_max > SpecConfig::draft_max_limit)) {
        return fail("{draft} must be between 1 and " + std::to_string(SpecConfig::draft_max_limit));
    }
    // A probability, so [0,1]. 1 would mean "only draft what the head is certain of", which is a
    // legitimate (if extreme) setting; above 1 nothing would ever be drafted and the flag would be
    // an expensive way to decode one token at a time.
    if (cfg.spec.is_mtp() && !(cfg.spec.draft_p_min >= 0.0f && cfg.spec.draft_p_min <= SpecConfig::draft_p_min_max)) {
        return fail("{mtp-p-min} must be between 0 and 1");
    }
    // Both of these are source-specific knobs. Accepting one against the other source would silently
    // do nothing, which is exactly the class of "the flag was ignored" bug this validation exists to
    // prevent — the caller asked for a behaviour the chosen source does not have.
    if (cfg.spec.is_ngram() && cfg.spec.draft_p_min != 0.0f) {
        return fail("{mtp-p-min} is for MTP drafts (the head's own confidence in its proposal) and means nothing "
                    "to n-gram drafts, which have no probabilities: their confidence gate is {ngram-min-match}.");
    }
    if (cfg.spec.is_ngram() &&
        (cfg.spec.ngram_max_match < 1 || cfg.spec.ngram_max_match > SpecConfig::ngram_match_limit)) {
        return fail("{ngram-max-match} must be between 1 and " + std::to_string(SpecConfig::ngram_match_limit));
    }
    if (cfg.spec.is_ngram() && (cfg.spec.ngram_min_match < 1 || cfg.spec.ngram_min_match > cfg.spec.ngram_max_match)) {
        return fail("{ngram-min-match} must be between 1 and {ngram-max-match} (" +
                    std::to_string(cfg.spec.ngram_max_match) +
                    "): a floor above the longest run considered would never draft anything.");
    }
    // The verify pass is 1 + draft_max positions and its whole point is that they are computed
    // TOGETHER. A narrower graph splits it back into single-token passes, which spends the draft
    // and keeps none of the amortisation — the feature would cost time and buy nothing. Rejected
    // rather than silently degraded: nothing in the output would show that it happened. 0 means
    // "as wide as the context" and is always wide enough.
    if (cfg.spec.enabled() && cfg.n_ubatch > 0 && cfg.n_ubatch < cfg.spec.draft_max + 1) {
        return fail("{ubatch} (" + std::to_string(cfg.n_ubatch) + ") is narrower than one verify batch (" +
                    std::to_string(cfg.spec.draft_max + 1) +
                    " tokens): it would be split back into single-token passes and drafting would cost time "
                    "for nothing. Raise {ubatch} or lower {draft}.");
    }

    if (!cfg.device_split.empty()) {
        long placed = 0;
        for (const auto & d : cfg.device_split)
            placed += d.second;
        if (placed != cfg.n_gpu_layers)
            return fail("{device-split} places " + std::to_string(placed) + " layers but {gpu-layers} is " +
                        std::to_string(cfg.n_gpu_layers) + ": the two describe one placement and must agree.");
    }

    if (cfg.prefill.enabled()) {
        // The prefill device loads as the model's only device, with no layer on it; placed layers
        // would all land on that one device instead of where they were placed.
        if (cfg.n_gpu_layers != 0)
            return fail("{prefill-device} does not combine with {gpu-layers} (" + std::to_string(cfg.n_gpu_layers) +
                        "): the prefill device loads as the model's only device. Set one of them to off.");
        if (cfg.prefill.min_tokens < PrefillDeviceConfig::min_tokens_floor)
            return fail("{prefill-min-tokens} must be at least " +
                        std::to_string(PrefillDeviceConfig::min_tokens_floor) +
                        ": a narrower device batch would have the same shape as a CPU decode step, and the "
                        "engine would run one in place of the other.");
        if (cfg.prefill.load_threads < 1 || cfg.prefill.load_threads > PrefillDeviceConfig::load_threads_max)
            return fail("{prefill-loaders} must be between 1 and " +
                        std::to_string(PrefillDeviceConfig::load_threads_max));
        if (cfg.prefill.slots < PrefillDeviceConfig::slots_min || cfg.prefill.slots > PrefillDeviceConfig::slots_max)
            return fail("{prefill-slots} must be 1 or 2");
        if (cfg.prefill.min_tokens > cfg.n_ctx)
            return fail("{prefill-min-tokens} (" + std::to_string(cfg.prefill.min_tokens) +
                        ") is larger than {ctx-size} (" + std::to_string(cfg.n_ctx) +
                        "): no prefill could ever reach the device.");
        // A prompt is fed in ubatch-wide pieces (the session prefills in n_ctx-wide batches, so the
        // ubatch is the width whenever it is set), and a piece narrower than min_tokens stays on the
        // CPU: without this the only sign would be prefill_dev_tokens=0.
        if (cfg.n_ubatch > 0 && cfg.n_ubatch < cfg.prefill.min_tokens)
            return fail("{ubatch} (" + std::to_string(cfg.n_ubatch) + ") is narrower than {prefill-min-tokens} (" +
                        std::to_string(cfg.prefill.min_tokens) + "): no prefill could ever reach the device.");
        // The row policy serves dense tables from the eval callback at each gather, and a device graph
        // bypasses the host streaming path in that callback entirely.
        if (cfg.moe.row_stream)
            return fail("{prefill-device} does not combine with {row-stream}: the tables it serves are "
                        "gathered on the host, inside a batch that would run on the device");
        // A verify batch is a host graph a few positions wide. llama.cpp reuses a graph of the same
        // shape without scheduling it again, so it must never be as wide as a graph the device runs:
        // narrower than the narrowest piece the device is handed, it can only reuse host graphs.
        if (cfg.spec.enabled() && cfg.spec.draft_max + 1 >= cfg.prefill.min_tokens)
            return fail("{draft} (" + std::to_string(cfg.spec.draft_max) + ") verifies " +
                        std::to_string(cfg.spec.draft_max + 1) + " tokens at once, as wide as {prefill-min-tokens} (" +
                        std::to_string(cfg.prefill.min_tokens) +
                        "): a verify step would be mistaken for a device batch. Lower {draft}, or raise "
                        "{prefill-min-tokens}.");
        // llama.cpp saves and restores a sequence through views of the KV cache it made once, over the
        // buffer the state was allocated in; the move rebinds the cache tensors, not those views, so a
        // saved or restored state would be read from and written to memory the model no longer uses.
        if (!(cfg.prefill.routed_full_frac > PrefillDeviceConfig::routed_full_frac_min &&
              cfg.prefill.routed_full_frac <= PrefillDeviceConfig::routed_full_frac_max))
            return fail("{prefill-routed-full} must be above 0 and at most 1");
        if (cfg.decide.enabled && cfg.decide.prefix_cache == PrefixCacheMode::On)
            return fail("{decide-prefix-cache=on} does not combine with {prefill-device}: the kept state would be "
                        "saved from where the model state was before it moved. Use auto (off with a device) "
                        "or off.");
    }

    // The decide probe reads graph nodes from the eval callback, which only streaming or a prefill
    // device installs; without either it would write empty lines.
    if (!cfg.decide.probe_path.empty()) {
        if (!cfg.decide.enabled) return fail("{decide-probe} needs {decide}");
        if (!cfg.moe.enabled && !cfg.prefill.enabled())
            return fail(
                "{decide-probe} needs {moe-stream} or {prefill-device}: it reads the model's steps through them");
    }

    // overlap is meaningless without streaming (it gates the streamer's own reads). The
    // hook-availability check is deferred to run(): validate() stays pure (no native).
    if (cfg.moe.overlap && !cfg.moe.enabled) {
        return fail("{overlap} needs {moe-stream}");
    }

    // The probe rides on the routing nodes the streamer already isolates. Off the streaming path
    // there is nothing for it to attach to — and nothing to learn either: routing does not depend
    // on how the weights got into memory, so a dense run would only reproduce the same numbers
    // more slowly.
    if (cfg.moe.predict_log && !cfg.moe.enabled) {
        return fail("{predict-log} needs {moe-stream}");
    }
    if (cfg.moe.predict_prefetch && !cfg.moe.enabled) {
        return fail("{predict-prefetch} needs {moe-stream}");
    }
    if (cfg.moe.route_ahead > 0 && !cfg.moe.enabled) {
        return fail("{route-ahead} needs {moe-stream}: it works on the expert choices the streamer reads, "
                    "and without streaming there is nothing to commit to");
    }

    // The row policy is discovered during the streamer's capture decode and acted on in its eval
    // callback; without streaming neither exists. It is also not a dense-mode variant — it applies
    // under every DenseWeightsMode, because what it changes is which tensors the mode is applied to.
    if (cfg.moe.row_stream && !cfg.moe.enabled) {
        return fail("{row-stream} needs {moe-stream}: the tables it serves are found and fed by the streamer, "
                    "which does not run without streaming");
    }
    if (cfg.moe.row_stream_mb < 0) {
        return fail("{row-stream-mb} cannot be negative (0 uses the built-in window)");
    }

    if (cfg.moe.enabled) {
        const MoeStreamConfig & m = cfg.moe;
        if (m.io_threads < 1 || m.io_threads > MoeStreamConfig::io_threads_max) {
            return fail("{io-threads} must be between 1 and " + std::to_string(MoeStreamConfig::io_threads_max));
        }
        if (m.cache_mb < 0) {
            return fail("{cache-mb} cannot be negative (0 turns the cache off)");
        }
        if (m.cache_mb > 0 && m.cache_mb < MoeStreamConfig::cache_min_mb && !m.force_cache) {
            return fail("{cache-mb} at " + std::to_string(m.cache_mb) + " MiB is below " +
                        std::to_string(MoeStreamConfig::cache_min_mb) +
                        " MiB: a cache smaller than one token's experts thrashes and is slower than no cache. "
                        "Use 0 to turn the cache off, at least " +
                        std::to_string(MoeStreamConfig::cache_min_mb) + " MiB, or {force-cache} to keep it.");
        }
        if (m.prefetch_layers < 0 || m.prefetch_layers > MoeStreamConfig::prefetch_layers_max) {
            return fail("{prefetch} must be between 0 and " + std::to_string(MoeStreamConfig::prefetch_layers_max));
        }
        if (m.routing_record_tokens < 0 || m.routing_record_tokens > MoeStreamConfig::routing_record_tokens_max) {
            return fail("{routing-record} must be between 0 and " +
                        std::to_string(MoeStreamConfig::routing_record_tokens_max));
        }
        if (m.cache_auto && m.cache_mb > 0) {
            return fail(
                "{cache-mb} is either auto or a fixed size, not both: choose {cache-mb=auto} or a size in MiB.");
        }
        if (m.cache_floor_mb < 0) {
            return fail("{cache-floor-mb} cannot be negative");
        }
        if (m.cache_ceil_mb < 0) {
            return fail("{cache-ceil-mb} cannot be negative (0 sets no ceiling)");
        }
        // "The LRU cache is on" means a fixed budget OR auto-sizing (which sizes a real LRU cache).
        const bool cache_on = m.cache_mb > 0 || m.cache_auto;
        if (m.prefetch_layers > 0 && !cache_on) {
            return fail("{prefetch} needs the expert cache ({cache-mb} above 0 or auto): early reads land in the "
                        "cache, which does not exist with it off.");
        }
        if (m.predict_prefetch && !cache_on) {
            return fail("{predict-prefetch} needs the expert cache ({cache-mb} above 0 or auto): early reads land in "
                        "the cache, which does not exist with it off.");
        }
        if (m.io_two_wave && !m.overlap) {
            return fail("{io-two-wave} needs {overlap}: without it the reads are waited for all at once, and there "
                        "is no idle reader to start early.");
        }
        if (m.io_two_wave && !cache_on) {
            return fail("{io-two-wave} needs the expert cache ({cache-mb} above 0 or auto): the second wave moves "
                        "work the cache does, which there is none of with it off.");
        }
        if (m.predict_spec_max < 0 || m.predict_spec_max > MoeStreamConfig::predict_spec_max_limit) {
            return fail("{predict-spec-max} must be between 0 and " +
                        std::to_string(MoeStreamConfig::predict_spec_max_limit) +
                        " (0 keeps predicted experts without reading any early)");
        }
        if (m.predict_prefetch && m.prefetch_layers > 0) {
            return fail("{predict-prefetch} and {prefetch} do not combine: they are two predictors for the same "
                        "early reads, and running both reads twice for the same future. Choose one.");
        }
        if (m.route_ahead < 0 || m.route_ahead > MoeStreamConfig::route_ahead_max) {
            return fail("{route-ahead} must be between 0 and " + std::to_string(MoeStreamConfig::route_ahead_max) +
                        " (0 is off)");
        }
        if (m.route_ahead > 0 && (m.predict_log || m.predict_prefetch || m.prefetch_layers > 0)) {
            return fail("{route-ahead} does not combine with {predict-log}, {predict-prefetch} or {prefetch}: the "
                        "probe would grade a prediction against choices made from that same prediction, and "
                        "an early read would bet on a future {route-ahead} has already fixed.");
        }
        // Measured, not theorised: with a draft source on, a verify decode is several positions wide
        // and route-ahead declines every one of them — a run at draft 3 committed 0 routings and
        // passed 249 through. It still pays for itself, though: the prediction GEMVs run, and its
        // early reads become ordinary speculation that can now miss (81% useful against 100% when it
        // commits). Cost with no commitment is worse than either feature alone, so the pair is
        // refused rather than silently charged for. Making it work means committing the whole verify
        // batch to one selection, which costs draft acceptance; see docs/route-ahead.md.
        if (m.route_ahead > 0 && cfg.spec.enabled()) {
            return fail("{route-ahead} does not combine with {spec}: a verify step is several tokens wide, so "
                        "{route-ahead} declines to commit on every one of them while still paying for its "
                        "prediction and its early reads. Turn one of them off.");
        }
        if (m.drop_cold_frac > 0.0f && !cache_on) {
            return fail("{drop-cold-experts} needs the expert cache ({cache-mb} above 0 or auto): with the cache off "
                        "every expert is a miss, so it would drop experts regardless of the cache, which is "
                        "what {n-expert-used} already does.");
        }
        // Written as a negated inclusive range so NaN (every comparison false) is rejected too,
        // instead of slipping past both this check and the cache_on one above.
        if (!(m.drop_cold_frac >= 0.0f && m.drop_cold_frac <= MoeStreamConfig::drop_cold_frac_max)) {
            return fail("{drop-cold-experts} must be between 0 and 1 (0 is off). Above 1 it could drop every "
                        "expert of a layer; 1 is an equal share among the active experts and the useful "
                        "maximum.");
        }
        if (m.substitute_lambda > 0.0f && !cache_on) {
            return fail("{expert-substitute} needs the expert cache ({cache-mb} above 0 or auto): with the cache off "
                        "no expert is kept in memory, so there is nothing to prefer.");
        }
        // Negated inclusive range, so NaN is rejected here too.
        if (!(m.substitute_lambda >= 0.0f && m.substitute_lambda <= MoeStreamConfig::substitute_lambda_max)) {
            return fail("{expert-substitute} must be between 0 and 1 (0 is off). At 1 an expert in memory "
                        "outranks every other whatever their scores, the strongest preference it can "
                        "express.");
        }
    }

    // The knob trial: each candidate is a knob value this run could equally have started with.
    const KnobTrialConfig & t = cfg.trial;
    if ((int) t.threads.size() > KnobTrialConfig::threads_max_candidates)
        return fail("{trial-threads} takes at most " + std::to_string(KnobTrialConfig::threads_max_candidates) +
                    " counts: every one costs part of the first answer's time");
    for (size_t i = 0; i < t.threads.size(); ++i) {
        const int n = t.threads[i];
        if (n <= 0 || n > RunConfig::threads_max)
            return fail("{trial-threads}: " + std::to_string(n) + " is not a thread count");
        if (n == cfg.n_threads)
            return fail("{trial-threads}: " + std::to_string(n) + " is already {threads} and measures nothing");
        for (size_t j = 0; j < i; ++j)
            if (t.threads[j] == n) return fail("{trial-threads}: " + std::to_string(n) + " is listed twice");
    }
    if (t.window_tokens < 1 || t.window_tokens > KnobTrialConfig::window_tokens_max)
        return fail("{trial-window} must be between 1 and " + std::to_string(KnobTrialConfig::window_tokens_max));
    if (t.overlap) {
        if (!cfg.moe.enabled)
            return fail("{trial-overlap} needs {moe-stream}: {overlap} is about the streamer's own reads");
        if (cfg.moe.io_two_wave)
            return fail("{trial-overlap} does not combine with {io-two-wave}, which needs {overlap} on throughout");
        if (cfg.moe.prefetch_sync)
            return fail(
                "{trial-overlap} does not combine with {prefetch-sync}, a test mode that reads one step at a time");
        if (cfg.n_gpu_layers != 0)
            return fail("{trial-overlap} needs {gpu-layers} at 0: with a layer on a device, {overlap} changes how the "
                        "model is placed, which is fixed when it loads");
    }

    return r;
}

} // namespace bmoe
