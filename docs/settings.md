# Settings reference

Every streaming setting: the name every UI shows, the flag it maps to, and what it does. The names,
bounds and help come from one parameter table (`core/src/params.cpp`), which the CLI parses its
flags from and every UI renders its Settings from: each group shows its basic rows, and a folded
Advanced section at its end holds the advanced, experimental and debug ones. Experimental settings
are marked here. `bmoe-cli --help` prints the same parameters with their bounds, and
`--describe-params` prints them as JSON.

A default is the CLI's unless the row says otherwise. `bmoe-server`, which both the desktop and the
Android app run, starts from its own: the expert cache at `auto`, overlap on, the chat template on,
a 4096-token context and 1024 tokens to generate.

`--auto` sets them from the machine and the model ([hardware-planning.md](hardware-planning.md)); a
setting passed by hand is left alone.

## Streaming

| Setting | Flag and values | What it does |
|---|---|---|
| Expert cache | `--cache-mb` &nbsp;`auto`, `0` (off), or a size in MiB &nbsp;(default `auto` with streaming on) | Keeps the most-used experts in RAM. `auto` sizes it once at load from free memory; a fixed value is reproducible. Below the engine's floor it only churns. |
| Cache: ceiling | `--cache-ceil-mb` &nbsp;`0` (no cap) or a size in MiB | Caps what `auto` may claim. The OS counts our own mapped weights as free, so uncapped it can ask for more than exists. |
| Read lanes | `--io-threads` &nbsp;`1` to `8` &nbsp;(default `4`) | Reads several expert slices at once. Helps until the flash saturates, which this engine's own measurements put at two lanes on the test device. |
| Direct I/O | `--no-odirect` disables it &nbsp;(on by default) | Bypasses the OS page cache, so the system holds no second copy of what the expert cache already has. Falls back where unsupported. |
| Overlap I/O | `--overlap` &nbsp;(off in the CLI, on in `bmoe-server`) | Issues the next reads while the current layer computes, hiding flash latency behind work. Byte-identical; needs a small optional add-on to llama.cpp ([seam](seam.md)). |
| Dense weights | `--dense-weights` &nbsp;`mmap`, `warm`, `anon`, `ahwb` &nbsp;(default `anon`) | How the always-needed non-expert weights are held. Decisive far past RAM. `ahwb` puts them where the kernel cannot reclaim them at all, on the platforms that have such a store (Android, macOS) ([data](bench-data/2026-07-21-pinned-dense-ab/findings.md)). |
| Row-stream tables | `--row-stream`, with `--row-stream-mb` &nbsp;(default `64`) | Serves a dense table the graph only gathers rows from, typically the token embedding, out of flash instead of RAM: only the rows about to be read are pulled in, inside a bounded window. Which tables qualify is read off the model's own graph. Lossless ([detail](row-gathered-tables.md)). |
| Release mapping | `--release-mmap` &nbsp;(off by default) | Unmaps the gguf once every weight has been rebound into the engine's own memory, and reopens the read lanes. On Windows a live mapping serialises the streamer's unbuffered reads, so this is worth +46% decode there; on Android it saves CPU instead. Needs `--dense-weights anon` or `ahwb`, and the engine declines if any weight still points into the mapping. Lossless ([data](bench-data/2026-08-29-mmap-serialisation/findings.md)). |
| Temporal prefetch | `--prefetch` &nbsp;`0` (off) to `8` layers | Bets a layer reuses the previous token's experts and fetches them on idle lanes. Needs the cache. |
| Predictive prefetch *(experimental)* | `--predict-prefetch`, with `--predict-spec-max` &nbsp;`0` (retention only) to `8` | Runs the next layer's own router early and fetches what it names. More accurate than the bet above; reading ahead on it still lost its on-device A/B ([why](expert-prediction.md)). |

## Speed and quality

| Setting | Flag and values | What it does |
|---|---|---|
| Drop cold experts | `--drop-cold-experts` &nbsp;`0` (off) to `1.0` | Skips a routed expert only when it is a cache miss *and* the router wanted it less than that share of an even split. Quality is spent only where it buys a read. Lossy and not reproducible: what is skipped depends on what the cache held. |
| Expert substitution *(experimental)* | `--expert-substitute` &nbsp;`0` (off) to `1.0` | Nudges each routing toward experts already in the cache: a resident expert takes a slot only when the router scored it within that margin of the one it displaces. Same number of experts, fewer reads. Lossy and cache-dependent, like dropping ([detail](cache-aware-substitution.md)). |
| Active experts | `--n-expert-used` &nbsp;`0` (the model's own), or any smaller count | Consults fewer experts per token than the model asks for, cutting compute and reads together. Lossy, but reproducible: the same prompt gives the same answer. |
| Speculative drafts | `--mtp` or `--ngram`, with `--draft` &nbsp;`1` to `8` &nbsp;and, for the head, `--mtp-p-min` &nbsp;`0` to `1.0` | Drafts the next few tokens and verifies the group in one decode, keeping only what the model itself would have produced. Nothing is approximated. Wins when weights move once per group, loses when the wider verify widens each layer's read set ([mtp](mtp.md), [ngram](ngram.md)). |
| Route ahead *(experimental)* | `--route-ahead` &nbsp;`0` (off) to `8` layers | Commits a layer's routing that many layers early, so its reads start early and can never be wasted. Lossy: some slots route differently. Excludes both prefetchers and speculative drafts ([detail](route-ahead.md)). |

Everything under Streaming changes *how* weights are fetched, never the math, so the answer is the
one the model would have given anyway. The settings in this section are different in kind: they
change *what* the model computes, and they are here because on a device far past its memory the
alternative is often not a slower answer but no answer at all.

They are not interchangeable. Reducing the active experts cuts the routing tail whether or not
those experts were already free to run from memory, and it does so identically every time. Dropping
cold experts spends quality only where it buys back a flash read, which is more surgical but makes
the answer depend on what the cache happened to hold, so the same prompt can come out differently.
Expert substitution is dropping's upstream sibling: instead of deciding whether to pay for a
missing expert it decides whether to need one, and it keeps the routing's width.
Speculative drafts give up nothing at all: it changes how many tokens a pass confirms, not what the
model computes.

Each one is measured rather than assumed, and the numbers live with the method that produced them:
[expert-dropping.md](expert-dropping.md),
[cache-aware-substitution.md](cache-aware-substitution.md), [mtp.md](mtp.md),
[ngram.md](ngram.md), [route-ahead.md](route-ahead.md). Judge any of them on your own
task before relying on it.
