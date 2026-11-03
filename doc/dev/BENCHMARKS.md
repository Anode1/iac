# Benchmarks

Two numbers matter for iac: how long the transport takes to wake a reader, and
what an agent spends to wait for that wake versus polling for it. Both are
measured; the cost equation behind the second is in [RECEIVE_MODEL.md](RECEIVE_MODEL.md).

## Latency, measured

Reproduce the quick form:

```sh
make                # build ./iac
scripts/bench.sh    # Linux; GNU date required
```

`bench.sh` parks a reader on a blocking `recv`, has a sender append, and times
the wall-clock gap until `recv` returns. The full run below used a C driver
around the binary at commit `1247745` (`iac/experiment/transport/` in
github.com/Anode1/articles): the log pre-filled with 1 KB frames addressed to
another member to 0, 1 and 100 MB, on ext4 over NVMe and on tmpfs, read once so
it sat in the page cache; 300 sends per parked cell, 30 per catch-up cell. One
Linux host, kernel 6.8, Intel i7-1165G7, 2026-11-02.

| Stage | median | p99 or max |
|-------|-------:|-----------:|
| `send` process, fork to exit, one frame under `flock`; six cells, 0 to 100 MB, ext4 and tmpfs | 1.6 to 1.7 ms | 2.0 to 2.2 ms |
| `send` to `recv` return, reader parked at the end, same six cells | 2.1 to 2.4 ms | 7.7 to 8.3 ms |
| `recv` catching up from 1 MB behind, ext4 and tmpfs | 1.1 and 2.5 ms | 1.6 and 3.5 ms |
| `recv` catching up from 100 MB behind, about 100,000 frames | 48 to 51 ms | 59 to 60 ms |
| first message into a room with no log yet, poll tick (`bench.sh`, n=30, 2026-11-03) | 101 ms | 101 ms |

The sender's process spin-up dominates; the append and the `inotify` event are a
fraction of the 1.7 ms, and the wake adds 0.4 to 0.6 ms at the median. Wake times
cluster near 2 ms and near 7 ms, which is scheduler state. One wake in 1,800 took
102 ms: a missed `inotify` event caught by the 100 ms fallback poll, which bounds
the cost of a miss. The log's size does not enter: a reader parked at the end of
100 MB wakes in the same 2.2 ms as at the end of an empty log, because it reads
forward from its own cursor. A reader that has fallen behind pays for the frames
it skips, about 2 GB/s from the page cache. Only the first message into a
not-yet-created room pays the poll tick, because the watch attaches once the
`log` file exists. A model turn is seconds to minutes, so the transport is three
to four orders below it.

## Tokens, measured

Measured on the Anthropic Messages API, which returns exact token usage per call.
Harness, recorded usage and the grid script: `iac/experiment/` in
github.com/Anode1/articles. Dollar figures are the measured token counts at list
rates (platform.claude.com/docs/en/about-claude/pricing, read 2026-11-02).

One agent on `claude-sonnet-5`, context 49,836 tokens, polling every 30 s for one
hour (120 activations). Sonnet 5 rates: $2 / $0.20 / $2.50 / $10 per million
input, cache-read, cache-write, output tokens.

| Approach | Activations | Tokens billed | USD | |
|----------|------------:|--------------:|----:|---|
| Model polls itself every 30 s, cache warm | 120 | 5,980,320 | $1.33 | measured |
| The same, no prompt cache | 120 | 5,980,320 | $11.97 | derived |
| `iac` wakeup (`recv` blocks in C) | **0** | **0** | **$0.00** | measured |

The warm hour is billed almost entirely as cache reads (5,926,081 tokens). The
no-cache row is the same count at the input rate. Under iac the poll runs in C,
an idle wait costs nothing, and the model wakes once, on a real message.

A grid on `claude-fable-5-1` (2026-11-02; rates $10 / $0.25 / $12.50 / $50), three
contexts by three intervals, 20 minutes per cell, steady state scaled to the hour:

| context (tokens) | billed per poll | every 10 s | every 30 s | every 120 s | first poll (cache write) |
|-----------------:|----------------:|-----------:|-----------:|------------:|-------------------------:|
| 12,211 | 12,218 | $1.37 | $0.46 | $0.11 | $0.15 |
| 60,726 | 60,742 | $5.89 | $1.96 | $0.49 | $0.76 |
| 181,936 | 181,943 | $16.6 | $5.55 | $1.39 | $2.27 |

Billed tokens per poll equal the context at every interval, 99% of them cache
reads: the idle bill is polls per hour times the per-poll bill, of which the
context at the read price is 80 to 98 per cent. Five-minute gaps read the cache
entry and ten-minute gaps rewrote it, so a 61k seat polling every ten minutes pays
about $0.76 a poll, $4.6 an hour, more than every 30 s warm. Five agents parked
eight hours on Sonnet 5 bill about $49 warm and $479 without a cache, against $0
for the board.

**Why it transfers.** Every current LLM serves a request statelessly, so a check
is a request over the whole context whatever the model; a prompt cache changes
the price of that request and never its count. Cache discounts differ by provider
and model, so the dollar rows are specific to the two models above.
