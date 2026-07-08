# graphify graph — provenance & rebuild

**Cross-repo graph** covering two repos, so it has no single source root
(that's why `.graphify_root` is intentionally absent — `graphify update` can't
run here; rebuild instead).

## Scope (staged, then built as one root)
- **brain/** — ktx KomodoBrain: `src/kbot_*.c`, `kbot.h`, `bot_movement.c`
  (from the `docs/kbot-architecture` worktree of `engine/ktx`), plus
  `CONTEXT.md`, ADR 0001/0002, `KBOT_CVARS.md`.
- **harness/** — KomodoBench bench: all `lab/*.py` from `komodobots2`
  (`run_bench, ledger, report, qw_min_client, results_table, speed_gate,
  dial_report, optimize_es`), plus `CONTEXT.md` and ADR 0001.

## Files
- `graph.json` — full cross-repo graph (393 nodes / 915 edges / 26 communities)
- `graph.brain-only.json` / `graph.harness-only.json` — per-side projections
- `graph.html` — interactive viewer · `GRAPH_REPORT.md` — audit report

## Rebuild
Re-stage the files above into one folder with `brain/` + `harness/` subdirs and
re-run the graphify pipeline (AST for code = free; one subagent for the 5 .md
concept files). Built 2026-07-08.
