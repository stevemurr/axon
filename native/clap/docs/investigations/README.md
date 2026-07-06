# Investigations

Parking lot for known-but-not-yet-scheduled technical investigations. Each file
is self-contained: the observed evidence, what is already ruled in/out, and a
concrete starting plan, so whoever picks it up (human or agent) needs nothing
else to begin.

Convention: one file per investigation, named `<topic>.md`, opening with a
**Status** line (`open` / `in progress` / `resolved — see <commit/doc>`). When
an investigation concludes, either move the useful findings into a permanent
doc under `native/clap/docs/` and mark this file resolved, or delete it if it
led nowhere (say why in the commit message).

Open investigations:

- `ort_render_nondeterminism.md` — same-binary renders on the ORT paths differ
  run-to-run at −86..−99 dBFS; matters only if bit-exact renders become a goal.
- `ort_audio_thread_allocations.md` — OrtMiniSession heap-allocates on the
  audio thread every inference call; RT-safety debt, not a throughput issue.
