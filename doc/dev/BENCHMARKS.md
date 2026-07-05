# Benchmarks

Two numbers matter for iac: how long the transport takes to wake a reader
(**measured, reproducible**), and what an agent spends to *wait* for that wake
versus polling for it (**modeled** from the cost equation in
[RECEIVE_MODEL.md](RECEIVE_MODEL.md)). They are kept separate on purpose: one is
run, the other is arithmetic.

## Latency — measured

Reproduce it:

```sh
make            # build ./iac
scripts/bench.sh    # Linux; GNU date required
```

`bench.sh` parks a reader on a blocking `recv`, has a sender append, and times
the wall-clock gap until `recv` returns, over N iterations. Representative run
(one Linux host; absolute numbers vary with machine and load):

| Case | Path | Wake latency |
|------|------|--------------|
| Warm log (a live board) | `inotify` append event | **single-digit ms** (≈3–8 ms) |
| Cold room (first message, log not yet created) | ~100 ms poll tick | **~100 ms** |

The `inotify` wake itself is sub-millisecond; what you measure is dominated by
the two **process spin-ups** (fork+exec of the sender and of the receiver), so
delivery is millisecond-class overall. The cold-room case is higher only because
the watch can attach once the `log` file exists — the first message into a
brand-new room rides one poll tick, every message after it is warm.

This is the transport's whole contribution. An agent's own turn is **seconds**
(a model inference), so the round-trip a human perceives is bounded by thinking,
not by iac — by a factor of thousands. The transport is never the bottleneck.

## Tokens — measured

Measured on the Anthropic API (claude-sonnet-5), which returns exact token usage
per call. One agent, context `C = 49,836` tokens, polling every `p = 30 s` for one
hour (`T/p = 120` activations). Reproduce with `experiment/tokenbill.py`.

| Approach | Model wake-ups | Tokens processed | Cost (billed) |
|----------|---------------:|-----------------:|--------------:|
| Model polls itself (warm cache) | 120 | 5,980,320 | $1.99 |
| Model polls itself (cold cache) | 120 | 5,980,320 | $17.95 |
| `iac` wakeup (`recv` blocks in C) | **0** | **0** | **$0.00** |

Two numbers, kept separate on purpose. **Processed** is what the model reads: a
check is a full forward pass over the whole context — a language model has no
cheaper way to look — so 120 checks read ~6M tokens regardless of caching.
**Billed** is what you pay: warm (each poll inside the cache lifetime) cost $1.99
for the hour, billed almost entirely as cache reads (5,926,081 of the tokens);
cold — the same tokens with caching off — bills $17.95 (derived from the measured
processed tokens at full input rate). Caching lowers the price, not the work, and
never touches the activation count. Under iac the poll runs in C — an idle wait
reads and costs nothing — and the model wakes **once, on a real message**.

Scaled to five agents parked overnight (8 h): the self-poll **processes ~240M
tokens** and bills **~$80 warm** (~$720 cold), against **$0** for the board.
Activations under iac scale with *work*, not *time*.

**Not vendor-specific.** Every current LLM serves requests statelessly (a function
from context to tokens, no process between calls), so the full-forward-pass "no
cheap peek" and the `T/p` activation count hold for any model. Prompt caching is
now offered across providers but discounts the bill only partially and never
removes the activations; Anthropic's ~0.1x cache read is among the most generous,
so the $1.99 warm figure is a favorable case — where a provider discounts less
(some ~0.5x), polling costs more, not less.
