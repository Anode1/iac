---
name: iac
description: Coordinate with other agents on the same machine over a shared room log: wait on a blocking recv instead of polling, and send a message to one agent, all of them, or whoever is free. Use when you have nothing to do until another agent acts, or when you need to hand work to, notify, or ask another agent.
---

# iac: coordinate agents by waiting, not polling

iac is a shared append-only log, one per room. An agent listens by parking a
blocking `recv`, which returns the instant a message addressed to it lands and
costs nothing while it waits (a throwaway C child sleeps in the kernel; there is
no inference per check). This is how agents on one machine coordinate without a
broker and without a poll loop.

Every verb takes a room (a path both agents agree on, e.g. `/tmp/room`) and the
agent's own name. The sender's identity is the `IAC_FROM` environment variable.

## When to use
- You have nothing to do until another agent acts: park `iac recv` and let the
  harness re-invoke you when a message arrives.
- You need to hand work to, notify, or ask another agent: `iac send` / `iac ask`.
- You are a fresh agent joining a running room: `iac log <room> -n 20` to orient,
  then `iac join`.

## Prerequisites
- The `iac` binary on PATH (build from https://github.com/Anode1/iac).
- A room path all participants agree on. No server, no setup: the first
  send/recv creates it.

## Receive (wait, do not poll)
- `iac recv <room> <me> [timeout] [--follow]` blocks until a message addressed to
  `<me>` (or to `*` / `?`) arrives, returns it, and exits. No timeout waits
  indefinitely; `--follow` keeps returning later messages.
- `iac drain <room> <me>` returns pending messages without blocking.
- Do not loop-check the log: a parked `recv` is free, a poll is an inference each
  time.

## Stay on the board (the one rule agents get wrong)
You are a PERSISTENT worker: finishing a task does NOT mean you are done. After you
send a result, your very next action is to block on `iac recv` again -- you are only
"done" while parked in that recv. Never end your turn without an outstanding recv; an
agent that finishes work and forgets to re-`recv` goes dormant, awake but unreachable.
Two setup slips bite first-timers: (1) every teammate must share the SAME room path --
run `iac who <room>` and confirm you see the others, or you are on the wrong board and
invisible; (2) you must be holding a `recv` to be reachable at all. For a hands-off
loop where bash re-blocks (no reparking to remember), use the background while-driver:
`while :; do m=$(iac recv <room> <me> 3600) && { # act on "$m"; }; done`.

## Send
- `IAC_FROM=<me> iac send <room> <to> <message>` where `<to>` is another agent's
  name, `*` (all), a list like `a,c`, or `?` (whoever is free claims it).
- Multi-line body: `printf 'a\nb\n' | IAC_FROM=<me> iac send <room> '*'`.
- `IAC_FROM=<me> iac ask <room> <to> <question>` sends and waits for a reply.

## Presence and tasks
- `iac join <room> <me>` registers and starts at the log's end (skips backlog).
- `iac hold <room> <me>` runs a background presence beacon for the agent's life;
  `iac leave <room> <me>` drops it.
- `iac who <room>` lists name -> pid, online (parked/held) or last-seen.
- A `?` message is a claimable, crash-recoverable task: `iac ack <room> <me> <id>`
  on completion.

## Notes
- Everything is local files under the room path; nothing leaves the machine.
- `iac log <room>` is the whole ordered stream (greppable); `iac compact <room>`
  reclaims frames every reader has passed.
- `iac help` lists every verb and env knob on one screen.
