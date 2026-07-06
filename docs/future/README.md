# Future work — idea lifecycle

Every unexplored idea, deferred decision, or open investigation lives here as
ONE self-contained markdown doc, and its **directory is its status**:

```
docs/future/
├── not-started/                ideas waiting to be picked up
├── active/                     currently being investigated/implemented
└── complete/
    ├── implemented/            investigated → shipped to the product
    └── not-implemented/        investigated → deliberately not shipped
```

The split under `complete/` is the point of the system: a quick view of which
ideas bore fruit and which didn't. **Negative results are wins** — a doc in
`not-implemented/` with a measured "why not" saves the next person from
re-doing the investigation. Idea docs are never deleted.

The whole pipeline at a glance:

```sh
find docs/future -name '*.md' -not -name README.md | sort
```

## Doc format

Each doc opens with a status header, then enough context that whoever picks
it up (human or agent) needs nothing else to begin:

```markdown
# <short imperative title>

Status: not-started | active | complete/implemented | complete/not-implemented
Opened: YYYY-MM-DD
Issue: #N                        (linked GitHub issue)
Concluded: YYYY-MM-DD            (complete/* only)
Outcome: <one line>              (complete/* only)

## Why / evidence      — what we know, with measured numbers + file:line
## Plan                — a concrete starting plan
## Acceptance          — how we'll know it worked (tests/evals/benchmarks)
```

The `Status:` line and the containing directory must always agree — move the
file (`git mv`) and update the line in the same change.

## GitHub issues

Every idea doc is mirrored by a GitHub issue (label `future`), linked via the
`Issue: #N` header line. The bucket ↔ issue mapping is native GitHub:

| Bucket | Issue state |
|---|---|
| not-started / active | open (`future` label) |
| complete/implemented | closed as **completed** |
| complete/not-implemented | closed as **not planned** |

So the GitHub issues view gives the same fruit/no-fruit picture as the
directory tree, and `/next-idea` keeps both in sync automatically.

## Working the pipeline

Use the project skill: **`/next-idea`** (optionally `/next-idea <doc-name>`).
It picks a doc from `not-started/`, moves it to `active/`, runs the
investigation, appends a Results section, then asks whether to implement.
Implemented work ships with tests, `uv run axon eval null` verification, and
benchmarks where performance is claimed; either way the doc lands in the
right `complete/` bucket with its outcome recorded. See
`.claude/skills/next-idea/SKILL.md` for the full procedure.
