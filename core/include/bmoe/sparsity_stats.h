// Intra-expert activation sparsity: how concentrated is the vector a routed expert's down
// projection consumes?
//
// The question this exists to answer is whether the "LLM in a Flash" family of ideas has anything
// left to offer a MoE engine. Top-k routing already skips whole experts; the open question is
// whether, INSIDE a routed expert, a small share of the intermediate neurons carries the output —
// in which case a row-sparse down projection would cut both the bytes read and the matmul.
//
// It measures only. Nothing here reaches the loading path, and no row is skipped: a probed run
// reads the same bytes and produces the same tokens as an unprobed one. It is not free (one
// isolated node per MoE layer, and a sort per routed slot), so a probed run is not a benchmark run.
#pragma once

namespace bmoe {

// The two curves are read in opposite directions and come from the same sort:
//   rows_for[i]  — how many rows you must keep to retain kMassTargets[i] of the slot's L1 mass.
//   mass_at[i]   — how much L1 mass survives if you keep the top kRowBudgets[i] of the rows.
// L1 (Σ|h|) rather than L2 because the down projection is linear in h: a row's contribution to the
// output is proportional to its magnitude, not to its square.
inline constexpr float kMassTargets[4] = {0.50f, 0.90f, 0.95f, 0.99f};
inline constexpr float kRowBudgets[4] = {0.0625f, 0.125f, 0.25f, 0.50f};

struct SparsityStats {
    long long slots = 0; // routed (layer, token, expert-slot) triples observed
    long long rows = 0;  // intermediate width summed over slots — the denominator for row fractions
    long long zeros = 0; // entries that are exactly 0 (what a ReLU model would hand you for free)

    long long rows_for[4] = {0, 0, 0, 0};     // rows needed per mass target, summed over slots
    double mass_at[4] = {0.0, 0.0, 0.0, 0.0}; // mass retained per row budget, summed over slots

    // Mean intermediate width of the slots observed; 0 when none were.
    double mean_width() const { return slots > 0 ? (double) rows / (double) slots : 0.0; }

    // Fraction of rows needed to hold kMassTargets[i], averaged over slots.
    double row_frac_for(int i) const { return rows > 0 ? (double) rows_for[i] / (double) rows : -1.0; }

    // Fraction of L1 mass retained at kRowBudgets[i], averaged over slots.
    double mass_frac_at(int i) const { return slots > 0 ? mass_at[i] / (double) slots : -1.0; }

    double zero_frac() const { return rows > 0 ? (double) zeros / (double) rows : -1.0; }

    void add(const SparsityStats & o) {
        slots += o.slots;
        rows += o.rows;
        zeros += o.zeros;
        for (int i = 0; i < 4; ++i) {
            rows_for[i] += o.rows_for[i];
            mass_at[i] += o.mass_at[i];
        }
    }
};

} // namespace bmoe
