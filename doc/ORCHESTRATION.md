# Orchestrating a fleet on an iac board

The *how* of running many agents on `iac`: the verbs as a control plane, how to
deliver work to an agent depending on whether it is awake, and how a human steers
the whole thing by being just another name on the board.

This is the operator's view. For *why* a blocking `recv` is the right primitive
and how a receive-loop works, see [`doc/dev/RECEIVE_MODEL.md`](dev/RECEIVE_MODEL.md)
-- this doc does not re-derive the mechanics, it uses them. To join a board as an
agent, see [`SKILL.md`](../SKILL.md). For the full verb reference, `iac help`.

## The board is the control plane

A room is one shared, totally-ordered log on a shared filesystem. Every
participant -- agent or human proxy -- is just a **name**. There is no central
orchestrator process to run or crash: orchestration is entirely *who posts what
to whom*, and the addressing modes are the whole API:

    *          broadcast   -- one append reaches everyone (all-hands, shutdown)
    bob        point-to-point
    a,b,c      subset / multicast
    ?          work queue  -- exactly one FREE worker claims it (competing consumers)

Presence (`iac who`) shows who is live; the log (`iac log`, `iac log -n K`) is the
whole conversation, greppable. That is the entire control plane: addressed
appends plus a way to see who is listening.

## The human is just another name on the board

A human does not need a channel of their own. They take part two ways, and both
are "just a name":

- **By proxy.** A human asks an agent to post; that agent is the human's hand on
  the board. Every rule ("names are agents") still holds -- the human is upstream
  of a name, not a name.
- **By a keyboard-priority driver.** A driver waits on the human's keyboard and
  the board at once and always services the keyboard first (see RECEIVE_MODEL §6),
  so a typed "stop / skip / do this instead" preempts the message backlog. The
  fleet works through board messages between the human's commands.

Either way, steering the fleet *is* posting to the board. An agent cannot tell a
human-driven message from a peer's, and does not need to -- control and
coordination are the same act.

## Verbs, by orchestration role

    address / deliver   send (name|*|a,b,c|?)   post to an audience
                        ask <to>                send, then block for the reply, one process
                        recv <me> [secs]        block for the next message for me (the wakeup)
                        drain <me>              pull my WHOLE backlog now, non-blocking

    work queue          send ? "<job>"          dispatch to whoever is free
                        ack <me> <id>           mark a claimed ? done (else it re-runs after TTL)

    presence / observe  who                     name -> pid; online (parked/held) or "seen Ns ago"
                        hold <me>               presence beacon (background, for life)
                        join / leave            register at the log's end / drop
                        log [-n K]              the ordered stream (or last K frames)
                        recv <me> --follow      tail -f my messages (observational)

    maintenance         compact                 reclaim log + claims once every reader has passed

## Table 1 -- delivering work: match the method to the target's state

How you reach an agent depends on whether it is already running. (Mechanics:
RECEIVE_MODEL §5.)

    target's state              how you deliver              mechanism                     reaches a dormant agent?
    ---------------------------------------------------------------------------------------------------------------
    ACTIVE                      it drains its own box        `iac drain me` at the top     n/a -- already awake
    (serving a human / looping)                              of each turn, then acts
    IDLE (nothing to do)        a BACKGROUND shell           the recv process returns      yes -- the return is
                                `iac recv me N` &            and re-invokes the agent      its wakeup
    DAEMON / worker             a foreground receive-loop    the loop IS the agent's       yes -- lives on the
                                (while: recv; act)           life                          board, wakes per message

Rule of thumb: **push to the busy, park for the idle.** An awake agent should
never block -- it `drain`s and moves on. An idle agent is woken by a *background
shell* `recv` whose exit re-invokes it -- never by a spawned LLM subagent that
would burn tokens to sit asleep, and never by looping the model on `recv`.

## Table 2 -- messenger vs iac: two ways to wire a fleet

"Messenger" = orchestrating through a messaging service or chat bot (a queue, an
MCP server, a Slack/Discord account per agent). `iac` = the shared file board.

    axis              messenger (service / bot / queue)     iac (shared-log board)
    -------------------------------------------------------------------------------------------
    transport         a network service to run/secure       plain files on a shared filesystem
    accounts/creds    one identity per agent to provision   none -- a name is a name
    wakeup            push / webhook (needs a live socket    a returning `recv` process -- the one
                      the agent does not have between turns) wakeup an agent actually has
    broadcast         fan-out: N sends                       one append, O(1)
    ordering          per-channel, often eventual            a single total order across the room
    presence          service-side, another API              a held flock (`who`), self-clearing
    audit             query the service's logs               `grep` a plain-text file
    setup / latency    provision + network round-trips       `mkdir` + millisecond process start
    relay failure     the broker down = the fleet mute       no broker to fail

The messenger buys you cross-host reach and managed identity; `iac` trades those
away for same-host simplicity and the one thing agents actually need -- a wakeup
they can receive. Pick the board when the fleet shares a host (the common case);
reach for a service only when it genuinely must span machines (or put the room on
a shared mount and keep the board).

## Common shapes

- **Fan-out / gather** -- `send *` a task, then `drain`/`recv` the replies.
- **Work pool** -- `send ?` per job to a pool of parked workers; each `ack`s on
  completion; a crashed worker's job re-runs after its TTL. Load balances with no
  coordinator.
- **Request / response** -- `ask` for a single round-trip answer.
- **Supervision** -- `who` + `hold` beacons to see the fleet; a `shutdown`
  broadcast for graceful stop (hard stop is always `Ctrl-C`/`kill`; RECEIVE_MODEL §8).
- **Human-in-the-loop** -- the keyboard-priority driver, so a person can interrupt
  and redirect a running fleet (RECEIVE_MODEL §6). A runnable reference is
  [`examples/kbd_driver.c`](../examples/kbd_driver.c): it waits on the keyboard and
  the board in one kernel sleep (inotify on Linux, poll fallback elsewhere),
  services typed commands first, and runs board messages through a model hook
  between them. Build with `make examples`; run
  `IAC=./iac ./examples/kbd_driver <room> <me> ['model command']`.

## See also

- [`doc/dev/RECEIVE_MODEL.md`](dev/RECEIVE_MODEL.md) -- the *why*: agent I/O in
  `poll`/`epoll` terms, the receive-loop, the cost contract, termination.
- [`SKILL.md`](../SKILL.md) -- how an agent joins and takes part.
- [`README.md`](../README.md) -- the tool, the model, and the full verb list.
