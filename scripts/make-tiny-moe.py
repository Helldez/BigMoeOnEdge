#!/usr/bin/env python3
"""Generate a tiny random-weight MoE gguf for the byte-identity gates.

The gates compare STREAMED-experts output against FULL-RESIDENT output of the SAME
file, so random weights are fine — quality is irrelevant, only that routing is a valid
top-k distribution (argsort of random logits) and that llama.cpp loads the model as a
MoE. The model is deliberately multi-layer with a few experts so the LRU cache path
sees real evictions on a small budget.

Two architectures are emitted, selected with --arch:

  qwen3moe  split expert layout — three tensors per layer
            (ffn_gate_exps / ffn_up_exps / ffn_down_exps).
  gemma4    fused gate+up layout — two expert tensors per layer
            (ffn_gate_up_exps / ffn_down_exps), plus a resident shared expert and an
            interleaved dense layer, so the gates cover the fused streaming path.

Requires: pip install gguf numpy

    python scripts/make-tiny-moe.py --arch qwen3moe --out tiny-moe.gguf
    python scripts/make-tiny-moe.py --arch gemma4   --out tiny-moe-gemma4.gguf
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


def add_attn_tensors(w, p, s):
    """Attention block shared by both architectures (numpy shapes are ggml dims reversed)."""
    w.add_tensor(p + "attn_q.weight",      rnd(N_EMBD_HEAD * N_HEAD, N_EMBD, seed=s + 1))
    w.add_tensor(p + "attn_k.weight",      rnd(N_EMBD_GQA, N_EMBD, seed=s + 2))
    w.add_tensor(p + "attn_v.weight",      rnd(N_EMBD_GQA, N_EMBD, seed=s + 3))
    w.add_tensor(p + "attn_output.weight", rnd(N_EMBD, N_EMBD_HEAD * N_HEAD, seed=s + 4))
    w.add_tensor(p + "attn_q_norm.weight", rnd(N_EMBD_HEAD, seed=s + 5))
    w.add_tensor(p + "attn_k_norm.weight", rnd(N_EMBD_HEAD, seed=s + 6))


# --- qwen3moe: split expert layout -------------------------------------------------
def build_qwen3moe(out):
    tokens, scores, toktypes = build_vocab()
    n_vocab = len(tokens)

    w = gguf.GGUFWriter(out, "qwen3moe")
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", choices=["qwen3moe", "gemma4"], default="qwen3moe")
    ap.add_argument("--out", default="tiny-moe.gguf")
    args = ap.parse_args()

    if args.arch == "gemma4":
        build_gemma4(args.out)
    else:
        build_qwen3moe(args.out)


if __name__ == "__main__":
    main()
