/*
 hm.c -- mm2humanmode core.

 See hm.h for the model. This stage wires the toggles only:
 - HMode_Active(): per-bot fb.hm override, else global cvar k_hm
 - capability gates (k_hm_emit / k_hm_parse / k_hm_tminfo / k_hm_iteminfo)
 - "botcmd hm" per-bot control
 - one-time activation: 3-letter k_nick tag (the Book self-prefix
   convention: 9/10 Book players lead every teamsay with a 3-letter tag,
   and the tag need not match the nick) + console log
 All perception/emit/parse hooks are declared but inert, so k_hm 0 and
 stock servers are bit-identical.
 */
#ifdef BOT_SUPPORT

#include "g_local.h"
#include "hm.h"

// ---- item-belief types (S3) ----

// Majors only: armors, mega, guns, powerups -- the items humans actually
// time. Minor items (shards, small health, ammo boxes) fall through to the
// engine value; they respawn in seconds and nobody times them (documented
// realism leak, see plan).
#define HMODE_MAX_ITEMS 64

// Audible range for pickup/respawn sounds without line of sight. QW sounds
// at ATTN_NORM are practically inaudible beyond ~1000 units.
#define HMODE_HEARD_RADIUS 1000.0f

typedef struct hm_item_belief_s
{
	int source;			// HM_SRC_* (SEEN covers self-pickup too); NONE = never witnessed
	float taken_time;	// when the bot believes it was taken
	float respawn_at;	// when the bot believes it comes back (0 = assume up)
} hm_item_belief_t;

// Global registry: stable index per major-item entity for the per-bot
// belief arrays. Item edicts persist for the whole map, so registration is
// lazy and never invalidated.
static gedict_t *hm_item_ents[HMODE_MAX_ITEMS];
static int hm_item_count;

// Per-bot humanmode runtime state, indexed by entity number (1..MAX_CLIENTS).
// Kept out of fb_entvars_t: every edict carries fb, only client slots need
// this, and the QVM bss budget is tight.
typedef struct hm_bot_s
{
	qbool active_logged;			// one-time activation done (tag + log)
	char tag[4];					// 3-letter lowercase self-prefix
	float next_tm_scan;				// visibility-scan throttle (engine trap)
	hm_tm_t tm[MAX_CLIENTS + 1];	// beliefs about teammates, by entity num
	hm_item_belief_t items[HMODE_MAX_ITEMS];	// beliefs about major items

	// Emit scheduler (S4)
	float emit_tokens;				// token bucket, 1 token = 1 message
	float emit_refill_time;			// last refill timestamp
	float next_periodic;			// next periodic status-class report
	float spawn_report_at;			// pending fresh-spawn report (0 = none)
	float coming_at;				// pending post-spawn "coming" (0 = none)
} hm_bot_t;

static hm_bot_t hm_bots[MAX_CLIENTS + 1];

static void HMode_TookReport(gedict_t *self, gedict_t *item); // emit, S4

static hm_bot_t* HMode_Slot(gedict_t *bot)
{
	int entity = NUM_FOR_EDICT(bot);

	if ((entity < 1) || (entity > MAX_CLIENTS))
	{
		return NULL;
	}

	return &hm_bots[entity];
}

qbool HMode_Active(gedict_t *bot)
{
	if (!bot || !bot->isBot)
	{
		return false;
	}

	if (bot->fb.hm == HMODE_ON)
	{
		return true;
	}

	if (bot->fb.hm == HMODE_OFF)
	{
		return false;
	}

	return cvar("k_hm") != 0;
}

// Capability cvars default ON when unset, so "k_hm 1" alone gives the full
// model. cvar() returns 0 for unset cvars, hence the explicit string check.
static qbool HMode_CapCvar(const char *name)
{
	char buf[16];

	trap_cvar_string(name, buf, sizeof(buf));

	if (strnull(buf))
	{
		return true; // unset = default on
	}

	return cvar(name) != 0;
}

qbool HMode_CapEmit(void)
{
	return HMode_CapCvar("k_hm_emit");
}

qbool HMode_CapParse(void)
{
	return HMode_CapCvar("k_hm_parse");
}

qbool HMode_CapTmInfo(void)
{
	return HMode_CapCvar("k_hm_tminfo");
}

qbool HMode_CapItemInfo(void)
{
	return HMode_CapCvar("k_hm_iteminfo");
}

// ---- 3-letter self-prefix tag ----

// Derive the tag from the bot name: skip the "kb:" identity prefix and any
// non-letters, take the first three letters lowercased. Falls back to
// "b<slot>" style if the name yields fewer than 3 letters.
static void HMode_DeriveTag(gedict_t *bot, char *out, int outsize)
{
	const char *src = bot->netname;
	int i = 0;

	if (!strncmp(src, "kb:", 3))
	{
		src += 3;
	}

	while (*src && (i < 3) && (i < outsize - 1))
	{
		char c = *src++;

		// QW names can carry colored/high-bit glyphs; fold the common
		// red-text range back to ascii before classifying.
		if ((unsigned char)c >= 128)
		{
			c = (char)((unsigned char)c - 128);
		}

		if ((c >= 'A') && (c <= 'Z'))
		{
			c = (char)(c - 'A' + 'a');
		}

		if ((c >= 'a') && (c <= 'z'))
		{
			out[i++] = c;
		}
	}

	while ((i < 3) && (i < outsize - 1))
	{
		// Name too exotic: pad with the slot number (still reads like a tag).
		out[i++] = (char)('0' + (NUM_FOR_EDICT(bot) % 10));
	}

	out[i] = '\0';
}

// Make the tag unique among same-team humanmode bots: on collision, replace
// the last letter with the slot digit (Book precedent: stepcop tags "nig" --
// the tag needs to be stable and short, not derived from the nick).
static void HMode_UniqueTag(gedict_t *bot, char *tag)
{
	int j;

	for (j = 1; j <= MAX_CLIENTS; j++)
	{
		gedict_t *other = &g_edicts[j];
		hm_bot_t *oslot;

		if (!other->isBot || (other == bot) || (other->ct != ctPlayer))
		{
			continue;
		}

		if (other->k_teamnum != bot->k_teamnum)
		{
			continue;
		}

		oslot = HMode_Slot(other);

		if (oslot && oslot->active_logged && streq(oslot->tag, tag))
		{
			tag[2] = (char)('0' + (NUM_FOR_EDICT(bot) % 10));

			return;
		}
	}
}

// One-time humanmode activation for a bot: derive + stamp the k_nick tag
// (TeamplayMM2 prefixes k_nick to every teamsay, which is exactly the Book
// convention) and log to the server console for run evidence.
static void HMode_Activate(gedict_t *self)
{
	hm_bot_t *slot = HMode_Slot(self);

	if (!slot || slot->active_logged)
	{
		return;
	}

	HMode_DeriveTag(self, slot->tag, sizeof(slot->tag));
	HMode_UniqueTag(self, slot->tag);
	trap_SetBotUserInfo(NUM_FOR_EDICT(self), "k_nick", slot->tag, 0);
	slot->active_logged = true;

	G_cprint("[hm] slot=%d name=%s tag=%s version=%s emit=%d parse=%d tminfo=%d iteminfo=%d\n",
				NUM_FOR_EDICT(self), self->netname, slot->tag, HMODE_VERSION,
				(int)HMode_CapEmit(), (int)HMode_CapParse(), (int)HMode_CapTmInfo(),
				(int)HMode_CapItemInfo());
}

// ---- teammate-state model (S2) ----

// LOS check with an explicit viewer (VisibleEntity() relies on the global
// self). Same three-trace shape as bot_bothelp.c PointVisible/VisibleEntity,
// which is also the perception definition the stock enemypwr spotting uses:
// PVS filter first (visible_to), then eye-to-body tracelines. No view cone,
// consistent with the enemy-perception model.
static qbool HMode_TraceVisible(gedict_t *viewer, vec3_t point)
{
	traceline(PASSVEC3(viewer->s.v.origin), PASSVEC3(point), true, viewer);

	return (g_globalvars.trace_fraction == 1)
			&& !(g_globalvars.trace_inopen && g_globalvars.trace_inwater);
}

static qbool HMode_EntityVisibleTo(gedict_t *viewer, gedict_t *ent)
{
	vec3_t vec;

	if (HMode_TraceVisible(viewer, ent->s.v.origin))
	{
		return true;
	}

	VectorCopy(ent->s.v.origin, vec);
	vec[2] = ent->s.v.absmin[2];

	if (HMode_TraceVisible(viewer, vec))
	{
		return true;
	}

	vec[2] = ent->s.v.absmax[2];

	return HMode_TraceVisible(viewer, vec);
}

// Refresh a snapshot from the live entity. Only ever called when the bot
// can actually see the teammate -- this is the single place where humanmode
// touches another player's real state, and sight is the license for it.
static void HMode_TmRefresh(hm_tm_t *tm, gedict_t *mate)
{
	tm->source = HMODE_SRC_SEEN;
	tm->fresh = true;
	tm->loc_known = true;
	tm->time = g_globalvars.time;
	tm->health = mate->s.v.health;
	tm->armor = mate->s.v.armorvalue;
	tm->items = (int)mate->s.v.items;
	tm->rockets = mate->s.v.ammo_rockets;
	tm->cells = mate->s.v.ammo_cells;
	VectorCopy(mate->s.v.origin, tm->org);
}

// Periodic teammate visibility scan. Visible teammates get a fresh snapshot;
// everyone else keeps their held snapshot untouched (fresh drops to false).
// Throttled like the stock powerup spotting to keep trap_VisibleTo cheap.
static void HMode_TmScan(gedict_t *self)
{
	byte visible[MAX_CLIENTS];
	hm_bot_t *slot = HMode_Slot(self);
	gedict_t *mate;
	int j;

	if (!slot || !teamplay)
	{
		return;
	}

	if (slot->next_tm_scan > g_globalvars.time)
	{
		return;
	}

	slot->next_tm_scan = g_globalvars.time + 0.3f;

	visible_to(self, g_edicts + 1, MAX_CLIENTS, visible);

	for (j = 1, mate = g_edicts + 1; j <= MAX_CLIENTS; j++, mate++)
	{
		hm_tm_t *tm = &slot->tm[j];

		if ((mate == self) || (mate->ct != ctPlayer)
				|| (mate->k_teamnum != self->k_teamnum))
		{
			continue;
		}

		if (ISLIVE(mate) && visible[j - 1] && HMode_EntityVisibleTo(self, mate))
		{
			HMode_TmRefresh(tm, mate);
		}
		else
		{
			tm->fresh = false; // hold the snapshot as-is
		}
	}
}

const hm_tm_t* HMode_TeammateInfo(gedict_t *bot, gedict_t *mate)
{
	hm_bot_t *slot;
	int m;

	if (!HMode_Active(bot) || !mate)
	{
		return NULL;
	}

	slot = HMode_Slot(bot);
	m = NUM_FOR_EDICT(mate);

	if (!slot || (m < 1) || (m > MAX_CLIENTS))
	{
		return NULL;
	}

	if (slot->tm[m].source == HMODE_SRC_NONE)
	{
		return NULL;
	}

	return &slot->tm[m];
}

// ---- item beliefs (S3) ----

// Fixed respawn duration per major item, in seconds. These are the "fixed
// parameters" a human just knows after learning the game (#256 §0): armors
// 20, guns 30 (dmm1), quad 60, pent/ring 300. Mega is dynamically timed by
// the engine (5s after the carrier drops below 100hp) which a human cannot
// know exactly -- 20s is the human approximation. 0 = not a major item.
static float HMode_ItemDuration(gedict_t *item)
{
	const char *cn = item->classname;

	if (!cn || !cn[0])
	{
		return 0;
	}

	if (streq(cn, "item_armor1") || streq(cn, "item_armor2")
			|| streq(cn, "item_armorInv"))
	{
		return 20;
	}

	if (streq(cn, "item_health") && ((int)item->s.v.spawnflags & H_MEGA))
	{
		return 20;
	}

	if (!strncmp(cn, "weapon_", 7))
	{
		return 30;
	}

	if (streq(cn, "item_artifact_super_damage"))
	{
		return 60;
	}

	if (streq(cn, "item_artifact_invulnerability")
			|| streq(cn, "item_artifact_invisibility"))
	{
		return 300;
	}

	return 0;
}

// Stable per-map index for a major item; -1 for non-majors or table
// overflow (dm3 has ~20 majors, the 64 cap is generous).
static int HMode_ItemIndex(gedict_t *item, qbool add)
{
	int i;

	for (i = 0; i < hm_item_count; i++)
	{
		if (hm_item_ents[i] == item)
		{
			return i;
		}
	}

	if (!add || (hm_item_count >= HMODE_MAX_ITEMS))
	{
		return -1;
	}

	if (HMode_ItemDuration(item) <= 0)
	{
		return -1;
	}

	hm_item_ents[hm_item_count] = item;

	return hm_item_count++;
}

float HMode_ItemRespawnTime(gedict_t *bot, gedict_t *item)
{
	hm_bot_t *slot;
	int idx;

	if (!bot || !bot->isBot || !HMode_Active(bot) || !HMode_CapItemInfo())
	{
		return item->fb.goal_respawn_time;
	}

	idx = HMode_ItemIndex(item, true);

	if (idx < 0)
	{
		// Not a tracked major: engine value (shards and small pickups
		// respawn in seconds; nobody times them).
		return item->fb.goal_respawn_time;
	}

	slot = HMode_Slot(bot);

	if (!slot || (slot->items[idx].source == HMODE_SRC_NONE))
	{
		return 0; // never witnessed anything: assume it could be up, go look
	}

	return slot->items[idx].respawn_at;
}

// Distribute an item event over the perception channels: SEEN needs PVS+LOS
// to the item at event time (the taker trivially qualifies), HEARD needs
// only earshot of the pickup/respawn sound. Everyone else learns nothing.
static void HMode_ItemEvent(gedict_t *item, gedict_t *taker, float respawn_at)
{
	int idx = HMode_ItemIndex(item, true);
	byte visible[1];
	vec3_t item_eye, delta;
	gedict_t *bot;
	int j;

	if (idx < 0)
	{
		return;
	}

	VectorCopy(item->s.v.origin, item_eye);
	item_eye[2] += 16;

	for (j = 1, bot = g_edicts + 1; j <= MAX_CLIENTS; j++, bot++)
	{
		hm_bot_t *slot;
		int src = HMODE_SRC_NONE;

		if (!bot->isBot || (bot->ct != ctPlayer) || !HMode_Active(bot))
		{
			continue;
		}

		slot = HMode_Slot(bot);

		if (!slot)
		{
			continue;
		}

		if (bot == taker)
		{
			src = HMODE_SRC_SEEN; // own pickup: exact, highest confidence
		}
		else if (!ISLIVE(bot))
		{
			continue; // dead bots perceive nothing
		}
		else
		{
			visible[0] = 0;
			visible_to(bot, item, 1, visible);

			if (visible[0] && HMode_TraceVisible(bot, item_eye))
			{
				src = HMODE_SRC_SEEN;
			}
			else
			{
				VectorSubtract(bot->s.v.origin, item->s.v.origin, delta);

				if (vlen(delta) < HMODE_HEARD_RADIUS)
				{
					src = HMODE_SRC_HEARD;
				}
			}
		}

		if (src == HMODE_SRC_NONE)
		{
			continue;
		}

		slot->items[idx].source = src;
		slot->items[idx].taken_time = g_globalvars.time;
		slot->items[idx].respawn_at = respawn_at;
	}
}

void HMode_ItemTaken(gedict_t *item, gedict_t *player)
{
	float duration = HMode_ItemDuration(item);

	if ((duration <= 0) || !player || (player->ct != ctPlayer))
	{
		return;
	}

	HMode_ItemEvent(item, player, g_globalvars.time + duration);

	// Taker-side "took" callout (weapons included, unlike the stock
	// mega/armor-only BotTookMessage which is bypassed for humanmode bots).
	if (player->isBot && HMode_Active(player))
	{
		HMode_TookReport(player, item);
	}
}

void HMode_ItemRespawned(gedict_t *item)
{
	if (HMode_ItemDuration(item) <= 0)
	{
		return;
	}

	// The respawn sound/sight: item is up NOW.
	HMode_ItemEvent(item, NULL, g_globalvars.time);
}

// ---- emit (S4) ----

// Book traffic band (docs/notes/book-teamsay-study.md §3): per-player median
// ~10.8 msg/min, hard ceiling 23/min, same-tick 2-message macro bursts are
// native. Token bucket: refill at the median rate scaled by k_hm_rate,
// burst headroom of 2, urgent families (lost) may run a small debt.
#define HMODE_EMIT_RATE_PER_MIN 11.0f
#define HMODE_EMIT_BURST 2.0f
#define HMODE_EMIT_DEBT -2.0f

// Same periodic base as the stock BotPeriodicMessages (bot_client.c).
#define HMODE_PERIODIC 4

static float HMode_RateScale(void)
{
	char buf[16];
	float v;

	trap_cvar_string("k_hm_rate", buf, sizeof(buf));

	if (strnull(buf))
	{
		return 1.0f;
	}

	v = cvar("k_hm_rate");

	return bound(0.0f, v, 5.0f);
}

static qbool HMode_TryEmit(hm_bot_t *slot, qbool urgent)
{
	float now = g_globalvars.time;
	float refill = (now - slot->emit_refill_time) * (HMODE_EMIT_RATE_PER_MIN / 60.0f)
			* HMode_RateScale();

	slot->emit_tokens = bound(HMODE_EMIT_DEBT, slot->emit_tokens + refill, HMODE_EMIT_BURST);
	slot->emit_refill_time = now;

	if (slot->emit_tokens >= 1.0f || (urgent && (slot->emit_tokens > HMODE_EMIT_DEBT + 1.0f)))
	{
		slot->emit_tokens -= 1.0f;

		return true;
	}

	return false;
}

// Per-family gate cvars (default on when unset, like the capability gates):
// k_hm_emit_status/_took/_lost/_coming/_safe/_help/_point/_pwr.
static qbool HMode_FamilyOn(const char *name)
{
	return HMode_CapCvar(name);
}

// Enemy-powerup spotting for humanmode bots: same perception logic as the
// stock TeamplayReportVisiblePowerups, but metered by the governor and the
// pwr family gate. (Reimplemented rather than wrapped so the token is only
// spent when a report actually fires.)
static void HMode_SpotEnemyPowerups(gedict_t *self, hm_bot_t *slot)
{
	byte visible[MAX_CLIENTS];
	gedict_t *opponent;

	if (FUTURE(last_mm2_spot_attempt))
	{
		return;
	}

	self->fb.last_mm2_spot_attempt = g_globalvars.time + 0.5 + g_random() * 0.2;

	visible_to(self, g_edicts + 1, MAX_CLIENTS, visible);

	for (opponent = g_edicts + 1; opponent <= g_edicts + MAX_CLIENTS; opponent++)
	{
		qbool diff_team = opponent->k_teamnum != self->k_teamnum;
		qbool powerups = (int)opponent->s.v.items & (IT_QUAD | IT_INVULNERABILITY);

		if (diff_team && powerups && (opponent->ct == ctPlayer)
				&& visible[opponent - (g_edicts + 1)]
				&& (opponent->fb.last_mm2_spot < g_globalvars.time))
		{
			if (VisibleEntity(opponent) && HMode_TryEmit(slot, true))
			{
				TeamplayMessageByName(self, "enemypwr");
				opponent->fb.last_mm2_spot = g_globalvars.time + 2;
				break;
			}
		}
	}
}

// The humanmode replacement for BotPeriodicMessages: same builders (they
// already produce the ezQuake-standard templates the Book corpus is made
// of), Book-informed triggers and rates. Status is the workhorse family
// (48.6% of real elite traffic) and fires regardless of strength; safe/help
// take precedence in their specific situations, exactly like the stock
// selection did.
void HMode_PeriodicMessages(gedict_t *self)
{
	hm_bot_t *slot = HMode_Slot(self);

	if (!slot || !HMode_CapEmit())
	{
		return;
	}

	// Fresh-spawn report: "0/100 sg {loc}" -- 1,082 instances in the Book
	// corpus; closes the killfeed knowledge gap for teammates.
	if (slot->spawn_report_at && (g_globalvars.time >= slot->spawn_report_at))
	{
		slot->spawn_report_at = 0;

		if (ISLIVE(self) && HMode_FamilyOn("k_hm_emit_status") && HMode_TryEmit(slot, false))
		{
			TeamplayMessageByName(self, "report");
			slot->next_periodic = g_globalvars.time
					+ HMODE_PERIODIC * (g_random() + 0.5);
		}
	}

	// Post-spawn "coming {loc}" -- third most common Book family.
	if (slot->coming_at && (g_globalvars.time >= slot->coming_at))
	{
		slot->coming_at = 0;

		if (ISLIVE(self) && HMode_FamilyOn("k_hm_emit_coming") && HMode_TryEmit(slot, false))
		{
			TeamplayMessageByName(self, "coming");
		}
	}

	// Periodic situation report.
	if (g_globalvars.time >= slot->next_periodic)
	{
		qbool has_rl = ((int)self->s.v.items & IT_ROCKET_LAUNCHER)
				&& (self->s.v.ammo_rockets >= 3);
		qbool has_lg = ((int)self->s.v.items & IT_LIGHTNING) && (self->s.v.ammo_cells >= 6);
		qbool is_strong = (has_rl || has_lg) && (self->fb.total_damage >= 120);

		slot->next_periodic = g_globalvars.time + HMODE_PERIODIC * (g_random() + 0.5);

		if (ISLIVE(self))
		{
			if (is_strong && (self->tp.enemy_count == 0))
			{
				if (HMode_FamilyOn("k_hm_emit_safe") && HMode_TryEmit(slot, false))
				{
					TeamplayMessageByName(self, "secure");
				}
			}
			else if (is_strong && (self->tp.enemy_count > self->tp.teammate_count))
			{
				if (HMode_FamilyOn("k_hm_emit_help") && HMode_TryEmit(slot, true))
				{
					TeamplayMessageByName(self, "help");
				}
			}
			else if (self->fb.look_object
					&& (NUM_FOR_EDICT(self->fb.look_object) == self->s.v.enemy))
			{
				if (HMode_FamilyOn("k_hm_emit_point") && HMode_TryEmit(slot, false))
				{
					TeamplayMessageByName(self, "point");
				}
			}
			else if (HMode_FamilyOn("k_hm_emit_status") && HMode_TryEmit(slot, false))
			{
				TeamplayMessageByName(self, "report");
			}
		}
	}

	if (HMode_FamilyOn("k_hm_emit_pwr"))
	{
		HMode_SpotEnemyPowerups(self, slot);
	}
}

// Death report: Book bots call "lost" on EVERY death (2,502 instances, the
// second-largest family; the builder upgrades it to "quad over" / "DROPPED
// RL" variants from death_items/death_weapon). Urgent: may run a debt.
void HMode_DeathReport(gedict_t *self)
{
	hm_bot_t *slot = HMode_Slot(self);

	if (!slot || !HMode_CapEmit() || !HMode_FamilyOn("k_hm_emit_lost"))
	{
		return;
	}

	if (HMode_TryEmit(slot, true))
	{
		TeamplayMessageByName(self, "lost");
	}
}

// Taker-side "took {item} {loc}" / "team {powerup}". Wired from the item
// event so weapons count too (stock BotTookMessage only covered mega+armor);
// tp.took memory is fresh because TeamplayEventItemTaken runs first in
// ItemTaken(). Powerup pickups report under the pwr family ("team quad"),
// everything else under took.
static void HMode_TookReport(gedict_t *self, gedict_t *item)
{
	hm_bot_t *slot = HMode_Slot(self);
	qbool is_powerup = !strncmp(item->classname, "item_artifact_", 14);

	if (!slot || !HMode_CapEmit() || !teamplay || (match_in_progress != 2))
	{
		return;
	}

	if (!HMode_FamilyOn(is_powerup ? "k_hm_emit_pwr" : "k_hm_emit_took"))
	{
		return;
	}

	if (HMode_TryEmit(slot, is_powerup))
	{
		TeamplayMessageByName(self, "took");
	}
}

// ---- hooks ----

void HMode_MapInit(void)
{
	memset(hm_bots, 0, sizeof(hm_bots));
	memset(hm_item_ents, 0, sizeof(hm_item_ents));
	hm_item_count = 0;
}

void HMode_Frame(gedict_t *self)
{
	if (!HMode_Active(self))
	{
		return;
	}

	HMode_Activate(self);

	if (HMode_CapTmInfo())
	{
		HMode_TmScan(self);
	}
}

void HMode_ClientEnters(gedict_t *self)
{
	hm_bot_t *slot;

	if (!HMode_Active(self))
	{
		return;
	}

	slot = HMode_Slot(self);

	if (slot && (match_in_progress != 2))
	{
		// Pre-match (re)spawn: clean knowledge baseline for the match.
		// Mid-match respawns keep the bot's memory -- humans do.
		memset(slot->tm, 0, sizeof(slot->tm));
		memset(slot->items, 0, sizeof(slot->items));
		slot->next_tm_scan = 0;
		slot->spawn_report_at = 0;
		slot->coming_at = 0;
	}

	if (slot && (match_in_progress == 2))
	{
		// Queue the fresh-spawn report and the follow-up "coming" call
		// (small jitter so four bots don't report in lockstep).
		slot->spawn_report_at = g_globalvars.time + 0.4 + g_random() * 0.8;
		slot->coming_at = g_globalvars.time + 2.0 + g_random() * 3.0;
	}
}

// Every player reads the frag feed, so a teammate's death is public
// knowledge: the held snapshot collapses to "respawned: 0/100 sg, location
// unknown". The gap usually closes within seconds -- the fresh-spawn report
// ("0/100 sg {loc}") is a real, frequent template (1,082 in the Book corpus).
void HMode_Killfeed(gedict_t *victim, gedict_t *attacker)
{
	int j, v;
	gedict_t *bot;

	if (!victim || (victim->ct != ctPlayer))
	{
		return;
	}

	v = NUM_FOR_EDICT(victim);

	if ((v < 1) || (v > MAX_CLIENTS))
	{
		return;
	}

	for (j = 1, bot = g_edicts + 1; j <= MAX_CLIENTS; j++, bot++)
	{
		hm_bot_t *slot;
		hm_tm_t *tm;

		if (!bot->isBot || (bot == victim) || (bot->ct != ctPlayer)
				|| (bot->k_teamnum != victim->k_teamnum))
		{
			continue;
		}

		if (!HMode_Active(bot) || !HMode_CapTmInfo())
		{
			continue;
		}

		slot = HMode_Slot(bot);

		if (!slot)
		{
			continue;
		}

		tm = &slot->tm[v];
		memset(tm, 0, sizeof(*tm));
		tm->source = HMODE_SRC_KILLFEED;
		tm->time = g_globalvars.time;
		tm->health = 100;
		tm->armor = 0;
		tm->items = IT_SHOTGUN | IT_AXE;
		tm->loc_known = false;
	}
}

void HMode_ParseTeamsay(gedict_t *receiver, gedict_t *sender, const char *text)
{
	// S5: multi-clan grammar -> teammate snapshots / item beliefs.
}

// ---- botcmd hm ----

static void HMode_PrintStatus(void)
{
	int j, printed = 0;

	G_sprint(self, 2, "humanmode global (k_hm): %s\n",
				cvar("k_hm") ? redtext("on") : redtext("off"));

	for (j = 1; j <= MAX_CLIENTS; j++)
	{
		gedict_t *bot = &g_edicts[j];
		hm_bot_t *slot;

		if (!bot->isBot)
		{
			continue;
		}

		slot = HMode_Slot(bot);
		G_sprint(self, 2, "  slot %2d %-16s %s%s%s\n", j, bot->netname,
					(bot->fb.hm == HMODE_ON) ? "on" :
					(bot->fb.hm == HMODE_OFF) ? "off" : "inherit",
					HMode_Active(bot) ? " [active" : " [inactive",
					(slot && slot->active_logged) ? va(" tag=%s]", slot->tag) : "]");
		printed++;
	}

	if (!printed)
	{
		G_sprint(self, 2, "  (no bots on server)\n");
	}
}

static int HMode_ParseMode(const char *arg, int *mode)
{
	if (streq(arg, "on") || streq(arg, "1"))
	{
		*mode = HMODE_ON;

		return 1;
	}

	if (streq(arg, "off") || streq(arg, "0"))
	{
		*mode = HMODE_OFF;

		return 1;
	}

	if (streq(arg, "inherit") || streq(arg, "-1"))
	{
		*mode = HMODE_INHERIT;

		return 1;
	}

	return 0;
}

void HMode_BotCmd(void)
{
	char arg[32];
	int mode = HMODE_INHERIT;
	int j, count = 0;

	if (trap_CmdArgc() < 4)
	{
		if (trap_CmdArgc() == 2)
		{
			HMode_PrintStatus();

			return;
		}

		G_sprint(self, 2, "Usage: /botcmd hm <slot|all> <on|off|inherit>\n");

		return;
	}

	trap_CmdArgv(3, arg, sizeof(arg));

	if (!HMode_ParseMode(arg, &mode))
	{
		G_sprint(self, 2, "Usage: /botcmd hm <slot|all> <on|off|inherit>\n");

		return;
	}

	trap_CmdArgv(2, arg, sizeof(arg));

	if (streq(arg, "all"))
	{
		for (j = 1; j <= MAX_CLIENTS; j++)
		{
			if (g_edicts[j].isBot)
			{
				g_edicts[j].fb.hm = mode;
				count++;
			}
		}

		G_sprint(self, 2, "humanmode %s for %d bot%s\n",
					(mode == HMODE_ON) ? "on" : (mode == HMODE_OFF) ? "off" : "inherit",
					count, (count == 1) ? "" : "s");

		return;
	}

	j = atoi(arg);

	if ((j < 1) || (j > MAX_CLIENTS) || !g_edicts[j].isBot)
	{
		G_sprint(self, 2, "slot %s is not a bot\n", arg);

		return;
	}

	g_edicts[j].fb.hm = mode;
	G_sprint(self, 2, "humanmode %s for slot %d (%s)\n",
				(mode == HMODE_ON) ? "on" : (mode == HMODE_OFF) ? "off" : "inherit",
				j, g_edicts[j].netname);
}

#endif // BOT_SUPPORT
