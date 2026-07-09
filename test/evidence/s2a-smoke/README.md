# S2a smoke evidence: nano-bots navigate dm3 land

This directory holds the reproducible smoke-run artifacts for the S2a
"nano-bot navigates dm3 land" tracer bullet. The run demonstrates that a
nano-flagged team moves and collects items under the nano brain's own command
path, with vanilla `BotSetCommand()` skipped for nano bots when `k_nano 1`.

## How to reproduce

From WSL2 Ubuntu-24.04, with the current-head nano-ON `qwprogs.so` deployed to
the serverdir:

```bash
# build from the reviewed HEAD
git clone --branch nano --depth 5 https://github.com/Xerialen/ktx.git /tmp/ktx-nano
cd /tmp/ktx-nano
cmake -B build -DBOT_SUPPORT=ON -DNANO_SUPPORT=ON .
cmake --build build -j$(nproc)
cp build/qwprogs.so /home/xerial/kbot/serverdir/ktx/qwprogs.so

# run the smoke bench
cd /mnt/c/Users/benya/projects/quakeworld/komodobots2-nanobots
python3 lab/run_bench.py --matches 1 --map dm3 --timelimit 3 \
    --team-a nano --candidate-version nano-df681334 \
    --serverdir /home/xerial/kbot/serverdir \
    --mvdsv /home/xerial/kbot/mvdsv/build/linux-amd64/mvdsv \
    --ktx-source /tmp/ktx-nano \
    --set k_nano_debug 1 \
    --skip-ledger --skip-endshots --no-auto --jobs 1
```

`k_nano_debug 1` emits one `[nano] cmd ...` line per frame for each nano bot,
proving the command is coming from `Nano_EmitCmd()` rather than the vanilla
frogbot path.

## Build provenance

- KTX source tree: `/tmp/ktx-nano` (clone of `Xerialen/ktx` branch `nano`)
- Reviewed HEAD: `34ba9563ec6fea8434e17f64748afc8640f2d770`
- Deployed `qwprogs.so` SHA-256: `1d20f5c06df90e89652c8402c337552e340073425c8a9a8c8987f281af23bcea`
- `s1d-verify-build.sh` result: `PASS` (nano-OFF `.text` byte-identical to baseline)

## Run summary

- Run id: `20260709T135638Z-p28599`
- Map: `dm3`, timelimit: 3 minutes
- Candidate team (red): 4 nano bots (`nb:/ bro`, `nb:/ goldenboy`, `nb:/ grue`, `nb:/ tincan`)
- Control team (blue): 4 stock frogbots
- Match completed without server crashes

## Command-ownership proof

The `server.log.excerpt` in this directory contains the boot/activation lines
and representative `[nano] cmd ...` debug output. Across the full 3-minute run
the log contains **37 042** `[nano] cmd` lines, one per frame per active nano
bot. A representative sample:

```
[nano] frame active slot=2 name=nb:/ bro version=nano-df681334 time=11.309956
[nano] frame active slot=3 name=nb:/ goldenboy version=nano-df681334 time=11.309956
[nano] cmd slot=2 yaw=0.0 fwd=-597 side=400 buttons=0
[nano] cmd slot=3 yaw=0.0 fwd=0 side=800 buttons=0
[nano] cmd slot=4 yaw=0.0 fwd=0 side=-800 buttons=0
[nano] cmd slot=5 yaw=-0.0 fwd=586 side=-410 buttons=0
```

Because `BotSetCommand()` is skipped for nano-marked bots when `k_nano` is
active, these commands are authoritative for the candidate team.

## Movement and item-collection evidence

`ktxstats.json` shows the nano bots traveled and picked up items:

- `nb:/ bro`: max speed 823.9, SG pickups 5, health_25 taken 1, 1 kill
- `nb:/ goldenboy`: max speed 695.4, SG pickups 3, health_25 taken 1
- `nb:/ grue`: max speed 831.7, SG pickups 4
- `nb:/ tincan`: max speed 709.1, SG pickups 3, health_25 taken 1

The analyzer gates (`analyzer-gates.json`) show no server tick starvation
(`starved_fraction: 0.0`). The idle/stuck gate is not meaningful on this short
smoke run because the nano bots are still learning dm3 land routing and die to
water (S4a), so they respawn mid-match; the gate is calibrated for 5-minute
control-vs-control matches.

## Artifact hashes

- `demo.mvd` SHA-256: `bcb7afd05ec3a15d2d9472487c568077a6c0253a7aa5bda8a54fe6278d5d372d`
- `qwprogs.so` SHA-256: `1d20f5c06df90e89652c8402c337552e340073425c8a9a8c8987f281af23bcea`
- `run-meta.json` records `ktx_qwprogs.git_sha: 34ba9563ec6fea8434e17f64748afc8640f2d770`

The raw run directory is also preserved on this machine at:
`/mnt/c/Users/benya/projects/quakeworld/komodobots2-nanobots/artifacts/lab-runs/20260709T135638Z-p28599`.
