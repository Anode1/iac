# iac -- roadmap

Genuine future work only; what already ships is in the [README](../README.md).
Small in keeping with the tool -- each item is a bounded change to the one C file.

## Bound the log's growth (rotation / feedback)

Today the log and `claims/` grow without limit. `iac compact <room>` reclaims
them, but only when run **by hand**, and only up to the slowest reader's cursor
(`keep = min` over every `<name>.cur`), so a single stale reader pins the whole
log. Two additions would keep the disk safe:

- **Size feedback (cheap, do first).** When the log crosses a threshold (env
  `IAC_LOG_WARN`, e.g. 64 MiB), have `send`/`recv` print a one-line warning to
  stderr so the operator notices *before* the disk fills. No behavior change --
  just a nudge to run `compact` (or reap, below).
- **Automatic compaction.** Opportunistically run the compact sweep from `send`
  once the log exceeds a threshold **and** every registered cursor has advanced,
  so routine use reclaims space with no cron job. (A literal "rotate to `log.1`"
  does not fit: cursors and claims are keyed on *absolute* offset, so rolling the
  file means re-keying them -- which is exactly what `compact` already does, in
  place.)

A hard bound that survives a *stale* reader needs the cleanup below (or an
explicit "drop oldest, warn" mode): compaction alone cannot pass a cursor that
never advances.

## Clean up old names (stale roster + orphaned cursors)

A member that left or died leaves its `roster/<name>` entry and its `<name>.cur`
cursor behind **forever**: `who` lists it as long-offline, and -- worse -- that
stale cursor pins `compact`, blocking the log bound above. Add a reap:

- `iac gc <room>` (or fold it into `compact`): drop `roster/<name>` and
  `<name>.cur` for any name that is **not** flock-held (offline) **and** whose
  last-seen is older than `IAC_STALE_TTL` (e.g. 24h). Stay conservative -- never
  reap an online member, never one seen recently -- so an idle-but-alive agent is
  not evicted.
- With dead readers reaped, automatic compaction can actually reclaim, and `who`
  stays a live roster instead of a graveyard.

The two are one story: **bounded logs need dead readers cleared out of the way.**
Ship the size warning first (safe, immediately useful), then the reap, then let
`send` compact opportunistically once nothing stale is pinning the log.
