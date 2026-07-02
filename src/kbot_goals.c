/*
 kbot_goals.c -- KomodoBrain WP3.1: predictive item-economy goal layer

 Frogbot's goal economy (bot_botgoals.c) is generic and reactive. This layer
 is predictive: it knows when the majors (RA/YA/GA, mega, quad, pent, RL, LG)
 come back up (fb.goal_respawn_time, maintained by KTX on every pickup),
 compares time-to-available against own travel time, and commits early so the
 bot ARRIVES at the spawn moment. Selection only -- the stock frogbot pathing
 machinery (ProcessNewLinkedMarker / PathScoringLogic) runs unchanged toward
 whatever this layer picks, and PathScoringLogic's own goal_late_time pacing
 does the terminal wait using the exact respawn time we hand it (vanilla
 blurs it with skill.prediction_error; kbots do not).

 KBot_SelectGoal() is called from UpdateGoal() (kbot-gated, after the lab
 fixed_goal pin so measurement pins still win). Returning true means the same
 state UpdateGoal's tail would set has been set; returning false means the
 vanilla economy runs untouched (used when nothing scores well -- vanilla is
 better at opportunistic pickups: backpacks, ammo top-ups, enemy hunting).
 */
#ifdef BOT_SUPPORT

#include "g_local.h"
#include "kbot.h"

// ---- tunables (one place; deterministic -- no g_random in scoring) ----
#define KBOT_GOAL_MIN_SCORE      2.0f   // best score below this: delegate to vanilla
#define KBOT_GOAL_SWITCH_FACTOR  1.25f  // challenger must beat the committed goal by 25%
// Max seconds the bot may arrive BEFORE the item spawns (its standing-around
// budget). Was 10 s in 0.4.0: bots parked on contested spawn points -- free
// frags for the enemy and an idle-honesty-gate risk (one 0.4.0 run was
// rejected at >10% idle). 4 s keeps the spawn-race play alive (the timing
// window below is 3 s) while capping stationary time well under the idle bar.
#define KBOT_GOAL_MAX_EARLY      4.0f
#define KBOT_GOAL_TIMING_WINDOW  3.0f   // |wait - ETA| window for the timed-arrival bonus
#define KBOT_GOAL_TIMING_BONUS   1.35f  // reward plays that arrive at the spawn moment
#define KBOT_GOAL_TEAM_DECAY     0.30f  // a closer same-team player already owns this goal
#define KBOT_GOAL_TIME_SOFT      6.0f   // score = value / (effective_time + SOFT)
#define KBOT_UNREACHABLE         100000.0f

// Base strategic values by classname only (no map hardcoding). Order is
// documentation, not priority -- scoring decides.
typedef struct kbot_item_value_s
{
	const char *classname;
	float base;
} kbot_item_value_t;

static kbot_item_value_t kbot_item_values[] =
{
	{ "item_artifact_invulnerability", 120.0f },	// pent
	{ "item_armorInv",                 100.0f },	// RA
	{ "item_artifact_super_damage",     90.0f },	// quad
	{ "item_armor2",                    70.0f },	// YA
	{ "item_health",                    65.0f },	// mega only (H_MEGA spawnflag)
	{ "weapon_rocketlauncher",          60.0f },
	{ "weapon_lightning",               55.0f },
	{ "item_armor1",                    30.0f },	// GA
};

// Per-slot commitment (stickiness): the goal we are en route to and the score
// it carried when last evaluated. Cleared on low score / invalid goal.
static gedict_t *kbot_goal_current[MAX_CLIENTS];

// Route travel time from a player's touch marker to an item's marker, using
// the same primitives EvalGoal uses (ZoneMarker + SubZoneArrivalTime -- the
// latter NULL-guards route-table holes since the M2 crash fix). >= 1000000
// means unreachable. NULL markers mean unreachable.
//
// Sentinel fallback (0.4.1): reachability must never depend on the zone-table
// primitive alone. Offline reproduction of the zone relaxation over the real
// dm3 FBMARKER dump measured 1.6% of (from-marker x major-item) pairs at the
// dropper/1e6 sentinel (5 dead markers account for nearly all of it) -- and
// other maps' graphs can be sparser. When both markers exist but the table
// says unreachable, fall back to straight-line 3D distance / 320 ups: an
// imperfect, roughly distance-ranked ETA that keeps the item selectable.
// Frogbot's own pathing reaches every goal without this primitive, so a
// selected goal is still walkable. NULL markers stay unreachable (pathing
// genuinely cannot route there); the NULL guards stay untouched.
static float KBot_TravelTime(gedict_t *player, gedict_t *item)
{
	float t;

	if (!player || !item || !player->fb.touch_marker || !item->fb.touch_marker)
	{
		return KBOT_UNREACHABLE;
	}

	ZoneMarker(player->fb.touch_marker, item->fb.touch_marker, true,
			   player->fb.canRocketJump);
	t = SubZoneArrivalTime(zone_time, middle_marker, item->fb.touch_marker,
						   player->fb.canRocketJump);
	if (t >= 1000000.0f)
	{
		vec3_t item_pos, diff;

		VectorAdd(item->fb.touch_marker->s.v.absmin,
				  item->fb.touch_marker->s.v.view_ofs, item_pos);
		VectorSubtract(item_pos, player->s.v.origin, diff);
		t = VectorLength(diff) / 320.0f;
	}

	return t;
}

// Stack-aware need multiplier: how much THIS bot wants THIS item right now.
static float KBot_NeedFactor(gedict_t *self, gedict_t *item)
{
	const char *cn = item->classname;
	int held = (int)self->s.v.items;

	if (streq(cn, "item_armorInv") || streq(cn, "item_armor2")
		|| streq(cn, "item_armor1"))
	{
		// 0 armor -> 1.6x ... 200 armor -> 0.2x (denial value keeps a floor)
		float f = 1.6f - (float)self->s.v.armorvalue / 125.0f;

		return (f < 0.2f) ? 0.2f : f;
	}
	if (streq(cn, "item_health"))	// mega (filtered before call)
	{
		// 50hp -> 1.5x, 100hp -> 1.0x, floor 0.2x when stacked
		float f = 2.0f - (float)self->s.v.health / 100.0f;

		if (f > 2.0f)
		{
			f = 2.0f;
		}

		return (f < 0.2f) ? 0.2f : f;
	}
	if (streq(cn, "weapon_rocketlauncher"))
	{
		return (held & IT_ROCKET_LAUNCHER) ? 0.25f : 2.5f;
	}
	if (streq(cn, "weapon_lightning"))
	{
		return (held & IT_LIGHTNING) ? 0.25f : 2.0f;
	}
	if (streq(cn, "item_artifact_super_damage"))
	{
		return (self->super_damage_finished > g_globalvars.time) ? 0.1f : 1.0f;
	}
	if (streq(cn, "item_artifact_invulnerability"))
	{
		return (self->invincible_finished > g_globalvars.time) ? 0.1f : 1.0f;
	}

	return 1.0f;
}

// Team dedup: a live same-team player already targeting this item with a
// shorter route owns it -- do not stack 4 bots on one armor.
static qbool KBot_TeammateOwnsGoal(gedict_t *self, gedict_t *item, float my_eta)
{
	gedict_t *p;

	if (!teamplay)
	{
		return false;
	}

	for (p = world; (p = find_plr(p));)
	{
		if ((p == self) || ISDEAD(p) || !SameTeam(p, self))
		{
			continue;
		}
		if (p->s.v.goalentity == NUM_FOR_EDICT(item))
		{
			if (KBot_TravelTime(p, item) < my_eta)
			{
				return true;
			}
		}
	}

	return false;
}

// Score one candidate. 0 = not a candidate right now.
static float KBot_ScoreItem(gedict_t *self, gedict_t *item, float base)
{
	float eta, wait, effective_time, value, score;

	if (!item->fb.touch_marker)
	{
		return 0;	// unlinked item: the marker graph cannot path to it
	}

	// Mirrors vanilla: an item that will not respawn before match end is dead.
	if (match_end_time && (item->fb.goal_respawn_time > match_end_time))
	{
		return 0;
	}

	eta = KBot_TravelTime(self, item);
	if (eta >= 1000000.0f)
	{
		return 0;	// no route
	}

	wait = item->fb.goal_respawn_time - g_globalvars.time;
	if (wait < 0)
	{
		wait = 0;
	}

	// Never commit to camping: if the item comes up much later than we can
	// arrive, someone else's timer knowledge is better spent elsewhere.
	if ((wait - eta) > KBOT_GOAL_MAX_EARLY)
	{
		return 0;
	}

	value = base * KBot_NeedFactor(self, item);

	// The timing play: an item that spawns right around our arrival is worth
	// MORE than a floor item -- committing now wins the spawn race.
	if ((wait > 0) && (wait - eta <= KBOT_GOAL_TIMING_WINDOW)
		&& (eta - wait <= KBOT_GOAL_TIMING_WINDOW))
	{
		value *= KBOT_GOAL_TIMING_BONUS;
	}

	effective_time = (eta > wait) ? eta : wait;
	score = value / (effective_time + KBOT_GOAL_TIME_SOFT);

	if ((score > 0) && KBot_TeammateOwnsGoal(self, item, eta))
	{
		score *= KBOT_GOAL_TEAM_DECAY;
	}

	return score;
}

// Predictive goal selection. Returns true when it committed a goal and set
// the same state UpdateGoal's tail would set; false = run the vanilla
// economy untouched.
qbool KBot_SelectGoal(gedict_t *self)
{
	int slot = NUM_FOR_EDICT(self) - 1;
	gedict_t *enemy_ = &g_edicts[self->s.v.enemy];
	gedict_t *best = NULL;
	gedict_t *item;
	float best_score = 0;
	float current_score = 0;
	unsigned int t;

	if (ISDEAD(self) || !self->fb.touch_marker || (slot < 0) || (slot >= MAX_CLIENTS))
	{
		return false;
	}

	for (t = 0; t < sizeof(kbot_item_values) / sizeof(kbot_item_values[0]); ++t)
	{
		for (item = world; (item = ez_find(item, (char *)kbot_item_values[t].classname));)
		{
			float score;

			// item_health covers small/large too: majors-only means mega.
			if (streq(kbot_item_values[t].classname, "item_health")
				&& !((int)item->s.v.spawnflags & H_MEGA))
			{
				continue;
			}

			score = KBot_ScoreItem(self, item, kbot_item_values[t].base);
			if (score <= 0)
			{
				continue;
			}
			if (item == kbot_goal_current[slot])
			{
				current_score = score;
			}
			if (score > best_score)
			{
				best_score = score;
				best = item;
			}
		}
	}

	// Stickiness: keep the committed goal unless the challenger clearly beats
	// it -- stops per-think oscillation between two similar-value items.
	if ((current_score > 0) && (best != kbot_goal_current[slot])
		&& (best_score < current_score * KBOT_GOAL_SWITCH_FACTOR))
	{
		best = kbot_goal_current[slot];
		best_score = current_score;
	}

	if (!best || (best_score < KBOT_GOAL_MIN_SCORE))
	{
		// Nothing worth a committed rotation: vanilla economy decides
		// (backpacks, ammo, enemy hunting, teammate help).
		kbot_goal_current[slot] = NULL;
		return false;
	}

	kbot_goal_current[slot] = best;

	// ---- mirror UpdateGoal's contract ----
	// bot_client.c dereferences fb.virtual_enemy without a NULL check, so it
	// must stay maintained exactly like vanilla's UpdateGoal does.
	self->fb.goal_enemy_repel = self->fb.goal_enemy_desire = 0;
	self->fb.virtual_enemy = (enemy_->fb.touch_marker) ? enemy_ : dropper;

	self->fb.best_goal = self->fb.best_goal2 = best;
	self->fb.best_goal_score = self->fb.best_score2 = best_score;
	best->fb.saved_goal_desire = best_score;

	self->s.v.goalentity = NUM_FOR_EDICT(best);
	// Exact spawn time, no prediction blur: PathScoringLogic paces arrival
	// (goal_late_time) off this.
	self->fb.goal_respawn_time = (best->fb.goal_respawn_time > g_globalvars.time)
			? best->fb.goal_respawn_time : g_globalvars.time;

	return true;
}

#endif // BOT_SUPPORT
