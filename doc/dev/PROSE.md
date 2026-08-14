# How the documents are written

The README, `SKILL.md`, the `doc/` notes and the case study.

`iac`'s prose is in better shape than most of the tree it belongs to, and this
file exists mainly so it stays that way. Three rules, and one deliberate
exception.

## Bold a term, not a sentence

`SKILL.md` and `doc/dev/GUI_TESTING.md` use a bolded imperative as a rule
heading, and that works: `**Drain first.**`, `**Boot headlessly.**`,
`**Grant the send verb, or every agent fails silently.**` Short, imperative,
and the paragraph under it is the reason.

What does not work is an ordinary topic sentence in bold, standing in front of
its own explanation. `**What the debate produced — this is the story a fan-out
can't tell:**` was that, with an epigram attached. It is now
`**What the debate produced:**`.

## Titles name

`## Presence`, not `## Presence (who is who, and who is live)`. `## Telegram,
the reference bridge`, not `## Telegram -- recommended, and the reference
bridge`.

## Cut the decorative contrast

Not `that is not a compromise but the entire cost` — `that is the whole cost`.
The negative half was doing no work.

## The exception

The whole argument of this project is a contrast: a wakeup, **not** a broker;
push the wait into a cheap child, **don't** poll from the expensive thing;
`iac recv` **not** a socket. That is the thesis, it is the paper's title, and it
is load-bearing everywhere it appears. Do not cull it for symmetry with the
other repositories. Cull the contrasts that are decoration; this one is the
claim.
