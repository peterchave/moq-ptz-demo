import { defineConfig } from 'vite';
import { resolve } from 'path';

export default defineConfig({
  server: {
    ...(process.env.VITE_HOST ? { host: process.env.VITE_HOST } : {}),
  },
  resolve: {
    alias: {
      '@moqt/transport': resolve(__dirname, './packages/transport/src/index.ts'),
      '@moqt/webtransport': resolve(__dirname, './packages/webtransport/src/index.ts'),
      '@moqt/msf': resolve(__dirname, './packages/msf/src/index.ts'),
      '@moqt/loc': resolve(__dirname, './packages/loc/src/index.ts'),
    },
  },
  build: {
    rollupOptions: {
      input: {
        main: resolve(__dirname, 'index.html'),
      },
    },
  },
});
