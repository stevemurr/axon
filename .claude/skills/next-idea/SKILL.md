---
name: next-idea
description: Pick the next future-work idea from docs/future, investigate it, record results, and (on approval) implement it with tests/evals/benchmarks — managing the lifecycle buckets and the linked GitHub issue automatically. Use when the user says "next idea", "work the next future item", or names a specific idea doc.
---

# next-idea — work a future-idea doc through its lifecycle

The pipeline lives in `docs/future/` (see its README): a doc's directory IS
its status — `not-started/` → `active/` → `complete/implemented/` or
`complete/not-implemented/`. Idea docs are never deleted; negative results
are recorded, not discarded. Each doc is mirrored by a GitHub issue.

## 1. Pick

- If the user named a doc (args), use it. Otherwise list
  `docs/future/not-started/*.md` with one-line summaries and either pick the
  one the user's context points at or ask which to work (AskUserQuestion)
  when several are equally plausible.
- Read the doc fully. It is self-contained: Why/evidence, Plan, Acceptance.

## 2. Activate (bucket + issue)

- `git mv docs/future/not-started/<doc>.md docs/future/active/` and update
  its `Status:` line to `active`.
- GitHub issue sync (`gh` CLI, repo stevemurr/axon):
  - If the doc has no `Issue: #N` header line: create one —
    `gh issue create --title "<doc title>" --label future --body "<first
    paragraph + link to the doc path>"` — and add `Issue: #N` to the header.
  - Comment on the issue that investigation has started.

## 3. Investigate

- Follow the doc's Plan. Use the standard tooling — `uv run axon build /
  test / bench / eval null / eval ssl-comp` — and scratch experiments
  (never litter the repo; temp dirs only). Honest measurement over
  plausible reasoning; the repo's null-test rules apply (data-chunk
  compares; ORT-flake retry protocol per
  docs/future/*/ort_render_nondeterminism.md wherever it lives).
- Append a `## Results (<date>)` section to the doc: what was measured,
  numbers, surprises, and a clear verdict with an implementation plan and
  expected cost/gain.

## 4. Prompt the user (required — do not skip to implementation)

Present: the verdict, the measured evidence, the implementation plan, and
expected effort/risk. Then ask explicitly (AskUserQuestion): implement now,
or conclude as not-implemented? Never implement without this approval.

## 5a. If IMPLEMENT

- Do the work, including — non-negotiable:
  - **Tests**: unit tests for new behavior, whole suite green
    (`uv run axon test`).
  - **Evals**: `uv run axon eval null` against the pre-change bundle
    (byte-identical where the change claims transparency; otherwise record
    and justify the measured delta). Model-related work: `eval ssl-comp` or
    a new eval subcommand if the doc calls for one.
  - **Benchmarks** (when performance is claimed): instrumented
    `uv run axon bench` before/after numbers in the doc.
- Append `## Outcome` (what shipped, measured result, commit SHAs), set
  header `Status: complete/implemented`, `Concluded: <date>`, `Outcome:`
  one-liner; `git mv` the doc to `docs/future/complete/implemented/`.
- Issue: `gh issue close <N> --reason completed --comment "<outcome +
  commits>"`.

## 5b. If NOT implementing

- Append `## Outcome` with the reason (measured dead end / rejected trade-off
  / superseded by X), set `Status: complete/not-implemented`, `Concluded:`,
  `Outcome:`; `git mv` to `docs/future/complete/not-implemented/`.
- Issue: `gh issue close <N> --reason "not planned" --comment "<why>"`.

## Rules

- One doc per invocation unless the user says otherwise.
- The `Status:` line, the containing directory, and the GitHub issue state
  must agree after every transition — move + edit + gh in the same step, and
  commit the doc move together with the work it describes.
- Docs stay self-contained: someone reading only the doc must understand
  what was tried, what was found, and why it did or didn't ship.
- New ideas discovered mid-investigation become NEW docs in `not-started/`
  (+ a `future`-labeled issue) — never scope creep into the current one.
