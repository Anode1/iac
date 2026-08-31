# Cross-lab board: does corpus diversity buy capability?

    question:  do models from different labs, conversing on one iac board,
               solve what none of them solves alone?
    arms:      SOLO / HOMO / HETERO, budgets matched in dollars
    status:    design; first step is a Claude-only pipeline shakedown

Inter-agent communication is what the board was built for. This experiment asks
the capability question that mechanism raises, and it is run by the board's
author. The design answers that: the board is held constant across every arm,
so no arm can flatter `iac` itself; the only variable is whose models sit on it.

## The idea

A fleet of subagents behind one model is correlated draws from one
distribution. The information-theoretic case that a solo agent beats a team at
equal budget (Tran & Kiela, arXiv 2604.02460) assumes exactly that. Models from
different labs are different distributions: different corpora, different
post-training, different blind spots. If a team can ever buy capability instead
of efficiency, this is the diversity that pays for it, because a solo agent
cannot sample its way into it at any budget.

## What is already measured, and the gap

    AgentRoom    2608.23740  blackboard beats parallel-merge at matched
                             compute; mixed-lab pairs only exploratory
                             (n=3, one greenfield task, scorers disagree)
    AgentCARD    2606.20629  cross-lab teams win cost-matched, up to 44%;
                             role routing, the models never converse
    Self-MoA     2502.00674  mixing models loses to self-ensembling the best
                             one; one-shot aggregation, unequal-quality mix
    Tran & Kiela 2604.02460  solo wins at equal token budget; one
                             distribution behind every agent

No published result puts different-lab peers in conversation on a shared board
over tasks each of them fails alone. That gap is this experiment.

## The tasks: harvest, do not plant

Planting solo-proof tasks fails: in the predecessor project (`hsearch`, retired
2026-08-31) a planted defect population believed hard fell to a single
instruction sentence. Harvest instead: take the residual set of a public
benchmark with executable ground truth (SWE-bench class) on which every lineup
member fails at high k on the published leaderboards. The held-out test suite
is the oracle, hardness is calibrated by the field's own failures, and nobody
in the loop authored it.

## Arms

Budgets are matched in dollars, the only unit that crosses vendors; this needs
metered API billing per seat, not subscriptions.

    SOLO    best single member, repeated until spend matches a board run
    HOMO    the board, three copies of the best member
    HETERO  the board, one member per lab

HOMO attributes any HETERO gain to corpus diversity rather than to the board
itself; board-versus-no-board is AgentRoom's result, not ours to re-prove.

Coordination costs no tokens here: a seat waits in a parked `recv`, woken by
the kernel when the log grows (measured in
[the paper](https://doi.org/10.5281/zenodo.21206970)), where AgentRoom's agents
poll by inference. At matched dollars every arm spends its whole budget
thinking.

## Criterion, before any numbers

HETERO must solve at least one residual instance that neither SOLO nor HOMO
solves at matched dollars across all repeats. Zero such instances at the
pre-registered repeat count means corpus diversity bought nothing on this
population; that null is archived like any other result. The room log is the
audit record: plain text, append-only, committed alongside the numbers.

## First step

A Claude-only shakedown: three Claude models on one board against one harvested
instance, to debug what has nothing to do with the hypothesis: each harness
sitting on a blocking `recv`, per-seat dollar metering, the log as audit trail.
It tests the pipeline, not the hypothesis; within-lab mixing at matched dollars
is already published (AgentRoom: two Haikus beat one Sonnet).

## Open

Second and third vendor and their metered API keys, benchmark and residual-set
rule, spend cap, repeat count. Recorded here when set.
