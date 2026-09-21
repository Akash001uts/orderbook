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

**License and redistribution.** The MIT license in the repository root covers this
project's original source code and documentation. It does not cover this file's
data. NASDAQ publishes these sample files publicly at `emi.nasdaq.com` with no
login; this slice is a small derived excerpt committed for automated testing.

Both `data/qqq_slice.itch` and the derived market-data artifacts under `site/data/`
are Nasdaq-sourced and are excluded from the MIT grant. They remain subject to
Nasdaq's applicable terms. Any use beyond this repository should attribute NASDAQ
as the source of the underlying TotalView-ITCH 5.0 data and observe those terms.
The details that identify the material are in the table above: source
`12302019.NASDAQ_ITCH50.gz` from NASDAQ's public sample archive, trading date
2019-12-30, symbol QQQ, committed as a 5 533 732 byte slice.

Two separate permissions apply and should not be conflated. The MIT license is the
repository owner's grant, and it covers only the source code and documentation.
Redistribution of the Nasdaq data is a distinct question that is Nasdaq's to permit:
it is included here on the assumption that Nasdaq's redistribution permission is
granted, and obtaining and retaining that written permission record is handled
separately from this repository. Nothing here waives Nasdaq's rights, and the MIT
grant does not certify the Nasdaq permission. See the NOTICE file in the repository
root.

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

Publishing the complete archive as GitHub release assets is the more substantial
ask of the two, so `scripts/publish_dataset.sh` is run deliberately rather than
automatically. Run it with `PUBLISH_DRY_RUN=1` first to see the parts and checksums
without uploading anything. Either way, the same attribution and Nasdaq terms noted
under the slice above apply to the full capture.

Everything in this directory other than this file and the slice is ignored by git.
