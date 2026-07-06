/*
 kbot_weapons.c -- owner weapon-discipline rules (2026-07-06)

 Owner verdict: frogbot picks guns well in the fight; what it lacks is the
 human discipline AROUND the fight. Three rules, one cvar each, byte-neutral
 defaults (R4):

   1 k_kbot_weap_quadlg: with quad running, LG owned and the enemy in shaft
     reach, always the shaft (quad-shaft converts instantly and rockets
     under quad self-damage the runner).
   2 k_kbot_weap_finish: an enemy KNOWN to be fresh-spawned that a rocket
     already clipped gets finished with hitscan at close range -- saves
     rockets and avoids self-damage as he closes. Freshness comes from the
     killfeed (all-player death stamps); the health read is truth-mode for
     now and swaps to the human_mode belief in one place.
   3 k_kbot_weap_sgdown: outside engagements the SG is the carried weapon;
     the big guns only come out when used. A death between fights then
     drops a shotgun pack, not the team's RL (Book pack economy 3.9:1
     enemy:own). Yields to WAIT camping and HARVEST holds (pre-placed aim
     wants the real gun out).

 Seam: top of DesiredWeapon (bot_botweap.c), after the teammate-damage
 guard. Returns an IT_* weapon or 0 for vanilla selection.
 */

#include "g_local.h"
#include "kbot.h"

#ifdef BOT_SUPPORT

static float khw_last_fire[MAX_EDICTS];
static float khw_last_death[MAX_EDICTS];
static int khw_prev_rule[MAX_EDICTS];

// seconds since a stamp, robust against map-restart clock rewinds
static float KHW_Since(float stamp)
{
	if ((stamp <= 0) || (stamp > g_globalvars.time))
	{
		return 99999;
	}

	return g_globalvars.time - stamp;
}

// Killed() -> BotPlayerKilledEvent seam: killfeed death stamps, all players.
void KBot_WeaponsDeathEvent(gedict_t *targ)
{
	int idx = NUM_FOR_EDICT(targ);

	if ((targ->ct == ctPlayer) && (idx > 0) && (idx < MAX_EDICTS))
	{
		khw_last_death[idx] = g_globalvars.time;
	}
}

static float khw_quadlg = 0, khw_quadlg_next = -1;
static float khw_finish = 0, khw_finish_next = -1;
static float khw_sgdown = 0, khw_sgdown_next = -1;

static float KHW_Cvar(const char *name, float *val, float *next)
{
	if (*next > g_globalvars.time + 3)
	{
		*next = -1; // clock rewound (map change/restart)
	}
	if (g_globalvars.time > *next)
	{
		*val = cvar(name);
		*next = g_globalvars.time + 1;
	}

	return *val;
}

int KBot_WeaponOverride(gedict_t *self)
{
	int items_ = (int)self->s.v.items;
	gedict_t *en = &g_edicts[self->s.v.enemy];
	qbool enemy_live, enemy_seen;
	int idx = NUM_FOR_EDICT(self);
	int eidx;
	int rule = 0, w = 0;

	if (!self->isBot || !self->fb.kbot || (idx <= 0) || (idx >= MAX_EDICTS))
	{
		return 0;
	}
	if (self->fb.firing)
	{
		khw_last_fire[idx] = g_globalvars.time;
	}
	enemy_live = self->s.v.enemy && (en->ct == ctPlayer) && ISLIVE(en)
			&& !SameTeam(en, self);
	enemy_seen = self->fb.look_object && (self->fb.look_object->ct == ctPlayer)
			&& !SameTeam(self->fb.look_object, self);
	eidx = NUM_FOR_EDICT(en);

	// rule 1: quad running + shaft in reach -> always the shaft
	if (KHW_Cvar("k_kbot_weap_quadlg", &khw_quadlg, &khw_quadlg_next)
			&& (self->super_damage_finished > g_globalvars.time)
			&& (items_ & IT_LIGHTNING) && (self->s.v.ammo_cells > 0)
			&& (self->s.v.waterlevel <= 1)
			&& enemy_live && (self->fb.enemy_dist <= 600))
	{
		rule = 1;
		w = IT_LIGHTNING;
	}
	// rule 2: clipped fresh spawn at close range -> cheap hitscan finish
	else if (KHW_Cvar("k_kbot_weap_finish", &khw_finish, &khw_finish_next)
			&& enemy_live && (self->fb.enemy_dist <= 450)
			&& (eidx > 0) && (eidx < MAX_EDICTS)
			&& (KHW_Since(khw_last_death[eidx]) < 8)
			&& (en->s.v.health < 70) && (en->s.v.armorvalue <= 0)
			&& (en->super_damage_finished <= g_globalvars.time))
	{
		if ((items_ & IT_SUPER_SHOTGUN) && self->s.v.ammo_shells)
		{
			w = IT_SUPER_SHOTGUN;
		}
		else if ((items_ & IT_SUPER_NAILGUN) && self->s.v.ammo_nails)
		{
			w = IT_SUPER_NAILGUN;
		}
		else if ((items_ & IT_NAILGUN) && self->s.v.ammo_nails)
		{
			w = IT_NAILGUN;
		}
		else if (self->s.v.ammo_shells)
		{
			w = IT_SHOTGUN;
		}
		if (w)
		{
			rule = 2;
		}
	}
	// rule 3: no engagement -> the SG is the carried weapon. Owner: this is
	// cl_weaponhide semantics -- SG out ALWAYS outside the actual firing
	// action, including while camping/holding an angle; the real gun comes
	// out in the firing moment at no extra cost (the bot switches and fires
	// in the same think). No WAIT/hold exemptions.
	else if (KHW_Cvar("k_kbot_weap_sgdown", &khw_sgdown, &khw_sgdown_next)
			&& !enemy_seen && !self->fb.firing
			&& (KHW_Since(khw_last_fire[idx]) > 0.7f)
			&& (KHW_Since(self->fb.last_hurt) > 2.0f)
			&& (self->s.v.ammo_shells > 0))
	{
		rule = 3;
		w = IT_SHOTGUN;
	}

	// KDLOG on the rising edge only (rule transitions)
	if (rule != khw_prev_rule[idx])
	{
		if (rule)
		{
			KDLog_Play(self, "weap",
						(rule == 1) ? "quadlg" : (rule == 2) ? "finish" : "sgdown", "");
		}
		khw_prev_rule[idx] = rule;
	}

	return w;
}

#endif // BOT_SUPPORT
