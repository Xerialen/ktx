# S2a smoke evidence: nano-bots navigate dm3 land

This directory holds the reproducible smoke-run artifacts for the S2a
"nano-bot navigates dm3 land" tracer bullet. The run demonstrates that a
nano-flagged team moves and collects items under the nano brain's own command
path, with vanilla `BotSetCommand()` skipped for nano bots when `k_nano 1`.

## How to reproduce

From WSL2 Ubuntu-24.04, with a nano-ON `qwprogs.so` deployed to the serverdir:

```bash
cd /mnt/c/Users/benya/projects/quakeworld/komodobots2-nanobots
python3 lab/run_bench.py --matches 1 --map dm3 --timelimit 3 \
    --team-a nano --candidate-version nano-df681334 \
    --serverdir /home/xerial/kbot/serverdir \
    --mvdsv /home/xerial/kbot/mvdsv/build/linux-amd64/mvdsv \
    --ktx-source /mnt/c/Users/benya/projects/quakeworld/engine/ktx-nanobots \
    --set k_nano_debug 1 \
    --skip-ledger --skip-endshots --no-auto --jobs 1
```

`k_nano_debug 1` emits one `[nano] cmd ...` line per frame for each nano bot,
proving the command is coming from `Nano_EmitCmd()` rather than the vanilla
frogbot path.

## Build provenance

- KTX source tree: `/mnt/c/Users/benya/projects/quakeworld/engine/ktx-nanobots`
- PR head SHA at run time: `2699a92ecc777638c5d457418d122049e2355699`
- Deployed `qwprogs.so` SHA-256: `4fd602fb8aef4c9502d1648e7a7842aa25e5fec5be887e0f4a6481542dbf1bec`
- `s1d-verify-build.sh` result: `PASS` (nano-OFF `.text` byte-identical to baseline)

## Run summary

- Run id: `20260709T132821Z-p28599`
- Map: `dm3`, timelimit: 3 minutes
- Candidate team (red): 4 nano bots (`nb:/ bro`, `nb:/ goldenboy`, `nb:/ grue`, `nb:/ tincan`)
- Control team (blue): 4 stock frogbots
- Match completed without server crashes

## Command-ownership proof

The `server.log.excerpt` in this directory contains the boot/activation lines
and representative `[nano] cmd ...` debug output. Across the full 3-minute run
the log contains **38 647** `[nano] cmd` lines, one per frame per active nano
bot. A representative sample:

```
[nano] frame active slot=2 name=nb:/ bro version=nano-df681334 time=12.477139
[nano] frame active slot=3 name=nb:/ goldenboy version=nano-df681334 time=12.477139
[nano] cmd slot=2 yaw=0.0 fwd=-759 side=252 buttons=0
[nano] cmd slot=3 yaw=0.0 fwd=715 side=357 buttons=0
[nano] cmd slot=4 yaw=-0.0 fwd=0 side=800 buttons=0
[nano] cmd slot=5 yaw=0.0 fwd=758 side=-252 buttons=0
```

Because `BotSetCommand()` is now skipped for nano-marked bots when `k_nano` is
active, these commands are authoritative for the candidate team.

## Movement and item-collection evidence

`ktxstats.json` shows the nano bots traveled and picked up items:

- `nb:/ bro`: max speed 767.9, SG pickups 6, health_25 taken 1, 1 kill
- `nb:/ goldenboy`: SG pickups/drops, health items taken
- All four nano bots appear in the stats with non-zero movement speeds and item takes

The analyzer gates (`analyzer-gates.json`) show no server tick starvation
(`starved_fraction: 0.0`). The idle/stuck gate is not meaningful on this short
smoke run because the nano bots are still learning dm3 land routing and die to
water (S4a), so they respawn mid-match; the gate is calibrated for 5-minute
control-vs-control matches.

## Artifact hashes

- `demo.mvd` SHA-256: `74d9bf475fda53113921dcac2b285afeec3b72c5d384cd89a895feeb3fe367c9`
- `qwprogs.so` SHA-256: `4fd602fb8aef4c9502d1648e7a7842aa25e5fec5be887e0f4a6481542dbf1bec`

The raw run directory is also preserved on this machine at:
`/mnt/c/Users/benya/projects/quakeworld/komodobots2-nanobots/artifacts/lab-runs/20260709T132821Z-p28599`.
