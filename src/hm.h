/*
 hm.h -- mm2humanmode: information-realism + team comms for bots.

 A bot in humanmode must never read live game state it could not perceive:
 it knows only fixed map facts (spawns, respawn durations, routes) plus what
 it saw, heard, read on the HUD/killfeed, or was told over teamsay (mm2).
 That applies to enemies, items AND teammates. In exchange it talks like a
 real 4on4 player: the emit vocabulary is derived from 17,027 teamsays of
 team Book (komodobots docs/notes/book-teamsay-study.md) and the parse
 grammar from 98,468 teamsays across 142 clans (multiclan-teamsay-parse-
 study.md).

 Toggles: global cvar k_hm; per-bot override "botcmd hm <slot|all> <mode>";
 per-capability cvars k_hm_emit[_family], k_hm_parse, k_hm_tminfo,
 k_hm_iteminfo, k_hm_rate. k_hm 0 + no overrides == stock behavior,
 bit-identical.

 Expects g_local.h to have been included first (KTX header convention).
 */
#ifndef KTX_HM_H
#define KTX_HM_H

#ifdef BOT_SUPPORT

#define HMODE_VERSION "hm-0.5.0-parse"

// Knowledge sources, in rising order of directness. Kept on every snapshot
// so the trust/merge policy (told vs seen) stays explicit.
#define HMODE_SRC_NONE     0
#define HMODE_SRC_KILLFEED 1	// read in the frag feed (respawned somewhere)
#define HMODE_SRC_HEARD    2	// pickup/respawn sound in earshot, no LOS
#define HMODE_SRC_TOLD     3	// teamsay report (S5)
#define HMODE_SRC_SEEN     4	// direct line of sight (covers own pickups)

// What a humanmode bot believes about one teammate. While the teammate is
// in view the snapshot is refreshed every scan (fresh=true, the live
// team-overlay equivalence); once out of view it is HELD AS-IS until a
// teamsay report, a killfeed death, or re-sighting replaces it (owner
// decision 2026-07-06, komodobots docs/notes/humanmode-teammate-model.md).
typedef struct hm_tm_s
{
	int source;			// HM_SRC_*; NONE = no information at all
	qbool fresh;		// teammate in view right now
	qbool loc_known;	// org below is meaningful
	float time;			// when this info was captured
	float health;
	float armor;
	int items;			// IT_* flags at capture time (weapons + powerups)
	float rockets;
	float cells;
	vec3_t org;			// last known position
} hm_tm_t;

// Read-only view of what `bot` currently believes about `mate`.
// NULL when there is no information (or humanmode inactive). This is THE
// sanctioned way for decision code to ask about teammates when
// HMode_Active(bot) && HMode_CapTmInfo() -- direct entity reads are omniscience.
const hm_tm_t* HMode_TeammateInfo(gedict_t *bot, gedict_t *mate);

// States for fb.hm (per-bot override; memset-0 default = inherit global)
#define HMODE_INHERIT 0	// follow cvar k_hm
#define HMODE_ON      1	// humanmode forced on for this bot
#define HMODE_OFF     2	// humanmode forced off for this bot

// True when humanmode governs this bot right now (per-bot override first,
// then the global k_hm cvar). False for non-bots and world.
qbool HMode_Active(gedict_t *bot);

// Capability gates: effective only when HMode_Active(). Each reads its cvar
// with default ON so a bare "k_hm 1" enables the full model.
qbool HMode_CapEmit(void);		// k_hm_emit
qbool HMode_CapParse(void);	// k_hm_parse
qbool HMode_CapTmInfo(void);	// k_hm_tminfo
qbool HMode_CapItemInfo(void);	// k_hm_iteminfo

// "botcmd hm ..." handler: no args = show status; "<slot|all> <on|off|inherit>"
// sets the per-bot override.
void HMode_BotCmd(void);

// Once per map (SecondFrame): item edicts are recreated on map load, so the
// item registry and all per-bot humanmode state reset here.
void HMode_MapInit(void);

// Per-frame entry, called from BotPreThink for bots. Handles one-time
// activation (3-letter k_nick tag, console log) and the teammate scan.
void HMode_Frame(gedict_t *self);

// The humanmode replacement for the stock BotPeriodicMessages block:
// Book-derived triggers/rates through the same ezQuake-standard builders
// (status is the workhorse), metered by a token-bucket governor inside the
// Book traffic band (~11/min median, 23/min ceiling, k_hm_rate scales).
// Call only for hm-active bots in teamplay with match running (S4).
void HMode_PeriodicMessages(gedict_t *self);

// Death report: "lost {loc} {n}" on every death (builder upgrades to
// "quad over"/"DROPPED RL" from death state). Replaces the stock
// conditional lost call for hm-active bots (S4).
void HMode_DeathReport(gedict_t *self);

// Player (re)spawn hook: resets the bot's own perception state and queues
// the fresh-spawn report (S2/S4).
void HMode_ClientEnters(gedict_t *self);

// Item pickup event: the taker knows (source=self), bystanders with line of
// sight learn it too (source=seen), bots in earshot of the pickup sound get
// the timing anchor without the position confidence (source=heard, the #250
// div1 marker); everyone else stays ignorant (S3).
void HMode_ItemTaken(gedict_t *item, gedict_t *player);

// Item respawn event (SUB_regen): the respawn sound is a real perception
// cue -- humanmode bots that see or hear it learn the item is up (S3).
void HMode_ItemRespawned(gedict_t *item);

// The humanmode replacement for reading item->fb.goal_respawn_time (engine
// truth) in goal evaluation: returns the bot's BELIEF about when this item
// respawns. Unknown = 0 ("could be up, go look"). Falls through to the
// engine value when humanmode/k_hm_iteminfo is off or the item is not a
// tracked major (majors = armors, mega, guns, powerups -- what humans time).
float HMode_ItemRespawnTime(gedict_t *bot, gedict_t *item);

// Obituary/killfeed event: humanmode teammates of the victim invalidate
// their held snapshot of the victim (S2).
void HMode_Killfeed(gedict_t *victim, gedict_t *attacker);

// Incoming teamsay for a humanmode bot receiver: parse (multi-clan grammar)
// and update teammate snapshots / item beliefs (S5).
void HMode_ParseTeamsay(gedict_t *receiver, gedict_t *sender, const char *text);

#endif // BOT_SUPPORT

#endif // KTX_HM_H
