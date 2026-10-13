# BigMoeOnEdge web UI

The browser front end for `bmoe-server`: chat with live engine metrics, model management
(local files, catalog downloads, load and unload), settings and the hardware plan. It talks only
to the HTTP API described in [`docs/server-api.md`](../docs/server-api.md).

Svelte 5 + TypeScript + Vite, no UI framework. Pure logic lives in `src/lib/` with unit tests,
app state in `src/stores/`, views in `src/routes/` and `src/components/`.

## Develop

```bash
npm install
npm run dev
```

The dev server proxies `/api` and `/v1` to `http://127.0.0.1:8765`, presenting the server's own
origin (the server refuses state-changing requests from any other), so start the engine first:

```bash
bmoe-server --port 8765
```

Other scripts:

```bash
npm run check   # svelte-check (types and Svelte diagnostics)
npm test        # vitest unit tests for src/lib
npm run build   # production build
```

## Build output

`npm run build` writes a static site to `ui/dist/` (`index.html` plus hashed assets, with
relative paths). The CMake build copies it beside `bmoe-server` as `ui/` (see
`server/stage_assets.cmake`), and the server serves it as plain files. There is no SPA fallback,
so routes are hash based (`#/chat`, `#/models`, `#/settings`, `#/plan`).

## Settings are schema driven

The settings form is rendered entirely from `GET /api/params`: groups, order, labels, help,
types, bounds, choices, levels, scopes and defaults all come from the server. Do not list engine
parameters in the UI. A parameter added to the engine's table appears here with no UI change.
The only keys the UI names are `model`, `n-predict` and `think` (chat composer and model
loading) and `moe-stream` (the streaming badge).
