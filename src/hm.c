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

// ---- comm-consumption types (S7: the brain acts on comms) ----

// A believed enemy sighting, sourced from teamsay reports (enemy_seen /
// enemy_powerup / enemy_at_nick / lost-with-count / slipped). Ring buffer:
// old sightings age out via HMODE_SIGHT_TTL, overwrite via the head.
#define HMODE_MAX_SIGHT 8

typedef struct hm_sight_s
{
	float time;			// when the report arrived (0 = empty/invalidated)
	qbool org_known;	// org below is meaningful
	vec3_t org;			// believed enemy position
	int count;			// enemies reported (>= 1)
	qbool powerup;		// a quaded/pented/ringed enemy
	int source;			// HM_SRC_*
} hm_sight_t;

// One comm-influenced goal evaluation this refresh (for the [hm-goal]
// commit log: was the chosen goal comm-biased, and how).
#define HMODE_MAX_ACT 16

typedef struct hm_act_s
{
	gedict_t *goal;
	int kinds;			// HMODE_ACT_* bitmask
} hm_act_t;

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
	char tag_color[4];				// &cRGB tint for the tag ("" = plain)
	float next_tm_scan;				// visibility-scan throttle (engine trap)
	hm_tm_t tm[MAX_CLIENTS + 1];	// beliefs about teammates, by entity num
	hm_item_belief_t items[HMODE_MAX_ITEMS];	// beliefs about major items

	// Emit scheduler (S4)
	float emit_tokens;				// token bucket, 1 token = 1 message
	float emit_refill_time;			// last refill timestamp
	float next_periodic;			// next periodic status-class report
	float spawn_report_at;			// pending fresh-spawn report (0 = none)
	float coming_at;				// pending post-spawn "coming" (0 = none)

	// Comm-derived world model (S7): what teammates SAID, consumed by the
	// goal-desire bias. All of it ages out on its own TTL.
	hm_sight_t sights[HMODE_MAX_SIGHT];	// believed enemy sightings (ring)
	int sight_head;
	float pack_time;				// "pack/dropped at {loc}" report
	vec3_t pack_org;
	qbool pack_valid;
	float req_time;					// "get/take/camp {loc|item}" order
	vec3_t req_org;
	qbool req_valid;
	float need_time;				// "need {item}" -> yield to the mate
	gedict_t *need_item;
	float help_time;				// "help {loc}" -> assist bias
	vec3_t help_org;
	qbool help_valid;

	// Decision telemetry (k_hm_debug): change-throttles for the
	// [hm-belief-dec] / [hm-goal] log lines + this refresh's comm-biased
	// goal evals.
	float dec_log_last[HMODE_MAX_ITEMS];
	hm_act_t act[HMODE_MAX_ACT];
	int act_n;
	gedict_t *goal_logged;
	int goal_logged_kinds;
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

// Optional team scope for the global toggle: when k_hm_teams is set (space/
// comma-separated team names), only bots on a listed team inherit k_hm.
// Per-bot HMODE_ON overrides bypass this (explicit wins over scope).
static qbool HMode_TeamAllowed(gedict_t *bot)
{
	char list[128];
	char team[MAX_TEAM_NAME];
	const char *t;
	char *s;
	int i;

	trap_cvar_string("k_hm_teams", list, sizeof(list));

	if (strnull(list))
	{
		return true; // unset = every team
	}

	t = getteam(bot);

	for (i = 0; t[i] && (i < (int)sizeof(team) - 1); i++)
	{
		char c = t[i];

		if ((c >= 'A') && (c <= 'Z'))
		{
			c = (char)(c - 'A' + 'a');
		}

		team[i] = c;
	}

	team[i] = '\0';

	for (s = list; *s;)
	{
		char word[MAX_TEAM_NAME];
		int n = 0;

		while (*s && ((*s == ' ') || (*s == ',')))
		{
			s++;
		}

		while (*s && (*s != ' ') && (*s != ','))
		{
			char c = *s;

			if ((c >= 'A') && (c <= 'Z'))
			{
				c = (char)(c - 'A' + 'a');
			}

			if (n < (int)sizeof(word) - 1)
			{
				word[n++] = c;
			}

			s++;
		}

		word[n] = '\0';

		if (n && streq(word, team))
		{
			return true;
		}
	}

	return false;
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

	return (cvar("k_hm") != 0) && HMode_TeamAllowed(bot);
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

// Tag tint rotation: same-team hm bots alternate red/blue/orange/purple in
// activation (= seating) order so their teamsay aliases stand apart at a
// glance. Per-position override cvars k_hm_tag_color1..4 take a 3-hex ezQuake
// &cRGB value; anything else falls back to the default for that position.
static const char *hm_tag_color_defaults[4] = { "f00", "00f", "f80", "a0f" };

static qbool HMode_IsHexColorChar(char c)
{
	return ((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'f'))
			|| ((c >= 'A') && (c <= 'F'));
}

static qbool HMode_IsHexColor3(const char *s)
{
	return HMode_IsHexColorChar(s[0]) && HMode_IsHexColorChar(s[1])
			&& HMode_IsHexColorChar(s[2]) && (s[3] == '\0');
}

static void HMode_AssignTagColor(gedict_t *self, hm_bot_t *slot)
{
	char buf[8];
	int idx = 0;
	int j;

	// rotation position = same-team hm bots activated before this one
	for (j = 1; j <= MAX_CLIENTS; j++)
	{
		gedict_t *other = &g_edicts[j];
		hm_bot_t *oslot;

		if (!other->isBot || (other == self) || (other->ct != ctPlayer)
				|| (other->k_teamnum != self->k_teamnum))
		{
			continue;
		}

		oslot = HMode_Slot(other);

		if (oslot && oslot->active_logged)
		{
			idx++;
		}
	}

	idx = idx % 4;

	trap_cvar_string(va("k_hm_tag_color%d", idx + 1), buf, sizeof(buf));

	if (HMode_IsHexColor3(buf))
	{
		strlcpy(slot->tag_color, buf, sizeof(slot->tag_color));
	}
	else
	{
		strlcpy(slot->tag_color, hm_tag_color_defaults[idx], sizeof(slot->tag_color));
	}
}

// Master gate for the tag tint (default on): k_hm_tag_colorize 0 gives plain
// tags. Checked at emit time so a mid-match flip takes effect immediately.
qbool HMode_DecorateTag(gedict_t *client, const char *name, char *out, int outsize)
{
	hm_bot_t *slot;

	if (!client || !client->isBot || !HMode_Active(client)
			|| !HMode_CapCvar("k_hm_tag_colorize"))
	{
		return false;
	}

	slot = HMode_Slot(client);

	if (!slot || !slot->active_logged || strnull(slot->tag_color))
	{
		return false;
	}

	snprintf(out, outsize, "&c%s%s&r", slot->tag_color, name);

	return true;
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
	HMode_AssignTagColor(self, slot);
	trap_SetBotUserInfo(NUM_FOR_EDICT(self), "k_nick", slot->tag, 0);
	slot->active_logged = true;

	G_cprint("[hm] slot=%d name=%s tag=%s color=%s version=%s emit=%d parse=%d tminfo=%d iteminfo=%d\n",
				NUM_FOR_EDICT(self), self->netname, slot->tag,
				strnull(slot->tag_color) ? "-" : slot->tag_color, HMODE_VERSION,
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

// ---- debug telemetry helpers (k_hm_debug) ----
// The [hm-*] console lines form a machine-checkable causality chain:
// [hm-parse] (message -> category) -> [hm-belief-upd] (category -> world
// model) -> [hm-belief-dec]/[hm-act] (world model -> decision input) ->
// [hm-goal] (the committed goal). validate_hm_run.py asserts over them.

static qbool HMode_DebugOn(void)
{
	return cvar("k_hm_debug") != 0;
}

static const char *hm_src_names[] = { "none", "killfeed", "heard", "told", "seen" };

static const char* HMode_ItemShortName(gedict_t *item)
{
	const char *cn = item->classname;

	if (streq(cn, "item_armorInv"))
	{
		return "ra";
	}

	if (streq(cn, "item_armor2"))
	{
		return "ya";
	}

	if (streq(cn, "item_armor1"))
	{
		return "ga";
	}

	if (streq(cn, "item_health"))
	{
		return ((int)item->s.v.spawnflags & H_MEGA) ? "mega" : "health";
	}

	if (streq(cn, "item_artifact_super_damage"))
	{
		return "quad";
	}

	if (streq(cn, "item_artifact_invulnerability"))
	{
		return "pent";
	}

	if (streq(cn, "item_artifact_invisibility"))
	{
		return "ring";
	}

	if (streq(cn, "weapon_rocketlauncher"))
	{
		return "rl";
	}

	if (streq(cn, "weapon_lightning"))
	{
		return "lg";
	}

	if (streq(cn, "weapon_grenadelauncher"))
	{
		return "gl";
	}

	if (streq(cn, "weapon_supernailgun"))
	{
		return "sng";
	}

	if (streq(cn, "weapon_supershotgun"))
	{
		return "ssg";
	}

	return cn;
}

// "ya@-1088,-320": short name + xy disambiguates same-type items (two YAs)
static const char* HMode_ItemLogName(gedict_t *item)
{
	return va("%s@%d,%d", HMode_ItemShortName(item), (int)item->s.v.origin[0],
				(int)item->s.v.origin[1]);
}

static const char* HMode_GoalLogName(gedict_t *goal)
{
	if (goal->ct == ctPlayer)
	{
		return va("player:%s", goal->netname);
	}

	return HMode_ItemLogName(goal);
}

static void HMode_LogItemUpd(gedict_t *bot, gedict_t *item, int src, float old_at,
							 float new_at, const char *from)
{
	if (HMode_DebugOn())
	{
		G_cprint("[hm-belief-upd] bot=%s kind=item item=%s src=%s old=%.1f new=%.1f "
					"truth=%.1f from=%s t=%.1f\n",
					bot->netname, HMode_ItemLogName(item), hm_src_names[src], old_at,
					new_at, item->fb.goal_respawn_time, from, g_globalvars.time);
	}
}

float HMode_ItemRespawnTime(gedict_t *bot, gedict_t *item)
{
	hm_bot_t *slot;
	float believed;
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

	// never witnessed anything: assume it could be up, go look
	believed = (!slot || (slot->items[idx].source == HMODE_SRC_NONE))
			? 0 : slot->items[idx].respawn_at;

	// decision-read telemetry, throttled on-change per bot+item
	if (slot && HMode_DebugOn())
	{
		float d = believed - slot->dec_log_last[idx];

		if ((d > 0.05f) || (d < -0.05f))
		{
			slot->dec_log_last[idx] = believed;
			G_cprint("[hm-belief-dec] bot=%s item=%s believed=%.1f truth=%.1f "
						"src=%s t=%.1f\n",
						bot->netname, HMode_ItemLogName(item), believed,
						item->fb.goal_respawn_time,
						hm_src_names[slot->items[idx].source], g_globalvars.time);
		}
	}

	return believed;
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

		HMode_LogItemUpd(bot, item, src, slot->items[idx].respawn_at, respawn_at,
							(bot == taker) ? "self" : (taker ? "event" : "respawn"));

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

// ---- comm-driven decision bias (S7: the brain acts on comms) ----
// Consumed inside EvalGoal (bot_botgoals.c): every parsed teamsay category
// with actionable content now influences goal choice. Multiplicative
// factors keep the stock desire scale intact; k_hm 0 bots never get here.

#define HMODE_SIGHT_TTL 15.0f	// a told enemy position is stale after this
#define HMODE_PACK_TTL  20.0f
#define HMODE_REQ_TTL   20.0f
#define HMODE_NEED_TTL  15.0f
#define HMODE_HELP_TTL  15.0f

#define HMODE_ACT_AVOID    1	// enemy reported near goal + we are weak
#define HMODE_ACT_AVOID_PW 2	// quaded/pented enemy reported near goal
#define HMODE_ACT_PACK     4	// reported pack: boost matching pack goals
#define HMODE_ACT_OBEY     8	// request order near goal: boost
#define HMODE_ACT_YIELD    16	// teammate said "need X": stand off X
#define HMODE_ACT_ASSIST   32	// teammate called help: drift toward them
#define HMODE_ACT_COVERED  64	// believed teammate already covers this goal

static void HMode_ActKindsStr(int kinds, char *out, int outsize)
{
	static const struct { int bit; const char *name; } tab[] =
	{
		{ HMODE_ACT_AVOID, "avoid-enemy" },
		{ HMODE_ACT_AVOID_PW, "avoid-pw" },
		{ HMODE_ACT_PACK, "boost-pack" },
		{ HMODE_ACT_OBEY, "obey-req" },
		{ HMODE_ACT_YIELD, "yield-need" },
		{ HMODE_ACT_ASSIST, "assist-help" },
		{ HMODE_ACT_COVERED, "covered" },
	};
	int i;

	out[0] = '\0';

	for (i = 0; i < (int)(sizeof(tab) / sizeof(tab[0])); i++)
	{
		if (kinds & tab[i].bit)
		{
			if (out[0])
			{
				strlcat(out, "+", outsize);
			}

			strlcat(out, tab[i].name, outsize);
		}
	}

	if (!out[0])
	{
		strlcpy(out, "none", outsize);
	}
}

// Clear the per-refresh comm-bias record; called when UpdateGoal starts a
// fresh evaluation round for this bot.
void HMode_GoalRefreshBegin(gedict_t *self)
{
	hm_bot_t *slot;

	if (!self->isBot || !HMode_Active(self))
	{
		return;
	}

	slot = HMode_Slot(self);

	if (slot)
	{
		slot->act_n = 0;
	}
}

// The comm consumer: adjust one goal's desire from what teammates said.
// Returns the (possibly) biased desire; identity for non-hm bots.
float HMode_GoalDesireBias(gedict_t *self, gedict_t *goal, float desire)
{
	hm_bot_t *slot;
	float now = g_globalvars.time;
	float d0 = desire;
	qbool armed, strong;
	int kinds = 0;
	int i;

	if (!self->isBot || !HMode_Active(self) || !HMode_CapParse() || !goal
			|| (desire <= 0))
	{
		return desire;
	}

	slot = HMode_Slot(self);

	if (!slot)
	{
		return desire;
	}

	armed = ((((int)self->s.v.items & IT_ROCKET_LAUNCHER) && (self->s.v.ammo_rockets > 0))
			|| (((int)self->s.v.items & IT_LIGHTNING) && (self->s.v.ammo_cells > 0)));
	strong = armed && (self->s.v.health >= 65);

	// danger: fresh told-of enemies near this goal. A reported powerup
	// carrier scares everyone; a plain sighting only deters weak bots.
	for (i = 0; i < HMODE_MAX_SIGHT; i++)
	{
		hm_sight_t *s = &slot->sights[i];
		float d;

		if ((s->time <= 0) || (s->time < now - HMODE_SIGHT_TTL) || !s->org_known)
		{
			continue;
		}

		d = VectorDistance(goal->s.v.origin, s->org);

		if (s->powerup && (d < 1024))
		{
			desire *= 0.10f;
			kinds |= HMODE_ACT_AVOID_PW;
		}
		else if (!strong && (d < 768))
		{
			desire *= 0.25f;
			kinds |= HMODE_ACT_AVOID;
		}
	}

	// reported pack: boost pack-type goals near the reported spot
	if (slot->pack_valid && (slot->pack_time > now - HMODE_PACK_TTL)
			&& (streq(goal->classname, "backpack")
				|| !strncmp(goal->classname, "item_artifact_", 14))
			&& (VectorDistance(goal->s.v.origin, slot->pack_org) < 320))
	{
		desire *= 2.0f;
		kinds |= HMODE_ACT_PACK;
	}

	// obey a fresh request order pointing near this goal
	if (slot->req_valid && (slot->req_time > now - HMODE_REQ_TTL)
			&& (VectorDistance(goal->s.v.origin, slot->req_org) < 320))
	{
		desire *= 1.75f;
		kinds |= HMODE_ACT_OBEY;
	}

	// yield the item a teammate said they need (unless we are hurting too)
	if (slot->need_item && (slot->need_time > now - HMODE_NEED_TTL)
			&& (goal == slot->need_item) && (self->s.v.health >= 50))
	{
		desire *= 0.25f;
		kinds |= HMODE_ACT_YIELD;
	}

	// assist: an armed, healthy bot drifts toward a teammate's help call
	if (slot->help_valid && (slot->help_time > now - HMODE_HELP_TTL) && strong
			&& (VectorDistance(goal->s.v.origin, slot->help_org) < 512))
	{
		desire *= 1.5f;
		kinds |= HMODE_ACT_ASSIST;
	}

	// coverage: a believed teammate is at this item and closer than we are
	if (!strncmp(goal->classname, "item_", 5) || !strncmp(goal->classname, "weapon_", 7))
	{
		for (i = 1; i <= MAX_CLIENTS; i++)
		{
			hm_tm_t *tm = &slot->tm[i];
			gedict_t *mate = &g_edicts[i];
			float d;

			if ((mate == self) || (mate->ct != ctPlayer) || !SameTeam(mate, self))
			{
				continue;
			}

			if ((tm->source == HMODE_SRC_NONE) || !tm->loc_known
					|| (tm->time < now - 12))
			{
				continue;
			}

			d = VectorDistance(tm->org, goal->s.v.origin);

			if ((d < 320) && (d < VectorDistance(self->s.v.origin, goal->s.v.origin)))
			{
				desire *= 0.5f;
				kinds |= HMODE_ACT_COVERED;
				break;
			}
		}
	}

	if (kinds)
	{
		if (slot->act_n < HMODE_MAX_ACT)
		{
			slot->act[slot->act_n].goal = goal;
			slot->act[slot->act_n].kinds = kinds;
			slot->act_n++;
		}

		if (HMode_DebugOn())
		{
			char ks[96];

			HMode_ActKindsStr(kinds, ks, sizeof(ks));
			G_cprint("[hm-act] bot=%s kinds=%s goal=%s d0=%.1f d1=%.1f t=%.1f\n",
						self->netname, ks, HMode_GoalLogName(goal), d0, desire,
						g_globalvars.time);
		}
	}

	return desire;
}

// Commit-time telemetry: which goal the brain settled on, and whether comms
// biased it. Logged on-change only.
void HMode_LogGoalChoice(gedict_t *self, gedict_t *goal)
{
	hm_bot_t *slot;
	char ks[96];
	int kinds = 0, i;

	if (!goal || !self->isBot || !HMode_Active(self) || !HMode_DebugOn())
	{
		return;
	}

	slot = HMode_Slot(self);

	if (!slot)
	{
		return;
	}

	for (i = 0; i < slot->act_n; i++)
	{
		if (slot->act[i].goal == goal)
		{
			kinds |= slot->act[i].kinds;
		}
	}

	if ((goal == slot->goal_logged) && (kinds == slot->goal_logged_kinds))
	{
		return;
	}

	slot->goal_logged = goal;
	slot->goal_logged_kinds = kinds;

	HMode_ActKindsStr(kinds, ks, sizeof(ks));
	G_cprint("[hm-goal] bot=%s goal=%s commed=%s t=%.1f\n", self->netname,
				HMode_GoalLogName(goal), ks, g_globalvars.time);
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

// ---- teamsay parsing (S5) ----
//
// C port of the 18-rule ordered ruleset validated at 98.1% category hit
// rate over 98,468 teamsays from 142 clans (komodobots docs/notes/
// multiclan-teamsay-parse-study.md; executable reference
// experiments/mm2_comms/scripts/analyze2_parser.py::RULES). Token-based
// instead of regex (QVM has no regex): tokens are typed into slots
// ({hp} {wpn} {loc} {n} {e}> nick ##) and the category rules check
// keywords + slot shapes in the same order as the Python spec.

#define HMP_MAX_TOKENS 24
#define HMP_TOK_LEN 24

typedef enum
{
	HMP_WORD = 0,
	HMP_NICK,		// leading sender tag ("wim", "mil ...", or "name:")
	HMP_HP,			// armor/health: "0/100", "r200/172", "150y/87"
	HMP_WPN,		// weapon:ammo: "rl:5", "lg=40"
	HMP_ECOUNT,		// enemy count: "2>", "|3>"
	HMP_TIME,		// "m:ss" (never emitted by elites; parsed anyway)
	HMP_NUM,		// bare 1-3 digit number (usually a place name on dm3!)
	HMP_LOC			// location token (empirical dm3 lexicon)
	// NOTE: "##" enemy-marker runs stay literal WORD tokens, exactly like
	// the spec's normalize() leaves them; the classifier looks for the
	// double-hash substring itself (a single '#' is NOT an enemy marker).
} hmp_toktype_t;

typedef struct
{
	int type;					// hmp_toktype_t
	char text[HMP_TOK_LEN];		// cleaned lowercase token
	int num;					// HP: armor / WPN: ammo / ECOUNT,NUM: value
	int num2;					// HP: health
	char kind;					// HP: armor letter r/y/g or 0; WPN: 0
} hmp_tok_t;

// Empirical dm3 loc-root lexicon (study §3: 135 surface tokens, ~24 roots).
static const char *hmp_loc_roots[] =
{
	"ra", "ya", "ga", "mega", "mh", "quad", "pent", "penta", "ring", "eyes",
	"sng", "ssg", "gl", "rl", "lg", "hill", "water", "bridge", "tunnel",
	"tele", "window", "win", "lifts", "lift", "high", "low", "big",
	"bigroom", "wind", "sw", "ledge", "spawn", "spawns", "box", "outs",
	"out", "mid", "back", "front", "top", "bottom", "stairs", "door",
	"hall", "pillar", "corner", "side", "room", "base", "ramp", "walk",
	"floor", "entrance", "exit", NULL
};

static qbool HMP_IsLocRoot(const char *s, int len)
{
	int i;

	for (i = 0; hmp_loc_roots[i]; i++)
	{
		if (((int)strlen(hmp_loc_roots[i]) == len)
				&& !strncmp(hmp_loc_roots[i], s, len))
		{
			return true;
		}
	}

	return false;
}

// a loc "part": a root or an exact concatenation of two roots ("ratunnel")
static qbool HMP_IsLocPart(const char *s, int len)
{
	int i;

	if (HMP_IsLocRoot(s, len))
	{
		return true;
	}

	for (i = 1; i < len; i++)
	{
		if (HMP_IsLocRoot(s, i) && HMP_IsLocRoot(s + i, len - i))
		{
			return true;
		}
	}

	return false;
}

// token is a loc if it is a part (root/two-root concat) or dot/dash/slash-
// separated parts that are all parts ("ra.tunnel", "ratunnel-mega").
static qbool HMP_IsLocToken(const char *s)
{
	int len = strlen(s), i;

	if (!len)
	{
		return false;
	}

	if (HMP_IsLocPart(s, len))
	{
		return true;
	}

	// separator-split parts, all parts
	{
		int start = 0, parts = 0;

		for (i = 0; i <= len; i++)
		{
			if ((i == len) || (s[i] == '.') || (s[i] == '-') || (s[i] == '/')
					|| (s[i] == '_'))
			{
				if (i > start)
				{
					if (!HMP_IsLocPart(s + start, i - start))
					{
						return false;
					}

					parts++;
				}

				start = i + 1;
			}
		}

		return parts > 1;
	}
}

static qbool HMP_AllDigits(const char *s, int len)
{
	int i;

	if ((len < 1) || (len > 3))
	{
		return false;
	}

	for (i = 0; i < len; i++)
	{
		if ((s[i] < '0') || (s[i] > '9'))
		{
			return false;
		}
	}

	return true;
}

static int HMP_ParseInt(const char *s, int len)
{
	int v = 0, i;

	for (i = 0; i < len; i++)
	{
		v = v * 10 + (s[i] - '0');
	}

	return v;
}

// "r200/172" / "0/100" / "150y/87" -> armor kind + armor/health values
static qbool HMP_ParseHp(const char *s, hmp_tok_t *tok)
{
	const char *slash = strchr(s, '/');
	const char *a = s;
	int alen, hlen;
	char kind = 0;

	if (!slash)
	{
		return false;
	}

	// optional leading armor tag: ra/ya/ga or single r/y/g/b
	if (!strncmp(a, "ra", 2) || !strncmp(a, "ya", 2) || !strncmp(a, "ga", 2))
	{
		if ((a[2] >= '0') && (a[2] <= '9'))
		{
			kind = a[0];
			a += 2;
		}
	}
	else if ((*a == 'r') || (*a == 'y') || (*a == 'g') || (*a == 'b'))
	{
		if ((a[1] >= '0') && (a[1] <= '9'))
		{
			kind = (*a == 'b') ? 0 : *a;
			a++;
		}
	}

	alen = slash - a;

	// optional trailing armor letter before the slash: "150y/87"
	if ((alen > 1) && ((a[alen - 1] == 'r') || (a[alen - 1] == 'y') || (a[alen - 1] == 'g')))
	{
		kind = a[alen - 1];
		alen--;
	}

	hlen = strlen(slash + 1);

	if (!HMP_AllDigits(a, alen) || !HMP_AllDigits(slash + 1, hlen))
	{
		return false;
	}

	tok->kind = kind;
	tok->num = HMP_ParseInt(a, alen);
	tok->num2 = HMP_ParseInt(slash + 1, hlen);

	return true;
}

// "rl:5" / "lg=40" / single-letter forms "r:12", optional chained groups
// "r:12c:100" -- anchored like the spec's re_wpnammo (trailing junk = word)
static qbool HMP_ParseWpn(const char *s, hmp_tok_t *tok)
{
	static const char *wpns[] =
	{
		"rl", "lg", "gl", "sng", "ssg", "sg", "ng", "c", "r", "n", "s", "b", NULL
	};
	int i;

	for (i = 0; wpns[i]; i++)
	{
		int wl = strlen(wpns[i]);

		if (!strncmp(s, wpns[i], wl) && ((s[wl] == ':') || (s[wl] == '=')))
		{
			const char *d = s + wl + 1;
			const char *p;
			int dl = 0;

			while (d[dl] && (d[dl] >= '0') && (d[dl] <= '9'))
			{
				dl++;
			}

			if ((dl < 1) || (dl > 3))
			{
				continue;
			}

			// "([a-z]:?\d+)*$" tail groups, then end of token
			p = d + dl;

			while (*p)
			{
				int gd = 0;

				if ((*p < 'a') || (*p > 'z'))
				{
					break;
				}

				p++;

				if (*p == ':')
				{
					p++;
				}

				while (*p && (*p >= '0') && (*p <= '9'))
				{
					p++;
					gd++;
				}

				if (!gd)
				{
					break;
				}
			}

			if (*p)
			{
				continue;
			}

			strlcpy(tok->text, wpns[i], HMP_TOK_LEN);
			tok->num = HMP_ParseInt(d, dl);

			return true;
		}
	}

	return false;
}

// "m:ss" clock token (spec re_time: 1-2 digits, colon, exactly 2 digits)
static qbool HMP_ParseTime(const char *s)
{
	const char *colon = strchr(s, ':');
	int alen;

	if (!colon)
	{
		return false;
	}

	alen = colon - s;

	return (alen >= 1) && (alen <= 2) && HMP_AllDigits(s, alen)
			&& (strlen(colon + 1) == 2) && HMP_AllDigits(colon + 1, 2);
}

// nick check, spec nick_matches(): both sides cleaned to [a-z0-9], true when
// the cleaned player starts with the token OR the token starts with the
// player's 2-3 char stem. Runtime extra: the k_nick userinfo tag also counts.
static qbool HMP_NickMatches(const char *tok, gedict_t *sender)
{
	char p[32], t[32];
	const char *nick;
	int i, np = 0, nt = 0, k;

	for (i = 0; sender->netname[i] && (np < (int)sizeof(p) - 1); i++)
	{
		unsigned char c = (unsigned char)sender->netname[i];

		if (c >= 128)
		{
			c = (unsigned char)(c - 128);
		}

		if ((c >= 'A') && (c <= 'Z'))
		{
			c = (unsigned char)(c - 'A' + 'a');
		}

		if (((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')))
		{
			p[np++] = (char)c;
		}
	}

	p[np] = '\0';

	for (i = 0; tok[i] && (nt < (int)sizeof(t) - 1); i++)
	{
		char c = tok[i];	// tokenizer already folded + lowercased

		if (((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')))
		{
			t[nt++] = c;
		}
	}

	t[nt] = '\0';

	if (np && nt)
	{
		if ((nt <= np) && !strncmp(p, t, nt))
		{
			return true;	// p.startswith(t)
		}

		k = (np < 3) ? np : 3;	// len(p[:max(2, min(3, len(p)))])

		if ((nt >= k) && !strncmp(t, p, k))
		{
			return true;	// t.startswith(p[:k])
		}
	}

	nick = ezinfokey(sender, "k_nick");

	return !strnull(nick) && streq(nick, tok);
}

// Tokenize an uncolored teamsay: strip markup glyphs (\r \20 \21 { } [ ]),
// fold high-bit chars, lowercase, split, type each token.
static int HMP_Tokenize(const char *text, gedict_t *sender, hmp_tok_t *toks)
{
	char clean[256];
	int n = 0, i, c;
	int ntok = 0;
	const char *p;

	for (i = 0, p = text; *p && (n < (int)sizeof(clean) - 1); p++)
	{
		c = (unsigned char)*p;

		if (c >= 128)
		{
			c = c - 128;
		}

		// ezQuake color markup from tinted tags (HMode_DecorateTag): "&cRGB"
		// and "&r" vanish as separators so "&cf00hib&r" tokenizes as "hib".
		if ((c == '&') && ((p[1] == 'c') || (p[1] == 'C'))
				&& p[2] && p[3] && p[4]
				&& HMode_IsHexColorChar(p[2]) && HMode_IsHexColorChar(p[3])
				&& HMode_IsHexColorChar(p[4]))
		{
			clean[n++] = ' ';
			p += 4;
			continue;
		}

		if ((c == '&') && ((p[1] == 'r') || (p[1] == 'R')))
		{
			clean[n++] = ' ';
			p += 1;
			continue;
		}

		if ((c == '\r') || (c == '\n') || (c == 16) || (c == 17) || (c == '{')
				|| (c == '}') || (c == '[') || (c == ']'))
		{
			// markup wrappers become separators
			clean[n++] = ' ';
			continue;
		}

		if (c == 0x0b)
		{
			// the point glyph renders as '#'; a run of them is the
			// double-hash enemy marker the classifier looks for
			clean[n++] = '#';
			continue;
		}

		if ((c >= 'A') && (c <= 'Z'))
		{
			c = c - 'A' + 'a';
		}

		if (c < 32)
		{
			clean[n++] = ' ';
			continue;
		}

		clean[n++] = (char)c;
	}

	clean[n] = '\0';

	// split
	{
		char *s = clean;

		while (*s && (ntok < HMP_MAX_TOKENS))
		{
			char raw[HMP_TOK_LEN];
			int rl = 0;
			hmp_tok_t *tok;

			while (*s == ' ')
			{
				s++;
			}

			if (!*s)
			{
				break;
			}

			while (*s && (*s != ' ') && (rl < HMP_TOK_LEN - 1))
			{
				raw[rl++] = *s++;
			}

			raw[rl] = '\0';

			tok = &toks[ntok];
			memset(tok, 0, sizeof(*tok));

			// spec normalize(): core = tok.rstrip(":").strip("|"); the nick
			// check sees the raw colon, {e}> matches the raw token, all
			// other slot types match the core.
			{
				char core[HMP_TOK_LEN];
				int cl = rl, lead = 0;
				qbool had_colon = (rl > 0) && (raw[rl - 1] == ':');

				strlcpy(core, raw, HMP_TOK_LEN);

				while ((cl > 0) && (core[cl - 1] == ':'))
				{
					core[--cl] = '\0';
				}

				while ((cl > 0) && (core[cl - 1] == '|'))
				{
					core[--cl] = '\0';
				}

				while (core[lead] == '|')
				{
					lead++;
				}

				if (lead)
				{
					memmove(core, core + lead, cl - lead + 1);
					cl -= lead;
				}

				strlcpy(tok->text, cl ? core : raw, HMP_TOK_LEN);

				if ((ntok == 0) && (had_colon || HMP_NickMatches(core, sender)))
				{
					tok->type = HMP_NICK;
				}
				else if (HMP_ParseHp(core, tok))
				{
					tok->type = HMP_HP;
				}
				else if (HMP_ParseWpn(core, tok))
				{
					tok->type = HMP_WPN;
				}
				else if (((rl == 2) && HMP_AllDigits(raw, 1) && (raw[1] == '>'))
						|| ((rl == 3) && (raw[0] == '|') && HMP_AllDigits(raw + 1, 1)
							&& (raw[2] == '>')))
				{
					// spec {e}>: fullmatch "\|?\d\>" on the RAW token
					tok->type = HMP_ECOUNT;
					tok->num = raw[(raw[0] == '|') ? 1 : 0] - '0';
				}
				else if (HMP_ParseTime(core))
				{
					tok->type = HMP_TIME;
				}
				else if (HMP_AllDigits(core, cl))
				{
					tok->type = HMP_NUM;
					tok->num = HMP_ParseInt(core, cl);
				}
				else if (HMP_IsLocToken(core))
				{
					tok->type = HMP_LOC;
				}
				else
				{
					tok->type = HMP_WORD;
				}
			}

			ntok++;
		}
	}

	return ntok;
}

// ---- spec-faithful matching helpers ----
// The classify rules are regexes over the spec's normalized template string
// ("took ra" -> "took {loc}"). Rather than joining a string, each token
// exposes its template text via HMP_Tpl() and the helpers below reproduce
// the \b / adjacency semantics of the regexes over those texts. Crucially,
// pure quad/pent/penta/ring/eyes tokens are loc-roots and normalize to
// "{loc}", so the literal powerup alternations only fire inside punctuated
// tokens ("quad!", "pent,rl") -- the spec quirk behind parity bucket 1.

static const char* HMP_Tpl(const hmp_tok_t *tok)
{
	switch (tok->type)
	{
		case HMP_NICK:   return "{nick}";
		case HMP_HP:     return "{hp}";
		case HMP_WPN:    return "{wpn}";
		case HMP_ECOUNT: return "{e}>";
		case HMP_TIME:   return "{t}";
		case HMP_NUM:    return "{n}";
		case HMP_LOC:    return "{loc}";
		default:         return tok->text;
	}
}

static qbool HMP_IsWordChar(char c)
{
	return ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z'))
			|| ((c >= '0') && (c <= '9')) || (c == '_');
}

static qbool HMP_StartsWith(const char *hay, const char *w)
{
	return !strncmp(hay, w, strlen(w));
}

static qbool HMP_EndsWith(const char *hay, const char *w)
{
	int hl = strlen(hay), wl = strlen(w);

	return (hl >= wl) && !strcmp(hay + hl - wl, w);
}

// "w..." with a \b after w (end of token or non-word char)
static qbool HMP_StartsWord(const char *hay, const char *w)
{
	int wl = strlen(w);

	return !strncmp(hay, w, wl) && !HMP_IsWordChar(hay[wl]);
}

// "...w" with a \b before w (start of token or non-word char)
static qbool HMP_EndsWord(const char *hay, const char *w)
{
	int hl = strlen(hay), wl = strlen(w);

	return (hl >= wl) && !strcmp(hay + hl - wl, w)
			&& ((hl == wl) || !HMP_IsWordChar(hay[hl - wl - 1]));
}

// \bw\b anywhere inside the token text
static qbool HMP_StrWord(const char *hay, const char *w)
{
	int wl = strlen(w);
	const char *p;

	for (p = hay; *p; p++)
	{
		if (!strncmp(p, w, wl)
				&& ((p == hay) || !HMP_IsWordChar(p[-1]))
				&& !HMP_IsWordChar(p[wl]))
		{
			return true;
		}
	}

	return false;
}

// keyword helpers over the token templates (\bw\b over the joined template:
// a word cannot span tokens, so scanning per-token is equivalent)
static qbool HMP_HasWord(hmp_tok_t *toks, int n, const char *w)
{
	int i;

	for (i = 0; i < n; i++)
	{
		if (HMP_StrWord(HMP_Tpl(&toks[i]), w))
		{
			return true;
		}
	}

	return false;
}

static qbool HMP_HasAny(hmp_tok_t *toks, int n, const char **words)
{
	int i;

	for (i = 0; words[i]; i++)
	{
		if (HMP_HasWord(toks, n, words[i]))
		{
			return true;
		}
	}

	return false;
}

static qbool HMP_StartsWordAny(const char *hay, const char **words)
{
	int i;

	for (i = 0; words[i]; i++)
	{
		if (HMP_StartsWord(hay, words[i]))
		{
			return true;
		}
	}

	return false;
}

static int HMP_FirstType(hmp_tok_t *toks, int n, int type)
{
	int i;

	for (i = 0; i < n; i++)
	{
		if (toks[i].type == type)
		{
			return i;
		}
	}

	return -1;
}

// \b(quad|pent|penta|ring|eyes)\b inside one token's template text
static qbool HMP_TplHasPw(const char *tpl)
{
	return HMP_StrWord(tpl, "quad") || HMP_StrWord(tpl, "pent")
			|| HMP_StrWord(tpl, "penta") || HMP_StrWord(tpl, "ring")
			|| HMP_StrWord(tpl, "eyes");
}

// Categories (superset of #256 §2, from the study)
typedef enum
{
	HMP_CAT_UNPARSED = 0,
	HMP_CAT_ENEMY_POWERUP,
	HMP_CAT_ENEMY_SEEN,
	HMP_CAT_LOST,
	HMP_CAT_DROPPED,
	HMP_CAT_TOOK,
	HMP_CAT_HELP,
	HMP_CAT_PACK,
	HMP_CAT_ITEM_AT,
	HMP_CAT_NEED,
	HMP_CAT_TEAM_POWERUP,
	HMP_CAT_POWERUP_STATUS,
	HMP_CAT_REQUEST,
	HMP_CAT_COMING,
	HMP_CAT_WAITING,
	HMP_CAT_SAFE,
	HMP_CAT_STATUS,
	HMP_CAT_ENEMY_AT_NICK,
	HMP_CAT_LOCATION_PING,
	HMP_CAT_SLIPPED,
	HMP_CAT_DEATH
} hmp_cat_t;

// Ordered classification: a 1:1 port of analyze2_parser.py::RULES (first
// match wins). Each block header quotes the spec regex it implements.
static int HMP_Classify(hmp_tok_t *toks, int n)
{
	static const char *w_dropped[] = { "dropped", "droped", "drop", NULL };
	static const char *w_took[] = { "took", "taken", "got", "taking", "peguei", NULL };
	static const char *w_help[] = { "help", "hlp", NULL };
	static const char *w_ammo[] = { "rox", "cells", "rockets", "ammo", "pens", NULL };
	static const char *w_need[] = { "need", "gimme", "gime", "want", NULL };
	static const char *w_status_pw[] = { "up", "spawning", "spawned", "over",
			"soon", "dead", "out", "now", NULL };
	static const char *w_req[] = { "get", "take", "go", "push", "camp", "hold",
			"wait", "rush", "kill", "focus", "replace", "pega", "fix",
			"attack", "bierz", NULL };
	static const char *w_coming[] = { "coming", "comming", "incoming", "omw", NULL };
	static const char *w_wait[] = { "waiting", "await", "awaits", "awaiting",
			"camping", "holding", NULL };
	static const char *w_safe[] = { "safe", "clear", "secure", NULL };
	static const char *w_slip[] = { "slipped", "missed", NULL };
	static const char *w_death[] = { "died", "dead", "rip", NULL };
	int i, j;

	if (!n)
	{
		return HMP_CAT_UNPARSED;
	}

	// 1. enemy_powerup:
	//    "## .*\b(quad|pent|penta|ring|eyes)\b|enemy (quad|pent|ring)|e (quad|pent)\b"
	for (i = 0; i + 1 < n; i++)
	{
		const char *a = HMP_Tpl(&toks[i]);
		const char *b = HMP_Tpl(&toks[i + 1]);

		if (HMP_EndsWith(a, "##"))
		{
			for (j = i + 1; j < n; j++)
			{
				if (HMP_TplHasPw(HMP_Tpl(&toks[j])))
				{
					return HMP_CAT_ENEMY_POWERUP;
				}
			}
		}

		if (HMP_EndsWith(a, "enemy")
				&& (HMP_StartsWith(b, "quad") || HMP_StartsWith(b, "pent")
					|| HMP_StartsWith(b, "ring")))
		{
			return HMP_CAT_ENEMY_POWERUP;
		}

		if (HMP_EndsWith(a, "e")
				&& (HMP_StartsWord(b, "quad") || HMP_StartsWord(b, "pent")))
		{
			return HMP_CAT_ENEMY_POWERUP;
		}
	}

	// 2. enemy_seen:
	//    "##|enem(y|ies)|\bnmy\b|\bnme\b|\beyes\b at|\be\b at {loc}|{e}> only$"
	for (i = 0; i < n; i++)
	{
		const char *a = HMP_Tpl(&toks[i]);

		if (strstr(a, "##") || strstr(a, "enemy") || strstr(a, "enemies")
				|| HMP_StrWord(a, "nmy") || HMP_StrWord(a, "nme"))
		{
			return HMP_CAT_ENEMY_SEEN;
		}

		if ((i + 1 < n) && HMP_EndsWord(a, "eyes")
				&& HMP_StartsWith(HMP_Tpl(&toks[i + 1]), "at"))
		{
			return HMP_CAT_ENEMY_SEEN;
		}

		if ((i + 2 < n) && HMP_EndsWord(a, "e")
				&& streq(HMP_Tpl(&toks[i + 1]), "at")
				&& HMP_StartsWith(HMP_Tpl(&toks[i + 2]), "{loc}"))
		{
			return HMP_CAT_ENEMY_SEEN;
		}

		if ((i + 2 == n) && streq(a, "{e}>")
				&& streq(HMP_Tpl(&toks[i + 1]), "only"))
		{
			return HMP_CAT_ENEMY_SEEN;
		}
	}

	// 3. lost: "\blost\b"
	if (HMP_HasWord(toks, n, "lost"))
	{
		return HMP_CAT_LOST;
	}

	// 4. dropped: "\bdropp?ed\b|\bdrop\b"
	if (HMP_HasAny(toks, n, w_dropped))
	{
		return HMP_CAT_DROPPED;
	}

	// 5. took: "\btook\b|\btaken\b|\bgot\b|\bhave\b {loc}|\btaking\b|\bpeguei\b"
	if (HMP_HasAny(toks, n, w_took))
	{
		return HMP_CAT_TOOK;
	}

	for (i = 0; i + 1 < n; i++)
	{
		if (HMP_EndsWord(HMP_Tpl(&toks[i]), "have")
				&& HMP_StartsWith(HMP_Tpl(&toks[i + 1]), "{loc}"))
		{
			return HMP_CAT_TOOK;
		}
	}

	// 6. help: "\bhelp\b|\bhlp\b"
	if (HMP_HasAny(toks, n, w_help))
	{
		return HMP_CAT_HELP;
	}

	// 7. pack_available: "pack (at|left|here|<|@)|{loc} pack\b|pack {loc}|
	//    left at|\bpack\b at|\b(rox|cells|rockets|ammo|pens) (at|<|@) "
	for (i = 0; i + 1 < n; i++)
	{
		const char *a = HMP_Tpl(&toks[i]);
		const char *b = HMP_Tpl(&toks[i + 1]);

		if (HMP_EndsWith(a, "pack")
				&& (HMP_StartsWith(b, "at") || HMP_StartsWith(b, "left")
					|| HMP_StartsWith(b, "here") || HMP_StartsWith(b, "<")
					|| HMP_StartsWith(b, "@") || HMP_StartsWith(b, "{loc}")))
		{
			return HMP_CAT_PACK;
		}

		if (HMP_EndsWith(a, "{loc}") && HMP_StartsWord(b, "pack"))
		{
			return HMP_CAT_PACK;
		}

		if (HMP_EndsWith(a, "left") && HMP_StartsWith(b, "at"))
		{
			return HMP_CAT_PACK;
		}

		// ammo-word alternative: trailing space in the regex means the
		// at/</@ token cannot be the last one
		if ((i + 2 < n)
				&& (streq(b, "at") || streq(b, "<") || streq(b, "@")))
		{
			for (j = 0; w_ammo[j]; j++)
			{
				if (HMP_EndsWord(a, w_ammo[j]))
				{
					return HMP_CAT_PACK;
				}
			}
		}
	}

	// 8. item_at: "{loc} (at|<|@|no) ({loc}|{n}|raup)|\bmega at\b|
	//    \bspawn(ed)? at\b|<<+ {loc}"
	for (i = 0; i + 1 < n; i++)
	{
		const char *a = HMP_Tpl(&toks[i]);
		const char *b = HMP_Tpl(&toks[i + 1]);
		const char *c = (i + 2 < n) ? HMP_Tpl(&toks[i + 2]) : NULL;

		if (c && HMP_EndsWith(a, "{loc}")
				&& (streq(b, "at") || streq(b, "<") || streq(b, "@") || streq(b, "no"))
				&& (HMP_StartsWith(c, "{loc}") || HMP_StartsWith(c, "{n}")
					|| HMP_StartsWith(c, "raup")))
		{
			return HMP_CAT_ITEM_AT;
		}

		if (HMP_EndsWord(a, "mega") && HMP_StartsWord(b, "at"))
		{
			return HMP_CAT_ITEM_AT;
		}

		if ((HMP_EndsWord(a, "spawn") || HMP_EndsWord(a, "spawned"))
				&& HMP_StartsWord(b, "at"))
		{
			return HMP_CAT_ITEM_AT;
		}

		if (HMP_EndsWith(a, "<<") && HMP_StartsWith(b, "{loc}"))
		{
			return HMP_CAT_ITEM_AT;
		}
	}

	// 9. need: "\bneed\b|\bgimm?e\b|\bwant\b"
	if (HMP_HasAny(toks, n, w_need))
	{
		return HMP_CAT_NEED;
	}

	// 10. team_powerup: "\bteam (quad|pent|ring|{loc})"
	for (i = 0; i + 1 < n; i++)
	{
		const char *b = HMP_Tpl(&toks[i + 1]);

		if (HMP_EndsWord(HMP_Tpl(&toks[i]), "team")
				&& (HMP_StartsWith(b, "quad") || HMP_StartsWith(b, "pent")
					|| HMP_StartsWith(b, "ring") || HMP_StartsWith(b, "{loc}")))
		{
			return HMP_CAT_TEAM_POWERUP;
		}
	}

	// 11. powerup_status: "(quad|pent) {t}|(quad|pent|ring|{loc}) (is )?
	//     (up|spawning|spawned|over|soon|dead|out|now)\b|(quad|pent|ring|{loc})
	//     (on|in) {n}|\bover {loc}|\bsoon {loc}|quad soon|pent soon"
	//     (the "^{nick} (quad|...) " alternative can never fire: a pure
	//     powerup token normalizes to {loc}, a punctuated one breaks the
	//     required trailing space -- dead spec branch, omitted)
	for (i = 0; i + 1 < n; i++)
	{
		const char *a = HMP_Tpl(&toks[i]);
		const char *b = HMP_Tpl(&toks[i + 1]);
		qbool qp = HMP_EndsWith(a, "quad") || HMP_EndsWith(a, "pent");
		qbool pwloc = qp || HMP_EndsWith(a, "ring") || HMP_EndsWith(a, "{loc}");

		if (qp && (HMP_StartsWith(b, "{t}") || HMP_StartsWith(b, "soon")))
		{
			return HMP_CAT_POWERUP_STATUS;
		}

		if (pwloc)
		{
			if (HMP_StartsWordAny(b, w_status_pw))
			{
				return HMP_CAT_POWERUP_STATUS;
			}

			if ((i + 2 < n) && streq(b, "is")
					&& HMP_StartsWordAny(HMP_Tpl(&toks[i + 2]), w_status_pw))
			{
				return HMP_CAT_POWERUP_STATUS;
			}

			if ((i + 2 < n) && (streq(b, "on") || streq(b, "in"))
					&& HMP_StartsWith(HMP_Tpl(&toks[i + 2]), "{n}"))
			{
				return HMP_CAT_POWERUP_STATUS;
			}
		}

		if ((HMP_EndsWord(a, "over") || HMP_EndsWord(a, "soon"))
				&& HMP_StartsWith(b, "{loc}"))
		{
			return HMP_CAT_POWERUP_STATUS;
		}
	}

	// 12. request_action: "\bget\b|\btake\b|\bgo\b|...|\bwait\b(?!ing)"
	//     (the (?!ing) guard falls out of \b matching: "waiting" has no
	//     boundary after "wait")
	if (HMP_HasAny(toks, n, w_req))
	{
		return HMP_CAT_REQUEST;
	}

	// 13. coming: "\bcoming\b|\bcomming\b|\bincoming\b|\bon (my|the) way\b|\bomw\b"
	if (HMP_HasAny(toks, n, w_coming))
	{
		return HMP_CAT_COMING;
	}

	for (i = 0; i + 2 < n; i++)
	{
		const char *b = HMP_Tpl(&toks[i + 1]);

		if (HMP_EndsWord(HMP_Tpl(&toks[i]), "on")
				&& (streq(b, "my") || streq(b, "the"))
				&& HMP_StartsWord(HMP_Tpl(&toks[i + 2]), "way"))
		{
			return HMP_CAT_COMING;
		}
	}

	// 14. waiting: "\bwaiting\b|\bawait(s|ing)?\b|\bcamping\b|\bholding\b"
	if (HMP_HasAny(toks, n, w_wait))
	{
		return HMP_CAT_WAITING;
	}

	// 15. safe: "\bsafe\b|\bclear\b|\bok\b {loc}|\bsecure\b"
	if (HMP_HasAny(toks, n, w_safe))
	{
		return HMP_CAT_SAFE;
	}

	for (i = 0; i + 1 < n; i++)
	{
		if (HMP_EndsWord(HMP_Tpl(&toks[i]), "ok")
				&& HMP_StartsWith(HMP_Tpl(&toks[i + 1]), "{loc}"))
		{
			return HMP_CAT_SAFE;
		}
	}

	// 16. status_report: "{hp}"
	if (HMP_FirstType(toks, n, HMP_HP) >= 0)
	{
		return HMP_CAT_STATUS;
	}

	// 17. enemy_at_nick: "\b[\w*'.-]+ at {loc}" -- the run must END right
	//     before " at", so the preceding token's last char must be in the
	//     class (typed slots end in '}'/'>' and can never fire)
	for (i = 0; i + 2 < n; i++)
	{
		const char *a = HMP_Tpl(&toks[i]);
		int al = strlen(a);
		char lc = al ? a[al - 1] : '\0';

		if ((HMP_IsWordChar(lc) || (lc == '*') || (lc == '\'') || (lc == '.')
					|| (lc == '-'))
				&& streq(HMP_Tpl(&toks[i + 1]), "at")
				&& HMP_StartsWith(HMP_Tpl(&toks[i + 2]), "{loc}"))
		{
			return HMP_CAT_ENEMY_AT_NICK;
		}
	}

	// 18. location_ping (anchored): "^{nick} {loc}( {loc})*( {e}>)?$|
	//     ^{nick} @ {loc}$|^{loc}( {loc})*$"
	if ((n >= 2) && (toks[0].type == HMP_NICK))
	{
		if (toks[1].type == HMP_LOC)
		{
			qbool ok = true;

			for (i = 1; i < n; i++)
			{
				if (toks[i].type == HMP_LOC)
				{
					continue;
				}

				if ((i == n - 1) && (toks[i].type == HMP_ECOUNT))
				{
					continue;	// optional single trailing {e}>
				}

				ok = false;
				break;
			}

			if (ok)
			{
				return HMP_CAT_LOCATION_PING;
			}
		}

		if ((n == 3) && streq(HMP_Tpl(&toks[1]), "@") && (toks[2].type == HMP_LOC))
		{
			return HMP_CAT_LOCATION_PING;
		}
	}

	{
		qbool all_loc = true;

		for (i = 0; i < n; i++)
		{
			if (toks[i].type != HMP_LOC)
			{
				all_loc = false;
				break;
			}
		}

		if (all_loc)
		{
			return HMP_CAT_LOCATION_PING;
		}
	}

	// 19. slipped: "\bslipped\b|\bquad missed\b|\bmissed\b"
	if (HMP_HasAny(toks, n, w_slip))
	{
		return HMP_CAT_SLIPPED;
	}

	// 20. death_report: "\bdied\b|\bdead\b|\brip\b"
	if (HMP_HasAny(toks, n, w_death))
	{
		return HMP_CAT_DEATH;
	}

	return HMP_CAT_UNPARSED;
}

// Resolve consecutive LOC/NUM tokens starting at `from` into coordinates
// via the .loc node table. Returns true + out on success.
static qbool HMP_ResolveLoc(hmp_tok_t *toks, int n, vec3_t out)
{
	char name[64];
	int i;

	for (i = 0; i < n; i++)
	{
		if ((toks[i].type == HMP_LOC) || (toks[i].type == HMP_NUM))
		{
			int j = i;

			name[0] = '\0';

			while ((j < n) && ((toks[j].type == HMP_LOC) || (toks[j].type == HMP_NUM)))
			{
				if (name[0])
				{
					strlcat(name, " ", sizeof(name));
				}

				strlcat(name, toks[j].text, sizeof(name));
				j++;
			}

			if (LocationCoordsByName(name, out))
			{
				return true;
			}

			// try the single first token too ("ra" out of "ra tunnel")
			if (LocationCoordsByName(toks[i].text, out))
			{
				return true;
			}

			i = j;
		}
	}

	return false;
}

// Map an item word to candidate classname (+mega flag), for belief updates.
static const char* HMP_ItemClassname(const char *w, qbool *want_mega)
{
	*want_mega = false;

	if (streq(w, "ra"))
	{
		return "item_armorInv";
	}

	if (streq(w, "ya"))
	{
		return "item_armor2";
	}

	if (streq(w, "ga"))
	{
		return "item_armor1";
	}

	if (streq(w, "mega") || streq(w, "mh"))
	{
		*want_mega = true;

		return "item_health";
	}

	if (streq(w, "quad"))
	{
		return "item_artifact_super_damage";
	}

	if (streq(w, "pent") || streq(w, "penta"))
	{
		return "item_artifact_invulnerability";
	}

	if (streq(w, "ring") || streq(w, "eyes"))
	{
		return "item_artifact_invisibility";
	}

	if (streq(w, "rl"))
	{
		return "weapon_rocketlauncher";
	}

	if (streq(w, "lg"))
	{
		return "weapon_lightning";
	}

	if (streq(w, "gl"))
	{
		return "weapon_grenadelauncher";
	}

	if (streq(w, "sng"))
	{
		return "weapon_supernailgun";
	}

	if (streq(w, "ssg"))
	{
		return "weapon_supershotgun";
	}

	return NULL;
}

// Find the item entity a report names: the first item word (WORD or LOC
// token) that resolves wins. Ambiguous types (two YAs on dm3) need a
// resolvable loc (nearest match); without one they are skipped.
static gedict_t* HMP_FindReportedItem(hmp_tok_t *toks, int n)
{
	vec3_t loc;
	qbool have_loc = HMP_ResolveLoc(toks, n, loc);
	int i;

	for (i = 0; i < n; i++)
	{
		qbool want_mega = false;
		const char *cn;
		gedict_t *best = NULL, *it;
		float best_d = 0;

		if ((toks[i].type != HMP_WORD) && (toks[i].type != HMP_LOC))
		{
			continue;
		}

		cn = HMP_ItemClassname(toks[i].text, &want_mega);

		if (!cn)
		{
			continue;
		}

		for (it = world; (it = find(it, FOFCLSN, (char *)cn));)
		{
			if (want_mega && !((int)it->s.v.spawnflags & H_MEGA))
			{
				continue;
			}

			if (!have_loc)
			{
				if (best)
				{
					best = NULL; // ambiguous without a location: skip

					break;
				}

				best = it;
			}
			else
			{
				float d = VectorDistance(it->s.v.origin, loc);

				if (!best || (d < best_d))
				{
					best = it;
					best_d = d;
				}
			}
		}

		if (best)
		{
			return best; // first resolvable item word wins
		}
	}

	return NULL;
}

// A teammate said "took {item} [{loc}]" (or implied it): the item's belief
// clock starts now.
static void HMP_ApplyItemTaken(gedict_t *receiver, gedict_t *sender,
							   hmp_tok_t *toks, int n)
{
	hm_bot_t *slot = HMode_Slot(receiver);
	gedict_t *best = HMP_FindReportedItem(toks, n);
	int idx;

	if (!slot || !best)
	{
		return;
	}

	idx = HMode_ItemIndex(best, true);

	if (idx >= 0)
	{
		float duration = HMode_ItemDuration(best);

		HMode_LogItemUpd(receiver, best, HMODE_SRC_TOLD, slot->items[idx].respawn_at,
							g_globalvars.time + duration, sender->netname);

		// A told report never downgrades a same-moment direct sighting;
		// otherwise the teammate's word is the freshest info we have.
		slot->items[idx].source = HMODE_SRC_TOLD;
		slot->items[idx].taken_time = g_globalvars.time;
		slot->items[idx].respawn_at = g_globalvars.time + duration;
	}
}

// A teammate reports an item UP ("ra at raup", "quad up", "mega at 5"):
// the belief clock collapses to "back at up_at".
static void HMP_ApplyItemUp(gedict_t *receiver, gedict_t *sender,
							hmp_tok_t *toks, int n, float up_at)
{
	hm_bot_t *slot = HMode_Slot(receiver);
	gedict_t *best = HMP_FindReportedItem(toks, n);
	int idx;

	if (!slot || !best)
	{
		return;
	}

	idx = HMode_ItemIndex(best, true);

	if (idx >= 0)
	{
		HMode_LogItemUpd(receiver, best, HMODE_SRC_TOLD, slot->items[idx].respawn_at,
							up_at, sender->netname);

		slot->items[idx].source = HMODE_SRC_TOLD;
		slot->items[idx].respawn_at = up_at;
	}
}

// Record a told-of enemy sighting (org NULL = position unknown).
static void HMP_AddSighting(gedict_t *receiver, gedict_t *sender, float *org,
							int count, qbool powerup)
{
	hm_bot_t *slot = HMode_Slot(receiver);
	hm_sight_t *s;

	if (!slot)
	{
		return;
	}

	s = &slot->sights[slot->sight_head];
	slot->sight_head = (slot->sight_head + 1) % HMODE_MAX_SIGHT;

	memset(s, 0, sizeof(*s));
	s->time = g_globalvars.time;
	s->count = (count > 0) ? count : 1;
	s->powerup = powerup;
	s->source = HMODE_SRC_TOLD;

	if (org)
	{
		VectorCopy(org, s->org);
		s->org_known = true;
	}

	if (HMode_DebugOn())
	{
		G_cprint("[hm-belief-upd] bot=%s kind=enemy org=%s count=%d pw=%d "
					"from=%s t=%.1f\n",
					receiver->netname,
					org ? va("%d,%d", (int)org[0], (int)org[1]) : "unknown",
					s->count, (int)s->powerup, sender->netname, g_globalvars.time);
	}
}

// "quad over/dead/out": the carried powerup wore off -- stand down the
// powerup alarm on every held sighting (they stay plain enemy sightings).
static void HMP_ClearPowerupAlarm(gedict_t *receiver, gedict_t *sender)
{
	hm_bot_t *slot = HMode_Slot(receiver);
	int i, cleared = 0;

	if (!slot)
	{
		return;
	}

	for (i = 0; i < HMODE_MAX_SIGHT; i++)
	{
		if ((slot->sights[i].time > 0) && slot->sights[i].powerup)
		{
			slot->sights[i].powerup = false;
			cleared++;
		}
	}

	if (cleared && HMode_DebugOn())
	{
		G_cprint("[hm-belief-upd] bot=%s kind=pw-over cleared=%d from=%s t=%.1f\n",
					receiver->netname, cleared, sender->netname, g_globalvars.time);
	}
}

// "safe {loc}": drop believed enemy sightings near the called-clear spot.
static void HMP_ClearSightingsNear(gedict_t *receiver, gedict_t *sender, vec3_t loc)
{
	hm_bot_t *slot = HMode_Slot(receiver);
	int i, cleared = 0;

	if (!slot)
	{
		return;
	}

	for (i = 0; i < HMODE_MAX_SIGHT; i++)
	{
		hm_sight_t *s = &slot->sights[i];

		if ((s->time > 0) && s->org_known && (VectorDistance(s->org, loc) < 512))
		{
			s->time = 0;
			cleared++;
		}
	}

	if (cleared && HMode_DebugOn())
	{
		G_cprint("[hm-belief-upd] bot=%s kind=safe-clear org=%d,%d cleared=%d "
					"from=%s t=%.1f\n",
					receiver->netname, (int)loc[0], (int)loc[1], cleared,
					sender->netname, g_globalvars.time);
	}
}

// Where does a report point? An explicit loc wins; else the sender's own
// fresh believed position (players talk about where they are).
static qbool HMP_ReportOrg(gedict_t *receiver, gedict_t *sender, hmp_tok_t *toks,
						   int n, vec3_t out)
{
	hm_bot_t *slot = HMode_Slot(receiver);
	int s = NUM_FOR_EDICT(sender);

	if (HMP_ResolveLoc(toks, n, out))
	{
		return true;
	}

	if (slot && (s >= 1) && (s <= MAX_CLIENTS))
	{
		hm_tm_t *tm = &slot->tm[s];

		if ((tm->source != HMODE_SRC_NONE) && tm->loc_known
				&& (tm->time > g_globalvars.time - 5))
		{
			VectorCopy(tm->org, out);

			return true;
		}
	}

	return false;
}

// does the message name a powerup at all (word or loc-typed token)?
static qbool HMP_MentionsPowerup(hmp_tok_t *toks, int n)
{
	int i;

	for (i = 0; i < n; i++)
	{
		const char *t = toks[i].text;

		if (((toks[i].type == HMP_WORD) || (toks[i].type == HMP_LOC))
				&& (streq(t, "quad") || streq(t, "pent") || streq(t, "penta")
					|| streq(t, "ring") || streq(t, "eyes")))
		{
			return true;
		}
	}

	return false;
}

static int HMP_EcountOr(hmp_tok_t *toks, int n, int fallback)
{
	int i = HMP_FirstType(toks, n, HMP_ECOUNT);

	return (i >= 0) ? toks[i].num : fallback;
}

// location-bearing sender report: update the sender's last known position
static void HMP_UpdateSenderLoc(gedict_t *receiver, gedict_t *sender, hm_tm_t *tm,
								hmp_tok_t *toks, int n)
{
	vec3_t loc;

	if (tm && !tm->fresh && HMP_ResolveLoc(toks, n, loc))
	{
		VectorCopy(loc, tm->org);
		tm->loc_known = true;
		tm->time = g_globalvars.time;

		if (tm->source < HMODE_SRC_TOLD)
		{
			tm->source = HMODE_SRC_TOLD;
		}

		if (HMode_DebugOn())
		{
			G_cprint("[hm-belief-upd] bot=%s kind=tm-loc mate=%s org=%d,%d "
						"src=told t=%.1f\n",
						receiver->netname, sender->netname, (int)loc[0],
						(int)loc[1], g_globalvars.time);
		}
	}
}

// Apply a parsed message to the receiver's world model.
static void HMP_Apply(gedict_t *receiver, gedict_t *sender, int cat,
					  hmp_tok_t *toks, int n)
{
	hm_bot_t *slot = HMode_Slot(receiver);
	int s = NUM_FOR_EDICT(sender);
	hm_tm_t *tm = NULL;
	vec3_t loc;

	if (!slot)
	{
		return;
	}

	if ((s >= 1) && (s <= MAX_CLIENTS) && (sender != receiver))
	{
		tm = &slot->tm[s];
	}

	switch (cat)
	{
		case HMP_CAT_STATUS:
		{
			// sender self-report: hp/armor, weapons, location
			int i;

			if (!tm)
			{
				break;
			}

			// don't let a report overwrite live sight
			if (tm->fresh)
			{
				break;
			}

			tm->source = HMODE_SRC_TOLD;
			tm->time = g_globalvars.time;

			for (i = 0; i < n; i++)
			{
				if (toks[i].type == HMP_HP)
				{
					tm->armor = toks[i].num;
					tm->health = toks[i].num2;

					if (toks[i].kind == 'r')
					{
						tm->items = (tm->items & ~(IT_ARMOR1 | IT_ARMOR2)) | IT_ARMOR3;
					}
					else if (toks[i].kind == 'y')
					{
						tm->items = (tm->items & ~(IT_ARMOR1 | IT_ARMOR3)) | IT_ARMOR2;
					}
					else if (toks[i].kind == 'g')
					{
						tm->items = (tm->items & ~(IT_ARMOR2 | IT_ARMOR3)) | IT_ARMOR1;
					}
				}
				else if (toks[i].type == HMP_WPN)
				{
					if (streq(toks[i].text, "rl") || streq(toks[i].text, "r"))
					{
						tm->items |= IT_ROCKET_LAUNCHER;
						tm->rockets = toks[i].num;
					}
					else if (streq(toks[i].text, "lg") || streq(toks[i].text, "c"))
					{
						tm->items |= IT_LIGHTNING;
						tm->cells = toks[i].num;
					}
					else if (streq(toks[i].text, "gl"))
					{
						tm->items |= IT_GRENADE_LAUNCHER;
					}
					else if (streq(toks[i].text, "sng"))
					{
						tm->items |= IT_SUPER_NAILGUN;
					}
					else if (streq(toks[i].text, "ssg"))
					{
						tm->items |= IT_SUPER_SHOTGUN;
					}
				}
			}

			if (HMP_ResolveLoc(toks, n, loc))
			{
				VectorCopy(loc, tm->org);
				tm->loc_known = true;
			}

			if (HMode_DebugOn())
			{
				G_cprint("[hm-belief-upd] bot=%s kind=tm mate=%s hp=%.0f arm=%.0f "
							"loc=%s src=told t=%.1f\n",
							receiver->netname, sender->netname, tm->health,
							tm->armor,
							tm->loc_known ? va("%d,%d", (int)tm->org[0],
												(int)tm->org[1]) : "unknown",
							g_globalvars.time);
			}

			break;
		}

		case HMP_CAT_LOST:
		case HMP_CAT_DEATH:
		{
			// sender died: same collapse as killfeed, but with a location --
			// and a death report with an enemy count marks where enemies are
			if (tm)
			{
				memset(tm, 0, sizeof(*tm));
				tm->source = HMODE_SRC_TOLD;
				tm->time = g_globalvars.time;
				tm->health = 100;
				tm->items = IT_SHOTGUN | IT_AXE;
			}

			if ((cat == HMP_CAT_LOST) && (HMP_EcountOr(toks, n, 0) > 0)
					&& HMP_ResolveLoc(toks, n, loc))
			{
				HMP_AddSighting(receiver, sender, loc, HMP_EcountOr(toks, n, 1),
								false);
			}

			break;
		}

		case HMP_CAT_TOOK:
		{
			HMP_ApplyItemTaken(receiver, sender, toks, n);

			// took also implies the sender is at that location
			HMP_UpdateSenderLoc(receiver, sender, tm, toks, n);

			break;
		}

		case HMP_CAT_TEAM_POWERUP:
		{
			// "team quad": some teammate holds it -> the item was taken now
			HMP_ApplyItemTaken(receiver, sender, toks, n);

			break;
		}

		case HMP_CAT_ENEMY_POWERUP:
		{
			// "enemy quad [at {loc}]": the powerup is down (clock starts) and
			// a powered enemy is about, maybe with a known position
			qbool has = HMP_ResolveLoc(toks, n, loc);

			HMP_ApplyItemTaken(receiver, sender, toks, n);
			HMP_AddSighting(receiver, sender, has ? loc : NULL,
							HMP_EcountOr(toks, n, 1), true);

			break;
		}

		case HMP_CAT_ENEMY_SEEN:
		case HMP_CAT_ENEMY_AT_NICK:
		{
			// "## 2 mega" / "carn at ra": enemies at a (maybe known) spot
			qbool has = HMP_ResolveLoc(toks, n, loc);

			HMP_AddSighting(receiver, sender, has ? loc : NULL,
							HMP_EcountOr(toks, n, 1), false);

			break;
		}

		case HMP_CAT_SLIPPED:
		{
			// "quad missed": the enemy got it -> clock starts, powered enemy
			// about (position unknown). Without a powerup word: no-op.
			if (HMP_MentionsPowerup(toks, n))
			{
				HMP_ApplyItemTaken(receiver, sender, toks, n);
				HMP_AddSighting(receiver, sender, NULL, 1, true);
			}

			break;
		}

		case HMP_CAT_DROPPED:
		case HMP_CAT_PACK:
		{
			// "dropped rl" / "pack at ya": a pack opportunity near the
			// reported (or sender's believed) position
			hm_bot_t *bslot = HMode_Slot(receiver);

			if (bslot && HMP_ReportOrg(receiver, sender, toks, n, loc))
			{
				VectorCopy(loc, bslot->pack_org);
				bslot->pack_time = g_globalvars.time;
				bslot->pack_valid = true;

				if (HMode_DebugOn())
				{
					G_cprint("[hm-belief-upd] bot=%s kind=pack org=%d,%d from=%s "
								"t=%.1f\n",
								receiver->netname, (int)loc[0], (int)loc[1],
								sender->netname, g_globalvars.time);
				}
			}

			break;
		}

		case HMP_CAT_ITEM_AT:
		{
			// "{item} at {loc}": the item is up right now
			HMP_ApplyItemUp(receiver, sender, toks, n, g_globalvars.time);

			break;
		}

		case HMP_CAT_POWERUP_STATUS:
		{
			// "quad up/now/spawned" -> up now; "quad soon/spawning" -> up
			// shortly; "quad over/dead/out" -> the carried powerup wore off
			if (HMP_HasWord(toks, n, "up") || HMP_HasWord(toks, n, "now")
					|| HMP_HasWord(toks, n, "spawned"))
			{
				HMP_ApplyItemUp(receiver, sender, toks, n, g_globalvars.time);
			}
			else if (HMP_HasWord(toks, n, "soon") || HMP_HasWord(toks, n, "spawning"))
			{
				HMP_ApplyItemUp(receiver, sender, toks, n, g_globalvars.time + 10);
			}
			else if (HMP_HasWord(toks, n, "over") || HMP_HasWord(toks, n, "dead")
					|| HMP_HasWord(toks, n, "out"))
			{
				HMP_ClearPowerupAlarm(receiver, sender);
			}

			break;
		}

		case HMP_CAT_NEED:
		{
			// "need ra": remember the mate's claim -> yield bias
			hm_bot_t *bslot = HMode_Slot(receiver);
			gedict_t *it = HMP_FindReportedItem(toks, n);

			if (bslot && it)
			{
				bslot->need_item = it;
				bslot->need_time = g_globalvars.time;

				if (HMode_DebugOn())
				{
					G_cprint("[hm-belief-upd] bot=%s kind=need item=%s from=%s "
								"t=%.1f\n",
								receiver->netname, HMode_ItemLogName(it),
								sender->netname, g_globalvars.time);
				}
			}

			break;
		}

		case HMP_CAT_HELP:
		{
			// "help sng": sender position update + assist bias toward them
			hm_bot_t *bslot = HMode_Slot(receiver);

			HMP_UpdateSenderLoc(receiver, sender, tm, toks, n);

			if (bslot && HMP_ReportOrg(receiver, sender, toks, n, loc))
			{
				VectorCopy(loc, bslot->help_org);
				bslot->help_time = g_globalvars.time;
				bslot->help_valid = true;

				if (HMode_DebugOn())
				{
					G_cprint("[hm-belief-upd] bot=%s kind=help org=%d,%d from=%s "
								"t=%.1f\n",
								receiver->netname, (int)loc[0], (int)loc[1],
								sender->netname, g_globalvars.time);
				}
			}

			break;
		}

		case HMP_CAT_REQUEST:
		{
			// "get quad" / "camp ra": an order pointing at a loc or item
			hm_bot_t *bslot = HMode_Slot(receiver);
			qbool has = HMP_ResolveLoc(toks, n, loc);

			if (!has)
			{
				gedict_t *it = HMP_FindReportedItem(toks, n);

				if (it)
				{
					VectorCopy(it->s.v.origin, loc);
					has = true;
				}
			}

			if (bslot && has)
			{
				VectorCopy(loc, bslot->req_org);
				bslot->req_time = g_globalvars.time;
				bslot->req_valid = true;

				if (HMode_DebugOn())
				{
					G_cprint("[hm-belief-upd] bot=%s kind=req org=%d,%d from=%s "
								"t=%.1f\n",
								receiver->netname, (int)loc[0], (int)loc[1],
								sender->netname, g_globalvars.time);
				}
			}

			break;
		}

		case HMP_CAT_SAFE:
		{
			// "safe ra": sender position update + stand down sightings there
			HMP_UpdateSenderLoc(receiver, sender, tm, toks, n);

			if (HMP_ResolveLoc(toks, n, loc))
			{
				HMP_ClearSightingsNear(receiver, sender, loc);
			}

			break;
		}

		case HMP_CAT_COMING:
		case HMP_CAT_WAITING:
		case HMP_CAT_LOCATION_PING:
		{
			// location-bearing sender reports: update last known position
			HMP_UpdateSenderLoc(receiver, sender, tm, toks, n);

			break;
		}

		default:
			// unparsed: nothing actionable. Every parsed category above now
			// has a consumer (S7); [hm-parse] + [hm-belief-upd] keep the
			// chain auditable.
			break;
	}
}

void HMode_ParseTeamsay(gedict_t *receiver, gedict_t *sender, const char *text)
{
	hmp_tok_t toks[HMP_MAX_TOKENS];
	int n, cat;

	if (!receiver || !receiver->isBot || !HMode_Active(receiver) || !HMode_CapParse())
	{
		return;
	}

	if (!sender || (sender == receiver) || (sender->ct != ctPlayer))
	{
		return;
	}

	n = HMP_Tokenize(text, sender, toks);
	cat = HMP_Classify(toks, n);

	if (cvar("k_hm_debug"))
	{
		static const char *cat_names[] =
		{
			"unparsed", "enemy_powerup", "enemy_seen", "lost", "dropped",
			"took", "help", "pack", "item_at", "need", "team_powerup",
			"powerup_status", "request", "coming", "waiting", "safe",
			"status", "enemy_at_nick", "location_ping", "slipped", "death"
		};

		G_cprint("[hm-parse] bot=%s from=%s cat=%s text=\"%s\"\n",
					receiver->netname, sender->netname,
					cat_names[(int)bound(0, cat, 20)], text);
	}

	HMP_Apply(receiver, sender, cat, toks, n);
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
