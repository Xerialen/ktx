/*
 hm.c -- mm2humanmode core (S1 skeleton).

 See hm.h for the model. This stage wires the toggles only:
 - HM_Active(): per-bot fb.hm override, else global cvar k_hm
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

// Per-bot humanmode runtime state, indexed by entity number (1..MAX_CLIENTS).
// Kept out of fb_entvars_t: every edict carries fb, only client slots need
// this, and the QVM bss budget is tight.
typedef struct hm_bot_s
{
	qbool active_logged;			// one-time activation done (tag + log)
	char tag[4];					// 3-letter lowercase self-prefix
	float next_tm_scan;				// visibility-scan throttle (engine trap)
	hm_tm_t tm[MAX_CLIENTS + 1];	// beliefs about teammates, by entity num
} hm_bot_t;

static hm_bot_t hm_bots[MAX_CLIENTS + 1];

static hm_bot_t* HM_Slot(gedict_t *bot)
{
	int entity = NUM_FOR_EDICT(bot);

	if ((entity < 1) || (entity > MAX_CLIENTS))
	{
		return NULL;
	}

	return &hm_bots[entity];
}

qbool HM_Active(gedict_t *bot)
{
	if (!bot || !bot->isBot)
	{
		return false;
	}

	if (bot->fb.hm == HM_ON)
	{
		return true;
	}

	if (bot->fb.hm == HM_OFF)
	{
		return false;
	}

	return cvar("k_hm") != 0;
}

// Capability cvars default ON when unset, so "k_hm 1" alone gives the full
// model. cvar() returns 0 for unset cvars, hence the explicit string check.
static qbool HM_CapCvar(const char *name)
{
	char buf[16];

	trap_cvar_string(name, buf, sizeof(buf));

	if (strnull(buf))
	{
		return true; // unset = default on
	}

	return cvar(name) != 0;
}

qbool HM_CapEmit(void)
{
	return HM_CapCvar("k_hm_emit");
}

qbool HM_CapParse(void)
{
	return HM_CapCvar("k_hm_parse");
}

qbool HM_CapTmInfo(void)
{
	return HM_CapCvar("k_hm_tminfo");
}

qbool HM_CapItemInfo(void)
{
	return HM_CapCvar("k_hm_iteminfo");
}

// ---- 3-letter self-prefix tag ----

// Derive the tag from the bot name: skip the "kb:" identity prefix and any
// non-letters, take the first three letters lowercased. Falls back to
// "b<slot>" style if the name yields fewer than 3 letters.
static void HM_DeriveTag(gedict_t *bot, char *out, int outsize)
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
static void HM_UniqueTag(gedict_t *bot, char *tag)
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

		oslot = HM_Slot(other);

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
static void HM_Activate(gedict_t *self)
{
	hm_bot_t *slot = HM_Slot(self);

	if (!slot || slot->active_logged)
	{
		return;
	}

	HM_DeriveTag(self, slot->tag, sizeof(slot->tag));
	HM_UniqueTag(self, slot->tag);
	trap_SetBotUserInfo(NUM_FOR_EDICT(self), "k_nick", slot->tag, 0);
	slot->active_logged = true;

	G_cprint("[hm] slot=%d name=%s tag=%s version=%s emit=%d parse=%d tminfo=%d iteminfo=%d\n",
				NUM_FOR_EDICT(self), self->netname, slot->tag, HM_VERSION,
				(int)HM_CapEmit(), (int)HM_CapParse(), (int)HM_CapTmInfo(),
				(int)HM_CapItemInfo());
}

// ---- teammate-state model (S2) ----

// LOS check with an explicit viewer (VisibleEntity() relies on the global
// self). Same three-trace shape as bot_bothelp.c PointVisible/VisibleEntity,
// which is also the perception definition the stock enemypwr spotting uses:
// PVS filter first (visible_to), then eye-to-body tracelines. No view cone,
// consistent with the enemy-perception model.
static qbool HM_TraceVisible(gedict_t *viewer, vec3_t point)
{
	traceline(PASSVEC3(viewer->s.v.origin), PASSVEC3(point), true, viewer);

	return (g_globalvars.trace_fraction == 1)
			&& !(g_globalvars.trace_inopen && g_globalvars.trace_inwater);
}

static qbool HM_EntityVisibleTo(gedict_t *viewer, gedict_t *ent)
{
	vec3_t vec;

	if (HM_TraceVisible(viewer, ent->s.v.origin))
	{
		return true;
	}

	VectorCopy(ent->s.v.origin, vec);
	vec[2] = ent->s.v.absmin[2];

	if (HM_TraceVisible(viewer, vec))
	{
		return true;
	}

	vec[2] = ent->s.v.absmax[2];

	return HM_TraceVisible(viewer, vec);
}

// Refresh a snapshot from the live entity. Only ever called when the bot
// can actually see the teammate -- this is the single place where humanmode
// touches another player's real state, and sight is the license for it.
static void HM_TmRefresh(hm_tm_t *tm, gedict_t *mate)
{
	tm->source = HM_SRC_SEEN;
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
static void HM_TmScan(gedict_t *self)
{
	byte visible[MAX_CLIENTS];
	hm_bot_t *slot = HM_Slot(self);
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

		if (ISLIVE(mate) && visible[j - 1] && HM_EntityVisibleTo(self, mate))
		{
			HM_TmRefresh(tm, mate);
		}
		else
		{
			tm->fresh = false; // hold the snapshot as-is
		}
	}
}

const hm_tm_t* HM_TeammateInfo(gedict_t *bot, gedict_t *mate)
{
	hm_bot_t *slot;
	int m;

	if (!HM_Active(bot) || !mate)
	{
		return NULL;
	}

	slot = HM_Slot(bot);
	m = NUM_FOR_EDICT(mate);

	if (!slot || (m < 1) || (m > MAX_CLIENTS))
	{
		return NULL;
	}

	if (slot->tm[m].source == HM_SRC_NONE)
	{
		return NULL;
	}

	return &slot->tm[m];
}

// ---- hooks ----

void HM_Frame(gedict_t *self)
{
	if (!HM_Active(self))
	{
		return;
	}

	HM_Activate(self);

	if (HM_CapTmInfo())
	{
		HM_TmScan(self);
	}
}

void HM_ClientEnters(gedict_t *self)
{
	hm_bot_t *slot;

	if (!HM_Active(self))
	{
		return;
	}

	slot = HM_Slot(self);

	if (slot && (match_in_progress != 2))
	{
		// Pre-match (re)spawn: clean knowledge baseline for the match.
		// Mid-match respawns keep the bot's memory -- humans do.
		memset(slot->tm, 0, sizeof(slot->tm));
		slot->next_tm_scan = 0;
	}

	// S4: queue the fresh-spawn report ("0/100 sg {loc}") here.
}

void HM_ItemTaken(gedict_t *item, gedict_t *player)
{
	// S3: taker + PVS witnesses update item beliefs.
}

// Every player reads the frag feed, so a teammate's death is public
// knowledge: the held snapshot collapses to "respawned: 0/100 sg, location
// unknown". The gap usually closes within seconds -- the fresh-spawn report
// ("0/100 sg {loc}") is a real, frequent template (1,082 in the Book corpus).
void HM_Killfeed(gedict_t *victim, gedict_t *attacker)
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

		if (!HM_Active(bot) || !HM_CapTmInfo())
		{
			continue;
		}

		slot = HM_Slot(bot);

		if (!slot)
		{
			continue;
		}

		tm = &slot->tm[v];
		memset(tm, 0, sizeof(*tm));
		tm->source = HM_SRC_KILLFEED;
		tm->time = g_globalvars.time;
		tm->health = 100;
		tm->armor = 0;
		tm->items = IT_SHOTGUN | IT_AXE;
		tm->loc_known = false;
	}
}

void HM_ParseTeamsay(gedict_t *receiver, gedict_t *sender, const char *text)
{
	// S5: multi-clan grammar -> teammate snapshots / item beliefs.
}

// ---- botcmd hm ----

static void HM_PrintStatus(void)
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

		slot = HM_Slot(bot);
		G_sprint(self, 2, "  slot %2d %-16s %s%s%s\n", j, bot->netname,
					(bot->fb.hm == HM_ON) ? "on" :
					(bot->fb.hm == HM_OFF) ? "off" : "inherit",
					HM_Active(bot) ? " [active" : " [inactive",
					(slot && slot->active_logged) ? va(" tag=%s]", slot->tag) : "]");
		printed++;
	}

	if (!printed)
	{
		G_sprint(self, 2, "  (no bots on server)\n");
	}
}

static int HM_ParseMode(const char *arg, int *mode)
{
	if (streq(arg, "on") || streq(arg, "1"))
	{
		*mode = HM_ON;

		return 1;
	}

	if (streq(arg, "off") || streq(arg, "0"))
	{
		*mode = HM_OFF;

		return 1;
	}

	if (streq(arg, "inherit") || streq(arg, "-1"))
	{
		*mode = HM_INHERIT;

		return 1;
	}

	return 0;
}

void HM_BotCmd(void)
{
	char arg[32];
	int mode = HM_INHERIT;
	int j, count = 0;

	if (trap_CmdArgc() < 4)
	{
		if (trap_CmdArgc() == 2)
		{
			HM_PrintStatus();

			return;
		}

		G_sprint(self, 2, "Usage: /botcmd hm <slot|all> <on|off|inherit>\n");

		return;
	}

	trap_CmdArgv(3, arg, sizeof(arg));

	if (!HM_ParseMode(arg, &mode))
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
					(mode == HM_ON) ? "on" : (mode == HM_OFF) ? "off" : "inherit",
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
				(mode == HM_ON) ? "on" : (mode == HM_OFF) ? "off" : "inherit",
				j, g_edicts[j].netname);
}

#endif // BOT_SUPPORT
