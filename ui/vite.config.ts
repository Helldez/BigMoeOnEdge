import { defineConfig } from 'vitest/config';
import { svelte } from '@sveltejs/vite-plugin-svelte';

// The engine server this UI talks to during development (`bmoe-server --port 8765`).
const server = 'http://127.0.0.1:8765';

export default defineConfig({
  plugins: [svelte()],
  // Relative asset paths: the server serves `dist/` as plain static files from any mount point.
  base: './',
  build: {
    outDir: 'dist',
    emptyOutDir: true,
  },
  // The server refuses state-changing requests from any origin but its own (a page on another
  // site must not drive the engine), so the dev proxy presents the server's origin.
  server: {
    proxy: {
      '/api': { target: server, changeOrigin: true, headers: { origin: server } },
      '/v1': { target: server, changeOrigin: true, headers: { origin: server } },
    },
  },
  test: {
    include: ['src/**/*.test.ts'],
    environment: 'node',
  },
});
