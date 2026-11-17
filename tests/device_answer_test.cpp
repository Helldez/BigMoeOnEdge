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

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ok", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
