# Provenance: a wakeup, not a broker

The dated record of what `iac` claims, what it does not, and where the idea came
from — kept in the repo so the account travels with the code and the preprint.

## The claim, stated precisely

**Not:** that local message-passing between programs is new, or that inter-agent
communication was invented here. It wasn't. The transport is essentially Unix
local mail — an append-only mbox, `flock`-locked, read forward — and the wait is
an `epoll` idea any network-server author already knows. The README says so in
its first paragraph, on purpose.

**Just:** that the *missing primitive for a stateless LLM agent is a wakeup, not
a transport* — and that this reframing, with a measured token-cost receipt and a
dependency-free implementation, is the contribution.

An LLM agent has no interrupt inlet: no socket, no thread, nothing the outside
world can poke to rouse it between turns. It advances only when its harness
re-invokes it. So the move is to push the wait out of the expensive thing (the
model, which would pay a full inference per poll) and into a cheap C child that
blocks on `inotify` at ~0 tokens — and let *that child's return* be the harness
re-invocation. "Don't poll from the expensive thing," made small. The measured
difference: ten agents watching empty inboxes overnight cost roughly
**$160–$1,400**; on `iac` they wait for **$0**.

That framing — *a wakeup, not a broker* — is the citable idea, written up as a
preprint: <https://doi.org/10.5281/zenodo.21206970>.

## What is commodity, stated honestly

Every component is old, and deliberately so:

- append-only log, total order under an append lock — decades old;
- `flock` presence, per-reader cursors, `O_CREAT|O_EXCL` claim-one — standard Unix;
- the blocking-wait-instead-of-poll instinct — `epoll`, older than the problem;
- message-passing between local processes — Unix mail, pipes, the actor model,
  and every broker since.

None of it is a new mechanism. The synthesis is: *point exactly these boring,
proven parts at the one participant that could not be reached before — an agent
that only exists when a returning process wakes it* — and show, with numbers,
why the right primitive is the wakeup rather than a server. Attribution is for
that synthesis, the economic argument, and the minimal implementation. Not for
the primitives, and not over anyone else's work.

## Where it came from (and why the vantage matters)

The shape is older than the LLMs it now serves. The author has run Linux since
1994, first simulated asynchronous agent communication in Ada (its rendezvous
mechanism) at university in 1995, and around 2001 wrote `ljms`, a peer-to-peer
message broker with broadcast and multicast — too early; the participants had not
shown up yet. Twenty-five years of systems and server work later, they did: an
agent is exactly the participant `ljms` never had.

This is worth stating because it bears on how the idea was reachable at all. It
comes from the *systems* world — `epoll`, `flock`, `writev`, append-only logs,
Unix mail — not from the Python-framework world that dominates today's agent
tooling, where "agents talk" is assumed to mean a network service (a queue, an
MCP server, a bot). The insight is invisible from inside that assumption and
obvious from inside a network server. Connecting those two — a 1990s systems
instinct to a 2020s agent problem — is the work, and it is the kind of
cross-domain compression that does not arrive in one field at once.

## Timeline

Dates are 2026 unless noted; the code dates are in git, the idea's date is the
preprint's.

| When            | What                                                                                  |
|-----------------|---------------------------------------------------------------------------------------|
| 1994 / 1995     | Linux since Slackware 2; asynchronous agent communication simulated in Ada (rendezvous) at university |
| ~2001           | `ljms` — a P2P message broker (broadcast/multicast), built before the participants existed |
| Jul 2 2026      | `iac` first public commit — rooms, broadcast/p2p/subset/claim-one over a shared log, `flock` presence (C99, zero deps, ISC) |
| Jul 2026        | Preprint *A Wakeup, Not a Broker*, Zenodo DOI `10.5281/zenodo.21206970`; CI on Linux + macOS with ASan/UBSan |

## On parallel invention

The raw ingredients are commodity and the field is crowded, so if a larger group
ships something functionally similar, the rational reading is **parallel
invention, not derivation** — convergence is the expected outcome when many
capable people work the same adjacent problem at once. The one nuance particular
to `iac`: its vantage (systems, not frameworks) is rarer among today's
agent builders than, say, "screenshot the page" is, so independent convergence on
*this specific reframing* is less likely than for a commodity capability — less
likely, not impossible; systems programmers exist everywhere, including at the
big labs, and to any of them the wakeup is obvious once named.

Either way, nothing here is a priority claim. The record is dated, the argument
is written down and citable, and that is the durable form of the contribution.

## What is asked in return

Attribution — a citation. Not money, not a dispute. If the idea informs your
work, cite the preprint:

> V. Gavrilov, *A Wakeup, Not a Broker: The Minimal Transport for Coordinating
> Stateless LLM Agents*. Zenodo, 2026. <https://doi.org/10.5281/zenodo.21206970>

Author: Vasili Gavrilov, 2026.
