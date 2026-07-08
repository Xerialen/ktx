/*
 nano.h -- nano-bots: the rtx-port peer brain (qw-ctf/rtx bots in KTX).

 nano is a third server-side brain kind alongside frog and kbot. Unlike kbot
 (a decision-layer overlay that delegates movement/nav/combat 100% to stock
 frogbot logic), nano brings its OWN navigation stack -- a navmesh generated
 from the BSP clip hull, ported from qw-ctf/rtx -- plus rtx-style perception,
 combat and movement intent. It is a peer brain, not an overlay.

 Seating:  botcmd addnano -> FrogbotsAddNano_f -> Nano_MarkBot flags the bot.
 Dispatch: BotsThinkTime() routes a nano-flagged bot to Nano_Frame(); when that
           returns true the stock frogbot think is skipped entirely.

 Non-interference: the whole feature sits behind cmake NANO_SUPPORT (default
 OFF). With NANO_SUPPORT off, no nano source is compiled, no cvars register,
 no dispatch branch exists, and the gedict_t/fb struct is untouched -- the
 built qwprogs.so is byte-identical to the stock frogbot baseline. nano's
 per-bot flag lives in a nano-side runtime array (never in gedict_t), so even
 with NANO_SUPPORT on the shared bot structs are unchanged.

 Expects g_local.h to have been included first (KTX header convention).
*/
#ifndef KTX_NANO_H
#define KTX_NANO_H

#ifdef NANO_SUPPORT

#define NANO_VERSION "nano-df681334"	// rtx main @ df681334 (2026-07-08)

// nano flag states (nano-side array, indexed by edict number).
#define NANO_STATE_OFF     0	// stock frogbot (default)
#define NANO_STATE_MARKED  1	// nano-bot, identity not yet logged this match
#define NANO_STATE_ACTIVE  2	// nano-bot, one-time frame log emitted

// True iff this bot is flagged nano (any state > OFF).
qbool Nano_IsMarked(gedict_t *self);

// Flag a freshly added bot as a nano-bot: sets the nano flag, stamps identity
// markers (userinfo "nano" key + "nb:" name prefix) and logs to the console.
void Nano_MarkBot(gedict_t *bot);

// Clear a bot's nano flag (on slot reuse by a plain addbot). Wired in S4.
void Nano_ClearMark(gedict_t *bot);

// Per-frame brain entry point, called from BotsThinkTime() for nano-flagged
// bots. Returns true if the brain fully handled this frame's think
// (BotsThinkTime then skips the stock frogbot logic); false to fall through.
//
// S0 scaffolding: always returns false (pure delegation, like kbot WP2.1) --
// proves the seam with zero behavior change. The real rtx-port brain
// (navmesh, perception, combat, movement) arrives in later stages.
qbool Nano_Frame(gedict_t *self);

#endif // NANO_SUPPORT

#endif // KTX_NANO_H
