# iac -- inter-agent communication

A minimal, efficient dispatcher for agents (or any processes) to message each
other on a shared filesystem: broadcast, point-to-point, and subset, in named
rooms. No daemon, no sockets, no third-party service, no accounts -- one small
C99 binary and plain-text files you can `cat`, `grep`, and version.

It exists because the transport was never the hard part between agents. Bytes
move fine across processes; what an LLM agent lacks is an *interrupt inlet* -- a
way for an inbound message to wake it, the way a kernel wakes a blocked
`poll()`. `iac` supplies that inlet the only way an agent can have one: a `recv`
that blocks in C until a message addressed to it lands, so a parked receiver is
one process asleep and returns once, on delivery -- one wakeup per message, not
per poll.

## Why not a message queue, an MCP server, or a bot?

Most agent-coordination tooling reaches for something heavy: a cloud message
queue, an MCP server, a shared vector store, or a chat account per agent
(Slack/Discord bots). That all assumes "agents talk" means a network service,
with the infrastructure, credentials, and latency to match.

But an LLM agent has no interrupt inlet (above): it cannot be woken by a packet,
only re-invoked by its harness. So the primitive it actually needs is not a
socket but a blocking `recv` whose *return* is the wakeup -- the one signal a
parent already gets ("a background child finished"). Once receive is a blocking
poll, the whole problem collapses to files on a shared disk. For a fleet of
subagents on one machine (the common case), that is not a compromise, it is the
entire cost: no server to run, no accounts to provision, no network to secure,
microsecond process start.

That makes `iac` a point in the design space almost nobody targets -- a
dependency-free, single-binary, same-host dispatcher -- precisely because the
network-service assumption hides it. The parts are old and boring on purpose
(append-only log, per-reader cursor, flock presence); the uncommon move is
aiming them at agents that can only be woken by a returning child process.

## Model

    a ROOM      is a directory
    the LOG     <room>/log              one append-only, totally-ordered stream
    a CURSOR    <room>/<name>.cur        each member's read position into the log
    the ROSTER  <room>/roster/<name>     presence, for who/join/leave

Every message is appended to the log *once*, whatever its audience, so
**broadcast is one write, not N**. Each member reads the shared stream forward
from its own cursor and delivers only the frames addressed to it. That gives a
single total order (a real "chat"), O(1) broadcast, and P2P/subset for free.

The room directory is created on demand by the first agent to `send`/`join`/`hold`;
every other agent just reuses it (no separate setup step, no coordinator).

Recipient field (`to`):

    *          broadcast -- every member but the sender
    bob        point-to-point
    a,b,c      a subset (multicast)
    ?          any one FREE member claims it -- a work queue (competing consumers)

`?` is competing-consumers: every idle member sees the task, but exactly one
atomically claims it (an `O_CREAT|O_EXCL` create keyed on the message's log
offset), so a "whoever is free" job runs once across a pool of workers with no
coordinator. It is an agents-only board -- every name is an agent, and a human
takes part by asking an agent to post, not by holding a channel of their own.

## Use

    # send (body from args, or stdin for exact/multi-line bytes).
    # sender name is $IAC_FROM (default "anon").
    IAC_FROM=hub iac send /tmp/room '*'   all-hands: standup in 5
    IAC_FROM=hub iac send /tmp/room bob   just for you
    IAC_FROM=hub iac send /tmp/room a,c   you two rebase
    IAC_FROM=hub iac send /tmp/room '?'   whoever is free: take this job
    printf 'multi\nline\n' | IAC_FROM=hub iac send /tmp/room '*'

    # receive the next message addressed to me, blocking up to N seconds
    # (default 60). body to stdout, "from/to/when" to stderr.
    # exit 0 = delivered, 1 = timed out, 2 = error.
    iac recv /tmp/room me 300

    iac join  /tmp/room me     # start at the log's end (skip backlog) + register
    iac hold  /tmp/room me     # presence beacon: run in the background for the agent's life
    iac leave /tmp/room me     # drop registration
    iac who   /tmp/room        # who is who: name -> pid, online (beacon held) or offline
    iac log   /tmp/room        # the whole ordered stream (debug)

A new agent joins the chat by knowing the room's directory path and picking a
name: `iac join`, then loop `iac recv`. `recv` skips messages not addressed to
it (and its own), advancing its cursor, so successive calls drain in order.
`send` appends with one `writev` under `flock`, so the log stays a clean total
order even with many concurrent senders.

## Presence (who is who, and who is live)

Each agent picks a name; the roster maps it to a pid, so an outside observer can
tell which OS process is `john` and which is `nick`:

    /tmp/room/roster/john   ->  "<epoch> <pid>"
    /tmp/room/roster/nick   ->  "<epoch> <pid>"

Liveness is a held `flock`, not a heartbeat: an agent runs `iac hold` in the
background for its lifetime, which flock-holds its roster entry. `iac who` probes
the lock -- held means online. Because the OS drops the lock the instant the
holder dies (even on SIGKILL), a crashed agent shows offline immediately, with no
stale entry to reap and no pid-reuse guessing (the lock, not the pid, is the signal).

    iac who /tmp/room
    john   online   pid 40021  since 3s ago
    nick   offline  pid 40022  since 1s ago     # nick's process died; auto-cleared

## Frame (on disk, plain text)

    <from>|<to>|<epoch>|<len>\n
    <len bytes of body>\n

Length-framed, not line-based, so a body may contain any bytes.

## The agent pattern (why the blocking recv matters)

An agent cannot be interrupted; it only advances when its harness re-invokes it.
The one wakeup a parent gets is "a background child finished." So `recv` is the
`recv()` an agent can actually use: park a watcher subagent on the room, and its
return IS the inbound-message interrupt.

    # a watcher: block up to 5 min for the next message for me, print it, exit.
    # the WAIT is in C -> one model invocation per message, not per poll.
    # re-arm a fresh watcher after each message.
    iac recv /tmp/room me 300

Any number of agents each keep a watcher parked on the room; anyone drops a
message to one agent, a subset, or all at once. That is a symmetric, event-ish,
multi-party channel built entirely out of polling -- the polling just lives in C
where it is cheap, instead of in the model where it would burn a turn per check.

## Limits (honest)

- Same host / shared filesystem. For across-host, put the room dir on a shared
  mount, or swap the log for a socket (the framing is unchanged).
- Latency is poll-interval (100 ms) + process spin-up: right for coordination,
  wrong for a chatty inner-loop protocol.
- Each member reads the whole stream to filter -- fine at coordination scale;
  for a very high-traffic room, shard into more rooms.
- Presence liveness needs each agent to run one background `iac hold` beacon;
  a member without one shows offline. The flock (not the pid) is the signal, so
  who() stays correct even if a pid is later reused.

## Build

    make        # -> ./iac  (C99, -W -Wall -Wextra, zero deps)
    make ut     # end-to-end tests: p2p, broadcast, multicast, order, join, who

## Lineage

The shape is older than the LLMs it now serves, and so is the taste behind it.
The author has run Linux since 1994 (Slackware 2, kernel 1.x) and has always
preferred plain text files and small Unix tools to heavier machinery. He first
simulated asynchronous agent communication in Ada at university in 1995. Having
specialized in AI, he tried to build software agents in Java in early 2001, and
around the same time wrote `ljms`, a peer-to-peer message broker with broadcast
and multicast -- but no one was hiring AI specialists in his proximity then, and
agents stayed a private pursuit. Twenty-five years later the participants finally
showed up. `iac` is those same instincts -- a shared broker; addressed,
broadcast, and claim-one messages; presence; every bit of it a greppable file --
distilled to a single dependency-free C binary and pointed at the participant
that at last exists: an agent, which can only be woken by a returning process.

## License

ISC (see LICENSE) -- do anything, keep the notice, no warranty.
