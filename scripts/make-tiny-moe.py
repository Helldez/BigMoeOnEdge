#!/usr/bin/env python3
"""Generate a tiny random-weight MoE gguf for the byte-identity gates.

The gates compare STREAMED-experts output against FULL-RESIDENT output of the SAME
file, so random weights are fine — quality is irrelevant, only that routing is a valid
top-k distribution (argsort of random logits) and that llama.cpp loads the model as a
MoE. The model is deliberately multi-layer with a few experts so the LRU cache path
sees real evictions on a small budget.

Three architectures are emitted, selected with --arch:

  qwen3moe  split expert layout — three tensors per layer
            (ffn_gate_exps / ffn_up_exps / ffn_down_exps).
  gemma4    fused gate+up layout — two expert tensors per layer
            (ffn_gate_up_exps / ffn_down_exps), plus a resident shared expert and an
            interleaved dense layer, so the gates cover the fused streaming path.
  nemotron_h_moe  gate-less layout: two expert tensors per layer
            (ffn_up_exps / ffn_down_exps, ReLU^2) in a hybrid Mamba2 / attention / MoE
            stack with latent projections, a resident shared expert and a trailing MTP
            block that is never loaded, so the gates cover the up+down streaming path.

Requires: pip install gguf numpy

    python scripts/make-tiny-moe.py --arch qwen3moe --out tiny-moe.gguf
    python scripts/make-tiny-moe.py --arch gemma4   --out tiny-moe-gemma4.gguf
    python scripts/make-tiny-moe.py --arch nemotron_h_moe --out tiny-moe-nemotron_h_moe.gguf
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
    w.add_sliding_window(N_CTX)
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", choices=["qwen3moe", "gemma4", "nemotron_h_moe"], default="qwen3moe")
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
    else:
        build_qwen3moe(args.out, args.split_max_tensors)


if __name__ == "__main__":
    main()
