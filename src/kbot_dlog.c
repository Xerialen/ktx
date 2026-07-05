/*
 kbot_dlog.c -- KDLOG decision-log emitter (kbot-0.23.0-dlog)

 Structured tactical-decision telemetry on the FBMOVEPROBE precedent:
 G_cprint key=value lines into the server console (captured to server.log
 by the lab harness), consumed downstream by mvd_analyzer's -decision-log
 pass which resolves entities/locations/times into the analyzer's own
 canonical vocabulary. Design: komodobots2
 docs/specs/2026-07-05-decision-log-design.md.

 Gate: cvar k_kbot_dlog (registered default "0" in world.c).
   0 = off (no work at all), 1 = komodobots only, 2 = all bots.
 Only emits while a match is running ((int)match_in_progress == 2), so
 marker/entity identities are match-mode-valid (technical-context trap #1).

 One anchor line binds the timebases once per match:
   KDLOG_ANCHOR v=1 emitter=<stamp> map=<map> level_time=<s> match_start=<s> dlog=<n>
 Every record line carries t = seconds since match start (the analyzer's
 match-relative axis) plus the deciding bot's resource snapshot.

 Record types (v1 "tactical core", owner call 2026-07-05):
   type=goal   chosen goal + top-K viable candidates (desire/traveltime/score)
   type=enemy  enemy-target changes from BotsPickBestEnemy
   type=evade  bot_evade flips from BotEvadeLogic
   type=play   gapjump/chainhop state transitions (engage/launch/land/...)

 The emitter is strictly read-only: no field written, no decision changed.
 */
#ifdef BOT_SUPPORT

#include "g_local.h"
#include "kbot.h"

#define KDLOG_TOPK 4

typedef struct
{
	gedict_t *ent;
	float desire;
	float goal_time;
	float score;
} kdlog_cand_t;

static kdlog_cand_t kdlog_cand[KDLOG_TOPK];
static int kdlog_cand_n;
static gedict_t *kdlog_cand_owner;

static char kdlog_trig[MAX_CLIENTS][16];
static int kdlog_evade_prev[MAX_CLIENTS]; // 0/1; zero-init = the "off" every bot starts in
static float kdlog_anchor_match; // match_start_time of the last anchor emitted

// Cheap level read; mvdsv resolves registered cvars without file I/O.
static int KDLog_Level(void)
{
	return (int)cvar("k_kbot_dlog");
}

static qbool KDLog_ActiveFor(gedict_t *self)
{
	int level;

	if (!self || !self->isBot)
	{
		return false;
	}

	if ((int)match_in_progress != 2)
	{
		return false;
	}

	level = KDLog_Level();
	if (level >= 2)
	{
		return true;
	}

	return (level == 1) && self->fb.kbot;
}

static float KDLog_MatchTime(void)
{
	return g_globalvars.time - match_start_time;
}

// Emit the timebase anchor once per match (lazily, before the first record).
// Stamps the emitter version so runs can never silently claim dlog coverage
// they don't have (technical-context trap #4).
static void KDLog_AnchorMaybe(void)
{
	char suffix[32];

	if (kdlog_anchor_match == match_start_time)
	{
		return;
	}

	kdlog_anchor_match = match_start_time;
	trap_cvar_string("k_kbot_version_suffix", suffix, sizeof(suffix));
	G_cprint("KDLOG_ANCHOR v=1 emitter=%s%s map=%s level_time=%.3f match_start=%.3f dlog=%d\n",
			 KBOT_VERSION, suffix, mapname, g_globalvars.time, match_start_time, KDLog_Level());
}

// Append " h= a= it= aw= sh= nl= rk= cl= pos=" snapshot to buf.
static void KDLog_Snapshot(gedict_t *self, char *buf, int buf_size)
{
	char part[192];

	snprintf(part, sizeof(part),
			 " h=%d a=%d it=%d aw=%d sh=%d nl=%d rk=%d cl=%d pos=%.1f,%.1f,%.1f",
			 (int)self->s.v.health, (int)self->s.v.armorvalue, (int)self->s.v.items,
			 (int)self->s.v.weapon, (int)self->s.v.ammo_shells, (int)self->s.v.ammo_nails,
			 (int)self->s.v.ammo_rockets, (int)self->s.v.ammo_cells,
			 self->s.v.origin[0], self->s.v.origin[1], self->s.v.origin[2]);
	strlcat(buf, part, buf_size);
}

// Space-free entity descriptor: "cls=<classname>;ied=<edict>;org=x,y,z;m=<marker>"
// (players carry no org/m -- the analyzer joins them by edict/slot).
static void KDLog_EntDesc(gedict_t *ent, char *out, int out_size)
{
	int marker = (ent->fb.touch_marker ? ent->fb.touch_marker->fb.index + 1 : 0);

	if (streq(ent->classname, "player"))
	{
		snprintf(out, out_size, "cls=player;ied=%d;m=%d", NUM_FOR_EDICT(ent), marker);
	}
	else
	{
		snprintf(out, out_size, "cls=%s;ied=%d;org=%.1f,%.1f,%.1f;m=%d",
				 ent->classname && ent->classname[0] ? ent->classname : "unknown",
				 NUM_FOR_EDICT(ent), ent->s.v.origin[0], ent->s.v.origin[1],
				 ent->s.v.origin[2], marker);
	}
}

// ---- trigger bookkeeping -------------------------------------------------

// Mark why the NEXT goal refresh for this player fires (item_taken /
// enemy_event). Called from the sites that zero goal_refresh_time; consumed
// (and cleared) by KDLog_GoalChosen. Default when unmarked: "refresh".
void KDLog_MarkTrigger(gedict_t *plr, const char *trig)
{
	int slot;

	if (!plr || !plr->isBot || KDLog_Level() <= 0)
	{
		return;
	}

	slot = NUM_FOR_EDICT(plr) - 1;
	if ((slot >= 0) && (slot < MAX_CLIENTS))
	{
		strlcpy(kdlog_trig[slot], trig, sizeof(kdlog_trig[slot]));
	}
}

// ---- goal decisions --------------------------------------------------------

// Arm the candidate collector for this bot's UpdateGoal pass.
void KDLog_GoalReset(gedict_t *self)
{
	kdlog_cand_owner = (KDLog_ActiveFor(self) ? self : NULL);
	kdlog_cand_n = 0;
}

// Record one viable candidate (reached EvalGoal's scoring stage). Keeps the
// top-K by score. Score uses the same formula as EvalGoal so candidates are
// comparable even when goal_time exceeds lookahead (negative score).
void KDLog_GoalCandidate(gedict_t *self, gedict_t *goal, float desire, float goal_time)
{
	float score;
	int i, lo;

	if (!goal || (self != kdlog_cand_owner))
	{
		return;
	}

	score = desire * (self->fb.skill.lookahead_time - goal_time) / (goal_time + 5);

	// De-dup: EvalGoal can see the same virtual goal via several markers.
	for (i = 0; i < kdlog_cand_n; ++i)
	{
		if (kdlog_cand[i].ent == goal)
		{
			if (score > kdlog_cand[i].score)
			{
				kdlog_cand[i].desire = desire;
				kdlog_cand[i].goal_time = goal_time;
				kdlog_cand[i].score = score;
			}

			return;
		}
	}

	if (kdlog_cand_n < KDLOG_TOPK)
	{
		lo = kdlog_cand_n++;
	}
	else
	{
		lo = 0;
		for (i = 1; i < KDLOG_TOPK; ++i)
		{
			if (kdlog_cand[i].score < kdlog_cand[lo].score)
			{
				lo = i;
			}
		}

		if (score <= kdlog_cand[lo].score)
		{
			return;
		}
	}

	kdlog_cand[lo].ent = goal;
	kdlog_cand[lo].desire = desire;
	kdlog_cand[lo].goal_time = goal_time;
	kdlog_cand[lo].score = score;
}

static int KDLog_CandCompareDesc(const void *a, const void *b)
{
	float sa = ((const kdlog_cand_t *)a)->score;
	float sb = ((const kdlog_cand_t *)b)->score;

	return (sa < sb) ? 1 : ((sa > sb) ? -1 : 0);
}

// Emit the goal record at the end of UpdateGoal. chosen = what the bot now
// pursues (goalentity / best_goal2); prim = best_goal when the two-step
// lookahead routed through an intermediate goal.
void KDLog_GoalChosen(gedict_t *self)
{
	char buf[1024];
	char desc[192];
	char part[288];
	gedict_t *chosen;
	int slot, i;
	const char *trig = "refresh";

	if (self != kdlog_cand_owner)
	{
		return;
	}

	kdlog_cand_owner = NULL; // one emission per collector pass

	slot = NUM_FOR_EDICT(self) - 1;
	if ((slot >= 0) && (slot < MAX_CLIENTS) && kdlog_trig[slot][0])
	{
		trig = kdlog_trig[slot];
	}

	KDLog_AnchorMaybe();

	snprintf(buf, sizeof(buf), "KDLOG t=%.3f ed=%d type=goal trig=%s m=%d",
			 KDLog_MatchTime(), NUM_FOR_EDICT(self), trig,
			 (self->fb.touch_marker ? self->fb.touch_marker->fb.index + 1 : 0));
	KDLog_Snapshot(self, buf, sizeof(buf));

	chosen = (self->fb.best_goal2 ? self->fb.best_goal2 : self->fb.best_goal);
	if (chosen)
	{
		KDLog_EntDesc(chosen, desc, sizeof(desc));
		// Chosen's desire/tt/score come from the candidate table (EnemyGoalLogic
		// zeroes saved_goal_desire on the winner before we get here).
		part[0] = 0;
		for (i = 0; i < kdlog_cand_n; ++i)
		{
			if (kdlog_cand[i].ent == chosen)
			{
				snprintf(part, sizeof(part), ";des=%.2f;tt=%.2f;sc=%.3f",
						 kdlog_cand[i].desire, kdlog_cand[i].goal_time, kdlog_cand[i].score);
				break;
			}
		}

		strlcat(buf, " chosen=", sizeof(buf));
		strlcat(buf, desc, sizeof(buf));
		strlcat(buf, part, sizeof(buf));

		if (self->fb.best_goal && (self->fb.best_goal != chosen))
		{
			KDLog_EntDesc(self->fb.best_goal, desc, sizeof(desc));
			strlcat(buf, " prim=", sizeof(buf));
			strlcat(buf, desc, sizeof(buf));
		}
	}
	else
	{
		strlcat(buf, " chosen=none", sizeof(buf));
	}

	if (kdlog_cand_n > 1)
	{
		qsort(kdlog_cand, kdlog_cand_n, sizeof(kdlog_cand[0]), KDLog_CandCompareDesc);
	}

	for (i = 0; i < kdlog_cand_n; ++i)
	{
		KDLog_EntDesc(kdlog_cand[i].ent, desc, sizeof(desc));
		snprintf(part, sizeof(part), " c%d=%s;des=%.2f;tt=%.2f;sc=%.3f", i + 1, desc,
				 kdlog_cand[i].desire, kdlog_cand[i].goal_time, kdlog_cand[i].score);
		strlcat(buf, part, sizeof(buf));
	}

	if ((slot >= 0) && (slot < MAX_CLIENTS))
	{
		kdlog_trig[slot][0] = 0;
	}

	G_cprint("%s\n", buf);
}

// ---- enemy / evade / play --------------------------------------------------

// Emit on enemy-target change (called from BotsPickBestEnemy when the pick
// differs from the old target; ted=0 means "no enemy").
void KDLog_Enemy(gedict_t *self)
{
	char buf[512];

	if (!KDLog_ActiveFor(self))
	{
		return;
	}

	KDLog_AnchorMaybe();
	snprintf(buf, sizeof(buf), "KDLOG t=%.3f ed=%d type=enemy ted=%d dist=%.0f m=%d",
			 KDLog_MatchTime(), NUM_FOR_EDICT(self), (int)self->s.v.enemy,
			 self->fb.enemy_dist,
			 (self->fb.touch_marker ? self->fb.touch_marker->fb.index + 1 : 0));
	KDLog_Snapshot(self, buf, sizeof(buf));
	G_cprint("%s\n", buf);
}

// Emit on bot_evade flips (called at the end of BotEvadeLogic).
void KDLog_Evade(gedict_t *self)
{
	char buf[512];
	int slot, cur;

	if (!KDLog_ActiveFor(self))
	{
		return;
	}

	slot = NUM_FOR_EDICT(self) - 1;
	if ((slot < 0) || (slot >= MAX_CLIENTS))
	{
		return;
	}

	cur = (self->fb.bot_evade ? 1 : 0);
	if (kdlog_evade_prev[slot] == cur)
	{
		return;
	}

	kdlog_evade_prev[slot] = cur;
	KDLog_AnchorMaybe();
	snprintf(buf, sizeof(buf), "KDLOG t=%.3f ed=%d type=evade on=%d",
			 KDLog_MatchTime(), NUM_FOR_EDICT(self), cur);
	KDLog_Snapshot(self, buf, sizeof(buf));
	G_cprint("%s\n", buf);
}

// Emit a movement-play state transition (gapjump/chainhop). lane/phase/detail
// must be space-free tokens; detail may be NULL.
void KDLog_Play(gedict_t *self, const char *lane, const char *phase, const char *detail)
{
	char buf[512];

	if (!KDLog_ActiveFor(self))
	{
		return;
	}

	KDLog_AnchorMaybe();
	snprintf(buf, sizeof(buf), "KDLOG t=%.3f ed=%d type=play play=gapjump lane=%s phase=%s",
			 KDLog_MatchTime(), NUM_FOR_EDICT(self), lane ? lane : "?", phase ? phase : "?");
	if (detail && detail[0])
	{
		strlcat(buf, " detail=", sizeof(buf));
		strlcat(buf, detail, sizeof(buf));
	}

	KDLog_Snapshot(self, buf, sizeof(buf));
	G_cprint("%s\n", buf);
}

#endif // BOT_SUPPORT
