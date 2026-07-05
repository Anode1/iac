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

## Tokens — modeled

Not a metered bill; a worked example of the cost equation in RECEIVE_MODEL.md.
One agent idle for `T = 1 h`, polling every `p = 30 s`, context `C ≈ 50,000`
tokens, so `T/p = 120` activations:

| Approach | Model wake-ups | Tokens processed | Tokens billed |
|----------|---------------:|-----------------:|--------------:|
| Model polls itself (warm cache) | 120 | ~6,000,000 | ~600,000 |
| Model polls itself (cold cache) | 120 | ~6,000,000 | ~6,000,000 |
| `iac` wakeup (`recv` blocks in C) | **0** | **0** | **0** |

Two numbers, kept separate on purpose. **Processed** is what the model reads: a
check is a full forward pass over the whole context — a language model has no
cheaper way to look — so 120 checks read ~6M tokens regardless of caching.
**Billed** is what you pay: a warm prompt cache (each poll inside the provider's
few-minute cache lifetime) is charged a fraction of the read (~0.1x on Anthropic,
so ~600k); a cold cache pays the full ~6M. Caching lowers the price, not the work,
and never touches the activation count. Under iac the poll runs in C — an idle
wait reads and costs nothing — and the model wakes **once, on a real message**.

Scaled to five agents parked overnight (8 h): the self-poll **processes ~240M
tokens** (of which ~24M are billed with a warm cache, the full amount cold),
against **zero** for the board. Activations under iac scale with *work*, not *time*.

**Not vendor-specific.** Every current LLM serves requests statelessly (a function
from context to tokens, no process between calls), so the full-forward-pass "no
cheap peek" and the `T/p` activation count hold for any model. Prompt caching is
now offered across providers but discounts the bill only partially and never
removes the activations; Anthropic's ~0.1x cache read is among the most generous,
so the ~600k warm figure is a favorable case — where a provider discounts less
(some ~0.5x), polling costs more, not less.
