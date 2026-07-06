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

#define HM_VERSION "hm-0.2.0-tmmodel"

// Knowledge sources, in rising order of directness. Kept on every snapshot
// so the trust/merge policy (told vs seen) stays explicit.
#define HM_SRC_NONE     0
#define HM_SRC_KILLFEED 1	// read in the frag feed (respawned somewhere)
#define HM_SRC_TOLD     2	// teamsay report (S5)
#define HM_SRC_SEEN     3	// direct line of sight

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
// HM_Active(bot) && HM_CapTmInfo() -- direct entity reads are omniscience.
const hm_tm_t* HM_TeammateInfo(gedict_t *bot, gedict_t *mate);

// States for fb.hm (per-bot override; memset-0 default = inherit global)
#define HM_INHERIT 0	// follow cvar k_hm
#define HM_ON      1	// humanmode forced on for this bot
#define HM_OFF     2	// humanmode forced off for this bot

// True when humanmode governs this bot right now (per-bot override first,
// then the global k_hm cvar). False for non-bots and world.
qbool HM_Active(gedict_t *bot);

// Capability gates: effective only when HM_Active(). Each reads its cvar
// with default ON so a bare "k_hm 1" enables the full model.
qbool HM_CapEmit(void);		// k_hm_emit
qbool HM_CapParse(void);	// k_hm_parse
qbool HM_CapTmInfo(void);	// k_hm_tminfo
qbool HM_CapItemInfo(void);	// k_hm_iteminfo

// "botcmd hm ..." handler: no args = show status; "<slot|all> <on|off|inherit>"
// sets the per-bot override.
void HM_BotCmd(void);

// Per-frame entry, called from BotPreThink for bots. Handles one-time
// activation (3-letter k_nick tag, console log) and, once active, the
// emit scheduler (S4).
void HM_Frame(gedict_t *self);

// Player (re)spawn hook: resets the bot's own perception state and queues
// the fresh-spawn report (S2/S4).
void HM_ClientEnters(gedict_t *self);

// Item pickup event: the taker knows (source=self), PVS-visible bystanders
// in humanmode learn it too (source=seen); everyone else stays ignorant (S3).
void HM_ItemTaken(gedict_t *item, gedict_t *player);

// Obituary/killfeed event: humanmode teammates of the victim invalidate
// their held snapshot of the victim (S2).
void HM_Killfeed(gedict_t *victim, gedict_t *attacker);

// Incoming teamsay for a humanmode bot receiver: parse (multi-clan grammar)
// and update teammate snapshots / item beliefs (S5).
void HM_ParseTeamsay(gedict_t *receiver, gedict_t *sender, const char *text);

#endif // BOT_SUPPORT

#endif // KTX_HM_H
