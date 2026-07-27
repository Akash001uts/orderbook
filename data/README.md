# Market data

## `qqq_slice.itch`

A single symbol slice of a real NASDAQ TotalView-ITCH 5.0 capture, committed so
that the repository has a real-data regression test that runs on every push with no
download step.

| | |
| --- | --- |
| Source | `12302019.NASDAQ_ITCH50.gz`, NASDAQ's public sample archive |
| Trading date | 2019-12-30 |
| Symbol | QQQ, stock locate 6556 |
| Extracted from | the first 400 MB of the decompressed stream |
| Messages | 183 954 |
| Size | 5 533 732 bytes |
| SHA-256 | `0ad09797f67716f7a6316c9d188c9406b66ee0ea7f7ea46dbf2627631b629588` |

It is real ITCH 5.0 binary, not a re-encoding. The slice keeps the system event
messages, the one stock directory entry naming QQQ, and every message carrying
QQQ's locate code, and drops everything else. Framing and message bytes are
untouched, so the parser reads it through exactly the same code path as the full
multi-gigabyte capture, and a replay of the slice reproduces a replay of the full
file exactly: identical message counts, identical final book, identical best bid
and ask.

Produced by:

```bash
itch_replay --symbol QQQ --extract data/qqq_slice.itch <full-capture>
```

**Provenance and redistribution.** NASDAQ publishes these sample files publicly at
`emi.nasdaq.com` with no login. This slice is a small derived excerpt committed for
automated testing. Confirm NASDAQ's current terms before the repository is made
public, since redistribution of exchange data is theirs to permit and not something
this project should assume.

## The full dataset

The complete capture is published as **GitHub release assets**, not committed to
the repository. Both exist on purpose and neither makes the other redundant: the
slice above is what makes `ctest` work on a fresh clone with no download, and the
release is the complete dataset behind every finding in DESIGN.md.

### Why not commit it, and why compressing harder does not help

Git rejects any file over 100 MB at push time, and that limit is not negotiable by
compressing better. The ratios below were measured on a 300 MB sample of the real
multi-symbol stream, which is the figure that matters: an earlier estimate
extrapolated from the single symbol slice and came out too optimistic, because one
symbol's traffic is far more repetitive than the whole feed.

| | Ratio on real multi-symbol ITCH |
| --- | --- |
| gzip -9, what NASDAQ ships | 2.34x |
| xz -9 | 3.91x |

So the 2019-12-30 capture is 7.68 GB decompressed, 3.28 GB as NASDAQ ships it, and
roughly 2.0 GB under xz. That is a 40 percent saving over gzip and still 20 times
over the 100 MB per-file limit. Git LFS does not close it either: the free tier is
1 GB of storage and 1 GB of monthly bandwidth.

Splitting the archive into sub-100 MB chunks would technically pass the check and
is the wrong answer anyway. It would put 2 GB into the git history permanently,
so every clone of a source repository would drag it down forever, and GitHub's own
guidance is explicit that repositories are not for bulk data.

### What release assets give instead

A release asset can be 2 GB, it appears on the repository page, and it is not part
of the git history, so a clone stays at a few megabytes. `scripts/publish_dataset.sh`
recompresses with xz, splits below the asset limit, checksums every part, and
uploads them with a manifest describing how to reassemble:

```bash
scripts/publish_dataset.sh data/12302019.NASDAQ_ITCH50
```

Run it with `PUBLISH_DRY_RUN=1` first to see the parts and checksums without
uploading anything.

### Or fetch it from the source

`scripts/fetch_nasdaq_sample.sh` downloads a full day straight from NASDAQ, which
is the authoritative copy and needs no intermediary:

```bash
scripts/fetch_nasdaq_sample.sh 12302019
scripts/fetch_nasdaq_sample.sh 12302019 data --prefix-only   # first 1 GiB only
```

`itch_replay --survey` is what to point at a full capture first. On the 2019-12-30
sample it reports 8 906 symbols and zero unknown message types.

## Before the repository goes public: the two options

**This is a decision for the repository owner, not a technical blocker, and
nothing below is legal advice.** Both options are fully implemented and either can
be taken in minutes. What follows is what each costs.

The situation: NASDAQ publishes these sample files at `emi.nasdaq.com` with no
login and no click-through agreement. Committing a 5.5 MB derived excerpt for
automated testing and republishing a complete 2 GB archive are different asks, and
the fact that the source is public does not by itself settle either.

### Option A: keep the committed slice

Leave `data/qqq_slice.itch` in the repository, and do not publish the full archive
as release assets.

| | |
| --- | --- |
| What ships | A 5.5 MB single-symbol excerpt, one symbol on one day |
| What it buys | `ctest` works on a fresh clone with no download. Two tests in `test_itch.cpp` assert against it, and the strategy backtest and latency harness both use it as their default input |
| The argument for | It is a small derived excerpt used for verification, not a substitute for the source. A reader cannot reconstruct the feed from one symbol's messages. This is the ordinary shape of a test fixture |
| The argument against | It is still exchange data being redistributed, and permission to redistribute is NASDAQ's to give rather than something a project should assume from public hosting |
| If it turns out to be wrong | Delete one file, regenerate the fixture on demand. Recoverable |

### Option B: ship no market data

Delete `data/qqq_slice.itch` and rely on `scripts/fetch_nasdaq_sample.sh`, which
downloads from NASDAQ directly.

| | |
| --- | --- |
| What ships | The fetch script and the extraction command, no data |
| The argument for | Nothing is redistributed at all. The script points at the authoritative source, so there is no question to answer |
| What it costs | A fresh clone can no longer run the full test suite. The two `ItchRealCapture` tests must skip when the file is absent, which means the real-data regression stops running in CI unless CI downloads several gigabytes on every push. The strategy and latency tools need a download before they do anything |
| Effort | Small: delete the file, make the two tests skip when it is missing, and change their fixture to be generated by the fetch script |

### The middle option, worth naming

Keep the slice but make it **synthetic and semantically equivalent**: replay the
real capture, then emit a generated stream with the same message mix, depth
profile, and symbol structure. `itch_gen` already produces semantically valid ITCH,
so the machinery exists. That removes the redistribution question entirely while
keeping a committed fixture.

The cost is real and is the reason it is not the default: a synthetic fixture can
no longer prove that this parser and the live feed agree, which is precisely what
the real slice was introduced to prove and what it actually caught, the sub-penny
prices in five of 92 705 adds. The regression test would keep its shape and lose
its evidential value.

### What is not in question

Publishing the **complete** archive as GitHub release assets is the more
substantial ask of the two, and `scripts/publish_dataset.sh` has deliberately never
been run against a live repository. That stays unrun until the question above is
answered, whichever way the slice goes.

Everything in this directory other than this file and the slice is ignored by git.
