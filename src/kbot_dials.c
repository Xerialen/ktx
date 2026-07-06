/*
 kbot_dials.c -- the five tactical dials (owner directive 2026-07-06)

   D1 engage  k_kbot_dial_engage   attack/flee appetite from own stack PLUS
                                    the team weapon balance the owner asked
                                    for (our armed count vs theirs: alone
                                    with the RL you are not as brave as with
                                    stacked mates). Acts on the UTBYTE
                                    exchange decision (one-step upgrades or
                                    downgrades) and on the weak-stack
                                    retreat threshold.
   D2 hoard   k_kbot_dial_hoard    heavy-item timer loops (1.0) vs holding
                                    bottlenecks (0.0): shapes perceived
                                    travel time for mega/RA goals and the
                                    B5 posting cadence.
   D3 adhere  k_kbot_dial_adhere   compact crossfire play (1.0) vs lone
                                    wolf (0.0 = vanilla): goals near living
                                    teammates look closer, far ones look
                                    farther.
   D4 quad    k_kbot_dial_quad     quad-window commitment: scales the B4
                                    convergence lead, deflation and whether
                                    a third bot joins. Modulates B4 -- needs
                                    k_kbot_harvest_quad on.
   D5 share   k_kbot_dial_share    yield RA/mega to a needier teammate
                                    (1.0) vs selfish pickup (0.0 = vanilla):
                                    inflates the richer bot's perceived
                                    travel time when a poorer mate wants
                                    the same heavy item.

 Conventions (R2/R4 inherited): cost shaping only -- perceived travel time
 and OUR OWN decision layers (UTBYTE), never frogbot item desires. Default
 -1 = dial off = byte-neutral. Active range 0..1. Per-bot overrides:
 k_kbot_dial_<name>_s<slot> (slot = fb.kbot_slot join order 1..4) wins over
 the global cvar -- set today for all bots, ready for individuals.

 Logging contract (owner: enough logs to tweak and verify autonomously):
 every dial application that CHANGES an outcome emits a throttled KDLOG line
 lane=dial phase=<engage|hoard|adhere|quad|share> with inputs and effect in
 detail=. Aggregation: komodobots2 lab/dial_report.py.
 */

#include "g_local.h"
#include "kbot.h"

#ifdef BOT_SUPPORT

#define KDIAL_ENGAGE 0
#define KDIAL_HOARD  1
#define KDIAL_ADHERE 2
#define KDIAL_QUAD   3
#define KDIAL_SHARE  4
#define KDIAL_COUNT  5

static const char *kdial_names[KDIAL_COUNT] = {
	"engage", "hoard", "adhere", "quad", "share"
};

// per (dial, slot 0=global/1..4) cvar cache, refreshed once per second
static float kdial_val[KDIAL_COUNT][5];
static float kdial_next[KDIAL_COUNT][5];

static float KDial_Read(int dial, int slot)
{
	if (g_globalvars.time > kdial_next[dial][slot])
	{
		char name[48];

		if (slot)
		{
			snprintf(name, sizeof(name), "k_kbot_dial_%s_s%d", kdial_names[dial], slot);
		}
		else
		{
			snprintf(name, sizeof(name), "k_kbot_dial_%s", kdial_names[dial]);
		}
		kdial_val[dial][slot] = cvar(name);
		kdial_next[dial][slot] = g_globalvars.time + 1;
	}

	return kdial_val[dial][slot];
}

// Resolve the effective dial for this bot: slot override wins over global;
// negative = off. Result clamped to 0..1.
float KBot_Dial(gedict_t *self, int dial)
{
	int slot = self->fb.kbot_slot;
	float v = -1;

	if (!self->isBot || !self->fb.kbot || (dial < 0) || (dial >= KDIAL_COUNT))
	{
		return -1;
	}
	if ((slot >= 1) && (slot <= 4))
	{
		v = KDial_Read(dial, slot);
	}
	if (v < 0)
	{
		v = KDial_Read(dial, 0);
	}
	if (v < 0)
	{
		return -1;
	}

	return (v > 1) ? 1 : v;
}

// KDLOG throttle per bot per dial
static float kdial_log_next[MAX_EDICTS][KDIAL_COUNT];

static void KDial_Log(gedict_t *self, int dial, const char *detail)
{
	int idx = NUM_FOR_EDICT(self);

	if ((idx <= 0) || (idx >= MAX_EDICTS))
	{
		return;
	}
	if (kdial_log_next[idx][dial] > g_globalvars.time + 3)
	{
		kdial_log_next[idx][dial] = 0;	// clock rewound
	}
	if (g_globalvars.time < kdial_log_next[idx][dial])
	{
		return;
	}
	kdial_log_next[idx][dial] = g_globalvars.time + 2.0f;
	KDLog_Play(self, "dial", kdial_names[dial], detail);
}

// ---------------------------------------------------------------------------
// D1 engage: team weapon balance + aggression
// ---------------------------------------------------------------------------
// Armed counts on a 1 s cache (find_plr walk off the hot path). Enemy class
// reads go through KBot_EnemyClassEst -- the single human_mode swap point.
static float kdial_bal_next = -1;
static int kdial_our_armed = 0;
static int kdial_their_armed = 0;
static char kdial_bal_team[16];

static void KDial_RefreshBalance(gedict_t *self)
{
	gedict_t *p;
	char *team = getteam(self);

	if ((g_globalvars.time < kdial_bal_next) && team && streq(team, kdial_bal_team))
	{
		return;
	}
	kdial_bal_next = g_globalvars.time + 1.0f;
	strlcpy(kdial_bal_team, team ? team : "", sizeof(kdial_bal_team));
	kdial_our_armed = kdial_their_armed = 0;
	for (p = world; (p = find_plr(p));)
	{
		if (ISDEAD(p))
		{
			continue;
		}
		if ((p == self) || SameTeam(p, self))
		{
			kdial_our_armed += (KBot_StackClass(p) >= KBM_ARMED);
		}
		else
		{
			kdial_their_armed += (KBot_EnemyClassEst(self, p) >= KBM_ARMED);
		}
	}
}

// Effective aggression 0..1: the dial shifted by the team weapon balance.
// -1 when the dial is off.
float KBot_DialEngageAggr(gedict_t *self)
{
	float dial = KBot_Dial(self, KDIAL_ENGAGE);
	float aggr;

	if (dial < 0)
	{
		return -1;
	}
	KDial_RefreshBalance(self);
	aggr = dial + 0.15f * (kdial_our_armed - kdial_their_armed);

	return (aggr < 0) ? 0 : (aggr > 1) ? 1 : aggr;
}

// Weak-stack retreat scaling for KBot_AvoidFights: low aggression retreats
// earlier (1.5x threshold at 0), high fights to the bone (0.5x at 1).
float KBot_DialWeakScale(gedict_t *self)
{
	float aggr = KBot_DialEngageAggr(self);

	if (aggr < 0)
	{
		return 1.0f;
	}

	return 1.5f - aggr;
}

// One-step exchange upgrade/downgrade around the UTBYTE table. Decision
// codes mirror kbot_models.c (KBX_TA 0 / POKA 1 / VAGRA 2 / FINISH 3).
int KBot_DialEngageAdjust(gedict_t *self, gedict_t *en, int decision)
{
	float aggr = KBot_DialEngageAggr(self);
	int out = decision;
	char detail[96];

	if ((aggr < 0) || (decision == 3))
	{
		return decision;	// off, or FINISH is never modulated
	}
	if ((aggr >= 0.7f) && (decision > 0))
	{
		out = decision - 1;	// VAGRA->POKA->TA
	}
	else if ((aggr <= 0.3f) && (decision < 2))
	{
		out = decision + 1;	// TA->POKA->VAGRA
	}
	if (out != decision)
	{
		snprintf(detail, sizeof(detail), "a=%.2f;our=%d;their=%d;from=%d;to=%d",
					aggr, kdial_our_armed, kdial_their_armed, decision, out);
		KDial_Log(self, KDIAL_ENGAGE, detail);
	}

	return out;
}

// ---------------------------------------------------------------------------
// D2/D3/D5: one goal_time shim (EvalGoal seam, after the HARVEST shims)
// ---------------------------------------------------------------------------
float KBot_DialGoalShim(gedict_t *self, gedict_t *goal, float goal_time)
{
	float hoard = KBot_Dial(self, KDIAL_HOARD);
	float adhere = KBot_Dial(self, KDIAL_ADHERE);
	float share = KBot_Dial(self, KDIAL_SHARE);
	float t0 = goal_time;
	int cat;
	char detail[96];

	if ((hoard < 0) && (adhere < 0) && (share < 0))
	{
		return goal_time;
	}
	if (goal->ct == ctPlayer)
	{
		return goal_time;	// hunting is D1/UTBYTE territory
	}
	cat = KBot_GoalCategory(goal);

	// D2 hoard: heavy timer items (mega + armors) look closer at 1.0,
	// farther at 0.0 -- the low end frees time for holding ground (the B5
	// cadence scaling lives in KBot_DialHoldScale).
	if ((hoard >= 0) && ((cat == KBC_MEGA) || (cat == KBC_ARMOR)))
	{
		goal_time *= 1.2f - 0.4f * hoard;
	}

	// D3 adhere: compactness -- goals near a living teammate look closer,
	// far ones farther. 0 = vanilla lone wolf.
	if (adhere > 0)
	{
		gedict_t *p;
		float best = 999999;

		for (p = world; (p = find_plr(p));)
		{
			vec3_t d;

			if ((p == self) || ISDEAD(p) || !SameTeam(p, self))
			{
				continue;
			}
			VectorSubtract(goal->s.v.origin, p->s.v.origin, d);
			if (vlen(d) < best)
			{
				best = vlen(d);
			}
		}
		if (best < 999999)
		{
			goal_time *= (best < 700) ? (1.0f - 0.3f * adhere) : (1.0f + 0.3f * adhere);
		}
	}

	// D5 share: a poorer living teammate near (or targeting) the same heavy
	// item makes it look farther for the richer bot. 0 = vanilla selfish.
	if ((share > 0) && ((cat == KBC_MEGA) || (cat == KBC_ARMOR)))
	{
		gedict_t *p;
		float mine = self->s.v.health + self->s.v.armorvalue;

		for (p = world; (p = find_plr(p));)
		{
			vec3_t d;
			qbool wants;

			if ((p == self) || ISDEAD(p) || !SameTeam(p, self))
			{
				continue;
			}
			if ((mine - (p->s.v.health + p->s.v.armorvalue)) <= 50)
			{
				continue;	// not meaningfully richer
			}
			VectorSubtract(goal->s.v.origin, p->s.v.origin, d);
			wants = (vlen(d) < 500)
					|| (p->isBot && ((int)p->s.v.goalentity == NUM_FOR_EDICT(goal)));
			if (wants)
			{
				goal_time *= 1.0f + 0.8f * share;
				break;
			}
		}
	}

	if (goal_time != t0)
	{
		snprintf(detail, sizeof(detail), "cat=%d;mult=%.2f;h=%.1f;ad=%.1f;sh=%.1f",
					cat, goal_time / t0, hoard, adhere, share);
		KDial_Log(self, (goal_time > t0) ? KDIAL_SHARE : KDIAL_HOARD, detail);
	}

	return goal_time;
}

// ---------------------------------------------------------------------------
// D2 low end: posting cadence scale for the B5 hold (cooldown & duration)
// ---------------------------------------------------------------------------
// hoard 0 -> post much more (0.4x cooldown), hoard 1 -> rarely (1.6x).
float KBot_DialHoldScale(gedict_t *self)
{
	float hoard = KBot_Dial(self, KDIAL_HOARD);

	if (hoard < 0)
	{
		return 1.0f;
	}

	return 0.4f + 1.2f * hoard;
}

// ---------------------------------------------------------------------------
// D4 quad commitment: B4 parameters
// ---------------------------------------------------------------------------
float KBot_DialQuadLead(gedict_t *self)
{
	float dial = KBot_Dial(self, KDIAL_QUAD);

	return (dial < 0) ? 10.0f : (5.0f + 10.0f * dial);
}

float KBot_DialQuadDeflate(gedict_t *self)
{
	float dial = KBot_Dial(self, KDIAL_QUAD);

	if (dial < 0)
	{
		return 0.6f;
	}
	KDial_Log(self, KDIAL_QUAD, "converge");

	return 1.0f - 0.5f * dial;
}

// how many bots converge: 2 normally, 3 at high commitment
int KBot_DialQuadCount(gedict_t *self)
{
	float dial = KBot_Dial(self, KDIAL_QUAD);

	return ((dial >= 0.75f)) ? 3 : 2;
}

#endif // BOT_SUPPORT
