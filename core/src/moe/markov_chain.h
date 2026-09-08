#pragma once
#include <vector>
#include <cstdint>
#include <algorithm>
namespace bmoe {
class MarkovChain {
public:
MarkovChain() = default;
void init(int n_layer, int n_expert) {
if (n_layer <= 0 || n_expert <= 0) return;
n_layer_ = n_layer;
n_expert_ = n_expert;
counts_.assign((size_t) n_layer_ * n_expert_ * n_expert_, 0);
best_trans_.assign((size_t) n_layer_ * n_expert_, -1);
}
void reset() {
std::fill(counts_.begin(), counts_.end(), 0);
std::fill(best_trans_.begin(), best_trans_.end(), -1);
}
bool is_inited() const {
return n_expert_ > 0 && n_layer_ > 0;
}
int n_expert() const {
return n_expert_;
}
void update(int il, int32_t prev_exp, int32_t curr_exp) {
if (il < 0 || il >= n_layer_ || prev_exp < 0 || prev_exp >= n_expert_ || curr_exp < 0 || curr_exp >= n_expert_) {
return;
}
const size_t row_offset = ((size_t) il * n_expert_ + prev_exp) * n_expert_;
uint32_t & cell = counts_[row_offset + curr_exp];
if (cell >= kDecayTrigger) {
decay();
}
++cell;
const size_t best_idx = (size_t) il * n_expert_ + prev_exp;
int32_t best = best_trans_[best_idx];
if (best < 0 || cell > counts_[row_offset + best]) {
best_trans_[best_idx] = curr_exp;
}
}
int32_t predict(int il, int32_t prev_exp) const {
if (il < 0 || il >= n_layer_ || prev_exp < 0 || prev_exp >= n_expert_) {
return -1;
}
return best_trans_[(size_t) il * n_expert_ + prev_exp];
}
private:
void decay() {
for (uint32_t & c : counts_) {
c >>= 1;
}
}
static constexpr uint32_t kDecayTrigger = 1u << 16;
int n_layer_ = 0;
int n_expert_ = 0;
std::vector<uint32_t> counts_;
std::vector<int32_t> best_trans_;
};
} // namespace bmoe