# BigMoeOnEdge web UI

The browser front end for `bmoe-server`, and the page the desktop app shows: conversations with
the agent (folders, modes, approvals, a preview of the files it writes), skills, scheduled tasks,
connecting other apps, model management, the hardware plan, settings and live engine metrics. It
talks only to the HTTP API described in [`docs/server-api.md`](../docs/server-api.md) and
[`docs/agent.md`](../docs/agent.md), and uses the desktop app's bridge
([`docs/desktop-app.md`](../docs/desktop-app.md)) where it runs inside the app. How it looks is
[`docs/design.md`](../docs/design.md); what each screen does is [`docs/screens.md`](../docs/screens.md).

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

Without an engine (no model, no server), a mock answers every route with made-up data and plays a
short agent run on each message: `mock/server.ts`.

```bash
npm run dev:mock
npx vite build && npx vite preview --mode mock   # the build, under the server's CSP
```

Other scripts:

```bash
npm run check   # svelte-check (types and Svelte diagnostics)
npm test        # vitest: src/lib, the palette's contrast, the dictionaries' completeness
npm run build   # production build
```

## Build output

`npm run build` writes a static site to `ui/dist/` (`index.html` plus hashed assets, with
relative paths). The CMake build copies it beside `bmoe-server` as `ui/` (see
`server/stage_assets.cmake`), and the server serves it as plain files. There is no SPA fallback,
so routes are hash based (`#/chat/<conversation>`, `#/models`, `#/plan`, `#/skills`, `#/tasks`,
`#/connect`, `#/settings/<section or parameter>`, `#/metrics`, `#/setup`).

## Words and languages

Every word is a key of `src/lib/i18n/en.ts` (English, the key language), translated in the other
files there (`it.ts`). A test fails when a language misses a key, drops a placeholder, or the code
names a key English does not have. The language follows the browser unless Settings picks one.

## Settings are schema driven

The settings form is rendered entirely from `GET /api/params`: groups, order, labels, help,
types, bounds, choices, levels, scopes and defaults all come from the server. Do not list engine
parameters in the UI. A parameter added to the engine's table appears here with no UI change.
The only keys the UI names are `model` (model loading) and `think` (the composer's reasoning
switch, and its default from the config).
