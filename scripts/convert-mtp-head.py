#!/usr/bin/env python3
"""Write a model's MTP head as a GGUF of its own, with llama.cpp's converter, matched to a model file.

    python3 scripts/convert-mtp-head.py mtp-src --like model-00001-of-00002.gguf --outfile head.gguf [--outtype q8_0]

`mtp-src` is a checkpoint directory holding the head's tensors (scripts/fetch-hf-subset.py writes one).
This runs third_party/llama.cpp/convert_hf_to_gguf.py --mtp on it unchanged, with one thing taken from
the model file the head will draft for instead of from the converter's own table: the tokenizer's
pre-tokenizer name. The converter recognises a pre-tokenizer by hashing what it does to a fixed text,
and a tokenizer newer than its table stops the conversion, though the model file next to it already
says which one it is. Reading it from there keeps the head's vocabulary the model's own.
Nothing here knows an architecture.
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LLAMA = os.path.join(HERE, "..", "third_party", "llama.cpp")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("src")
    ap.add_argument("--like", required=True, help="the model GGUF (its first shard) the head drafts for")
    ap.add_argument("--outfile", required=True)
    ap.add_argument("--outtype", default="q8_0")
    args = ap.parse_args()

    sys.path.insert(0, os.path.join(LLAMA, "gguf-py"))
    sys.path.insert(0, LLAMA)
    import gguf

    field = gguf.GGUFReader(args.like).fields.get("tokenizer.ggml.pre")
    if field is None:
        sys.exit(f"{args.like} names no pre-tokenizer (tokenizer.ggml.pre)")
    pre = bytes(field.parts[-1]).decode()

    from conversion import base
    base.TextModel.get_vocab_base_pre = lambda self, tokenizer: pre
    import convert_hf_to_gguf

    sys.argv = ["convert_hf_to_gguf.py", args.src, "--mtp", "--outtype", args.outtype, "--outfile", args.outfile]
    print(f"pre-tokenizer {pre!r}, from {args.like}", flush=True)
    convert_hf_to_gguf.main()


if __name__ == "__main__":
    main()
