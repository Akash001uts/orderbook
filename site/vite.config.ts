import { defineConfig } from 'vite'

// The artifacts live in site/data/ and are pointed at by SCHEMA.md, the CI guard,
// and both generator scripts. Rather than move them into a conventional public/
// directory and rewrite every one of those references, publicDir points at them
// where they already are. Vite then serves and copies that tree verbatim, so
// /symbols/QQQ/replay.json in a fetch is site/data/symbols/QQQ/replay.json on
// disk with nothing in between.
export default defineConfig({
  base: './',
  publicDir: 'data',
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    // The whole page is a few hundred lines of TypeScript plus uPlot. Splitting
    // that into chunks would add round trips to save nothing.
    chunkSizeWarningLimit: 800,
  },
  server: {
    port: 5180,
  },
})
