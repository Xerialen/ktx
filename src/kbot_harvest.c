/*
 kbot_harvest.c -- HARVEST: the possession layer on top of UTBYTE (2026-07-06)

 Data source: komodobots2 docs/specs/2026-07-06-harvest-model-design.md (v2,
 owner-reviewed) + the gap analysis vs Book's TB4 grand final (hub 218932).
 Book keeps what they take; our bots hand it back: RL uptime 15% vs 50%,
 CONTROL team-state 1% vs 36%, kills per RL-life 1.17 vs 5.37, and the deaths
 fall in transit (WATER/BOUNDARY residency). HARVEST is not a new brain -- it
 shapes what the bot does BETWEEN pickups: which route it takes, where it
 lives when stacked, how the team handles quad windows.

 Hard frames inherited unchanged:
   R1  s.v.enemy is never zeroed (dodge/evade live on it -- the b2 lesson)
   R2  item-category desires are never touched (triple proof: TDM -0.75,
       KAPTEN roles -13.2, absorption lever -4). HARVEST shapes COST
       (path score, perceived travel time) -- never desire.
   R3  gapjump capability untouched
   R4  every mechanism behind its own cvar, byte-neutral default off

 B1 route discipline (k_kbot_harvest_route, base penalty in score units,
 spec start 2.5): an armed bot pays a path-score penalty on water markers,
 scaled by carried value V. Changes WHICH ROUTE the bot takes to the same
 goal -- never which goal (the class dive gate already handles goals IN
 water; B1 covers transit). Corpus: our armed bots have WATER in their top-3
 residency and die in WATER/BOUNDARY; Milton logs 0 s of water as armed+.
 */

#include "g_local.h"

#ifdef BOT_SUPPORT

// ---------------------------------------------------------------------------
// carried-value scalar V (0..1) -- every HARVEST mechanism reads it
// ---------------------------------------------------------------------------
// Normalized stack x firepower (frogbot already counts these raw materials in
// the goal_client desire; Q3's AI cascades the same way: health/armor set the
// magnitude, the big gun gates it). Spawns score ~0.06 -- they must cross the
// map freely or the item economy starves (spec section 3).
float KBot_CarriedValue(gedict_t *p)
{
	int held = (int)p->s.v.items;
	float stack = (p->s.v.health + p->s.v.armorvalue) / 250.0f;
	float fire;

	if (((held & IT_ROCKET_LAUNCHER) && (p->s.v.ammo_rockets > 0))
			|| ((held & IT_LIGHTNING) && (p->s.v.ammo_cells > 0)))
	{
		fire = 1.0f;
	}
	else if (held & (IT_GRENADE_LAUNCHER | IT_SUPER_NAILGUN | IT_SUPER_SHOTGUN))
	{
		fire = 0.5f;
	}
	else
	{
		fire = 0.15f;
	}
	if (stack > 1)
	{
		stack = 1;
	}

	return stack * fire;
}

// ---------------------------------------------------------------------------
// B1: water-route penalty
// ---------------------------------------------------------------------------
// EvalPath is the hottest bot code path (per path candidate per think); the
// trap cvar read stays out of it via a once-per-second cache (the
// KBot_QuadItem pattern in kbot_models.c).
static float khv_route = 0;
static float khv_route_next = -1;

static float KBot_HarvestRouteBase(void)
{
	if (g_globalvars.time > khv_route_next)
	{
		khv_route = cvar("k_kbot_harvest_route");
		khv_route_next = g_globalvars.time + 1;
	}

	return khv_route;
}

// B1 seam, called from EvalPath for markers with fb.T & T_WATER. Returns the
// score penalty; 0 when the lever is off, for baseline frogbots, or when the
// carrier holds nothing worth protecting. A powerup runner keeps most of its
// route freedom (spec section 3 hard override: the quad harvests, it does
// not hide).
float KBot_HarvestWaterPenalty(gedict_t *p)
{
	float base = KBot_HarvestRouteBase();

	if (!base || !p->isBot || !p->fb.kbot)
	{
		return 0;
	}
	if ((p->super_damage_finished > g_globalvars.time)
			|| (p->invincible_finished > g_globalvars.time))
	{
		return base * KBot_CarriedValue(p) * 0.3f;
	}

	return base * KBot_CarriedValue(p);
}

#endif // BOT_SUPPORT
