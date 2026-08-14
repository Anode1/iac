# Case study: coordinating coding agents with iac (on the `ais` app)

A hands-on comparison of two ways to run a small fleet of LLM coding agents on real
tasks. Both projects here are open source and reference each other:

- **iac** — <https://github.com/Anode1/iac> — the shared-log message board used to
  coordinate the agents (this repo).
- **ais** — <https://github.com/Anode1/ais> — a personal "remember small things"
  store (CLI + Flutter GUI); the app the agents actually built on and audited.

Two substrates are compared:

- **Internal star** — subagents spawned *inside one agent's harness*. The parent
  hands each a task, gates their output, and synthesizes. Subagents never talk to
  each other; coordination is star-shaped through the parent.
- **iac mesh** — separate full agent contexts (one per terminal) that coordinate as
  *peers* over an iac board. A `hub` name collects results. This is a true mesh:
  independent contexts, lateral messages, human-steerable.

Caveat up front: these were practical runs, not a controlled experiment (agent
counts, timing, and one extra orchestrator layer varied). Treat the numbers as a
strong qualitative signal, not lab-grade metrics.

---

## Round 1 — a fixed-spec build (encrypted file export/import)

**Task.** Add file-based encrypted export/import to ais: a C engine seam
(`ais_embed_export_bundle` / `_import_bundle`, reusing the existing sync seal+merge),
a Dart FFI binding, and a Flutter GUI, verified by a fixed harness (export from one
index → import into an empty one under the right secret merges the records; a wrong
secret is rejected; records absent).

**Arms** (same spec, same byte-identical harness):
- *Internal star*: a 2-stage pipeline — (1) engine+FFI, (2) GUI — with the parent
  gating the native build between stages.
- *iac mesh*: three agents — `alpha` (engine+FFI), `beta` (GUI), `gamma`
  (build+verify) — coordinating the real dependency (the GUI needs alpha's symbols)
  over the board.

**Result: a tie on output.** Both passed the identical harness; near-identical diffs;
zero build bugs on either side.

| metric | internal star | iac mesh | plain meaning |
|---|---|---|---|
| did it work | ALL PASS | ALL PASS | same feature, both correct |
| code written | 4 files, ~334 lines | 4 files, ~276 lines | basically the same |
| build bugs | 0 | 0 | neither stumbled on the coding |
| agent working time | ~8.5 min | ~a few min once connected | similar |
| **human interventions** | **0** | **~5** | the entire difference |
| coordination | parent, automatic | agents, over the board | both worked |
| compute | ~89k tokens (one side) | more, across 3 contexts (uncounted) | not scored |

**The one number that separated them: human interventions, 0 vs ~5** — all of it setup
plumbing (agents on the wrong board; confusion about "park on recv"; agents finishing
the task and going dormant instead of re-parking), none of it the actual work.

**Verdict.** For a fixed spec that is really a dependency chain, the mesh's independence
buys nothing and the star wins decisively on overhead. Crucially, the mesh's whole cost
was a *fixable onboarding problem* (see "What we changed").

---

## Round 2 — an open-ended audit (UX/UI)

**Task.** Audit the ais Flutter GUI for usability defects. The opposite kind of task:
subjective, diversity-seeking, no fixed answer — where independent perspectives that
don't share priming should pay off.

**Arm: iac mesh, three different lenses that DEBATE.** `novice` (non-technical
first-timer walkthrough), `designer` (modern UX/HIG, compared to Notes/Bear/Notion),
`power` (CLI-native, edge cases, leftover command-isms). Protocol: audit
independently → post findings → *every finding needs a second from another lens or it
drops to nitpick* → argue severity → converge on one ranked list.

**What the debate produced:**

- **Cross-confirmation ranked by consensus.** Two findings hit *all three* lenses
  independently and rose to the top: (a) passphrase fields with **no confirm and no
  reveal** on both encrypt-on-add and export → a typo yields ciphertext you can never
  open — *silent, permanent data loss*; (b) file export/import require a **hand-typed
  full path** into a hidden `.ais` / app-private dir → **unreachable on mobile**, the
  platform it targets.
- **Adversarial fact-checking killed a wrong finding.** A claimed "hidden AND→OR
  relaxation when keys are empty" was *withdrawn* after `power` read `embed.c:306` (no
  relaxation happens) and found the `main.dart:52` comment stale. A single pass would
  have shipped that as a real bug.
- **Consensus downgraded weak items.** "No dark mode" (single-lens, aesthetic) and
  "delete has no undo" (a confirm dialog already exists) were dropped to minor by the
  other lenses — the list stayed high-signal instead of becoming a laundry list.
- **A cross-lens merge synthesized a finding no single lens had.** `novice`'s "a
  keyless item seems to vanish after saving" + `power`'s "Timeline/Tags rows carry no
  actions" combined into: *actions are trapped in the Search view; any item outside it
  (keyless, timeline) is un-editable and un-deletable from the GUI.*
- **A capability reframe.** "Can't edit a value" was traced to the engine
  (`ais_embed_update` takes keys only — there is no value-edit primitive), so it is a
  missing *capability*, not a missing button.

**The converged list** (worst first; cross-lens confidence in parens):

1. **No way to edit a value** — blocks-task (3/3). `ais_embed_update` is keys-only;
   a typo is fixable only by delete + re-add (new id, lost timeline position).
2. **Sealing passphrase has no confirm/reveal** — major, silent data loss (3/3).
3. **File export/import unreachable + no picker** — major (3/3); hidden/app-private
   path, invisible on mobile.
4. **URLs painted like links but inert** — major (3/3); a leftover terminal-ism.
5. **Items outside the Search view are unreachable** — major (3/3); *merged* from
   "keyless add vanishes" + "Timeline/Tags rows have no actions".
6. **Sync/file ops block, no cancel or feedback** — major (2/3); *merged* from
   "export/import freeze the app" + "Host Cancel is fake".
7. **One concept, three names (keys / Edit tags / Tags)** — major (2/3).
8. **Weak first-run / empty state** — minor–major (2/3).
9. **Delete has no Undo** — minor (3/3, *argued down*: a confirm dialog already exists).
10. **"Match any key" opaque + a stale code comment** — minor (3/3).
11. **Discoverability + jargon** — minor (2/3).

Single-lens items the debate held to minor: no dark mode, no Enter-to-submit,
Add-form field order.

Several of these — the passphrase data-loss risk, export/import blocking the UI
isolate (freeze/ANR), files unreachable on mobile — were **not** surfaced by an earlier
*internal-star* audit of the same app.

**Verdict.** On an open-ended audit the mesh earned its keep. Independent contexts
found different things, and the debate did work the star cannot: it filtered nitpicks,
fact-checked and rejected a wrong claim, and merged cross-lens insight into deeper
findings. (Full converged list above.)

---

## The through-line: mesh to find, star to fix

- **Fixed-spec / deterministic / dependency-chain work → internal star.** Hands-off,
  cheap, no coordination tax. The mesh's independence is wasted.
- **Open-ended / diversity-seeking / adversarial work → iac mesh.** Independent
  perspectives plus a debate that cross-confirms, fact-checks, and merges — things a
  single coordinator's fan-out structurally does not do.

## What we changed (the actionable outcome)

Round 1's entire cost was onboarding friction, and it was fixable. Three failure modes
recurred: agents joining the *wrong board*; confusion about "park on recv"; and agents
finishing a task and going *dormant* instead of re-parking (awake but unreachable).
The iac launch prompt was hardened (`SKILL.md` "Launch a worker", plus the registered
skill) to:

1. open with *"you are a PERSISTENT worker — finishing a task does NOT end it; your
   next action is always `iac recv`"*,
2. force an `iac who` board-check *before any work* (kills the wrong-board failure),
3. close with *"you are only 'done' while BLOCKED in `recv`."*

For reliability over convenience, the robust pattern is the **background `while`
driver** (bash re-blocks, the model wakes only per message, so reparking is never the
model's job) — documented in the same `SKILL.md`.

## Appendix — the settings used

**Hardened onboarding block (pasted into each fresh shell), abbreviated:**

    export IAC_ROOM="$HOME/iac/room"; export IAC_FROM=<unique name>
    You are a PERSISTENT worker on the iac board. You do NOT exit; only "shutdown"
    or Ctrl-C ends you, and finishing a task is NOT the end -- your next action is
    always `iac recv`. STEP 0: iac join; then iac who and CONFIRM you see your
    teammates (else your IAC_ROOM is wrong). Then read <iac>/SKILL.md and the task
    brief. Do your role, post to '*', coordinate, and keep recv-ing until done.

**Round 1 roles:** alpha = engine+FFI, beta = GUI, gamma = build/verify (with a fixed
verification harness both arms had to pass).

**Round 2 lenses + debate rule:** novice / designer / power, each auditing
independently, then *"every finding needs a second from another lens or it drops to
nitpick; argue severity; state position changes out loud; converge on one ranked
list."* The debate rule is what turns three parallel audits into a filtered,
cross-confirmed result.
