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

<table align="center">
<tr>
<td align="center" width="50%"><img src="docs/assets/screens/android.gif" width="220" alt="12 GB の Android スマートフォンで Qwen3.6-35B-A3B がリアルタイムに回答を生成している様子。"></td>
<td align="center" width="50%"><img src="docs/assets/screens/iphone.gif" width="220" alt="12 GB の iPhone で Qwen3.6-35B-A3B がリアルタイムに回答を生成している様子。"></td>
</tr>
<tr>
<td align="center">12 GB Android スマートフォン</td>
<td align="center">12 GB iPhone</td>
</tr>
<tr>
<td align="center" colspan="2"><img src="docs/assets/screens/desktop.gif" width="620" alt="16 GB の Apple silicon ノート PC 上のデスクトップアプリで、Qwen3.6-35B-A3B が 20 tok/s でリアルタイムに回答を生成している様子。"></td>
</tr>
<tr>
<td align="center" colspan="2">16 GB Apple silicon ノート PC</td>
</tr>
</table>

各デバイスで、配布しているアプリ上で Qwen3.6-35B-A3B がリアルタイムに生成している様子です。モデルはいずれのデバイスのメモリよりも大きいサイズです。

## 概要

Mixture-of-Experts モデルは、各トークンでエキスパートのうち少数のみを使用します。BigMoeOnEdge は、すべてのトークンで必要な重みと、最近使用したエキスパートのキャッシュをメモリに保持し、それ以外のエキスパートはルーターが要求したときにフラッシュから読み込みます。出力はモデルを完全にメモリ上で実行した場合と同一であり、テストでそれを確認しています。

- **プランナー。** `--auto` はマシン（利用可能なメモリ、ストレージ速度、各 CPU・GPU・NPU）とモデルのヘッダーを計測し、キャッシュサイズ、常駐させるもの、そしてプロンプトと回答をそれぞれどのデバイスで処理するかを設定します。初回の実行後は、そのモデル自身のルーティング記録（サーバーがモデルごとに保持）から計画し、最初の回答でスレッド数とオーバーラップを計測します。そのルールは特定のプラットフォームやデバイスを名指ししません。手動で指定した設定は維持されます。
- **モデル。** llama.cpp が読み込める任意の MoE gguf を、任意の量子化で利用できます。エキスパートテンソルはファイルから検出されるため、新しいアーキテクチャにこちら側のコードは不要です。
- **バックエンド。** CPU、Metal、Vulkan、OpenCL、CUDA、HIP、Hexagon NPU をモジュールとして読み込みます。専用のツールチェーンでビルドした SYCL モジュールも同じ方法で読み込めます。GPU では、よく使われるエキスパートを GPU メモリにキャッシュし、残りは RAM キャッシュの背後でストリーミングします。カードに収まるモデルは、すべてカード上で実行されます。
- **アプリとサーバー。** Android と iOS ではネイティブアプリ、デスクトップでは Web アプリ、そしてそのすべてで OpenAI 互換サーバーを提供し、コーディングエージェント（Claude Code、Codex CLI、opencode）から利用できます。

## パフォーマンス

以下のデバイスで計測したデコード速度です。エンジンはマシンに合わせて自身の規模を調整するため、より強力なハードウェアほど高速に動作します。

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/diagrams/speed-dark.svg">
    <img src="docs/assets/diagrams/speed-light.svg" width="820" alt="Qwen3.6-35B-A3B Q4_K_M のデコード速度：16 GB Apple silicon ノート PC で 18-24 tok/s、16 GB Windows ノート PC で 5、12 GB Android スマートフォンで 5、12 GB iPhone で 3。">
  </picture>
</p>

| ハードウェア | メモリ | モデル | ディスク上のサイズ | エキスパートの読み出し元 | デコード tok/s |
|---|---|---|---:|---|---:|
| **Android スマートフォン** | 12 GB | DeepSeek V4 Flash 284B | 91 GB | フラッシュ | 1 |
|  |  | Qwen3.8-Flash-Next 125B | 80 GB | フラッシュ | 2-4 |
|  |  | Qwen3.6-35B-A3B | 21 GB | フラッシュ | 5 |
| **iPhone** | 12 GB | Qwen3.6-35B-A3B | 21 GB | フラッシュ | 3 |
| **Apple silicon ノート PC** | 16 GB | Qwen3.8-Flash-Next 125B | 76 GB | フラッシュ | 7-9 |
|  |  | Qwen3.6-35B-A3B | 21 GB | フラッシュ | 18-24 |
| **Windows ノート PC** | 16 GB | Qwen3.6-35B-A3B | 21 GB | フラッシュ | 5 |
| **デスクトップ GPU**（NVIDIA） | 16 GB + 32 GB RAM | Qwen3.8-Flash-Next 125B | 66 GB | フラッシュ、GPU キャッシュ | 19-21 |
| **デスクトップ GPU**（NVIDIA） | 24 GB + 96 GB RAM | Qwen3.8-Flash-Next 125B | 66 GB | RAM、GPU キャッシュ | 70 |
|  |  | Qwen3.6-35B-A3B | 21 GB | GPU | 217 |
| **データセンター GPU**（AMD、ROCm） | 192 GB | Qwen3.8-Flash-Next 125B | 66 GB | GPU | 53 |

tok/s は整数で示し、範囲は複数回の実行でのばらつきです。「エキスパートの読み出し元」は、ルーティングされた
エキスパートをどこから読むかを示します：フラッシュからのストリーミング、RAM 上、または GPU 上で、よく使われる
ものは GPU メモリにキャッシュされます。

構成と計測方法：[docs/benchmarks.md](docs/benchmarks.md)。マシンを追加するには、
`scripts/bench-report.sh /path/to/model.gguf` を実行し、その表を
[ベンチマークレポート](https://github.com/Helldez/BigMoeOnEdge/issues/new?template=benchmark-report.yml)に貼り付けてください。

## クイックスタート

以下はすべて[最新リリース](https://github.com/Helldez/BigMoeOnEdge/releases/latest)から入手できます。
各パッケージはエンジンを起動し、ブラウザで `http://127.0.0.1:8765/` のアプリを開きます。
**Models** でモデルを選択します。

- **macOS**（Apple silicon）：`BigMoeOnEdge-<バージョン>-macos-arm64.dmg` を開き、アプリを
  アプリケーションにドラッグします。実行中は Dock とメニューバーに表示され、**Stop and Quit** で停止します。
  まだ公証されていないため、初回起動には dmg 内の README.txt の手順が必要です。
- **Windows**：`BigMoeOnEdge-<バージョン>-windows-x86_64-setup.exe`（または `-windows-arm64-setup.exe`）
  を実行します。管理者権限は不要です。スタートメニューから BigMoeOnEdge を起動し、ウィンドウを閉じると停止します。
- **Linux**：`chmod +x BigMoeOnEdge-<バージョン>-linux-x86_64.AppImage`（または `-linux-aarch64`）
  して実行します。ファイルマネージャーから起動するとターミナルウィンドウで開き、閉じると停止します。
- **Android**：リリースページから `app-dev-release.apk` をインストールします。各リリースは前のバージョンの上に
  インストールされます。**Settings → Serve on the network** で、キー付きでスマートフォンのエンジンを共有できます。

**iOS。** [examples/ios](examples/ios/README.md) から Xcode でアプリをビルドします。実験的な対応です。

**アーカイブ。** `bmoe-desktop` アーカイブには同じアプリがフォルダーとして入っており、スクリプトによる
インストール向けです。展開して `bmoe-server --open` を実行します。`bmoe-cli` アーカイブには
コマンドライン版のエンジンが入っています。

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
    <img src="docs/assets/diagrams/plan-light.svg" width="820" alt="16 GB Apple silicon ノート PC で --auto が Qwen3.6-35B-A3B に選んだ構成：RAM に 1.7 GiB の密な重みと 7.2 GiB のエキスパートキャッシュ、キャッシュヒット率 85%、トークンあたりフラッシュから 69.6 MiB を読み込み、プロンプトは GPU で処理、デコードは 8 つの CPU スレッドで 18 tok/s。">
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
| アクセラレーター | Hexagon NPU, Vulkan, OpenCL | Metal | Vulkan, CUDA（x86_64、別途ダウンロード）, OpenCL (arm64), Hexagon NPU (arm64, opt-in) | Vulkan, CUDA, HIP（x86_64。CUDA は別途ダウンロード、HIP は ROCm 7 がインストールされている環境で） | Metal |
| 実機での動作 | 12 GB スマートフォン | 16 GB ノート PC | 16 GB ノート PC | CI、レンタルした 12-24 GB の NVIDIA カードと 192 GB の AMD カード | 12 GB iPhone（[#235](https://github.com/Helldez/BigMoeOnEdge/issues/235)） |

詳細と、どこで何を計測したか：[docs/platforms.md](docs/platforms.md)。

## 他のエンジンとの比較

| | 動作環境 | モデル | RAM を超えるエキスパート | 自動設定 |
|---|---|---|---|---|
| **BigMoeOnEdge** | Android、iOS、macOS、Windows、Linux；CPU、Metal、CUDA、HIP、Vulkan、OpenCL、Hexagon NPU | llama.cpp が読める任意の MoE GGUF | フラッシュからストリーミング（RAM キャッシュと GPU キャッシュ付き） | **`--auto`：マシンとモデルを計測し、すべての設定を決め、理由を説明** |
| llama.cpp | Android、iOS、macOS、Windows、Linux | 任意の GGUF | OS のページキャッシュ | `--fit` がレイヤーを配置 |
| Strata | Windows、Linux、macOS；NVIDIA、AMD、Intel Arc、Apple silicon | 3 つのモデルファミリー | DeepSeek V4.1 のみ SSD から | メモリに応じたモデル選択 |
| ds4 | macOS、Linux；Metal、CUDA、ROCm | DeepSeek V4/V4.1、GLM 5.x、Qwen3.8（独自 GGUF） | SSD から | エキスパートキャッシュの予算 |
| colibri | Linux、macOS、Windows；CPU、Vulkan、CUDA、Metal | 独自形式（ファミリーごと） | ディスクから | `coli plan`、`coli tune` |
| KTransformers | Linux、Windows；NVIDIA、AMD、Intel Arc、Ascend | 対応ファミリー | RAM に保持 | 記載なし |

モデルがゲーミング GPU と RAM の合計に収まる場合、Strata とその Blackwell 向けフォークはこのエンジンより
デコードが 2〜4 倍速く、長いプロンプトのプリフィルも数倍速い。フラッシュからストリーミングする場合は、
どのエンジンも毎秒 10 トークン未満で、速度を決めるのはドライブ。各プロジェクトが公表している速度と出典：
[docs/comparison.md](docs/comparison.md)。

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
