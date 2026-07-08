# Graph Report - .  (2026-07-08)

## Corpus Check
- 21 files · ~71,521 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 393 nodes · 915 edges · 26 communities (18 shown, 8 thin omitted)
- Extraction: 96% EXTRACTED · 4% INFERRED · 0% AMBIGUOUS · INFERRED: 37 edges (avg confidence: 0.81)
- Token cost: 0 input · 0 output

## Community Hubs (Navigation)
- [[_COMMUNITY_Bench Driver (run_bench)|Bench Driver (run_bench)]]
- [[_COMMUNITY_Movement & MoveProbe seam|Movement & MoveProbe seam]]
- [[_COMMUNITY_Gapjump Engine|Gapjump Engine]]
- [[_COMMUNITY_Ledger & Scoring|Ledger & Scoring]]
- [[_COMMUNITY_HARVEST Possession|HARVEST Possession]]
- [[_COMMUNITY_Min-Client Seating Shim|Min-Client Seating Shim]]
- [[_COMMUNITY_KDLOG Telemetry|KDLOG Telemetry]]
- [[_COMMUNITY_Tactical Dials|Tactical Dials]]
- [[_COMMUNITY_Decision Models|Decision Models]]
- [[_COMMUNITY_Cvar-Gated Seam|Cvar-Gated Seam]]
- [[_COMMUNITY_Behavior & Promotion|Behavior & Promotion]]
- [[_COMMUNITY_Results Table|Results Table]]
- [[_COMMUNITY_Trick Jumps|Trick Jumps]]
- [[_COMMUNITY_ES Optimizer|ES Optimizer]]
- [[_COMMUNITY_HARVEST Concepts|HARVEST Concepts]]
- [[_COMMUNITY_Speed Gate|Speed Gate]]
- [[_COMMUNITY_Report Renderer|Report Renderer]]
- [[_COMMUNITY_Bunnyhop (paused)|Bunnyhop (paused)]]
- [[_COMMUNITY_Combat IQ|Combat IQ]]
- [[_COMMUNITY_Communication (MM2)|Communication (MM2)]]
- [[_COMMUNITY_Weapons Module|Weapons Module]]
- [[_COMMUNITY_ktxstats Outcome|ktxstats Outcome]]
- [[_COMMUNITY_Mechanics|Mechanics]]
- [[_COMMUNITY_Rotation & Control|Rotation & Control]]
- [[_COMMUNITY_Teamplay|Teamplay]]

## God Nodes (most connected - your core abstractions)
1. `gedict_t` - 24 edges
2. `BotApplyMoveProbe()` - 24 edges
3. `KBot_GapjumpFrame()` - 20 edges
4. `Any` - 19 edges
5. `gedict_t` - 18 edges
6. `run_match()` - 17 edges
7. `KBot_HarvestHoldFrame()` - 16 edges
8. `Path` - 16 edges
9. `GJ_Cross()` - 15 edges
10. `GJ_ApproachFrame()` - 15 edges

## Surprising Connections (you probably didn't know these)
- `KomodoBrain (kbot layer)` --semantically_similar_to--> `KomodoBrain (decision layer)`  [INFERRED] [semantically similar]
  brain/CONTEXT.brain.md → harness/CONTEXT.harness.md
- `trick jump (GJ engine)` --semantically_similar_to--> `gapjump`  [INFERRED] [semantically similar]
  brain/CONTEXT.brain.md → harness/CONTEXT.harness.md
- `trick jump (GJ engine)` --references--> `KBot_GapjumpFrame()`  [INFERRED]
  brain/CONTEXT.brain.md → brain/kbot_main.c
- `dials module` --references--> `KBot_Dial()`  [INFERRED]
  brain/CONTEXT.brain.md → brain/kbot_dials.c
- `dlog module (KDLOG)` --conceptually_related_to--> `KDLOG (decision trace)`  [INFERRED]
  brain/CONTEXT.brain.md → harness/CONTEXT.harness.md

## Import Cycles
- None detected.

## Hyperedges (group relationships)
- **The five behaveval categories** — harness_context_harness_rotation_and_control, harness_context_harness_teamplay, harness_context_harness_mechanics, harness_context_harness_combat_iq, harness_context_harness_communication [EXTRACTED 1.00]
- **The five tactical dials (rattar)** — brain_context_brain_engage, brain_context_brain_hoard, brain_context_brain_adhere, brain_context_brain_quad_dial, brain_context_brain_share [EXTRACTED 1.00]
- **HARVEST mechanisms driven by carried-value V** — brain_context_brain_carried_value, brain_context_brain_anchoring, brain_context_brain_hold, brain_context_brain_quad_window [EXTRACTED 1.00]

## Communities (26 total, 8 thin omitted)

### Community 0 - "Bench Driver (run_bench)"
Cohesion: 0.06
Nodes (56): acquire_canonical_lock(), analyze_demo(), apply_auto_governor(), binary_provenance(), brain_stamp(), _bucket_alive(), build_roster(), build_server_cfg() (+48 more)

### Community 1 - "Movement & MoveProbe seam"
Cohesion: 0.11
Nodes (53): ApplyPhysics(), AverageTraceAngle(), BestJumpingDirection(), BotApplyMoveProbe(), BotApplyMoveProbeReplay(), BotLogMoveProbeCommand(), BotLogMoveProbeQwdEvent(), BotLogMoveProbeReplayEvent() (+45 more)

### Community 2 - "Gapjump Engine"
Cohesion: 0.17
Nodes (41): gedict_t, qbool, vec3_t, GJ_AirTime(), GJ_ApproachFrame(), GJ_Bearing(), GJ_BuildFrame(), GJ_CarveTarget() (+33 more)

### Community 3 - "Ledger & Scoring"
Cohesion: 0.16
Nodes (34): _aggregate_version(), build(), _find_first(), _int(), _is_two_teams_four_each(), main(), normalize_match(), _normalize_player() (+26 more)

### Community 4 - "HARVEST Possession"
Cohesion: 0.18
Nodes (28): gedict_t, qbool, vec3_t, KBot_CarriedValue(), KBot_EnemyClassEst(), KBot_HarvestAnchorFactor(), KBot_HarvestAnchorShim(), KBot_HarvestDeathEvent() (+20 more)

### Community 5 - "Min-Client Seating Shim"
Cohesion: 0.13
Nodes (16): build_userinfo(), main(), parse_args(), Namespace, QWMinClient, botcmd lines sent once right after sign-on.      The first auto `botcmd addbot, All reliable string commands sent once right after sign-on., Return the tiny allowlisted subset of stuffed client commands we execute. (+8 more)

### Community 6 - "KDLOG Telemetry"
Cohesion: 0.20
Nodes (22): gedict_t, qbool, KDLog_ActiveFor(), KDLog_AnchorMaybe(), KDLog_CandCompareDesc(), KDLog_CandSortDesc(), KDLog_Enemy(), KDLog_EntDesc() (+14 more)

### Community 7 - "Tactical Dials"
Cohesion: 0.20
Nodes (20): adhere dial, dials module, engage dial, hoard dial, quad dial, share dial, tunables (k_kbot_* surface), gedict_t (+12 more)

### Community 8 - "Decision Models"
Cohesion: 0.32
Nodes (19): gedict_t, qbool, vec3_t, KBot_ActiveModel(), KBot_ExchangeDecision(), KBot_GoalCategory(), KBot_KaptenAllocate(), KBot_KaptenScaleGoal() (+11 more)

### Community 9 - "Cvar-Gated Seam"
Cohesion: 0.22
Nodes (11): Cvar-gated default-off seam, cvar-gate (k_kbot_* default 0), kbot flag (fb.kbot), KomodoBrain (kbot layer), Seam (KBot_* inject points), actuation (vanilla Frogbot), fbots (skill-20 opponents), Frogbot (+3 more)

### Community 10 - "Behavior & Promotion"
Cohesion: 0.25
Nodes (9): dlog module (KDLOG), Promote kbot changes on behavior, not frag margin, behavior (what the kbot decided), elite (top-tier human 4on4), guardrail, KDLOG (decision trace), margin (frag differential), Milton (human reference player) (+1 more)

### Community 11 - "Results Table"
Cohesion: 0.42
Nodes (8): base_version(), build_table(), load_games(), main(), params_for(), Path, Concatenate games from all ledgers, deduped by run_id; sum invalids., run_time_cest()

### Community 12 - "Trick Jumps"
Cohesion: 0.33
Nodes (7): Trick jumps active on master, discipline veto, kbot_main module, trick jump (GJ engine), chainhop, gapjump, lane

### Community 13 - "ES Optimizer"
Cohesion: 0.57
Nodes (6): bench_candidate(), clamp(), fmt_value(), main(), save_state(), stamp_for()

### Community 14 - "HARVEST Concepts"
Cohesion: 0.33
Nodes (6): zone anchoring, carried-value scalar V, harvest module, position hold/guard, models module, quad-window commitment

### Community 15 - "Speed Gate"
Cohesion: 0.60
Nodes (4): collect(), main(), Path, verdicts()

### Community 16 - "Report Renderer"
Cohesion: 0.67
Nodes (3): format_report(), main(), Any

## Knowledge Gaps
- **34 isolated node(s):** `moveprobe_replay_frame_t`, `qbool`, `vec3_t`, `Namespace`, `Any` (+29 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **8 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `KBot_GapjumpFrame()` connect `Gapjump Engine` to `Movement & MoveProbe seam`, `Trick Jumps`, `KDLOG Telemetry`?**
  _High betweenness centrality (0.034) - this node is a cross-community bridge._
- **Why does `KBot_HarvestHoldFrame()` connect `HARVEST Possession` to `Decision Models`, `Movement & MoveProbe seam`, `KDLOG Telemetry`, `Tactical Dials`?**
  _High betweenness centrality (0.031) - this node is a cross-community bridge._
- **Why does `BotSetCommand()` connect `Movement & MoveProbe seam` to `Gapjump Engine`, `HARVEST Possession`?**
  _High betweenness centrality (0.030) - this node is a cross-community bridge._
- **Are the 3 inferred relationships involving `KBot_GapjumpFrame()` (e.g. with `BotSetCommand()` and `trick jump (GJ engine)`) actually correct?**
  _`KBot_GapjumpFrame()` has 3 INFERRED edges - model-reasoned connections that need verification._
- **What connects `moveprobe_replay_frame_t`, `qbool`, `vec3_t` to the rest of the system?**
  _71 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `Bench Driver (run_bench)` be split into smaller, more focused modules?**
  _Cohesion score 0.056189640035118525 - nodes in this community are weakly interconnected._
- **Should `Movement & MoveProbe seam` be split into smaller, more focused modules?**
  _Cohesion score 0.10831586303284417 - nodes in this community are weakly interconnected._