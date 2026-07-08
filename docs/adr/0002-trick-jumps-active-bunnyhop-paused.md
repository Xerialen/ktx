# Discrete trick jumps are active; continuous bunnyhop is paused

The movement work splits into two tracks that must NOT be conflated. **Trick
jumps** — discrete, scripted gapjump lanes (the `GJ_*` engine in `kbot_main.c`)
— are ACTIVE and part of canonical master. **Bunnyhop** — continuous strafe-jump
speed as a movement mode — is PAUSED and parked on `feat/kbot-*` branches (not
deleted).

Rationale (Lessons 6 & 8; evidence in KomodoBench `findings-log` +
`artifacts/records/speed-gate.json`): naive bunny and the decoupled carve-motor
each cost ~25–32 frags, because the movement channel is load-bearing for combat
— a bot can't strafe-dodge, position, or hold-for-the-shot while a motor carves
it for traversal. Discrete trick jumps are reachability edges that fire only
when no enemy is watching, so they don't fight combat for the movement channel.

## Consequences

- Gapjump / `GJ_*` code stays on mainline and is treated as a first-class
  capability.
- The bunny / carve-motor experiments stay parked, retrievable from
  `feat/kbot-*` branches, and are not reopened without a fundamentally different
  navigation stack.
