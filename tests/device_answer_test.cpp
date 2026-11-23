// The whole-model answer check's own rules (core/src/engine/device_answer.h, logit_agreement.h): how
// two computations of a model are judged to agree, which graphs the sequence goes through, and the
// configuration a session reopens with. Pure: no model is loaded.
#include "device_answer.h"
#include "logit_agreement.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace bmoe;
using namespace bmoe::detail;

static int failures = 0;
static void check(bool ok, const std::string & what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

int main() {
    const int nv = 16;
    // A row peaked at `top`, with a little structure under it.
    const auto row = [&](int top, float peak = 8.0f) {
        std::vector<float> r((size_t) nv);
        for (int v = 0; v < nv; ++v)
            r[(size_t) v] = 0.1f * (float) (v % 5);
        r[(size_t) top] = peak;
        return r;
    };

    {
        LogitAgreement a;
        check(!a.matches() && a.agree() == 0.0 && a.nmse() == 0.0, "agreement: nothing compared matches nothing");
        for (int r = 0; r < 20; ++r) {
            const std::vector<float> h = row(r % nv);
            a.add(h.data(), h.data(), nv);
        }
        check(a.rows == 20 && a.same == 20 && a.matches() && a.nmse() == 0.0 && std::fabs(a.kld) < 1e-12 &&
                  std::fabs(a.kld_max) < 1e-12,
              "agreement: the same rows agree everywhere, at no distance");
    }
    {
        // Rounding: every logit a little off, no top token moved.
        LogitAgreement a;
        for (int r = 0; r < 20; ++r) {
            const std::vector<float> h = row(r % nv);
            std::vector<float> d = h;
            for (int v = 0; v < nv; ++v)
                d[(size_t) v] += 0.01f * (float) ((v + r) % 3 - 1);
            a.add(h.data(), d.data(), nv);
        }
        check(a.same == 20 && a.matches() && a.nmse() > 0.0 && a.nmse() < 1e-3 && a.kld < 1e-3,
              "agreement: rounding moves the distances and no answer");
    }
    {
        // The bar is the prefill device's: nine rows in ten.
        LogitAgreement at_bar, under;
        for (int r = 0; r < 10; ++r) {
            const std::vector<float> h = row(1), other = row(2);
            at_bar.add(h.data(), r < 9 ? h.data() : other.data(), nv);
            under.add(h.data(), r < 8 ? h.data() : other.data(), nv);
        }
        check(at_bar.same == 9 && at_bar.matches(), "agreement: nine rows in ten is the bar, and passes");
        check(under.same == 8 && !under.matches() && under.kld_max > 1.0 && under.kld > 0.0 &&
                  under.kld < under.kld_max,
              "agreement: eight in ten does not, and the worst row's distance is kept beside the mean");
    }
    {
        // A row that is not a number agrees with nothing, wherever its scan happens to stop.
        LogitAgreement a;
        const std::vector<float> h = row(0);
        std::vector<float> d((size_t) nv, std::numeric_limits<float>::quiet_NaN());
        a.add(h.data(), d.data(), nv);
        check(a.rows == 1 && a.same == 0 && !a.matches(), "agreement: a row that is not a number agrees with nothing");
    }

    {
        // Every width a session's graphs take, narrow first, then one as wide as a prompt.
        const std::vector<int> w = answer_piece_widths(4096, 2048);
        bool ascending = (int) w.size() == k_answer_narrow_max + 1;
        for (int i = 0; ascending && i < k_answer_narrow_max; ++i)
            ascending = w[(size_t) i] == i + 1;
        check(ascending && w.back() == k_answer_wide_tokens, "pieces: one to the narrow bound, then the wide one");
        int rows = 0;
        for (int x : w) {
            rows += answer_rows_of(x);
            if (answer_rows_of(x) > k_answer_rows) rows = -1000;
        }
        check(rows == k_answer_narrow_max + k_answer_rows && answer_rows_of(1) == 1 &&
                  answer_rows_of(k_answer_wide_tokens) == k_answer_rows,
              "pieces: a narrow graph's one row, a wide graph's rows under the output head's bound");
        // A context that takes fewer tokens a call bounds the wide piece; one too small has none.
        const std::vector<int> small = answer_piece_widths(256, 24);
        check(small.back() == 24, "pieces: the wide piece is no wider than a call takes");
        const std::vector<int> tiny = answer_piece_widths(12, 12);
        check(tiny.empty() || tiny.back() <= k_answer_narrow_max, "pieces: a context too small has no wide piece");
    }

    {
        SessionConfig c;
        c.model_path = "m.gguf";
        c.n_ctx = 8192;
        c.n_threads = 6;
        c.n_gpu_layers = 31;
        c.device_split = {{"accel", 31}};
        c.device_list = {"accel"};
        c.device_use = DeviceUse::All;
        c.trial.answer_device = "accel";
        c.trial.shared_device_side = 1;
        c.trial.threads = {8};
        const SessionConfig h = on_cores(c);
        check(h.n_gpu_layers == 0 && h.device_split.empty() && h.device_list.empty() &&
                  h.device_use == DeviceUse::CpuOnly,
              "on the cores: no layer on a device, and no device in the room");
        check(h.trial.answer_device.empty() && h.trial.shared_device_side == 0,
              "on the cores: nothing left to compare, and the side the load times is the cores'");
        check(h.model_path == c.model_path && h.n_ctx == c.n_ctx && h.n_threads == c.n_threads &&
                  h.trial.threads == c.trial.threads,
              "on the cores: the same configuration otherwise");
        SessionConfig untimed = c;
        untimed.trial.shared_device_side = -1;
        check(on_cores(untimed).trial.shared_device_side == -1, "on the cores: a load that timed no side times none");
    }

    // Which side serves, by the check's own timing of the model on both. Per decoded token: its own
    // time, plus the prompt tokens a request brings with it at the rate the prompt graph ran.
    {
        const double margin = 0.10;
        DeviceAnswer a;
        a.prompt_width = 512;
        check(weigh_answer_sides(a, 0.0, margin).side == AnswerSide::Unweighed,
              "weighing: nothing timed, nothing weighed");
        a.device_token_s = 0.025;
        check(weigh_answer_sides(a, 0.0, margin).side == AnswerSide::Unweighed,
              "weighing: one side timed is no comparison");

        // A laptop whose probes read the cores 74% ahead and whose GPU decodes the model at 40 tok/s
        // against the cores' 18: the model's own timing says the device, by a wide margin.
        a.device_token_s = 1.0 / 40.0;
        a.host_token_s = 1.0 / 18.0;
        a.device_prompt_s = 1.0;
        a.host_prompt_s = 6.0;
        const AnswerWeighing fast = weigh_answer_sides(a, 0.0, margin);
        check(fast.side == AnswerSide::Device && std::fabs(fast.device_request_s - 0.025) < 1e-12 &&
                  std::fabs(fast.cores_request_s - 1.0 / 18.0) < 1e-12,
              "weighing: a device that decodes the model faster keeps it, on the decode alone");

        // Level within the margin: the device is the side open, and a reopening is not paid for noise.
        a.device_token_s = 0.0265;
        a.host_token_s = 0.0290;
        a.device_prompt_s = a.host_prompt_s = 4.0;
        check(weigh_answer_sides(a, 0.0, margin).side == AnswerSide::Device, "weighing: level, the device keeps it");
        a.device_token_s = 0.0310; // 7% slower than the cores: inside the margin
        check(weigh_answer_sides(a, 0.0, margin).side == AnswerSide::Device,
              "weighing: slower inside the margin, the device keeps it");
        a.device_token_s = 0.0330; // 14% slower
        check(weigh_answer_sides(a, 0.0, margin).side == AnswerSide::Cores,
              "weighing: slower past the margin, the cores take the model");

        // A device slower one token at a time and faster as wide as a prompt: which wins is the
        // request's. A machine that only decodes goes to the cores, one whose requests bring a long
        // prompt stays on the device. The rates of an integrated GPU and its cores on one MoE model:
        // 29.5 and 33.3 tok/s decoded, 146 and 103 prompt tok/s at 512 tokens wide.
        a.device_token_s = 1.0 / 29.5;
        a.host_token_s = 1.0 / 33.3;
        a.device_prompt_s = 512.0 / 146.0;
        a.host_prompt_s = 512.0 / 103.0;
        check(weigh_answer_sides(a, 0.0, margin).side == AnswerSide::Cores,
              "weighing: slower on the decode of a request that is all answer, the cores");
        const AnswerWeighing mixed = weigh_answer_sides(a, 4096.0 / 300.0, margin);
        check(mixed.side == AnswerSide::Device && mixed.device_request_s < mixed.cores_request_s,
              "weighing: the same device on the default request, where the prompt is most of the time: the device");
        // The same pair timed 64 tokens wide, where the GPU ran a prompt at 50 tok/s and the cores at
        // 89: the narrow graph ranks the prompt the other way round, which is why the width timed is
        // the prompt's (answer_prompt_width) and not the confirmation's.
        a.prompt_width = 64;
        a.device_prompt_s = 64.0 / 50.0;
        a.host_prompt_s = 64.0 / 89.0;
        check(weigh_answer_sides(a, 4096.0 / 300.0, margin).side == AnswerSide::Cores,
              "weighing: timed narrower than a prompt runs, the same device loses its prompt");
        // Without a prompt timing the prompt cannot be weighed, and the decode decides.
        a.prompt_width = 512;
        a.device_prompt_s = 0.0;
        check(weigh_answer_sides(a, 4096.0 / 300.0, margin).side == AnswerSide::Cores,
              "weighing: a prompt that was not timed weighs nothing");
        a.device_prompt_s = 512.0 / 146.0;
        a.prompt_width = 0;
        check(weigh_answer_sides(a, 4096.0 / 300.0, margin).side == AnswerSide::Cores,
              "weighing: a prompt timed at no width weighs nothing");
        // A negative margin is no margin, never a bonus for the cores.
        a.device_token_s = a.host_token_s = 0.03;
        check(weigh_answer_sides(a, 0.0, -1.0).side == AnswerSide::Device, "weighing: a margin below zero is zero");

        // What the load records is what it weighed: each side's rates, in tokens per second.
        a.prompt_width = 512;
        a.device_token_s = 0.04;
        a.host_prompt_s = 4.0;
        const SideRates on = answer_device_rates(a), off = answer_host_rates(a);
        check(std::fabs(on.decode_tps - 25.0) < 1e-9 && std::fabs(on.prompt_tps - 146.0) < 1e-9 &&
                  std::fabs(off.prompt_tps - 128.0) < 1e-9,
              "rates: a side's decode and prompt rates are the check's own timings");
        DeviceAnswer untimed;
        const SideRates none = answer_device_rates(untimed);
        check(none.decode_tps == 0.0 && none.prompt_tps == 0.0, "rates: nothing timed is no rate");
    }

    // The width the prompt side is timed at: the session's own graph width, bounded by its context and
    // by what a first load pays for, and nothing where it would be one of the narrow pieces.
    {
        check(answer_prompt_width(4096, 2048) == k_answer_prompt_tokens_max,
              "prompt width: a wide session is timed at the bound");
        check(answer_prompt_width(4096, 256) == 256, "prompt width: a session whose graphs are narrower, at theirs");
        check(answer_prompt_width(300, 2048) == 300, "prompt width: no wider than the context");
        check(answer_prompt_width(4096, k_answer_narrow_max) == 0,
              "prompt width: as narrow as the narrow pieces, no prompt timed");
        check(answer_prompt_width(0, 0) == 0, "prompt width: no context, no prompt timed");
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ok", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
