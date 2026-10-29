<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/logo/horizontal-ink.svg">
    <img src="docs/assets/logo/horizontal.svg" width="320" alt="BigMoeOnEdge">
  </picture>
</p>

<p align="center">
  <a href="https://github.com/Helldez/BigMoeOnEdge/releases/latest"><img src="https://img.shields.io/github/v/release/Helldez/BigMoeOnEdge" alt="Ultima release"></a>
  <a href="https://github.com/Helldez/BigMoeOnEdge/actions/workflows/ci.yml"><img src="https://github.com/Helldez/BigMoeOnEdge/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/Helldez/BigMoeOnEdge" alt="Licenza"></a>
</p>

<p align="center"><a href="README.md">English</a> · <a href="README_zh.md">中文</a> · <a href="README_ja.md">日本語</a> · <a href="README_es.md">Español</a> · <a href="README_fr.md">Français</a> · <a href="README_it.md">Italiano</a></p>

Questa è una traduzione; il README in inglese è il riferimento e potrebbe essere più aggiornato.

Esegui modelli Mixture-of-Experts più grandi della memoria del dispositivo leggendo gli esperti in streaming dalla flash.
Costruito sull'API pubblica di llama.cpp; Android, iOS, macOS, Windows e Linux; un server locale
compatibile OpenAI su ciascuno.

<table align="center">
<tr>
<td align="center" width="50%"><img src="docs/assets/screens/android.gif" width="220" alt="Qwen3.6-35B-A3B che genera una risposta su un telefono Android da 12 GB, in tempo reale."></td>
<td align="center" width="50%"><img src="docs/assets/screens/iphone.gif" width="220" alt="Qwen3.6-35B-A3B che genera una risposta su un iPhone da 12 GB, in tempo reale."></td>
</tr>
<tr>
<td align="center">Telefono Android da 12 GB</td>
<td align="center">iPhone da 12 GB</td>
</tr>
<tr>
<td align="center" colspan="2"><img src="docs/assets/screens/desktop.gif" width="620" alt="Qwen3.6-35B-A3B che genera una risposta nell'app desktop su un portatile Apple silicon da 16 GB, in tempo reale, a 20 tok/s."></td>
</tr>
<tr>
<td align="center" colspan="2">Portatile Apple silicon da 16 GB</td>
</tr>
</table>

Qwen3.6-35B-A3B in generazione su ciascun dispositivo, in tempo reale, nelle app distribuite. Il modello è più grande
della memoria di ciascun dispositivo.

## Descrizione

Un modello Mixture-of-Experts usa pochi dei suoi esperti per ogni token. BigMoeOnEdge tiene in memoria
i pesi che servono a ogni token e una cache degli esperti usati di recente, e legge gli altri esperti dalla
flash quando il router li richiede. L'output è identico a quello del modello eseguito interamente in memoria;
i test lo verificano.

- **Planner.** `--auto` misura la macchina (memoria disponibile, velocità dello storage, ogni CPU, GPU e
  NPU) e l'header del modello, poi imposta la dimensione della cache, cosa resta residente, e quale dispositivo esegue
  il prompt e quale la risposta. Dopo un primo run pianifica dal registro del routing del modello stesso
  (il server ne tiene uno per modello), e la prima risposta misura thread e overlap. Le sue regole non nominano
  alcuna piattaforma o dispositivo. Le impostazioni date a mano vengono mantenute.
- **Modelli.** Qualsiasi gguf MoE che llama.cpp carica, in qualsiasi quantizzazione. I tensori degli esperti vengono individuati dal
  file, quindi una nuova architettura non richiede codice qui.
- **Backend.** CPU, Metal, Vulkan, OpenCL, CUDA, HIP e Hexagon NPU, caricati come moduli; un modulo SYCL
  compilato con la sua toolchain si carica allo stesso modo. Su una GPU gli esperti più usati restano in cache
  nella sua memoria e gli altri arrivano in streaming dietro una cache in RAM; un modello che entra nella scheda
  gira tutto su di essa.
- **App e server.** App native su Android e iOS, una web app sul desktop, e un
  server compatibile OpenAI su tutti, utilizzabile dagli agenti di coding (Claude Code, Codex CLI, opencode).

## Prestazioni

Velocità di decodifica, misurata sui dispositivi qui sotto. Il motore si dimensiona sulla macchina, quindi un hardware
più potente va più veloce.

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/diagrams/speed-dark.svg">
    <img src="docs/assets/diagrams/speed-light.svg" width="820" alt="Velocità di decodifica di Qwen3.6-35B-A3B Q4_K_M: 18-24 tok/s su un portatile Apple silicon da 16 GB, 5 su un portatile Windows da 16 GB, 5 su un telefono Android da 12 GB, 3 su un iPhone da 12 GB.">
  </picture>
</p>

| Hardware | Memoria | Modello | Su disco | Esperti da | Decodifica tok/s |
|---|---|---|---:|---|---:|
| **Telefono Android** | 12 GB | DeepSeek V4 Flash 284B | 91 GB | flash | 1 |
|  |  | Qwen3.8-Flash-Next 125B | 80 GB | flash | 2-4 |
|  |  | Qwen3.6-35B-A3B | 21 GB | flash | 5 |
| **iPhone** | 12 GB | Qwen3.6-35B-A3B | 21 GB | flash | 3 |
| **Portatile Apple silicon** | 16 GB | Qwen3.8-Flash-Next 125B | 76 GB | flash | 7-9 |
|  |  | Qwen3.6-35B-A3B | 21 GB | flash | 18-24 |
| **Portatile Windows** | 16 GB | Qwen3.6-35B-A3B | 21 GB | flash | 5 |
| **GPU desktop** (NVIDIA) | 16 GB + 32 GB RAM | Qwen3.8-Flash-Next 125B | 66 GB | flash, cache GPU | 19-21 |
| **GPU desktop** (NVIDIA) | 24 GB + 96 GB RAM | Qwen3.8-Flash-Next 125B | 66 GB | RAM, cache GPU | 70 |
|  |  | Qwen3.6-35B-A3B | 21 GB | GPU | 217 |
| **GPU datacenter** (AMD, ROCm) | 192 GB | Qwen3.8-Flash-Next 125B | 66 GB | GPU | 53 |

Token al secondo interi; un intervallo è la variazione tra i run. "Esperti da" indica da dove
vengono letti gli esperti: in streaming dalla flash, tenuti in RAM o sulla GPU, con una cache
dei più usati nella memoria della GPU.

Configurazioni e metodo: [docs/benchmarks.md](docs/benchmarks.md). Per aggiungere una macchina, esegui
`scripts/bench-report.sh /path/to/model.gguf` e incolla la tabella in un
[benchmark report](https://github.com/Helldez/BigMoeOnEdge/issues/new?template=benchmark-report.yml).

## Avvio rapido

Tutto quanto segue viene dall'[ultima release](https://github.com/Helldez/BigMoeOnEdge/releases/latest).
Ogni pacchetto avvia il motore e apre l'app nel browser su `http://127.0.0.1:8765/`; scegli un
modello in **Models**.

- **macOS** (Apple silicon): apri `BigMoeOnEdge-<versione>-macos-arm64.dmg` e trascina l'app in
  Applicazioni. Mentre è in esecuzione resta nel Dock e nella barra dei menu; **Stop and Quit** la ferma.
  Non ancora notarizzata: il primo avvio richiede il passo descritto nel README.txt del dmg.
- **Windows**: esegui `BigMoeOnEdge-<versione>-windows-x86_64-setup.exe` (o `-windows-arm64-setup.exe`).
  Senza diritti di amministratore; avvia BigMoeOnEdge dal menu Start e chiudi la sua finestra per fermarlo.
- **Linux**: `chmod +x BigMoeOnEdge-<versione>-linux-x86_64.AppImage` (o `-linux-aarch64`) ed eseguilo.
  Avviato da un file manager si apre in una finestra di terminale; chiudila per fermarlo.
- **Android**: installa `app-dev-release.apk` dalla pagina della release; ogni release si installa
  sopra la precedente. **Settings → Serve on the network** condivide il motore del telefono, con una
  chiave.

**iOS.** Compila l'app con Xcode da [examples/ios](examples/ios/README.md). Sperimentale.

**Archivi.** L'archivio `bmoe-desktop` contiene la stessa app come cartella, per le installazioni da
script: estrailo ed esegui `bmoe-server --open`. L'archivio `bmoe-cli` contiene il motore da riga di
comando.

**Riga di comando.**

```bash
bmoe-cli -m model.gguf --auto -p "Explain MoE routing."   # measure, plan, run
bmoe-cli -m model.gguf --plan                              # print the plan and exit
```

## Server compatibile OpenAI

```python
from openai import OpenAI

client = OpenAI(base_url="http://127.0.0.1:8765/v1", api_key="local")
reply = client.chat.completions.create(
    model="local",
    messages=[{"role": "user", "content": "Write a haiku about experts on flash."}],
)
print(reply.choices[0].message.content)
```

Streaming, chiamate a strumenti e reasoning sono supportati. Per opencode, aggiungi il server come
provider compatibile OpenAI con `baseURL` impostato a `http://127.0.0.1:8765/v1`. Per servire altre macchine,
avvia il server con `--host` e `--api-key`. Riferimento API: [docs/server-api.md](docs/server-api.md).

## Come funziona

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/diagrams/token-dark.svg">
    <img src="docs/assets/diagrams/token-light.svg" width="820" alt="Un passo di decodifica: l'intero modello è sulla flash; la RAM contiene i pesi densi e una cache di esperti. Il router sceglie k esperti per layer; quelli in cache non costano nulla, gli altri vengono letti dalla flash.">
  </picture>
</p>

Il motore usa l'API pubblica di llama.cpp: ogni quantizzazione, tokenizer e chat template supportato da llama.cpp
funziona senza modifiche, e un aggiornamento di llama.cpp è un bump del submodule. Gli esperti instradati vengono letti
secondo il layout dei tensori del file ([docs/which-models-stream.md](docs/which-models-stream.md)).
`ctest` verifica che l'output in streaming sia uguale all'output interamente residente attraverso cache, evizioni,
overlap, sessioni e modelli suddivisi ([docs/architecture.md](docs/architecture.md)).

## Il planner

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/diagrams/plan-dark.svg">
    <img src="docs/assets/diagrams/plan-light.svg" width="820" alt="Cosa ha scelto --auto per Qwen3.6-35B-A3B su un portatile Apple silicon da 16 GB: 1.7 GiB di pesi densi e una cache di esperti da 7.2 GiB in RAM, 85% di hit della cache, 69.6 MiB letti dalla flash per token, il prompt sulla GPU e la decodifica su 8 thread CPU a 18 tok/s.">
  </picture>
</p>

Il planner legge la forma del modello dal suo header e misura la macchina. Le stesse regole danno a un
telefono e a un portatile piani diversi. La schermata Plan mostra il risultato: dove va la memoria, cosa fa
ciascun dispositivo, e il motivo di ogni decisione.
[docs/hardware-planning.md](docs/hardware-planning.md).

## Piattaforme

| | Android | macOS | Windows | Linux | iOS |
|---|---|---|---|---|---|
| Motore e CLI | arm64 | Apple silicon | x86_64, arm64 | x86_64, aarch64 | arm64 (nell'app) |
| App e server | app nativa | web app | web app | web app | app nativa, compilata dai sorgenti |
| Acceleratori | Hexagon NPU, Vulkan, OpenCL | Metal | Vulkan, CUDA (x86_64, download separato), OpenCL (arm64), Hexagon NPU (arm64, opt-in) | Vulkan, CUDA e HIP (x86_64; CUDA un download separato, HIP dove è installato ROCm 7) | Metal |
| Eseguito su hardware | telefono da 12 GB | portatile da 16 GB | portatile da 16 GB | CI; schede NVIDIA da 12-24 GB e AMD da 192 GB a noleggio | iPhone da 12 GB ([#235](https://github.com/Helldez/BigMoeOnEdge/issues/235)) |

Dettagli e cosa è stato misurato dove: [docs/platforms.md](docs/platforms.md).

## Compilare dai sorgenti

```bash
git clone --recursive https://github.com/Helldez/BigMoeOnEdge.git
cd BigMoeOnEdge
cd ui && npm ci && npm run build && cd ..
scripts/build-host.sh
build/bin/bmoe-server --open
```

Test: `cd build && ctest --output-on-failure` (richiede `python3` con il pacchetto `gguf`). Android:
`scripts/build-android.ps1`, poi l'APK in [`examples/android`](examples/android).
[CONTRIBUTING.md](CONTRIBUTING.md).

## Documentazione

[docs/](docs/README.md), indicizzata per attività. Inizia da [hardware-planning.md](docs/hardware-planning.md),
[settings.md](docs/settings.md), [server-api.md](docs/server-api.md),
[platforms.md](docs/platforms.md) e [limitations.md](docs/limitations.md).

## Lavori precedenti

AirLLM, *LLM in a flash* di Apple, FlexGen, PowerInfer ed EdgeMoE. Il lavoro recente più vicino è
[flash-moe](https://github.com/danveloper/flash-moe), un motore Metal che legge in streaming un MoE da 397B da SSD
su Apple silicon; BigMoeOnEdge usa l'API pubblica di llama.cpp su più architetture e piattaforme.

## Licenza

Apache-2.0. Vedi [LICENSE](LICENSE). I binari e le app includono anche llama.cpp e ggml,
cpp-httplib, nlohmann/json, le librerie JavaScript della UI web (MIT, Apache-2.0) e due font
(SIL OFL 1.1): le loro licenze sono in
[THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES), presente in ogni archivio e APK.
