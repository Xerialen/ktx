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

// ---------------------------------------------------------------------------
// B2: place-threat assessment (k_kbot_harvest_threat, conditional -- owner
// rule: no blind death-spot avoidance)
// ---------------------------------------------------------------------------
// penalty = base * heat(marker) * (weak base + enemy weight) * V * proximity.
// Death memory alone gives only the weak base -- places are dangerous because
// armed enemies are probably still there, not because someone died there.
// Enemies we outclass add nothing: press on (UTBYTE's TA runs the hunt).
// CS-bot pattern: +1.0 per team death at the area, lazy linear decay 1/120/s.

#define KHV_HEAT_DECAY   (1.0f / 120)
#define KHV_HEAT_MAX     3.0f
#define KHV_ENEMY_RADIUS 700.0f

static float khv_heat[MAX_EDICTS];
static float khv_heat_stamp[MAX_EDICTS];

static float KHV_HeatNow(int idx)
{
	float h;

	if (khv_heat_stamp[idx] > g_globalvars.time)
	{
		khv_heat[idx] = 0;	// map/match restart rewound the clock
	}
	h = khv_heat[idx] - (g_globalvars.time - khv_heat_stamp[idx]) * KHV_HEAT_DECAY;

	return (h > 0) ? h : 0;
}

// Killed() -> BotPlayerKilledEvent seam: feed the death memory on kbot deaths.
void KBot_HarvestDeathEvent(gedict_t *targ)
{
	gedict_t *m;
	int idx;

	if (!targ->isBot || !targ->fb.kbot || !(m = targ->fb.touch_marker))
	{
		return;
	}
	idx = NUM_FOR_EDICT(m);
	if ((idx <= 0) || (idx >= MAX_EDICTS))
	{
		return;
	}
	khv_heat[idx] = KHV_HeatNow(idx) + 1.0f;
	if (khv_heat[idx] > KHV_HEAT_MAX)
	{
		khv_heat[idx] = KHV_HEAT_MAX;
	}
	khv_heat_stamp[idx] = g_globalvars.time;
}

// Enemy-state reads behind one indirection: human_mode (spec
// 2026-07-06-human-mode-design.md) swaps truth for belief HERE, nowhere else.
static int KHV_EnemyClassEst(gedict_t *self, gedict_t *en)
{
	return KBot_StackClass(en);
}

// per-frame enemy snapshot so EvalPath never walks the entity list
typedef struct
{
	vec3_t pos;
	int cls;
	qbool quad;
} khv_enemy_t;

static khv_enemy_t khv_en[MAX_CLIENTS];
static int khv_en_count = 0;
static float khv_en_stamp = -1;
static char khv_en_team[16];

static void KHV_RefreshEnemies(gedict_t *self)
{
	gedict_t *p;
	char *team = getteam(self);

	if ((khv_en_stamp == g_globalvars.time) && team
			&& streq(team, khv_en_team))
	{
		return;
	}
	khv_en_stamp = g_globalvars.time;
	strlcpy(khv_en_team, team ? team : "", sizeof(khv_en_team));
	khv_en_count = 0;
	for (p = world; (p = find_plr(p)) && (khv_en_count < MAX_CLIENTS);)
	{
		if ((p == self) || SameTeam(p, self) || ISDEAD(p))
		{
			continue;
		}
		VectorCopy(p->s.v.origin, khv_en[khv_en_count].pos);
		khv_en[khv_en_count].cls = KHV_EnemyClassEst(self, p);
		khv_en[khv_en_count].quad = (p->super_damage_finished > g_globalvars.time);
		khv_en_count++;
	}
}

static float khv_threat = 0;
static float khv_threat_next = -1;

static float KBot_HarvestThreatBase(void)
{
	if (g_globalvars.time > khv_threat_next)
	{
		khv_threat = cvar("k_kbot_harvest_threat");
		khv_threat_next = g_globalvars.time + 1;
	}

	return khv_threat;
}

// B2 seam, called from EvalPath for every path candidate marker.
float KBot_HarvestThreatPenalty(gedict_t *self, gedict_t *m)
{
	float base = KBot_HarvestThreatBase();
	qbool powered;
	float heat, fw, threat, v;
	vec3_t mpos, d;
	int i, mc, idx;

	if (!base || !self->isBot || !self->fb.kbot)
	{
		return 0;
	}
	powered = (self->super_damage_finished > g_globalvars.time)
			|| (self->invincible_finished > g_globalvars.time);
	KHV_RefreshEnemies(self);
	VectorAdd(m->s.v.absmin, m->s.v.view_ofs, mpos);

	// hard rule: markers near a known enemy quad carrier get the max penalty
	// for everyone but our own powerup runner, while the window lives.
	if (!powered)
	{
		for (i = 0; i < khv_en_count; i++)
		{
			if (khv_en[i].quad)
			{
				VectorSubtract(mpos, khv_en[i].pos, d);
				if (vlen(d) < KHV_ENEMY_RADIUS)
				{
					return base * 2.4f;
				}
			}
		}
	}

	idx = NUM_FOR_EDICT(m);
	if ((idx <= 0) || (idx >= MAX_EDICTS))
	{
		return 0;
	}
	heat = KHV_HeatNow(idx);
	if (heat <= 0)
	{
		return 0;
	}

	// enemy weight: known enemies near the marker, class-conditioned; linear
	// proximity falloff keeps a hot spot across the map from bending routes.
	fw = 0;
	mc = KBot_StackClass(self);
	for (i = 0; i < khv_en_count; i++)
	{
		float dist, prox;

		VectorSubtract(mpos, khv_en[i].pos, d);
		dist = vlen(d);
		if (dist >= KHV_ENEMY_RADIUS)
		{
			continue;
		}
		prox = 1.0f - (dist / KHV_ENEMY_RADIUS);
		if (khv_en[i].cls > mc)
		{
			fw += 1.0f * prox;
		}
		else if (khv_en[i].cls == mc)
		{
			fw += 0.5f * prox;
		}
		// weaker class: 0 -- press on
	}

	threat = heat * (0.2f + fw);
	if (threat > 2.0f)
	{
		threat = 2.0f;
	}
	v = KBot_CarriedValue(self);
	if (powered)
	{
		v *= 0.3f;
	}

	return base * threat * v;
}

#endif // BOT_SUPPORT
