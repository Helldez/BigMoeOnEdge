#!/usr/bin/env python3
"""Generate a tiny random-weight MoE gguf for the byte-identity gates.

The gates compare STREAMED-experts output against FULL-RESIDENT output of the SAME
file, so random weights are fine — quality is irrelevant, only that routing is a valid
top-k distribution (argsort of random logits) and that llama.cpp loads the model as a
MoE. The model is deliberately multi-layer with a few experts so the LRU cache path
sees real evictions on a small budget.

Five architectures are emitted, selected with --arch:

  qwen3moe  split expert layout — three tensors per layer
            (ffn_gate_exps / ffn_up_exps / ffn_down_exps).
  gemma4    fused gate+up layout — two expert tensors per layer
            (ffn_gate_up_exps / ffn_down_exps), plus a resident shared expert and an
            interleaved dense layer, so the gates cover the fused streaming path.
  nemotron_h_moe  gate-less layout: two expert tensors per layer
            (ffn_up_exps / ffn_down_exps, ReLU^2) in a hybrid Mamba2 / attention / MoE
            stack with latent projections, a resident shared expert and a trailing MTP
            block that is never loaded, so the gates cover the up+down streaming path.
  inkling2_loop  a core of blocks run more than once per token: each later pass is a layer of
            the graph of its own over the first pass's tensors (fused layout, a recurrent
            convolution state beside a sliding-window attention), so the gates cover a graph
            whose layers and blocks are two counts.
  hy_v4_flash  split layout at a latent width: the experts run between a down and an up
            projection of their own, beside hyper-connection streams, MLA with a pooled
            sparse-attention indexer, sliding-window layers, a leading dense layer and an
            n-gram (Engram) table, so the gates cover experts narrower than the model.

Requires: pip install gguf numpy

    python scripts/make-tiny-moe.py --arch qwen3moe --out tiny-moe.gguf
    python scripts/make-tiny-moe.py --arch gemma4   --out tiny-moe-gemma4.gguf
    python scripts/make-tiny-moe.py --arch nemotron_h_moe --out tiny-moe-nemotron_h_moe.gguf
    python scripts/make-tiny-moe.py --arch inkling2_loop --out tiny-moe-inkling2_loop.gguf
    python scripts/make-tiny-moe.py --arch hy_v4_flash --out tiny-moe-hy_v4_flash.gguf
"""
import argparse
import numpy as np

try:
    import gguf
except ImportError:
    raise SystemExit("missing dependency: pip install gguf numpy")

# --- tiny architecture -------------------------------------------------------------
# Sized so the experts total a few MiB across layers: a small LRU budget (a couple MiB)
# then forces real evictions, exercising that path in the gates.
N_LAYER        = 4
N_EMBD         = 128
N_HEAD         = 4
N_HEAD_KV      = 2
N_EMBD_HEAD    = N_EMBD // N_HEAD          # 32
N_EMBD_GQA     = N_HEAD_KV * N_EMBD_HEAD   # 64
N_FF           = 256
N_EXPERT       = 8
N_EXPERT_USED  = 2
N_FF_EXP       = 128
N_CTX          = 256
RMS_EPS        = 1e-6
ROPE_BASE      = 1000000.0


def build_vocab():
    """Minimal SPM byte-fallback vocab: 3 specials + 256 byte tokens."""
    tokens, scores, toktypes = [], [], []
    for t, ty in (("<unk>", gguf.TokenType.UNKNOWN),
                  ("<s>", gguf.TokenType.CONTROL),
                  ("</s>", gguf.TokenType.CONTROL)):
        tokens.append(t); scores.append(0.0); toktypes.append(ty)
    for b in range(256):
        tokens.append(f"<0x{b:02X}>"); scores.append(0.0); toktypes.append(gguf.TokenType.BYTE)
    return tokens, scores, toktypes


def rnd(*shape, seed):
    g = np.random.default_rng(seed)
    return g.standard_normal(shape).astype(np.float32) * 0.02


def add_tokenizer(w, tokens, scores, toktypes):
    w.add_tokenizer_model("llama")
    w.add_tokenizer_pre("default")
    w.add_token_list(tokens)
    w.add_token_scores(scores)
    w.add_token_types(toktypes)
    w.add_unk_token_id(0)
    w.add_bos_token_id(1)
    w.add_eos_token_id(2)
    w.add_add_bos_token(True)
    w.add_add_eos_token(False)
    # SPM prepends a space by default, and a byte-only vocab spells it as the three bytes of U+2581,
    # so every text would start with the same token. The decide gates score choices by their first
    # token, and "A", "B", "C" must stay distinguishable. Every other gate compares runs on the same
    # model, so the setting is arbitrary for them.
    w.add_add_space_prefix(False)


def add_attn_tensors(w, p, s):
    """Attention block shared by both architectures (numpy shapes are ggml dims reversed)."""
    w.add_tensor(p + "attn_q.weight",      rnd(N_EMBD_HEAD * N_HEAD, N_EMBD, seed=s + 1))
    w.add_tensor(p + "attn_k.weight",      rnd(N_EMBD_GQA, N_EMBD, seed=s + 2))
    w.add_tensor(p + "attn_v.weight",      rnd(N_EMBD_GQA, N_EMBD, seed=s + 3))
    w.add_tensor(p + "attn_output.weight", rnd(N_EMBD, N_EMBD_HEAD * N_HEAD, seed=s + 4))
    w.add_tensor(p + "attn_q_norm.weight", rnd(N_EMBD_HEAD, seed=s + 5))
    w.add_tensor(p + "attn_k_norm.weight", rnd(N_EMBD_HEAD, seed=s + 6))


# --- qwen3moe: split expert layout -------------------------------------------------
def make_writer(out, arch, split_max_tensors):
    """A plain writer, or a sharding one when --split-max-tensors is set.

    Sharded output mirrors how real >50 GB models arrive from Hugging Face: the writer
    emits <out>-%05d-of-%05d.gguf siblings, with a metadata-only first shard
    (small_first_shard, the layout unsloth ships). The byte-identity gates then prove the
    multi-shard streaming path against the same tensors the single-file fixture uses.
    """
    if not split_max_tensors:
        return gguf.GGUFWriter(out, arch)
    try:
        return gguf.GGUFWriter(out, arch,
                               split_max_tensors=split_max_tensors,
                               small_first_shard=True)
    except TypeError:
        raise SystemExit("this gguf package cannot write split files: pip install -U gguf")


def build_qwen3moe(out, split_max_tensors=0):
    tokens, scores, toktypes = build_vocab()
    n_vocab = len(tokens)

    w = make_writer(out, "qwen3moe", split_max_tensors)
    w.add_name("tiny-moe")
    w.add_context_length(N_CTX)
    w.add_embedding_length(N_EMBD)
    w.add_block_count(N_LAYER)
    w.add_feed_forward_length(N_FF)
    w.add_head_count(N_HEAD)
    w.add_head_count_kv(N_HEAD_KV)
    w.add_key_length(N_EMBD_HEAD)
    w.add_value_length(N_EMBD_HEAD)
    w.add_rope_freq_base(ROPE_BASE)
    w.add_layer_norm_rms_eps(RMS_EPS)
    w.add_expert_count(N_EXPERT)
    w.add_expert_used_count(N_EXPERT_USED)
    w.add_expert_feed_forward_length(N_FF_EXP)
    w.add_file_type(gguf.LlamaFileType.ALL_F32)
    add_tokenizer(w, tokens, scores, toktypes)

    w.add_tensor("token_embd.weight",  rnd(n_vocab, N_EMBD, seed=1))
    w.add_tensor("output_norm.weight", rnd(N_EMBD, seed=2))
    w.add_tensor("output.weight",      rnd(n_vocab, N_EMBD, seed=3))

    s = 100
    for i in range(N_LAYER):
        p = f"blk.{i}."
        w.add_tensor(p + "attn_norm.weight", rnd(N_EMBD, seed=s + 0))
        add_attn_tensors(w, p, s)
        w.add_tensor(p + "ffn_norm.weight",     rnd(N_EMBD, seed=s + 7))
        w.add_tensor(p + "ffn_gate_inp.weight", rnd(N_EXPERT, N_EMBD, seed=s + 8))
        # experts: dim-2 (numpy axis 0) indexes the expert
        w.add_tensor(p + "ffn_gate_exps.weight", rnd(N_EXPERT, N_FF_EXP, N_EMBD, seed=s + 9))
        w.add_tensor(p + "ffn_down_exps.weight", rnd(N_EXPERT, N_EMBD, N_FF_EXP, seed=s + 10))
        w.add_tensor(p + "ffn_up_exps.weight",   rnd(N_EXPERT, N_FF_EXP, N_EMBD, seed=s + 11))
        s += 100

    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file()
    w.close()
    print(f"wrote {out}: qwen3moe, {N_LAYER} layers, {N_EXPERT} experts "
          f"(top-{N_EXPERT_USED}), vocab {n_vocab}")


# --- gemma4: fused gate+up layout --------------------------------------------------
# Gemma 4 MoE packs gate+up into one expert tensor (ffn_gate_up_exps) and keeps an
# always-on shared expert (the layer's dense ffn_{gate,up,down}). We interleave one dense
# layer (no ffn_gate_inp) and make one layer full-attention (the rest sliding-window) so
# the fixture covers dense/MoE interleaving and the mixed SWA KV cache. Only the two
# expert weight tensors stream; the shared expert, router and gate_inp.scale stay resident.
DENSE_LAYER = 0            # a dense (non-MoE) layer, to exercise interleaving
FULL_ATTN_LAYER = 2        # the one non-SWA layer (rest are sliding-window)
SWA_WINDOW = 16            # far shorter than a context, as a real model's window is (gate G22c)


def build_gemma4(out):
    tokens, scores, toktypes = build_vocab()
    n_vocab = len(tokens)

    w = gguf.GGUFWriter(out, "gemma4")
    w.add_name("tiny-moe")
    w.add_context_length(N_CTX)
    w.add_embedding_length(N_EMBD)
    w.add_block_count(N_LAYER)
    w.add_feed_forward_length(N_FF)
    w.add_head_count(N_HEAD)
    w.add_head_count_kv(N_HEAD_KV)
    w.add_key_length(N_EMBD_HEAD)
    w.add_value_length(N_EMBD_HEAD)
    w.add_rope_freq_base(ROPE_BASE)
    w.add_layer_norm_rms_eps(RMS_EPS)
    w.add_expert_count(N_EXPERT)
    w.add_expert_used_count(N_EXPERT_USED)
    w.add_expert_feed_forward_length(N_FF_EXP)
    w.add_file_type(gguf.LlamaFileType.ALL_F32)

    # gemma4-specific hparams. One full-attention layer, the rest sliding-window; SWA head
    # dims equal the global ones so every layer shares the same shape. Per-layer input
    # embeddings are disabled (length 0) to keep the tensor set minimal.
    swa_pattern = [i != FULL_ATTN_LAYER for i in range(N_LAYER)]
    w.add_sliding_window_pattern(swa_pattern)
    w.add_sliding_window(SWA_WINDOW)
    w.add_key_length_swa(N_EMBD_HEAD)
    w.add_value_length_swa(N_EMBD_HEAD)
    w.add_embedding_length_per_layer_input(0)

    add_tokenizer(w, tokens, scores, toktypes)

    # Tied output (no output.weight → llama.cpp reuses token_embd). One shared rope_freqs
    # tensor covers the full-attention layer.
    w.add_tensor("token_embd.weight",  rnd(n_vocab, N_EMBD, seed=1))
    w.add_tensor("output_norm.weight", rnd(N_EMBD, seed=2))
    w.add_tensor("rope_freqs.weight",  rnd(N_EMBD_HEAD // 2, seed=3))

    s = 100
    for i in range(N_LAYER):
        p = f"blk.{i}."
        w.add_tensor(p + "attn_norm.weight", rnd(N_EMBD, seed=s + 0))
        add_attn_tensors(w, p, s)
        w.add_tensor(p + "post_attention_norm.weight", rnd(N_EMBD, seed=s + 7))

        # shared / dense FFN (also the shared expert on MoE layers)
        w.add_tensor(p + "ffn_norm.weight", rnd(N_EMBD, seed=s + 8))
        w.add_tensor(p + "ffn_gate.weight", rnd(N_FF, N_EMBD, seed=s + 9))
        w.add_tensor(p + "ffn_up.weight",   rnd(N_FF, N_EMBD, seed=s + 10))
        w.add_tensor(p + "ffn_down.weight", rnd(N_EMBD, N_FF, seed=s + 11))
        w.add_tensor(p + "post_ffw_norm.weight", rnd(N_EMBD, seed=s + 12))

        if i != DENSE_LAYER:
            # MoE layer: router (+ its required scale), extra norms, and the two streamed
            # expert tensors. ffn_gate_up_exps fuses gate+up: dim-1 is 2*N_FF_EXP.
            w.add_tensor(p + "ffn_gate_inp.weight", rnd(N_EXPERT, N_EMBD, seed=s + 13))
            w.add_tensor(p + "ffn_gate_inp.scale",  rnd(N_EMBD, seed=s + 14))
            w.add_tensor(p + "pre_ffw_norm_2.weight",  rnd(N_EMBD, seed=s + 15))
            w.add_tensor(p + "post_ffw_norm_1.weight", rnd(N_EMBD, seed=s + 16))
            w.add_tensor(p + "post_ffw_norm_2.weight", rnd(N_EMBD, seed=s + 17))
            # experts: dim-2 (numpy axis 0) indexes the expert
            w.add_tensor(p + "ffn_gate_up_exps.weight", rnd(N_EXPERT, 2 * N_FF_EXP, N_EMBD, seed=s + 18))
            w.add_tensor(p + "ffn_down_exps.weight",    rnd(N_EXPERT, N_EMBD, N_FF_EXP, seed=s + 19))
        s += 100

    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file()
    w.close()
    n_moe = N_LAYER - 1
    print(f"wrote {out}: gemma4, {N_LAYER} layers ({n_moe} MoE, fused gate_up), "
          f"{N_EXPERT} experts (top-{N_EXPERT_USED}), vocab {n_vocab}")


# --- nemotron_h_moe: gate-less expert layout ---------------------------------------
# Nemotron-H MoE (Nemotron 3 / 3.5, e.g. 30B-A3B) has no gate projection: each expert is
# up -> ReLU^2 -> down, so a layer names two expert tensors (ffn_up_exps / ffn_down_exps).
# The stack is hybrid and the block kind is read from per-layer hparams: head_count_kv == 0
# and feed_forward_length == 0 is a Mamba2 block, head_count_kv > 0 an attention block,
# feed_forward_length > 0 a MoE block; no two MoE blocks are adjacent, as in the released
# models. The experts may run in a latent space (ffn_latent_down / ffn_latent_up project in
# and out of it; optional, the 30B has none, the fixture has it to cover the narrower rows),
# the router adds a per-expert bias before a sigmoid top-k, and an always-on shared expert
# (ffn_{up,down}_shexp) sits beside them. A trailing NextN/MTP block names expert tensors
# too but is skipped at load (load_mtp is off), so the fixture also proves an unloaded
# expert-named block never binds.
NEMO_LAYERS = ["mamba", "moe", "attn", "moe", "mamba", "moe"]
NEMO_LATENT = 64           # moe_latent_size: the experts' input/output width
# The latent width makes each expert small, so its hidden width is raised to keep the bank
# (3 layers x 8 experts x 256 KiB) well above the gates' 2 MiB cache, which has to evict.
NEMO_FF_EXP = 512
NEMO_FF_SHEXP = 128
SSM_D_CONV = 4
SSM_D_INNER = 128
SSM_D_STATE = 16
SSM_N_GROUP = 2
SSM_N_HEAD = 4             # ssm time_step_rank: d_inner must divide by it and by n_group


def add_nemotron_moe_tensors(w, p, s, mtp=False):
    """Router, latent projections, the two streamed expert tensors and the shared expert."""
    w.add_tensor(p + "ffn_gate_inp.weight", rnd(N_EXPERT, N_EMBD, seed=s + 20))
    # Zero selection bias. The loader requires the tensor, but at this model's scale a random
    # one would outweigh the logits and pin every token to the same experts, leaving the cache
    # and substitution gates nothing to exercise.
    w.add_tensor(p + "exp_probs_b.bias",    np.zeros(N_EXPERT, dtype=np.float32))
    # The loader sizes every expert tensor at the latent width, the MTP block's included,
    # but only the trunk's MoE blocks carry the latent projections.
    if not mtp:
        w.add_tensor(p + "ffn_latent_down.weight", rnd(NEMO_LATENT, N_EMBD, seed=s + 22))
        w.add_tensor(p + "ffn_latent_up.weight",   rnd(N_EMBD, NEMO_LATENT, seed=s + 23))
    # experts: dim-2 (numpy axis 0) indexes the expert
    w.add_tensor(p + "ffn_up_exps.weight",   rnd(N_EXPERT, NEMO_FF_EXP, NEMO_LATENT, seed=s + 24))
    w.add_tensor(p + "ffn_down_exps.weight", rnd(N_EXPERT, NEMO_LATENT, NEMO_FF_EXP, seed=s + 25))
    w.add_tensor(p + "ffn_up_shexp.weight",   rnd(NEMO_FF_SHEXP, N_EMBD, seed=s + 26))
    w.add_tensor(p + "ffn_down_shexp.weight", rnd(N_EMBD, NEMO_FF_SHEXP, seed=s + 27))


def build_nemotron_h_moe(out):
    tokens, scores, toktypes = build_vocab()
    n_vocab = len(tokens)
    n_layer = len(NEMO_LAYERS)
    n_all = n_layer + 1    # + the trailing MTP block
    kinds = NEMO_LAYERS + ["mtp"]

    w = gguf.GGUFWriter(out, "nemotron_h_moe")
    w.add_name("tiny-moe")
    w.add_context_length(N_CTX)
    w.add_embedding_length(N_EMBD)
    w.add_block_count(n_all)
    w.add_nextn_predict_layers(1)
    w.add_feed_forward_length([N_FF if k == "moe" else 0 for k in kinds])
    w.add_head_count([N_HEAD] * n_all)
    w.add_head_count_kv([N_HEAD_KV if k in ("attn", "mtp") else 0 for k in kinds])
    w.add_key_length(N_EMBD_HEAD)
    w.add_value_length(N_EMBD_HEAD)
    w.add_layer_norm_rms_eps(RMS_EPS)
    w.add_layer_norm_eps(RMS_EPS)
    w.add_ssm_conv_kernel(SSM_D_CONV)
    w.add_ssm_inner_size(SSM_D_INNER)
    w.add_ssm_state_size(SSM_D_STATE)
    w.add_ssm_group_count(SSM_N_GROUP)
    w.add_ssm_time_step_rank(SSM_N_HEAD)
    w.add_expert_count(N_EXPERT)
    w.add_expert_used_count(N_EXPERT_USED)
    w.add_expert_feed_forward_length(NEMO_FF_EXP)
    w.add_expert_shared_feed_forward_length(NEMO_FF_SHEXP)
    w.add_expert_shared_count(1)
    w.add_expert_weights_norm(True)
    w.add_expert_weights_scale(1.0)
    w.add_moe_latent_size(NEMO_LATENT)
    w.add_file_type(gguf.LlamaFileType.ALL_F32)
    add_tokenizer(w, tokens, scores, toktypes)

    w.add_tensor("token_embd.weight",  rnd(n_vocab, N_EMBD, seed=1))
    w.add_tensor("output_norm.weight", rnd(N_EMBD, seed=2))
    w.add_tensor("output.weight",      rnd(n_vocab, N_EMBD, seed=3))

    d_xbc = SSM_D_INNER + 2 * SSM_N_GROUP * SSM_D_STATE
    d_in_proj = 2 * SSM_D_INNER + 2 * SSM_N_GROUP * SSM_D_STATE + SSM_N_HEAD
    s = 100
    for i, kind in enumerate(NEMO_LAYERS):
        p = f"blk.{i}."
        w.add_tensor(p + "attn_norm.weight", rnd(N_EMBD, seed=s + 0))
        if kind == "mamba":
            w.add_tensor(p + "ssm_in.weight",     rnd(d_in_proj, N_EMBD, seed=s + 1))
            w.add_tensor(p + "ssm_conv1d.weight", rnd(d_xbc, SSM_D_CONV, seed=s + 2))
            w.add_tensor(p + "ssm_conv1d.bias",   rnd(d_xbc, seed=s + 3))
            w.add_tensor(p + "ssm_dt.bias",       rnd(SSM_N_HEAD, seed=s + 4))
            # A = -exp(A_log) is negative in a real checkpoint; keep it so the scan decays
            w.add_tensor(p + "ssm_a", -np.abs(rnd(SSM_N_HEAD, 1, seed=s + 5)) - 0.5)
            w.add_tensor(p + "ssm_d", rnd(SSM_N_HEAD, 1, seed=s + 6))
            w.add_tensor(p + "ssm_norm.weight", rnd(SSM_N_GROUP, SSM_D_INNER // SSM_N_GROUP, seed=s + 7))
            w.add_tensor(p + "ssm_out.weight",  rnd(N_EMBD, SSM_D_INNER, seed=s + 8))
        elif kind == "attn":
            w.add_tensor(p + "attn_q.weight",      rnd(N_EMBD_HEAD * N_HEAD, N_EMBD, seed=s + 1))
            w.add_tensor(p + "attn_k.weight",      rnd(N_EMBD_GQA, N_EMBD, seed=s + 2))
            w.add_tensor(p + "attn_v.weight",      rnd(N_EMBD_GQA, N_EMBD, seed=s + 3))
            w.add_tensor(p + "attn_output.weight", rnd(N_EMBD, N_EMBD_HEAD * N_HEAD, seed=s + 4))
        else:
            add_nemotron_moe_tensors(w, p, s)
        s += 100

    # The MTP block folds an attention and a MoE sub-layer into one trailing block.
    p = f"blk.{n_layer}."
    w.add_tensor(p + "nextn.enorm.weight",            rnd(N_EMBD, seed=s + 1))
    w.add_tensor(p + "nextn.hnorm.weight",            rnd(N_EMBD, seed=s + 2))
    w.add_tensor(p + "nextn.eh_proj.weight",          rnd(N_EMBD, 2 * N_EMBD, seed=s + 3))
    w.add_tensor(p + "nextn.shared_head_norm.weight", rnd(N_EMBD, seed=s + 4))
    w.add_tensor(p + "attn_norm.weight",   rnd(N_EMBD, seed=s + 5))
    w.add_tensor(p + "attn_q.weight",      rnd(N_EMBD_HEAD * N_HEAD, N_EMBD, seed=s + 6))
    w.add_tensor(p + "attn_k.weight",      rnd(N_EMBD_GQA, N_EMBD, seed=s + 7))
    w.add_tensor(p + "attn_v.weight",      rnd(N_EMBD_GQA, N_EMBD, seed=s + 8))
    w.add_tensor(p + "attn_output.weight", rnd(N_EMBD, N_EMBD_HEAD * N_HEAD, seed=s + 9))
    w.add_tensor(p + "post_attention_norm.weight", rnd(N_EMBD, seed=s + 10))
    add_nemotron_moe_tensors(w, p, s, mtp=True)

    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file()
    w.close()
    n_moe = NEMO_LAYERS.count("moe")
    print(f"wrote {out}: nemotron_h_moe, {n_layer} layers ({n_moe} MoE, gate-less) + 1 MTP, "
          f"{N_EXPERT} experts (top-{N_EXPERT_USED}), vocab {n_vocab}")


# --- inkling2_loop: a core of blocks run more than once per token ----------------------
# The looped Inkling 2 runs its middle blocks LOOP_COUNT times: each later pass is a layer of the
# graph of its own (its own KV and convolution state) over the first pass's weights. So a graph
# layer and the block whose experts it computes from are two numbers, and the streamer has to
# load a block's experts for every pass that routes to it. The fixture keeps the real model's
# shape at the smallest size its loader accepts: blocks of LOOP_BLOCK layers, the last of each
# full-attention and the rest sliding-window, experts in the fused gate+up layout.
LOOP_LAYERS    = 6
LOOP_BLOCK     = 2             # attention-residual block size; the loop core is a multiple of it
LOOP_START     = 2
LOOP_END       = 4
LOOP_COUNT     = 2             # the least a fixed loop may have
LOOP_GATE_RANK = 4
LOOP_N_REL     = 4             # relative-position bias rows
LOOP_EXTENT    = 16            # positions the bias spans, full-attention layers
LOOP_SWA       = 16
LOOP_CONV      = 2             # short-convolution taps
LOOP_PAD       = 5             # the embedding is padded past the vocabulary


def build_inkling2_loop(out):
    tokens, scores, toktypes = build_vocab()
    n_vocab = len(tokens)
    n_vocab_padded = n_vocab + LOOP_PAD
    arch = "inkling2_loop"
    swa = [(i + 1) % LOOP_BLOCK != 0 for i in range(LOOP_LAYERS)]
    n_q = N_EMBD_HEAD * N_HEAD

    w = gguf.GGUFWriter(out, arch)
    w.add_name("tiny-moe-inkling2-loop")
    w.add_context_length(N_CTX)
    w.add_embedding_length(N_EMBD)
    w.add_block_count(LOOP_LAYERS)
    w.add_feed_forward_length(N_FF_EXP)
    w.add_head_count([N_HEAD] * LOOP_LAYERS)
    w.add_head_count_kv([N_HEAD_KV] * LOOP_LAYERS)
    w.add_key_length(N_EMBD_HEAD)
    w.add_value_length(N_EMBD_HEAD)
    w.add_layer_norm_rms_eps(RMS_EPS)
    w.add_expert_count(N_EXPERT)
    w.add_expert_used_count(N_EXPERT_USED)
    w.add_expert_feed_forward_length(N_FF_EXP)
    w.add_sliding_window(LOOP_SWA)
    w.add_array(f"{arch}.attention.sliding_window_pattern", swa)
    w.add_uint32(f"{arch}.vocab_size", n_vocab_padded)
    w.add_uint32(f"{arch}.vocab_size_unpadded", n_vocab)
    w.add_uint32(f"{arch}.rope.dimension_count", 0)
    w.add_uint32(f"{arch}.shortconv.l_cache", LOOP_CONV)
    w.add_uint32(f"{arch}.attn_res.block_size", LOOP_BLOCK)
    w.add_uint32(f"{arch}.attention.relative_length", LOOP_N_REL)
    w.add_uint32(f"{arch}.attention.relative_extent", LOOP_EXTENT)
    w.add_uint32(f"{arch}.attention.log_scale_floor", 64)
    w.add_float32(f"{arch}.attention.log_scale_alpha", 0.1)
    w.add_float32(f"{arch}.attention.qk_clip", 8.0)
    w.add_float32(f"{arch}.attention.relative_clip", 8.0)
    w.add_float32(f"{arch}.activation.situ_beta", 4.0)
    w.add_float32(f"{arch}.activation.situ_linear_beta", 25.0)
    w.add_float32(f"{arch}.expert_weights_scale", 1.0)
    w.add_uint32(f"{arch}.loop.start", LOOP_START)
    w.add_uint32(f"{arch}.loop.end", LOOP_END)
    w.add_uint32(f"{arch}.loop.count", LOOP_COUNT)
    w.add_uint32(f"{arch}.loop.gate_rank", LOOP_GATE_RANK)
    w.add_bool(f"{arch}.loop.varying", False)
    w.add_file_type(gguf.LlamaFileType.ALL_F32)
    add_tokenizer(w, tokens, scores, toktypes)

    w.add_tensor("token_embd.weight",       rnd(n_vocab_padded, N_EMBD, seed=1))
    w.add_tensor("token_embd_norm.weight",  1.0 + rnd(N_EMBD, seed=2))
    w.add_tensor("output.weight",           rnd(n_vocab_padded, N_EMBD, seed=3))
    w.add_tensor("output_norm.weight",      1.0 + rnd(N_EMBD, seed=4))
    w.add_tensor("output_res_query.weight", rnd(N_EMBD, seed=5))

    s = 100
    for i in range(LOOP_LAYERS):
        p = f"blk.{i}."
        extent = LOOP_SWA if swa[i] else LOOP_EXTENT
        w.add_tensor(p + "attn_q.weight",         rnd(n_q, N_EMBD, seed=s + 1))
        w.add_tensor(p + "attn_k.weight",         rnd(N_EMBD_GQA, N_EMBD, seed=s + 2))
        w.add_tensor(p + "attn_v.weight",         rnd(N_EMBD_GQA, N_EMBD, seed=s + 3))
        w.add_tensor(p + "attn_output.weight",    rnd(N_EMBD, n_q, seed=s + 4))
        w.add_tensor(p + "attn_gate.weight",      rnd(n_q, N_EMBD, seed=s + 5))
        w.add_tensor(p + "attn_q_clip.weight",    1.0 + rnd(N_HEAD, N_EMBD_HEAD, seed=s + 6))
        w.add_tensor(p + "attn_sink_proj.weight", rnd(N_HEAD, N_EMBD, seed=s + 7))
        w.add_tensor(p + "attn_rel.weight",       rnd(LOOP_N_REL * N_HEAD, N_EMBD, seed=s + 8))
        w.add_tensor(p + "attn_rel_bias.weight",  rnd(extent, LOOP_N_REL, seed=s + 9))
        w.add_tensor(p + "attn_rel_clip.weight",  1.0 + rnd(N_HEAD, LOOP_N_REL, seed=s + 10))
        w.add_tensor(p + "attn_k_conv.weight",    rnd(N_EMBD_GQA, LOOP_CONV, seed=s + 11))
        w.add_tensor(p + "attn_v_conv.weight",    rnd(N_EMBD_GQA, LOOP_CONV, seed=s + 12))
        w.add_tensor(p + "attn_post_conv.weight", rnd(N_EMBD, LOOP_CONV, seed=s + 13))
        w.add_tensor(p + "ffn_post_conv.weight",  rnd(N_EMBD, LOOP_CONV, seed=s + 14))
        if swa[i]:
            w.add_tensor(p + "attn_norm.weight", 1.0 + rnd(N_EMBD, seed=s + 15))
        else:
            w.add_tensor(p + "attn_q_inp_norm.weight",  1.0 + rnd(N_EMBD, seed=s + 16))
            w.add_tensor(p + "attn_k_inp_norm.weight",  1.0 + rnd(N_EMBD, seed=s + 17))
            w.add_tensor(p + "attn_v_inp_norm.weight",  1.0 + rnd(N_EMBD, seed=s + 18))
            w.add_tensor(p + "attn_k_conv_proj.weight", rnd(LOOP_CONV * N_HEAD_KV, N_EMBD, seed=s + 19))
            w.add_tensor(p + "attn_v_conv_proj.weight", rnd(LOOP_CONV * N_HEAD_KV, N_EMBD, seed=s + 20))
        if i > 0:
            w.add_tensor(p + "attn_res_query.weight", rnd(1 if swa[i] else 3, N_EMBD, seed=s + 21))
        w.add_tensor(p + "ffn_res_query.weight", rnd(N_EMBD, seed=s + 22))
        w.add_tensor(p + "ffn_norm.weight",      1.0 + rnd(N_EMBD, seed=s + 23))
        w.add_tensor(p + "ffn_gate_inp.weight",  rnd(N_EXPERT, N_EMBD, seed=s + 24))
        # experts: dim-2 (numpy axis 0) indexes the expert; gate and up fused along the output
        w.add_tensor(p + "ffn_gate_up_exps.weight", rnd(N_EXPERT, 2 * N_FF_EXP, N_EMBD, seed=s + 25))
        w.add_tensor(p + "ffn_down_exps.weight",    rnd(N_EXPERT, N_EMBD, N_FF_EXP, seed=s + 26))
        if LOOP_START <= i < LOOP_END:
            w.add_tensor(p + "loop_msg_query.weight", rnd(4, N_EMBD, seed=s + 30))
            for c, name in enumerate(("q", "k", "v", "ffn")):
                w.add_tensor(p + f"loop_{name}_ctrl_down.weight",   rnd(LOOP_GATE_RANK, 2 * N_EMBD, seed=s + 31 + 3 * c))
                w.add_tensor(p + f"loop_{name}_ctrl_gain.weight",   rnd(N_EMBD, LOOP_GATE_RANK, seed=s + 32 + 3 * c))
                w.add_tensor(p + f"loop_{name}_ctrl_forget.weight", rnd(N_EMBD, LOOP_GATE_RANK, seed=s + 33 + 3 * c))
        s += 100

    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file()
    w.close()
    slots = LOOP_LAYERS + (LOOP_COUNT - 1) * (LOOP_END - LOOP_START)
    print(f"wrote {out}: inkling2_loop, {LOOP_LAYERS} blocks run as {slots} graph layers "
          f"(core [{LOOP_START}, {LOOP_END}) x{LOOP_COUNT}), {N_EXPERT} experts (top-{N_EXPERT_USED})")


# --- hy_v4_flash: latent experts beside hyper-connections and an n-gram table ----------
HY_LAYERS      = 4             # layer 0 dense, then sparse / sliding-window MoE layers
HY_DENSE_LEAD  = 1
HY_HC          = 4             # hyper-connection streams
HY_N_HEAD      = 2             # sparse layers; the sliding-window ones carry twice as many
HY_Q_LORA      = 64
HY_KV_LORA     = 64
HY_ROPE        = 32
HY_K_MLA       = 48            # nope 16 + rope 32
HY_V_MLA       = 32
HY_LATENT      = 64            # the experts' width, distinct from n_embd and from n_ff_exp
HY_FF_EXP      = 256           # wider than N_FF_EXP: at the latent width the experts must still
                               # outgrow the gates' 2 MiB cache, or nothing is ever evicted or prefetched
HY_N_SHARED    = 1
HY_SWA         = 16
HY_IDX_HEADS   = 2
HY_IDX_DIM     = 64            # must exceed the rope width
HY_KPOOL       = 4
HY_IDX_TOP_K   = 16            # four pools: shorter than the gates' prompts, so the selection is live
HY_PLE_DIM     = 32
HY_PLE_PRIMES  = [7, 11, 13, 17]


def u32_array(w, key, values):
    w.add_key_value(key, list(values), gguf.GGUFValueType.ARRAY, sub_type=gguf.GGUFValueType.UINT32)


def build_hy_v4_flash(out):
    tokens, scores, toktypes = build_vocab()
    n_vocab = len(tokens)
    arch = "hy_v4_flash"
    swa = [i % 2 == 1 for i in range(HY_LAYERS)]
    heads = [2 * HY_N_HEAD if s else HY_N_HEAD for s in swa]
    nope = HY_K_MLA - HY_ROPE
    offsets = [sum(HY_PLE_PRIMES[:c]) for c in range(len(HY_PLE_PRIMES))]
    ple_heads = len(HY_PLE_PRIMES)
    ple_rows = sum(HY_PLE_PRIMES)
    engram_layer = 1

    w = gguf.GGUFWriter(out, arch)
    w.add_name("tiny-moe-hy-v4-flash")
    w.add_context_length(N_CTX)
    w.add_embedding_length(N_EMBD)
    w.add_block_count(HY_LAYERS)
    w.add_feed_forward_length(N_FF)
    u32_array(w, f"{arch}.attention.head_count", heads)
    w.add_head_count_kv(1)
    w.add_key_length(HY_KV_LORA + HY_ROPE)
    w.add_value_length(HY_KV_LORA)
    w.add_uint32(f"{arch}.attention.key_length_mla", HY_K_MLA)
    w.add_uint32(f"{arch}.attention.value_length_mla", HY_V_MLA)
    w.add_uint32(f"{arch}.attention.q_lora_rank", HY_Q_LORA)
    w.add_uint32(f"{arch}.attention.kv_lora_rank", HY_KV_LORA)
    w.add_uint32(f"{arch}.rope.dimension_count", HY_ROPE)
    # the published files carry M-RoPE sections (summing to n_rot / 2), so the text path runs them
    w.add_key_value(f"{arch}.rope.dimension_sections", [6, 5, 5, 0], gguf.GGUFValueType.ARRAY,
                    sub_type=gguf.GGUFValueType.INT32)
    w.add_rope_freq_base(ROPE_BASE)
    w.add_float32(f"{arch}.rope.freq_base_swa", 10000.0)
    w.add_layer_norm_rms_eps(RMS_EPS)
    w.add_sliding_window(HY_SWA)
    w.add_array(f"{arch}.attention.sliding_window_pattern", swa)
    w.add_uint32(f"{arch}.leading_dense_block_count", HY_DENSE_LEAD)
    w.add_expert_count(N_EXPERT)
    w.add_expert_used_count(N_EXPERT_USED)
    w.add_expert_feed_forward_length(HY_FF_EXP)
    w.add_uint32(f"{arch}.expert_shared_count", HY_N_SHARED)
    w.add_uint32(f"{arch}.expert_latent_length", HY_LATENT)
    w.add_float32(f"{arch}.expert_weights_scale", 1.0)
    w.add_bool(f"{arch}.expert_weights_norm", True)
    # sigmoid scores with a selection bias (exp_probs_b): the file must name one, there is no default
    w.add_uint32(f"{arch}.expert_gating_func", int(gguf.ExpertGatingFuncType.SIGMOID))
    w.add_float32(f"{arch}.swiglu_clamp_exp", 10.0)
    w.add_uint32(f"{arch}.hyper_connection.count", HY_HC)
    w.add_float32(f"{arch}.hyper_connection.epsilon", 1e-6)
    w.add_float32(f"{arch}.hyper_connection.magnitude", 2.0)
    w.add_uint32(f"{arch}.attention.indexer.top_k", HY_IDX_TOP_K)
    w.add_uint32(f"{arch}.attention.indexer.head_count", HY_IDX_HEADS)
    w.add_uint32(f"{arch}.attention.indexer.key_length", HY_IDX_DIM)
    w.add_uint32(f"{arch}.attention.indexer.kpool", HY_KPOOL)
    w.add_bool(f"{arch}.attention.indexer.kpool_select_tail", True)
    u32_array(w, f"{arch}.attention.indexer.types", [0 if s else 1 for s in swa])
    # one Engram module: the n-gram hash table every module shares, its buckets back to back
    u32_array(w, f"{arch}.ple.layers", [engram_layer])
    w.add_uint32(f"{arch}.ple.ngram_size", 3)
    w.add_uint32(f"{arch}.ple.heads_per_ngram", 2)
    w.add_uint32(f"{arch}.embedding_length_per_layer_input", HY_PLE_DIM)
    for key, vals in (("layer_multipliers", [3, 5, 7]), ("head_offsets", offsets),
                      ("head_vocab_sizes", HY_PLE_PRIMES)):
        w.add_key_value(f"{arch}.ple.{key}", vals, gguf.GGUFValueType.ARRAY, sub_type=gguf.GGUFValueType.UINT64)
    w.add_uint32(f"{arch}.engram.pad_token_id", 1)
    w.add_key_value(f"{arch}.engram.token_map", list(range(n_vocab)), gguf.GGUFValueType.ARRAY,
                    sub_type=gguf.GGUFValueType.INT32)
    w.add_file_type(gguf.LlamaFileType.ALL_F32)
    add_tokenizer(w, tokens, scores, toktypes)

    w.add_tensor("token_embd.weight",           rnd(n_vocab, N_EMBD, seed=1))
    w.add_tensor("output_norm.weight",          1.0 + rnd(N_EMBD, seed=2))
    w.add_tensor("output.weight",               rnd(n_vocab, N_EMBD, seed=3))
    w.add_tensor("output_hc_fn.weight",         rnd(HY_HC, HY_HC * N_EMBD, seed=4))
    w.add_tensor("output_hc_base.weight",       rnd(HY_HC, seed=5))
    w.add_tensor("output_hc_scale.weight",      1.0 + rnd(1, seed=6))
    w.add_tensor("per_layer_token_embd.weight", rnd(ple_rows, HY_PLE_DIM, seed=7))

    s = 100
    for i in range(HY_LAYERS):
        p = f"blk.{i}."
        h = heads[i]
        w.add_tensor(p + "attn_norm.weight",           1.0 + rnd(N_EMBD, seed=s + 1))
        w.add_tensor(p + "attn_sinks.weight",          rnd(h, seed=s + 2))
        w.add_tensor(p + "attn_q_a.weight",            rnd(HY_Q_LORA, N_EMBD, seed=s + 3))
        w.add_tensor(p + "attn_q_a_norm.weight",       1.0 + rnd(HY_Q_LORA, seed=s + 4))
        w.add_tensor(p + "attn_q_b.weight",            rnd(h * HY_K_MLA, HY_Q_LORA, seed=s + 5))
        w.add_tensor(p + "attn_kv_a_mqa.weight",       rnd(HY_KV_LORA + HY_ROPE, N_EMBD, seed=s + 6))
        w.add_tensor(p + "attn_kv_a_norm.weight",      1.0 + rnd(HY_KV_LORA, seed=s + 7))
        w.add_tensor(p + "attn_k_b.weight",            rnd(h, HY_KV_LORA, nope, seed=s + 8))
        w.add_tensor(p + "attn_v_b.weight",            rnd(h, HY_V_MLA, HY_KV_LORA, seed=s + 9))
        w.add_tensor(p + "attn_output.weight",         rnd(N_EMBD, h * HY_V_MLA, seed=s + 10))
        w.add_tensor(p + "attn_gate.weight",           rnd(h * HY_V_MLA, N_EMBD, seed=s + 11))
        w.add_tensor(p + "post_attention_norm.weight", 1.0 + rnd(N_EMBD, seed=s + 12))
        w.add_tensor(p + "hc_attn_fn.weight",          rnd(2 * HY_HC, HY_HC * N_EMBD, seed=s + 13))
        w.add_tensor(p + "hc_attn_base.weight",        rnd(2 * HY_HC, seed=s + 14))
        w.add_tensor(p + "hc_attn_scale.weight",       1.0 + rnd(2, seed=s + 15))
        w.add_tensor(p + "hc_ffn_fn.weight",           rnd(2 * HY_HC, HY_HC * N_EMBD, seed=s + 16))
        w.add_tensor(p + "hc_ffn_base.weight",         rnd(2 * HY_HC, seed=s + 17))
        w.add_tensor(p + "hc_ffn_scale.weight",        1.0 + rnd(2, seed=s + 18))
        w.add_tensor(p + "ffn_norm.weight",            1.0 + rnd(N_EMBD, seed=s + 19))
        w.add_tensor(p + "post_ffw_norm.weight",       1.0 + rnd(N_EMBD, seed=s + 20))
        if not swa[i]:
            w.add_tensor(p + "indexer.attn_k.weight",              rnd(HY_IDX_DIM, N_EMBD, seed=s + 21))
            w.add_tensor(p + "indexer.k_norm.weight",              1.0 + rnd(HY_IDX_DIM, seed=s + 22))
            w.add_tensor(p + "indexer_compressor_value.weight",    rnd(HY_IDX_DIM, HY_IDX_DIM, seed=s + 23))
            w.add_tensor(p + "indexer_compressor_gate.weight",     rnd(HY_IDX_DIM, HY_IDX_DIM, seed=s + 24))
            w.add_tensor(p + "indexer_compressor_ape.weight",      rnd(HY_KPOOL, HY_IDX_DIM, seed=s + 25))
            w.add_tensor(p + "indexer_compressor_norm.weight",     1.0 + rnd(HY_IDX_DIM, seed=s + 26))
            w.add_tensor(p + "indexer.attn_q_b.weight",            rnd(HY_IDX_DIM * HY_IDX_HEADS, HY_Q_LORA, seed=s + 27))
            w.add_tensor(p + "indexer.proj.weight",                rnd(HY_IDX_HEADS, N_EMBD, seed=s + 28))
        if i == engram_layer:
            w.add_tensor(p + "engram_kv.weight",          rnd(N_EMBD * (HY_HC + 1), ple_heads * HY_PLE_DIM, seed=s + 29))
            w.add_tensor(p + "engram_kv_b.weight",        rnd(HY_HC + 1, N_EMBD, seed=s + 30))
            w.add_tensor(p + "engram_gate.weight",        rnd(HY_HC, N_EMBD, seed=s + 31))
            w.add_tensor(p + "engram_lookup_norm.weight", 1.0 + rnd(ple_heads, HY_PLE_DIM, seed=s + 32))
            w.add_tensor(p + "engram_pre_gamma.weight",   1.0 + rnd(N_EMBD, seed=s + 33))
            w.add_tensor(p + "engram_hc_scale.weight",    rnd(HY_HC, N_EMBD, seed=s + 34))
        if i < HY_DENSE_LEAD:
            w.add_tensor(p + "ffn_gate.weight", rnd(N_FF, N_EMBD, seed=s + 40))
            w.add_tensor(p + "ffn_up.weight",   rnd(N_FF, N_EMBD, seed=s + 41))
            w.add_tensor(p + "ffn_down.weight", rnd(N_EMBD, N_FF, seed=s + 42))
        else:
            w.add_tensor(p + "ffn_gate_inp.weight",    rnd(N_EXPERT, N_EMBD, seed=s + 43))
            w.add_tensor(p + "exp_probs_b.bias",       rnd(N_EXPERT, seed=s + 44))
            # the experts run at the latent width, between a down and an up projection of their own
            w.add_tensor(p + "ffn_routed_down.weight", rnd(HY_LATENT, N_EMBD, seed=s + 45))
            w.add_tensor(p + "ffn_routed_up.weight",   rnd(N_EMBD, HY_LATENT, seed=s + 46))
            # experts: dim-2 (numpy axis 0) indexes the expert
            w.add_tensor(p + "ffn_gate_exps.weight",   rnd(N_EXPERT, HY_FF_EXP, HY_LATENT, seed=s + 47))
            w.add_tensor(p + "ffn_up_exps.weight",     rnd(N_EXPERT, HY_FF_EXP, HY_LATENT, seed=s + 48))
            w.add_tensor(p + "ffn_down_exps.weight",   rnd(N_EXPERT, HY_LATENT, HY_FF_EXP, seed=s + 49))
            w.add_tensor(p + "ffn_gate_shexp.weight",  rnd(HY_FF_EXP * HY_N_SHARED, N_EMBD, seed=s + 50))
            w.add_tensor(p + "ffn_up_shexp.weight",    rnd(HY_FF_EXP * HY_N_SHARED, N_EMBD, seed=s + 51))
            w.add_tensor(p + "ffn_down_shexp.weight",  rnd(N_EMBD, HY_FF_EXP * HY_N_SHARED, seed=s + 52))
        s += 100

    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file()
    w.close()
    print(f"wrote {out}: hy_v4_flash, {HY_LAYERS} layers ({HY_DENSE_LEAD} dense), {N_EXPERT} experts "
          f"(top-{N_EXPERT_USED}) at latent width {HY_LATENT}, {HY_HC} hyper-connection streams, "
          f"one n-gram module of {ple_rows} rows")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", choices=["qwen3moe", "gemma4", "nemotron_h_moe", "inkling2_loop", "hy_v4_flash"],
                    default="qwen3moe")
    ap.add_argument("--out", default="tiny-moe.gguf")
    ap.add_argument("--split-max-tensors", type=int, default=0,
                    help="emit a sharded gguf (N tensors per shard, metadata-only first shard)")
    args = ap.parse_args()

    if args.arch != "qwen3moe" and args.split_max_tensors:
        raise SystemExit("--split-max-tensors is exercised via the qwen3moe fixture only")
    if args.arch == "gemma4":
        build_gemma4(args.out)
    elif args.arch == "nemotron_h_moe":
        build_nemotron_h_moe(args.out)
    elif args.arch == "inkling2_loop":
        build_inkling2_loop(args.out)
    elif args.arch == "hy_v4_flash":
        build_hy_v4_flash(args.out)
    else:
        build_qwen3moe(args.out, args.split_max_tensors)


if __name__ == "__main__":
    main()
