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
#include "kbot.h"

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

// ---------------------------------------------------------------------------
// B3: zone anchoring (k_kbot_harvest_anchor) -- the stacked bot lives in its
// zone
// ---------------------------------------------------------------------------
// Owner zone doctrine (spec section 2, dm3): RA zone (RA + Ring + SNG +
// lifts), Quad zone (quad + hill corridor), Pent zone, YA zone (YA + RL +
// window). Goals outside the bound zone look FURTHER away (goal_time
// inflation at the KBot_GJ_RouteShim seam) -- no desire is touched (R2);
// Book does not want armor less, they just never cross the map for it.
// Objective rotation exempt: powerups and big-weapon spawns (deny) are what
// you LEAVE the zone for. Zone membership = the mod's own location table
// (LocationName nearest-node, same vocabulary as the analyzer + mm2).

#define KHZ_NONE 0
#define KHZ_RA   1
#define KHZ_QUAD 2
#define KHZ_PENT 3
#define KHZ_YA   4

char* LocationName(float x, float y, float z);	// teamplay.c

static int KHV_ZoneOfName(const char *n)
{
	// precedence matters: "ya-quad"/"window-quad" belong to the quad zone,
	// so quad tests run before ya/window.
	if (strstr(n, "pent"))
	{
		return KHZ_PENT;
	}
	if (strstr(n, "quad") || !strncmp(n, "hill", 4))
	{
		return KHZ_QUAD;
	}
	if (!strncmp(n, "ra", 2) || !strncmp(n, "ring", 4) || !strncmp(n, "sng", 3)
			|| !strncmp(n, "lifts", 5))
	{
		return KHZ_RA;
	}
	if (!strncmp(n, "rl", 2) || !strncmp(n, "window", 6) || !strncmp(n, "ya", 2))
	{
		return KHZ_YA;
	}

	return KHZ_NONE;	// water/bridge/boundary: no zone
}

static int KHV_ZoneOfPoint(vec3_t p)
{
	return KHV_ZoneOfName(LocationName(p[0], p[1], p[2]));
}

// zone lookups are O(node_count); cache per entity for static world goals
static signed char khv_zone_cache[MAX_EDICTS];
static char khv_zone_map[64];

static int KHV_ZoneOfEnt(gedict_t *e)
{
	int idx = NUM_FOR_EDICT(e);

	if ((idx <= 0) || (idx >= MAX_EDICTS))
	{
		return KHZ_NONE;
	}
	if (!streq(khv_zone_map, mapname))
	{
		memset(khv_zone_cache, 0, sizeof(khv_zone_cache));
		strlcpy(khv_zone_map, mapname, sizeof(khv_zone_map));
	}
	if (!khv_zone_cache[idx])
	{
		khv_zone_cache[idx] = (signed char)(KHV_ZoneOfPoint(e->s.v.origin) + 1);
	}

	return khv_zone_cache[idx] - 1;
}

// zone representative points (world units) for the nearest-zone fallback
// when a bot is bound while in transit (no zone under its feet)
static const float khv_zone_center[5][3] = {
	{ 0, 0, 0 },			// KHZ_NONE (unused)
	{ 59, -528, 152 },		// RA plateau
	{ 920, 160, 56 },		// quad floor
	{ 1312, 712, -300 },	// pent
	{ 1500, -812, -24 },	// YA floor
};

static int KHV_NearestZone(vec3_t org)
{
	int z, best = KHZ_RA;
	float bd = 999999;

	for (z = KHZ_RA; z <= KHZ_YA; z++)
	{
		vec3_t d;

		VectorSubtract(org, khv_zone_center[z], d);
		if (vlen(d) < bd)
		{
			bd = vlen(d);
			best = z;
		}
	}

	return best;
}

// blackboard tick (KAPTEN allocator pattern, 2 s cadence): the most stacked
// armed+ kbot is the ANKARE, bound to the team control zone (doctrine start:
// RA). Other armed+ kbots get a weaker binding to the zone they are in (or
// nearest). Spawn/mid bots are unbound -- they must cross the map freely.
static float khv_anchor_next = 0;
static gedict_t *khv_anchor = NULL;
static signed char khv_bound[MAX_EDICTS];

static void KHV_AnchorTick(void)
{
	gedict_t *p, *best = NULL;
	float bs = -1;

	if (g_globalvars.time < khv_anchor_next)
	{
		return;
	}
	if (khv_anchor_next > g_globalvars.time + 3)
	{
		khv_anchor_next = 0;	// clock rewound (map restart)
	}
	khv_anchor_next = g_globalvars.time + 2.0f;
	memset(khv_bound, 0, sizeof(khv_bound));
	khv_anchor = NULL;
	for (p = world; (p = find_plr(p));)
	{
		float s;
		int idx = NUM_FOR_EDICT(p);
		int zone;

		if (!p->isBot || !p->fb.kbot || ISDEAD(p)
				|| (KBot_StackClass(p) < KBM_ARMED)
				|| (idx <= 0) || (idx >= MAX_EDICTS))
		{
			continue;
		}
		zone = KHV_ZoneOfPoint(p->s.v.origin);
		khv_bound[idx] = (signed char)(zone ? zone : KHV_NearestZone(p->s.v.origin));
		s = KBot_StackClass(p) * 1000.0f + p->s.v.health + p->s.v.armorvalue;
		if (s > bs)
		{
			bs = s;
			best = p;
		}
	}
	khv_anchor = best;
	if (best)
	{
		khv_bound[NUM_FOR_EDICT(best)] = KHZ_RA;	// control zone doctrine
	}
}

static float khv_anchor_f = 0;
static float khv_anchor_f_next = -1;

static float KBot_HarvestAnchorFactor(void)
{
	if (g_globalvars.time > khv_anchor_f_next)
	{
		khv_anchor_f = cvar("k_kbot_harvest_anchor");
		khv_anchor_f_next = g_globalvars.time + 1;
	}

	return khv_anchor_f;
}

// B3 seam, called from EvalGoal right after KBot_GJ_RouteShim: goals outside
// the bound zone get their perceived travel time inflated.
float KBot_HarvestAnchorShim(gedict_t *self, gedict_t *goal, float goal_time)
{
	float f = KBot_HarvestAnchorFactor();
	int idx, zs, zg, cat;

	if ((f <= 1) || !self->isBot || !self->fb.kbot || (goal->ct == ctPlayer))
	{
		return goal_time;
	}
	cat = KBot_GoalCategory(goal);
	if ((cat == KBC_POWERUP) || (cat == KBC_WBIG))
	{
		return goal_time;	// strategic objectives: rotation exempt
	}
	KHV_AnchorTick();
	idx = NUM_FOR_EDICT(self);
	if ((idx <= 0) || (idx >= MAX_EDICTS) || !khv_bound[idx])
	{
		return goal_time;
	}
	zs = khv_bound[idx];
	zg = KHV_ZoneOfEnt(goal);
	if (zg == zs)
	{
		return goal_time;
	}

	return goal_time * ((self == khv_anchor) ? f : (1.0f + (f - 1.0f) * 0.5f));
}

// ---------------------------------------------------------------------------
// B4: quad convergence + guard stance (k_kbot_harvest_quad, k_kbot_harvest_guard)
// ---------------------------------------------------------------------------
// Guard study (spec section 7, 285 Book quad windows): Book sends 2-3, never
// the whole team (>=2 present in 49% of windows); the taker arrives ~200 qu
// out, the guards stand 300-580 qu out AT THE OPENINGS (RL entrance east is
// the enemies' dominant approach), crosshair held on the opening line (56%
// sideways). Convergence: T-10 s the two nearest armed+ kbots see the quad
// goal as closer (objective rotation through the B3 exemption); guard: the
// non-takers already in the zone at guard distance hold and watch an entry.

static gedict_t *khv_quad = NULL;
static float khv_quad_checked = 0;

static gedict_t* KHV_QuadItem(void)
{
	if ((khv_quad == NULL) && (g_globalvars.time > khv_quad_checked))
	{
		khv_quad = ez_find(world, "item_artifact_super_damage");
		khv_quad_checked = g_globalvars.time + 5;
	}

	return khv_quad;
}

// The WINDOW is the timed team event around a quad spawn: respawning within
// lead seconds, or freshly up (<15 s). "Lying untaken on the floor" is NOT a
// window -- with kbot quad take rates a solid==TRIGGER test held the window
// open for minutes and kept B5 posting permanently gated (hvdebug diagnosis)
// while making B4 convergence near-constant instead of T-10-timed.
static float khv_quad_live_since = -1;
static qbool khv_quad_was_live = false;

static qbool KHV_QuadWindowSoon(float lead)
{
	gedict_t *q = KHV_QuadItem();
	qbool live;

	if (!q)
	{
		return false;
	}
	live = (q->s.v.solid == SOLID_TRIGGER);
	if (khv_quad_live_since > g_globalvars.time)
	{
		khv_quad_live_since = -1;	// clock rewound (map restart)
	}
	if (live && !khv_quad_was_live)
	{
		khv_quad_live_since = g_globalvars.time;
	}
	khv_quad_was_live = live;

	if ((q->fb.goal_respawn_time > g_globalvars.time)
			&& (q->fb.goal_respawn_time < g_globalvars.time + lead))
	{
		return true;
	}

	return live && (khv_quad_live_since >= 0)
			&& (g_globalvars.time - khv_quad_live_since < 15);
}

// 2 s cadence: rank armed+ kbots by distance to the quad; [0] is the TAKER,
// [1] the second converger. Everyone else stays on the zone loop.
static float khv_quad_next = 0;
static gedict_t *khv_quad_near[2] = { NULL, NULL };

static void KHV_QuadTick(void)
{
	gedict_t *p, *q = KHV_QuadItem();
	float d0 = 999999, d1 = 999999;

	if (g_globalvars.time < khv_quad_next)
	{
		return;
	}
	if (khv_quad_next > g_globalvars.time + 3)
	{
		khv_quad_next = 0;
	}
	khv_quad_next = g_globalvars.time + 2.0f;
	khv_quad_near[0] = khv_quad_near[1] = NULL;
	if (!q)
	{
		return;
	}
	for (p = world; (p = find_plr(p));)
	{
		vec3_t d;
		float dist;

		if (!p->isBot || !p->fb.kbot || ISDEAD(p) || (KBot_StackClass(p) < KBM_ARMED))
		{
			continue;
		}
		VectorSubtract(p->s.v.origin, q->s.v.origin, d);
		dist = vlen(d);
		if (dist < d0)
		{
			d1 = d0;
			khv_quad_near[1] = khv_quad_near[0];
			d0 = dist;
			khv_quad_near[0] = p;
		}
		else if (dist < d1)
		{
			d1 = dist;
			khv_quad_near[1] = p;
		}
	}
}

static float khv_quad_f = 0;
static float khv_quad_f_next = -1;
static float khv_guard_f = 0;
static float khv_guard_f_next = -1;
static float khv_hold_f = 0;
static float khv_hold_f_next = -1;

static float KHV_CvarCached(const char *name, float *val, float *next)
{
	if (g_globalvars.time > *next)
	{
		*val = cvar(name);
		*next = g_globalvars.time + 1;
	}

	return *val;
}

// B4 convergence seam (same call site as the B3 shim): the two nearest
// armed+ kbots see the quad goal deflated while the window opens.
float KBot_HarvestQuadShim(gedict_t *self, gedict_t *goal, float goal_time)
{
	if (!KHV_CvarCached("k_kbot_harvest_quad", &khv_quad_f, &khv_quad_f_next)
			|| !self->isBot || !self->fb.kbot)
	{
		return goal_time;
	}
	if ((goal != KHV_QuadItem()) || !KHV_QuadWindowSoon(10))
	{
		return goal_time;
	}
	KHV_QuadTick();
	if ((self == khv_quad_near[0]) || (self == khv_quad_near[1]))
	{
		return goal_time * 0.6f;
	}

	return goal_time;
}

// ---------------------------------------------------------------------------
// B4 guard + B5 posting: the hold primitive (movement + locked look)
// ---------------------------------------------------------------------------
// Posting study (spec section 7b, 105 Book episodes): a stacked RL player
// stands still 3-6 s with the crosshair LOCKED on an entry line (54% <40
// degrees sweep), enemy within 800 qu in 56%, dies in 1%, converts 30% to a
// kill -- the victim walks into a pre-placed aim. Never for spawn/mid bots,
// never outside the own zone, never during the own quad window (converge
// instead). Combat stays vanilla: any visible enemy or damage aborts the
// hold instantly (R1).

// zone entry points (world units, from the dm3 loc nodes): where enemies
// come FROM when they enter the zone -- the guard's look line.
static const float khv_zone_entry[5][3] = {
	{ 0, 0, 0 },			// KHZ_NONE
	{ 496, -461, 56 },		// RA: RA.entry (the low approach)
	{ 1300, 81, -24 },		// QUAD: east / RL entrance (dominant approach)
	{ 710, 672, -264 },		// PENT: lifts side
	{ 1168, -617, -24 },	// YA: YA.entry
};

typedef struct
{
	float until;	// holding while time < until
	float cool;		// no re-trigger while time < cool
	float yaw;
} khv_hold_state_t;

static khv_hold_state_t khv_hold[MAX_EDICTS];

// accessor for kbot_weapons.c rule 3: a hold wants the real gun out
qbool KBot_HarvestHolding(gedict_t *p)
{
	int idx = NUM_FOR_EDICT(p);

	return (idx > 0) && (idx < MAX_EDICTS) && (g_globalvars.time < khv_hold[idx].until);
}

static float KHV_YawTo(gedict_t *self, const float *point)
{
	vec3_t d;

	d[0] = point[0] - self->s.v.origin[0];
	d[1] = point[1] - self->s.v.origin[1];
	d[2] = 0;

	return vectoyaw(d);
}

// Movement-frame seam (bot_movement.c, before the gapjump final authority so
// a triggered crossing still overrides). Returns true when the hold owns
// this frame's movement: zeroed direction, locked yaw.
qbool KBot_HarvestHoldFrame(gedict_t *self, qbool *jumping, vec3_t direction)
{
	float guard_on = KHV_CvarCached("k_kbot_harvest_guard", &khv_guard_f, &khv_guard_f_next);
	float hold_on = KHV_CvarCached("k_kbot_harvest_hold", &khv_hold_f, &khv_hold_f_next);
	khv_hold_state_t *h;
	int idx, zone;
	qbool enemy_seen;

	if ((!guard_on && !hold_on) || !self->isBot || !self->fb.kbot
			|| (match_in_progress != 2))
	{
		return false;
	}
	idx = NUM_FOR_EDICT(self);
	if ((idx <= 0) || (idx >= MAX_EDICTS))
	{
		return false;
	}
	h = &khv_hold[idx];
	if (h->cool > g_globalvars.time + 30)
	{
		h->until = h->cool = 0;	// clock rewound (map restart)
	}

	enemy_seen = self->fb.look_object && (self->fb.look_object->ct == ctPlayer)
			&& !SameTeam(self->fb.look_object, self);

	// active hold: abort on contact/damage/death/water, else own the frame
	if (g_globalvars.time < h->until)
	{
		if (ISDEAD(self) || enemy_seen || (self->s.v.waterlevel > 1)
				|| (self->fb.last_hurt > g_globalvars.time - 0.3f))
		{
			h->until = 0;

			return false;
		}
		VectorClear(direction);
		*jumping = false;
		self->fb.desired_angle[0] = 0;
		self->fb.desired_angle[1] = h->yaw;

		return true;
	}

	// trigger evaluation
	if ((g_globalvars.time < h->cool) || ISDEAD(self) || enemy_seen
			|| !((int)self->s.v.flags & FL_ONGROUND) || (self->s.v.waterlevel > 1)
			|| (self->fb.last_hurt > g_globalvars.time - 1.0f))
	{
		return false;
	}
	zone = KHV_ZoneOfPoint(self->s.v.origin);

	// B4 guard: window live/opening, armed+, in the quad zone, not the taker,
	// at guard distance -> hold a short watch on the east entry (rolling
	// 2.5 s holds so the take still releases everyone).
	if (guard_on && KHV_QuadWindowSoon(10))
	{
		gedict_t *q = KHV_QuadItem();

		KHV_QuadTick();
		if (q && (zone == KHZ_QUAD) && (KBot_StackClass(self) >= KBM_ARMED)
				&& (self != khv_quad_near[0]))
		{
			vec3_t d;
			float dist;

			VectorSubtract(self->s.v.origin, q->s.v.origin, d);
			dist = vlen(d);
			if ((dist > 280) && (dist < 620))
			{
				h->until = g_globalvars.time + 2.5f;
				h->cool = h->until + 0.5f;
				h->yaw = KHV_YawTo(self, khv_zone_entry[KHZ_QUAD]);
				KDLog_Play(self, "harvest", "guard", "quadwindow");
				VectorClear(direction);
				*jumping = false;
				self->fb.desired_angle[0] = 0;
				self->fb.desired_angle[1] = h->yaw;

				return true;
			}
		}

		return false;	// own window, not guarding: converge, never post
	}

	// diagnosis emitter (k_kbot_harvest_debug): why does posting not trigger?
	// 2 s throttle per bot, control-class frames only. Temporary tooling.
	if (hold_on && (KBot_StackClass(self) == KBM_CONTROL))
	{
		static float khv_dbg_next[MAX_EDICTS];
		static float khv_dbg_f = 0;
		static float khv_dbg_f_next = -1;

		if (KHV_CvarCached("k_kbot_harvest_debug", &khv_dbg_f, &khv_dbg_f_next)
				&& (g_globalvars.time > khv_dbg_next[idx]))
		{
			gedict_t *goal = &g_edicts[(int)self->s.v.goalentity];
			char detail[128];

			KHV_RefreshEnemies(self);
			khv_dbg_next[idx] = g_globalvars.time + 2.0f;
			snprintf(detail, sizeof(detail),
					 "zone=%d;qwin=%d;seen=%d;ong=%d;hurt=%d;goalrt=%.1f;encnt=%d",
					 zone, (int)KHV_QuadWindowSoon(10), (int)enemy_seen,
					 (int)((int)self->s.v.flags & FL_ONGROUND ? 1 : 0),
					 (int)(self->fb.last_hurt > g_globalvars.time - 1.0f),
					 goal ? (goal->fb.goal_respawn_time - g_globalvars.time) : -999,
					 khv_en_count);
			KDLog_Play(self, "hvdebug", "eval", detail);
		}
	}

	// B5 posting: control class with RL+rockets, standing inside a tactical
	// zone (never on open transit ground -- Book posts at the armor spots and
	// chokes of whatever zone he is stacked in, spec 7b), never during a quad
	// window (converge instead). NOT gated on the B3 binding: the anchor is
	// force-bound to RA but lives at quad/YA, so requiring bound==current
	// made posting unreachable (second inert arm, found via KDLOG + the
	// control-position grid).
	if (hold_on && !KHV_QuadWindowSoon(10) && (KBot_StackClass(self) == KBM_CONTROL)
			&& ((int)self->s.v.items & IT_ROCKET_LAUNCHER)
			&& (self->s.v.ammo_rockets > 0)
			&& zone)
	{
		gedict_t *goal = &g_edicts[(int)self->s.v.goalentity];
		qbool timing_watch = false;
		qbool ambush = false;
		float yaw = 0;
		int i;

		// (a) timing watch: standing at an item whose respawn is 2-15 s out
		// (the vanilla wait already stops the bot -- we lock the look on the
		// ENTRY line instead of the item)
		if (goal && (goal->ct != ctPlayer) && goal->fb.goal_respawn_time
				&& (goal->fb.goal_respawn_time > g_globalvars.time + 2)
				&& (goal->fb.goal_respawn_time < g_globalvars.time + 15))
		{
			vec3_t d;

			VectorSubtract(self->s.v.origin, goal->s.v.origin, d);
			if (vlen(d) < 150)
			{
				timing_watch = true;
				yaw = KHV_YawTo(self, khv_zone_entry[zone]);
			}
		}
		// (b) ambush: a known enemy within 800 qu -- pre-place the aim on him
		if (!timing_watch)
		{
			KHV_RefreshEnemies(self);
			for (i = 0; i < khv_en_count; i++)
			{
				vec3_t d;

				VectorSubtract(khv_en[i].pos, self->s.v.origin, d);
				d[2] = 0;
				if (vlen(d) < 800)
				{
					ambush = true;
					yaw = vectoyaw(d);
					break;
				}
			}
		}
		if (timing_watch || ambush)
		{
			h->until = g_globalvars.time + 3.0f + g_random() * 3.0f;	// 3-6 s
			h->cool = h->until + 8.0f;
			h->yaw = yaw;
			KDLog_Play(self, "harvest", timing_watch ? "hold-timing" : "hold-ambush", "");
			VectorClear(direction);
			*jumping = false;
			self->fb.desired_angle[0] = 0;
			self->fb.desired_angle[1] = h->yaw;

			return true;
		}
	}

	return false;
}

#endif // BOT_SUPPORT
