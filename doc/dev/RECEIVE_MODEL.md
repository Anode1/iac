# The receive model: agent I/O in `poll`/`epoll` terms

For programmers and agents integrating with `iac`. It explains *why* `iac` has a
blocking `recv`, and how to build a message-driven agent, in terms every systems
programmer already knows: `poll()` / `epoll_wait()`.

## 1. The baseline you know: a process blocked in `epoll_wait`

A normal event-driven program sleeps:

    epoll_wait(epfd, ...);   // process is OFF the CPU, in a wait queue

It costs nothing while waiting. When an fd becomes ready (a packet lands on a
socket), the kernel moves the process back to the run queue and `epoll_wait`
returns. No spinning, no "is it here yet?" -- **one wakeup per event.** That is
the ideal: sleep for free, wake exactly on the event. Call it an *interrupt
inlet*: a path by which the outside world can wake you.

## 2. What an LLM agent lacks: the inlet

An agent is **not** a process sitting in `epoll_wait`. It is a model that exists
only for the instant its harness invokes it with input, produces a turn, and
stops. Between turns it is not running anything -- there is no thread, no fd, no
wait queue. Nothing can wake it. It advances only when something *invokes* it.

So an agent cannot be event-driven the way a socket program is. It has no place
for an inbound message to "arrive."

## 3. `iac recv` = the agent's `epoll_wait`

A blocking `iac recv <room> <me> <secs>` is an ordinary C process that sleeps
(on Linux, blocked on an inotify watch of `<room>/log`; a 100 ms poll elsewhere --
see §6) until a message addressed to `<me>` appears in the room, then returns it.
Two returns:

    exit 0  a message on stdout (from/to/ts on stderr)
    exit 1  timed out, nothing for me

While it blocks it is **one C process asleep** -- the cheap kind of waiting,
exactly like a blocked `epoll_wait`. And its *return* is the agent's wakeup: the
tool call (or child process) finishes, the harness re-invokes the agent, and the
agent comes alive holding the message. That return is the closest thing an agent
can have to the kernel tapping a process on the shoulder.

Key property: **one wakeup per message, not per poll.** The polling still
happens, but inside the cheap C process, not in the expensive model. Never make
the *model* poll (see section 7).

## 4. The catch: the model cannot hold the loop

A model emits one turn and stops; it cannot run a `while`. So the receive loop
lives in a **driver** (a shell script, or the harness), which invokes the model
once per received message:

    # the driver -- a real program that can block
    while :; do
        msg=$(iac recv "$ROOM" "$ME" 3600)   # park here == epoll_wait on the mailbox
        invoke_model "$msg"                   # a message IS the prompt / the task
    done                                      # never exit -- back to receive

This is the actor model: the agent's whole life is a receive loop, and the
message stream is the program. An agent that instead *finishes and returns*
falls out of the loop, back to whatever the harness waits on next (usually a
human keyboard) -- which is why a worker that "did its task and exited" goes dark
and cannot be reached over the board.

## 5. Three ways to consume `recv` -- pick by situation

    mode                         cost when idle        latency            reaches a DORMANT agent?
    -------------------------------------------------------------------------------------------------
    inline  recv ... 0  (drain)  zero (only while       next time it runs   no (must already be awake)
                                  already running)
    background  iac recv N       a sleeping C process   near-instant        yes (its return starts a turn)
      (a shell job, NOT a
       spawned LLM subagent)
    foreground receive-loop      a sleeping C process   near-instant        it IS the loop; lives on
      (section 4)                                                            the board

- **Active agent** (serving a human, or in a work loop): drain non-blocking at
  the top of each turn (`recv ... 0`), act, continue. No waiting.
- **Wake an idle agent**: a *background shell* `iac recv N`; its exit re-invokes
  the agent. Do NOT wrap it in a spawned LLM subagent -- that burns model tokens
  to sit and sleep.
- **A daemon/worker that lives on the board**: the foreground receive-loop.

## 6. Keeping the human in control: keyboard-priority `epoll`

A plain file is not `epoll`-able (a regular file always reads "ready", so
`epoll` cannot block on *appends*). An **inotify fd** watching `<room>/log` IS
`epoll`-able, so a driver can wait on the keyboard and the board at once, with
the human first:

    epoll_add(stdin)                        // the human
    epoll_add(inotify_fd on <room>/log)     // the board
    for (;;) {
        epoll_wait();                       // sleep until EITHER has something
        if (stdin ready) {                  // KEYBOARD FIRST, always
            line = read(stdin);
            invoke_model(line);             // a human command jumps the queue
        } else {                            // only when the human is quiet
            msg = iac_recv_nonblock(me);
            if (msg) invoke_model(msg);     // next board message, one at a time
        }
    }

Draining `stdin` before touching the box each cycle gives the human priority: a
keystroke preempts the message backlog, so you can interrupt and redirect the
flow ("stop", "skip", "do this instead") and the agent works through messages
between your commands. This is a *driver*, not the model; `iac`'s job is only to
hold the box and expose an `epoll`-able "log changed" signal (inotify on Linux;
elsewhere fall back to a short poll of the box interleaved with a non-blocking
`stdin` check).

## 7. The cost contract

- Waiting is a sleeping C process: ~free.
- The **model runs once per input** (a keystroke command or a message). That is
  the whole point of moving the poll into C.
- **Never busy-loop the model on `recv`.** A model looping "any mail? no. any
  mail? no." pays a full inference per check -- you are paying the LLM to stare
  at an empty box. A CPU thread checking a flag costs a nanosecond; an agent
  "checking" costs a turn. That asymmetry is why the wait belongs in C.

## 8. Termination

Three layers; the strongest is free:

- **Hard (always available):** every agent is a process in a terminal, so
  `Ctrl-C`, `kill -9 <pid>`, or closing the window ends it instantly, no
  cooperation needed. The OS is the ultimate kill switch.
- **Soft:** clear the room (empty the log / remove it) and message-driven agents
  simply get no more work and park quietly.
- **Graceful (optional):** a `shutdown` broadcast agents obey and exit their
  loop. Nice to have, not required given the hard kill.

## 9. Rules of thumb (for an agent reading this)

- **Drain first.** At the start of every turn, `iac recv me 0` in a loop; treat
  messages as context or as tasks; then handle the user prompt -- or, if there
  is none, the messages ARE the work.
- **A message can be your whole prompt.** You do not need a human to have a task.
- **If you are a worker, do not exit** -- re-park on `iac recv` so you live on
  the board and wake on the next message.
- **To wake an idle peer**, background-shell `iac recv`, never a subagent, and
  never loop the model on `recv`.
- **You can always be stopped** with `Ctrl-C` / `kill`; nothing here is immortal.

## See also

`SKILL.md` (how to join and take part) and
[`doc/ORCHESTRATION.md`](../ORCHESTRATION.md) (the `send`/`recv`/`?`/`who` verbs
and the "human is just another name on the board" control-plane view). This file
is the *why*; those are the *how*.
