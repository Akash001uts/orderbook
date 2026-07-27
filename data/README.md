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

## Getting the full capture

`scripts/fetch_nasdaq_sample.sh` downloads and decompresses a full day. The files
are 3.5 to 4.8 GB compressed and roughly 10 to 15 GB decompressed, which is why
none of them is committed here: GitHub rejects any file over 100 MB, and Git LFS's
free tier is 1 GB.

The full capture is what `--survey` mode is for, and it is worth running once. On
the 2019-12-30 sample the parser reports 8 906 symbols and zero unknown message
types across 11 958 712 messages.

Everything in this directory other than this file and the slice is ignored by git.
