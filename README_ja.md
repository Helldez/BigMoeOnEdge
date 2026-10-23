<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/logo/horizontal-ink.svg">
    <img src="docs/assets/logo/horizontal.svg" width="320" alt="BigMoeOnEdge">
  </picture>
</p>

<p align="center">
  <a href="https://github.com/Helldez/BigMoeOnEdge/releases/latest"><img src="https://img.shields.io/github/v/release/Helldez/BigMoeOnEdge" alt="最新リリース"></a>
  <a href="https://github.com/Helldez/BigMoeOnEdge/actions/workflows/ci.yml"><img src="https://github.com/Helldez/BigMoeOnEdge/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/Helldez/BigMoeOnEdge" alt="ライセンス"></a>
</p>
<p align="center"><a href="README.md">English</a> · <a href="README_zh.md">中文</a> · <a href="README_ja.md">日本語</a> · <a href="README_es.md">Español</a> · <a href="README_fr.md">Français</a> · <a href="README_it.md">Italiano</a></p>

> これは翻訳です。英語版 README が正であり、そちらの方が新しい場合があります。

エキスパートをフラッシュストレージからストリーミングすることで、デバイスのメモリより大きな Mixture-of-Experts モデルを実行します。
llama.cpp の公開 API の上に構築されており、Android、iOS、macOS、Windows、Linux に対応し、それぞれでローカルの OpenAI 互換サーバーを提供します。

<table>
<tr>
<td align="center" width="50%"><img src="docs/assets/screens/android.gif" width="220" alt="12 GB の Android スマートフォンで Qwen3.6-35B-A3B がリアルタイムに回答を生成している様子。"></td>
<td align="center" width="50%"><img src="docs/assets/screens/iphone.gif" width="220" alt="12 GB の iPhone で Qwen3.6-35B-A3B がリアルタイムに回答を生成している様子。"></td>
</tr>
<tr>
<td align="center">12 GB Android スマートフォン</td>
<td align="center">12 GB iPhone</td>
</tr>
<tr>
<td align="center" colspan="2"><img src="docs/assets/screens/desktop.gif" width="620" alt="16 GB の Apple silicon ノート PC 上のデスクトップアプリで、Qwen3.6-35B-A3B が 19.9 tok/s でリアルタイムに回答を生成している様子。"></td>
</tr>
<tr>
<td align="center" colspan="2">16 GB Apple silicon ノート PC</td>
</tr>
</table>

各デバイスで、配布しているアプリ上で Qwen3.6-35B-A3B がリアルタイムに生成している様子です。モデルはいずれのデバイスのメモリよりも大きいサイズです。

## 概要

Mixture-of-Experts モデルは、各トークンでエキスパートのうち少数のみを使用します。BigMoeOnEdge は、すべてのトークンで必要な重みと、最近使用したエキスパートのキャッシュをメモリに保持し、それ以外のエキスパートはルーターが要求したときにフラッシュから読み込みます。出力はモデルを完全にメモリ上で実行した場合と同一であり、テストでそれを確認しています。

- **プランナー。** `--auto` はマシン（利用可能なメモリ、ストレージ速度、各 CPU・GPU・NPU）とモデルのヘッダーを計測し、キャッシュサイズ、常駐させるもの、そしてプロンプトと回答をそれぞれどのデバイスで処理するかを設定します。そのルールは特定のプラットフォームやデバイスを名指ししません。手動で指定した設定は維持されます。
- **モデル。** llama.cpp が読み込める任意の MoE gguf を、任意の量子化で利用できます。エキスパートテンソルはファイルから検出されるため、新しいアーキテクチャにこちら側のコードは不要です。
- **バックエンド。** CPU、Metal、Vulkan、OpenCL、Hexagon NPU をモジュールとして読み込みます。それぞれのツールチェーンでビルドした CUDA、HIP、SYCL モジュールも同じ方法で読み込めます。
- **アプリとサーバー。** Android と iOS ではネイティブアプリ、デスクトップでは Web アプリ、そしてそのすべてで OpenAI 互換サーバーを提供します。

## パフォーマンス

以下のデバイスで計測したデコード速度です。エンジンはマシンに合わせて自身の規模を調整するため、より強力なハードウェアほど高速に動作します。

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/diagrams/speed-dark.svg">
    <img src="docs/assets/diagrams/speed-light.svg" width="820" alt="Qwen3.6-35B-A3B Q4_K_M のデコード速度：16 GB Apple silicon ノート PC で 18-24 tok/s、16 GB Windows ノート PC で 4.8、12 GB Android スマートフォンで 4.8-5.0、12 GB iPhone で 2.6-3.0。">
  </picture>
</p>

| モデル | ディスク上のサイズ | デバイス | モデル / RAM | デコード |
|---|---:|---|---:|---:|
| DeepSeek V4 Flash 284B | 91 GB | 12 GB Android スマートフォン | 7.6× | 0.94 tok/s |
| Qwen3.8-Flash-Next 125B | 80 GB | 12 GB Android スマートフォン | 6.7× | 2.2-3.5 tok/s |
| gpt-oss-120b | 60 GB | 12 GB Android スマートフォン | 5× | 1.3 tok/s |
| Qwen3.6-35B-A3B | 21 GB | 12 GB Android スマートフォン | 1.9× | 4.8-5.0 tok/s |
| Qwen3.6-35B-A3B | 21 GB | 12 GB iPhone | 1.9× | 2.6-3.0 tok/s |
| Qwen3-30B-A3B | 18.5 GB | 12 GB Android スマートフォン | 1.5× | 5.2 tok/s |
| Gemma-4-26B-A4B | 17 GB | 12 GB Android スマートフォン | 1.4× | 4.1 tok/s |
| Qwen3.8-Flash-Next 125B | 76 GB | 16 GB Apple silicon ノート PC | 4.8× | 6.5-9 tok/s |
| Qwen3.6-35B-A3B | 21 GB | 16 GB Apple silicon ノート PC | 1.3× | 18-24 tok/s |
| Qwen3-Coder-30B-A3B | 18.6 GB | 16 GB Apple silicon ノート PC | 1.2× | 23 tok/s、opencode 内では 7-9 |
| Qwen3.6-35B-A3B | 21 GB | 16 GB Windows ノート PC | 1.3× | 4.8 tok/s |

構成と計測方法：[docs/benchmarks.md](docs/benchmarks.md)。マシンを追加するには、
`scripts/bench-report.sh /path/to/model.gguf` を実行し、その表を
[ベンチマークレポート](https://github.com/Helldez/BigMoeOnEdge/issues/new?template=benchmark-report.yml)に貼り付けてください。

## クイックスタート

**Android。** [最新リリース](https://github.com/Helldez/BigMoeOnEdge/releases/latest)から APK をインストールし、
**Models** でモデルを選択します。**Settings → Serve on the network** で、キー付きでモデルを共有できます。

**iOS。** [examples/ios](examples/ios/README.md) から Xcode でアプリをビルドします。実験的な対応です。

**デスクトップ（macOS、Windows、Linux）。** 同じリリースから `bmoe-desktop` アーカイブをダウンロードし、
`bmoe-server --open` を実行します。アプリが `http://127.0.0.1:8765/` で開きます。

**コマンドライン。**

```bash
bmoe-cli -m model.gguf --auto -p "Explain MoE routing."   # measure, plan, run
bmoe-cli -m model.gguf --plan                              # print the plan and exit
```

## OpenAI 互換サーバー

```python
from openai import OpenAI

client = OpenAI(base_url="http://127.0.0.1:8765/v1", api_key="local")
reply = client.chat.completions.create(
    model="local",
    messages=[{"role": "user", "content": "Write a haiku about experts on flash."}],
)
print(reply.choices[0].message.content)
```

ストリーミング、ツール呼び出し、推論（reasoning）に対応しています。opencode では、`baseURL` を
`http://127.0.0.1:8765/v1` に設定して、このサーバーを OpenAI 互換プロバイダーとして追加します。
他のマシンに提供するには、`--host` と `--api-key` を指定してサーバーを起動します。
API リファレンス：[docs/server-api.md](docs/server-api.md)。

## 仕組み

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/diagrams/token-dark.svg">
    <img src="docs/assets/diagrams/token-light.svg" width="820" alt="1 回のデコードステップ：モデル全体はフラッシュ上にあり、RAM には密な重みとエキスパートキャッシュが保持されます。ルーターは各層で k 個のエキスパートを選択し、キャッシュ済みのものはコストがかからず、それ以外はフラッシュから読み込まれます。">
  </picture>
</p>

エンジンは llama.cpp の公開 API を使用しています。llama.cpp が対応するすべての量子化、トークナイザー、
チャットテンプレートがそのまま動作し、llama.cpp のアップグレードはサブモジュールの更新だけで済みます。
ルーティングされるエキスパートは、ファイルのテンソルレイアウトに従って読み込まれます
（[docs/which-models-stream.md](docs/which-models-stream.md)）。
`ctest` は、キャッシュ、退避、オーバーラップ、セッション、分割モデルにわたって、ストリーミング時の出力が
完全常駐時の出力と等しいことを確認します（[docs/architecture.md](docs/architecture.md)）。

## プランナー

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/diagrams/plan-dark.svg">
    <img src="docs/assets/diagrams/plan-light.svg" width="820" alt="16 GB Apple silicon ノート PC で --auto が Qwen3.6-35B-A3B に選んだ構成：RAM に 1.7 GiB の密な重みと 7.2 GiB のエキスパートキャッシュ、キャッシュヒット率 85%、トークンあたりフラッシュから 69.6 MiB を読み込み、プロンプトは GPU で処理、デコードは 8 つの CPU スレッドで 17.6 tok/s。">
  </picture>
</p>

プランナーはヘッダーからモデルの構造を読み取り、マシンを計測します。同じルールから、スマートフォンと
ノート PC には異なるプランが導かれます。Plan 画面には結果が表示されます。メモリの割り当て先、
各デバイスの役割、そして各判断の理由です。
[docs/hardware-planning.md](docs/hardware-planning.md)。

## プラットフォーム

| | Android | macOS | Windows | Linux | iOS |
|---|---|---|---|---|---|
| エンジンと CLI | arm64 | Apple silicon | x86_64, arm64 | x86_64, aarch64 | arm64（アプリ内） |
| アプリとサーバー | ネイティブアプリ | Web アプリ | Web アプリ | Web アプリ | ネイティブアプリ（ソースからビルド） |
| アクセラレーター | Hexagon NPU, Vulkan, OpenCL | Metal | Vulkan, OpenCL (arm64) | Vulkan | Metal |
| 実機での動作 | 12 GB スマートフォン | 16 GB ノート PC | 16 GB ノート PC | CI、コミュニティ | 12 GB iPhone（[#235](https://github.com/Helldez/BigMoeOnEdge/issues/235)） |

詳細と、どこで何を計測したか：[docs/platforms.md](docs/platforms.md)。

## ソースからのビルド

```bash
git clone --recursive https://github.com/Helldez/BigMoeOnEdge.git
cd BigMoeOnEdge
cd ui && npm ci && npm run build && cd ..
scripts/build-host.sh
build/bin/bmoe-server --open
```

テスト：`cd build && ctest --output-on-failure`（`gguf` パッケージを入れた `python3` が必要）。Android：
`scripts/build-android.ps1` の後、[`examples/android`](examples/android) で APK をビルドします。
[CONTRIBUTING.md](CONTRIBUTING.md)。

## ドキュメント

[docs/](docs/README.md) はタスク別に索引付けされています。まずは [hardware-planning.md](docs/hardware-planning.md)、
[settings.md](docs/settings.md)、[server-api.md](docs/server-api.md)、
[platforms.md](docs/platforms.md)、[limitations.md](docs/limitations.md) から始めてください。

## 先行研究

AirLLM、Apple の *LLM in a flash*、FlexGen、PowerInfer、EdgeMoE。最も近い最近の取り組みは
[flash-moe](https://github.com/danveloper/flash-moe) で、Apple silicon 上で 397B の MoE を SSD からストリーミングする
Metal エンジンです。BigMoeOnEdge は、アーキテクチャやプラットフォームを問わず llama.cpp の公開 API を使用します。

## ライセンス

Apache-2.0。[LICENSE](LICENSE) を参照してください。バイナリとアプリには llama.cpp と ggml、
cpp-httplib、nlohmann/json、Web UI の JavaScript ライブラリ(MIT、Apache-2.0)と
2 つのフォント(SIL OFL 1.1)も含まれます。
それらのライセンスは [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES) にあり、すべてのアーカイブと APK に同梱されています。
