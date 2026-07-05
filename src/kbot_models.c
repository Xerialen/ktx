/*
 kbot_models.c -- the three tournament decision models (2026-07-06)

 Data source: komodobots2 docs/reports/2026-07-06-book-sr-win-analysis.md
 (153 Book/]sr[ dm3 wins, ~40k engagements, loser contrast group). Three
 independent, cvar-gated models over the same frogbot value function:

   TDM    (k_kbot_model 1) -- per-bot class x category desire policy +
          weapon-economy urgency + class-conditioned dive gate + quad-window
          division of labour. Seam: EvalGoal desire scaling.
   KAPTEN (k_kbot_model 2) -- team role allocation (ANKARE/FLYTARE/EKONOM)
          on a 2 s cadence; roles parameterize the same desire primitives;
          quad duty to the best-placed armed+; loot-your-frag pack call.
   UTBYTE (k_kbot_model 3) -- engagement economics TA/POKA/VAGRA/FINISH from
          the corpus exchange table; scales HUNT desire only
          (goal_enemy_desire); s.v.enemy is NEVER zeroed (b2 lesson: dodge
          and evade live on it).

 Tournament gating: k_kbot_model applies to all kbots; k_kbot_model_red /
 k_kbot_model_blue override per team (model-vs-model face-offs). 0 = off
 everywhere = byte-neutral baseline. Movement/jump layers (gapjump, plays)
 are strictly below all of this and untouched.
 */

#include "g_local.h"

#ifdef BOT_SUPPORT

#define BACKPACK_CLASSNAME "backpack"	// mirrors bot_botgoals.c

// ---------------------------------------------------------------------------
// shared state helpers
// ---------------------------------------------------------------------------

#define KBM_SPAWN   0
#define KBM_MID     1
#define KBM_ARMED   2
#define KBM_CONTROL 3

#define KBM_OFF     0
#define KBM_TDM     1
#define KBM_KAPTEN  2
#define KBM_UTBYTE  3

int KBot_StackClass(gedict_t *p)
{
	int held = (int)p->s.v.items;
	qbool armed = ((held & IT_ROCKET_LAUNCHER) && (p->s.v.ammo_rockets > 0))
			|| ((held & IT_LIGHTNING) && (p->s.v.ammo_cells > 0));

	if (armed)
	{
		return ((p->s.v.health + p->s.v.armorvalue) >= 150) ? KBM_CONTROL : KBM_ARMED;
	}
	if (held & (IT_GRENADE_LAUNCHER | IT_SUPER_NAILGUN | IT_SUPER_SHOTGUN | IT_LIGHTNING
			| IT_ROCKET_LAUNCHER))
	{
		return KBM_MID;
	}

	return KBM_SPAWN;
}

// Count RL/LG in the hands of self's team (owner parameter, 2026-07-06:
// winners hold >=2 team RL/LG 75% of match time; flow flips sign at 2).
int KBot_TeamRLLG(gedict_t *self)
{
	gedict_t *p;
	int n = 0;

	for (p = world; (p = find_plr(p));)
	{
		if (!SameTeam(p, self) && (p != self))
		{
			continue;
		}
		if ((int)p->s.v.items & (IT_ROCKET_LAUNCHER | IT_LIGHTNING))
		{
			n++;
		}
	}

	return n;
}

// Resolve which model runs for this bot: per-team cvar wins over global.
int KBot_ActiveModel(gedict_t *self)
{
	char *team;
	int m;

	if (!self->isBot || !self->fb.kbot)
	{
		return KBM_OFF;
	}
	team = getteam(self);
	if (team && streq(team, "red") && (m = (int)cvar("k_kbot_model_red")))
	{
		return m;
	}
	if (team && streq(team, "blue") && (m = (int)cvar("k_kbot_model_blue")))
	{
		return m;
	}

	return (int)cvar("k_kbot_model");
}

// goal category (mirrors the analyzer vocabulary 1:1)
#define KBC_ARMOR   0
#define KBC_MEGA    1
#define KBC_HEALTH  2
#define KBC_WBIG    3
#define KBC_WSMALL  4
#define KBC_AMMO    5
#define KBC_POWERUP 6
#define KBC_PACK    7
#define KBC_OTHER   8

static int KBot_GoalCategory(gedict_t *g)
{
	char *cn = g->classname;

	if (!cn || !cn[0])
	{
		return KBC_OTHER;
	}
	if (streq(cn, "item_armor1") || streq(cn, "item_armor2") || streq(cn, "item_armorInv"))
	{
		return KBC_ARMOR;
	}
	if (streq(cn, "item_health"))
	{
		return ((int)g->s.v.spawnflags & H_MEGA) ? KBC_MEGA : KBC_HEALTH;
	}
	if (streq(cn, "weapon_rocketlauncher") || streq(cn, "weapon_lightning"))
	{
		return KBC_WBIG;
	}
	if (!strncmp(cn, "weapon_", 7))
	{
		return KBC_WSMALL;
	}
	if (streq(cn, "item_shells") || streq(cn, "item_spikes") || streq(cn, "item_rockets")
			|| streq(cn, "item_cells"))
	{
		return KBC_AMMO;
	}
	if (!strncmp(cn, "item_artifact_", 14))
	{
		return KBC_POWERUP;
	}
	if (streq(cn, BACKPACK_CLASSNAME))
	{
		return KBC_PACK;
	}

	return KBC_OTHER;
}

// quad item entity + spatial cluster proxy (distance to quad origin; the dm3
// KLUSTER regions RA/RING/SNG/LIFTS/QUAD all sit within ~1100 units of quad)
static gedict_t *kbm_quad = NULL;
static float kbm_quad_checked = 0;

static gedict_t* KBot_QuadItem(void)
{
	if ((kbm_quad == NULL) && (g_globalvars.time > kbm_quad_checked))
	{
		kbm_quad = ez_find(world, "item_artifact_super_damage");
		kbm_quad_checked = g_globalvars.time + 5;
	}

	return kbm_quad;
}

static qbool KBot_QuadWindowOpen(void)
{
	gedict_t *q = KBot_QuadItem();

	// live on the floor, or respawning within 15 s
	return q && ((q->s.v.solid == SOLID_TRIGGER)
			|| ((q->fb.goal_respawn_time > g_globalvars.time)
					&& (q->fb.goal_respawn_time < g_globalvars.time + 15)));
}

static qbool KBot_NearQuad(vec3_t org)
{
	gedict_t *q = KBot_QuadItem();
	vec3_t d;

	if (!q)
	{
		return false;
	}
	VectorSubtract(org, q->s.v.origin, d);

	return vlen(d) < 1100;
}

// ---------------------------------------------------------------------------
// TDM v2: class x category policy (corpus-shaped, moderate magnitudes)
// ---------------------------------------------------------------------------
// Elite winner pickup shares per class (report 2.x / policy_dm3.json):
// spawn: ammo 48% wbig 13% -> arm first, take economy en route, leave armor.
// armed: armor 18% health 27% mega 8%, wbig 1% -> stack, no weapon detours.
// control: powerup 7%, ammo 42% upkeep, tempo FALLS (hold, don't hoover).
static float kbm_tdm_mul[4][9] = {
	//            armor mega health wbig wsmall ammo powerup pack other
	/* spawn   */ {0.80f, 0.80f, 1.00f, 1.80f, 1.30f, 1.10f, 0.50f, 1.20f, 1.0f},
	/* mid     */ {0.80f, 1.00f, 1.30f, 1.60f, 1.00f, 0.90f, 0.60f, 1.20f, 1.0f},
	/* armed   */ {1.50f, 1.30f, 1.20f, 0.50f, 0.70f, 0.90f, 1.30f, 1.00f, 1.0f},
	/* control */ {1.10f, 1.20f, 1.00f, 0.50f, 0.50f, 1.00f, 1.60f, 1.30f, 1.0f},
};

static float KBot_TdmScaleGoal(gedict_t *self, gedict_t *goal, float desire)
{
	int cls = KBot_StackClass(self);
	int cat = KBot_GoalCategory(goal);
	float m = kbm_tdm_mul[cls][cat];

	// weapon economy urgency: with the team under 2 RL/LG, the poor classes
	// drop everything vapen-shaped up another notch (the 0->4 team-RL/LG flow
	// staircase is the strongest predictor in the corpus).
	if ((cls <= KBM_MID) && (KBot_TeamRLLG(self) < 2)
			&& ((cat == KBC_WBIG) || (cat == KBC_PACK)))
	{
		m *= 1.8f;
	}
	// control tempo: hold, don't hoover (generic economy damped; armor/mega/
	// powerup/pack keep their slot = denial + loot still on).
	if ((cls == KBM_CONTROL)
			&& ((cat == KBC_HEALTH) || (cat == KBC_AMMO) || (cat == KBC_WSMALL)))
	{
		m *= 0.75f;
	}
	// quad-window division of labour (80% rule: quad is armed+ work).
	if ((cat == KBC_POWERUP) || KBot_NearQuad(goal->s.v.origin))
	{
		if (KBot_QuadWindowOpen())
		{
			m *= (cls >= KBM_ARMED) ? 1.5f : 0.6f;
		}
	}

	return desire * m;
}

// class-conditioned dive gate (elite armed+ water share is 0-3% across every
// player in the corpus; stack-based gating at 150 provably insufficient).
static qbool KBot_TdmDiveBlock(gedict_t *self, gedict_t *goal)
{
	return (KBot_StackClass(self) >= KBM_ARMED)
			&& (trap_pointcontents(PASSVEC3(goal->s.v.origin)) == CONTENT_WATER);
}

// ---------------------------------------------------------------------------
// KAPTEN: team role allocation
// ---------------------------------------------------------------------------
// Roles live in fb.kbot_role, refreshed by the strongest kbot each 2 s tick
// (any caller works -- allocation is deterministic from shared state).
#define KBR_NONE    0
#define KBR_ANKARE  1
#define KBR_FLYTARE 2
#define KBR_EKONOM  3

static float kbm_kapten_next = 0;

static float KBot_RoleScore(gedict_t *p)
{
	// suitability for the anchor end of the gradient: class first, stack next
	return KBot_StackClass(p) * 1000.0f + p->s.v.health + p->s.v.armorvalue;
}

static void KBot_KaptenAllocate(gedict_t *self)
{
	gedict_t *mates[8];
	int n = 0, i, j;
	gedict_t *p;

	if (g_globalvars.time < kbm_kapten_next)
	{
		return;
	}
	kbm_kapten_next = g_globalvars.time + 2.0f;

	for (p = world; (p = find_plr(p)) && n < 8;)
	{
		if (p->isBot && p->fb.kbot && (SameTeam(p, self) || (p == self))
				&& (KBot_ActiveModel(p) == KBM_KAPTEN))
		{
			mates[n++] = p;
		}
	}
	// sort by role score, descending (tiny n: insertion sort)
	for (i = 1; i < n; i++)
	{
		gedict_t *key = mates[i];

		for (j = i - 1; (j >= 0) && (KBot_RoleScore(mates[j]) < KBot_RoleScore(key)); j--)
		{
			mates[j + 1] = mates[j];
		}
		mates[j + 1] = key;
	}
	// gradient: 1 anchor, 1 floater, rest economy (the corpus rank profile:
	// rank1 43% control/1.32 dpm ... rank4 17%/2.65)
	for (i = 0; i < n; i++)
	{
		mates[i]->fb.kbot_role = (i == 0) ? KBR_ANKARE : (i == 1) ? KBR_FLYTARE : KBR_EKONOM;
	}
}

// role-keyed desire policy: same primitive as TDM but the row is the ROLE
// mandate, not the bot's own class.
static float kbm_role_mul[4][9] = {
	//            armor mega health wbig wsmall ammo powerup pack other
	/* none    */ {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
	/* ankare  */ {1.40f, 1.30f, 1.10f, 0.60f, 0.50f, 0.90f, 1.60f, 1.20f, 1.0f},
	/* flytare */ {1.00f, 1.00f, 1.00f, 1.00f, 0.80f, 1.00f, 1.40f, 1.30f, 1.0f},
	/* ekonom  */ {0.70f, 0.70f, 1.00f, 1.80f, 1.40f, 1.20f, 0.40f, 1.40f, 1.0f},
};

static float KBot_KaptenScaleGoal(gedict_t *self, gedict_t *goal, float desire)
{
	int role = self->fb.kbot_role;
	int cat = KBot_GoalCategory(goal);
	float m;

	KBot_KaptenAllocate(self);
	role = self->fb.kbot_role;
	if ((role < KBR_NONE) || (role > KBR_EKONOM))
	{
		role = KBR_NONE;
	}
	m = kbm_role_mul[role][cat];

	// weapon urgency is a team mandate: economy bots carry it
	if ((role == KBR_EKONOM) && (KBot_TeamRLLG(self) < 2)
			&& ((cat == KBC_WBIG) || (cat == KBC_PACK)))
	{
		m *= 1.8f;
	}
	// quad duty: in the window the best-placed armed+ (anchor/floater) leans
	// in; economy bots stay out of the cluster fight (80% rule).
	if ((cat == KBC_POWERUP) || KBot_NearQuad(goal->s.v.origin))
	{
		if (KBot_QuadWindowOpen())
		{
			m *= (role == KBR_EKONOM) ? 0.5f : 1.6f;
		}
	}
	// anchors never dive; economy bots may (class-conditioned water doctrine)
	if ((role != KBR_EKONOM) && (KBot_StackClass(self) >= KBM_ARMED)
			&& (trap_pointcontents(PASSVEC3(goal->s.v.origin)) == CONTENT_WATER))
	{
		return 0;
	}

	return desire * m;
}

// ---------------------------------------------------------------------------
// UTBYTE: engagement economics
// ---------------------------------------------------------------------------
// Corpus exchange table (winner side, ~40k engagements):
//   my\en     spawn        mid          armed        control
//   spawn     K28/D26      K19/D36      K16/D49      K2/D66
//   mid       K40/D17      K36/D26      K22/D40      K5/D60
//   armed     K54/D11      K46/D18      K36/D30      K11/D47
//   control   K69/D1       K66/D3       K55/D6       K26/D16 S57!
// Decisions: class edge >= +1 -> TA. Equal top classes -> POKA (the 57%
// separation norm). Down a class -> VAGRA (hunt suppressed; dodge intact).
// FINISH overrides on low-hp armed enemies (owner doctrine) and fresh frags
// feed the loot economy via the pack categories above.
#define KBX_TA     0
#define KBX_POKA   1
#define KBX_VAGRA  2
#define KBX_FINISH 3

static int KBot_ExchangeDecision(gedict_t *self, gedict_t *en)
{
	int mc, ec, rllg;
	qbool support = false;
	gedict_t *p;

	if (!en || (en->ct != ctPlayer))
	{
		return KBX_TA;
	}
	mc = KBot_StackClass(self);
	ec = KBot_StackClass(en);

	// finish-off: armed-class enemy on low hp converts with two shells and
	// swings the weapon economy by 2 (his -1, our +1 via the pack).
	if ((ec >= KBM_ARMED) && (en->s.v.health <= max(1, (int)cvar("k_kbot_finish_hp"))))
	{
		return KBX_FINISH;
	}

	// support: an armed+ teammate close by (the armed-vs-control K8->16 cell)
	for (p = world; (p = find_plr(p));)
	{
		if ((p != self) && !strnull(getteam(p)) && SameTeam(p, self)
				&& (KBot_StackClass(p) >= KBM_ARMED))
		{
			vec3_t d;

			VectorSubtract(p->s.v.origin, self->s.v.origin, d);
			if (vlen(d) < 768)
			{
				support = true;
				break;
			}
		}
	}

	rllg = KBot_TeamRLLG(self);

	if (mc > ec)
	{
		return KBX_TA;
	}
	if (mc == ec)
	{
		if (mc >= KBM_CONTROL)
		{
			return KBX_POKA;	// control never commits into control (57% sep)
		}
		// risk appetite from the team weapon economy: rich teams convert
		// even fights (flow +9.8..+14.4 at 3-4 RL/LG), poor teams must not.
		return (rllg >= 2) ? KBX_TA : ((rllg >= 1) ? KBX_POKA : KBX_VAGRA);
	}
	// down a class: armed may still trade UP into control with support
	if ((mc == KBM_ARMED) && (ec == KBM_CONTROL))
	{
		return support ? KBX_POKA : KBX_VAGRA;
	}

	return KBX_VAGRA;
}

// HUNT-desire multiplier -- the ONLY thing UTBYTE touches. Never s.v.enemy.
static float KBot_UtbyteScaleHunt(gedict_t *self, gedict_t *en, float desire)
{
	switch (KBot_ExchangeDecision(self, en))
	{
		case KBX_FINISH:
			return desire * 1.6f;
		case KBX_TA:
			return desire;
		case KBX_POKA:
			return desire * 0.4f;
		default:
			return 0;	// VAGRA: no hunting; vanilla repel/dodge untouched
	}
}

// ---------------------------------------------------------------------------
// public seams (called from bot_botgoals.c)
// ---------------------------------------------------------------------------

// Item/backpack/powerup goal desire, called from EvalGoal after the commit
// hysteresis. Returns the scaled desire; 0 kills the candidate (dive gates).
float KBot_ModelScaleGoal(gedict_t *self, gedict_t *goal, float desire)
{
	int model = KBot_ActiveModel(self);

	if ((model == KBM_OFF) || (desire <= 0) || (goal->ct == ctPlayer))
	{
		return desire;
	}
	switch (model)
	{
		case KBM_TDM:
			if (KBot_TdmDiveBlock(self, goal))
			{
				return 0;
			}
			return KBot_TdmScaleGoal(self, goal, desire);
		case KBM_KAPTEN:
			return KBot_KaptenScaleGoal(self, goal, desire);
		case KBM_UTBYTE:
			// exchange model keeps TDM's proven dive gate (class-conditioned)
			if (KBot_TdmDiveBlock(self, goal))
			{
				return 0;
			}
			return desire;
	}

	return desire;
}

// Enemy HUNT desire, called where WP3.3 clears goal_enemy_desire.
float KBot_ModelScaleHunt(gedict_t *self, gedict_t *en, float desire)
{
	int model = KBot_ActiveModel(self);

	if ((model == KBM_OFF) || (desire <= 0))
	{
		return desire;
	}
	if (model == KBM_UTBYTE)
	{
		return KBot_UtbyteScaleHunt(self, en, desire);
	}
	// TDM/KAPTEN keep the WP3.3 discipline via KBot_AvoidFights (caller).

	return desire;
}

#endif // BOT_SUPPORT
