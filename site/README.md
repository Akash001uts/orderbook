# The results site

A static page that tells this project's story and displays its measured results.
No backend, no C++ in the deploy path, no live engine: it reads the committed JSON
artifacts that the tools emit, and its only interactivity is a symbol selector.

```bash
npm install
npm run dev      # http://localhost:5180
npm run build    # dist/, static, deployable anywhere
```

## Where the data comes from

`site/data/` is the artifact tree, documented field by field in
[data/SCHEMA.md](data/SCHEMA.md). Vite's `publicDir` points at it, so a path in a
fetch is a path on disk with nothing in between: `symbols/QQQ/replay.json` in the
browser is `site/data/symbols/QQQ/replay.json` in the repository. That is why the
artifacts were not moved into a conventional `public/` directory. The CI guard, the
schema document, and both generator scripts already name those paths, and one
canonical location beats a copy.

Regenerating them:

```bash
scripts/generate_ci_artifacts.sh                       # QQQ from the committed slice
scripts/generate_symbol_artifacts.sh <capture> QQQ SPY # from a full day capture
python scripts/build_index.py                          # rebuild index.json
```

The first is what CI reruns and diffs. The second needs the 7.68 GB capture, which
is gitignored and lives on one machine.

## Deploying

Both Netlify and Vercel build this static site directly from the repository on their
free tiers, which work for both private and public repositories. Nothing server side
runs and there are no environment variables.

Netlify reads `netlify.toml` at the repository root and needs no dashboard
configuration. Vercel reads `site/vercel.json`, and needs its **Root Directory**
set to `site` in the project settings; everything else follows from that file.

Only derived JSON ships. No `.itch` data enters `site/`, which is worth checking
before the first deploy:

```bash
find site -name '*.itch' -o -name '*.gz'
```

## What this page is allowed to state rather than read

The hero's `std::map` comparison is read from `data/bench/baseline.json`, the same
canonical artifact `scripts/sync_benchmarks.py` renders the BENCHMARKS.md tables
from, so the page and the document cannot disagree. Every display string and ratio
in that table is derived from the numeric fields there rather than typed out.

One table still carries numbers that are not in any artifact: the measurement
section's arena curve. It describes a sweep the site does not ship an artifact for,
it lives in `src/content.ts`, and it cites the section of BENCHMARKS.md it came
from. Everything else on the page, including the cross symbol sizing table, is read
from JSON at load time.

## Things that are the way they are for a reason

**Charts render on a microtask, not an animation frame.** A chart host has no width
until its caller attaches it, so the first render has to wait for something.
`requestAnimationFrame` and `ResizeObserver` both deliver as part of the browser's
rendering steps, and those do not run in a tab that is not being composited. A
reader who opens the link and switches away while it loads would get four blank
rectangles. A microtask runs whether or not anything is on screen.

**Every wide element scrolls inside its own box.** The page body must never scroll
sideways, because most people open a shared link on a phone.

**No component framework.** The page has no state to reconcile. It rebuilds two
panels outright when the symbol changes, and a diffing engine to redraw eight
tables would be a strange trade. uPlot is here because it is about 40 KB and built
for exactly these line charts.
