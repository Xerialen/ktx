# KomodoBrain rides stock Frogbot through a cvar-gated, default-off seam

The kbot layer is wired into stock KTX Frogbot as additive `KBot_*` calls at
frogbot decision points — each behind a master `k_kbot_*` cvar that defaults
0 = bit-identical vanilla, plus a per-bot flag (`fb.kbot`). We chose this over
editing the frogbot logic in place so every feature is independently
A/B-testable on the bench and the fork stays a safe, reviewable superset of
upstream KTX.

## Consequences

- Many `k_kbot_*` cvars, read dynamically (no static table), set per bench run.
- New features MUST default off (0 = vanilla). A feature that changes behavior
  when its cvar is 0 is a bug.
- Upstream merges stay cheap because the frogbot files are only lightly touched
  at the call sites.
