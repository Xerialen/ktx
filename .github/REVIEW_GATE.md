# Review Gate Automation

This repository uses the same gate flow as the active Xerialen project repos.

- `PR Tests` is the deterministic CI floor.
- New PR commits reset terminal labels to `gate: reviewing`.
- A reviewer posts a top-level current-head gate comment containing `DECISION`, `LABEL`, and `HEAD_SHA`.
- `gate: ready` plus passing non-gate checks allows the deterministic merge executor to squash-merge into the default branch after its cooldown.
- `gate: blocked` prevents merge.
- Draft PRs cannot keep `gate: ready`.
