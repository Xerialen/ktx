/*
 kbot_routepolicy.c -- Milton-derived route policy (dm3)

 Replaces the frogbot goal economy's uniform randomness with the empirical
 route policy of the reference player (Milton, 15 tVS dm3 4on4 MVDs, 562
 resource transitions, 347 spawns -- komodobots
 experiments/route_policy/milton_reference_dm3.json).

 Tiers under master cvar k_kbot_routepolicy:
   0  off (default): bit-identical vanilla, no state, no reads.
   1  transition bias: in EvalGoal, a kbot's desire for a route resource is
      scaled by P(next resource | last visited resource) from the reference
      table, normalized against uniform. Sparse rows (< RP_MIN_ROW_SUPPORT)
      and unknown conditioning fall back to vanilla.
   2  + spawn openings: at spawn the bot samples an opening resource from the
      reference P(opening | spawn cluster) (conditioned on reaching a resource
      at all) and gets a strong desire boost toward it until reached, timeout
      (RP_OPENING_WINDOW_S), or death.
   3  tier 1 + FLOWCHART openings (kbot_flowchart_dm3.h, owner spec
      dm3spawns.png): each spawn cluster gets a deterministic opening
      SEQUENCE (up to RP_FLOW_MAX_LEGS legs); the active leg dominates goal
      desire, advances on visit or leg timeout (RP_FLOW_LEG_S), and the
      SNG.tele pair splits ring+RA / quad between simultaneous spawners.
      Replaces the tier-2 sampled openings.

 Decision-level only: this module biases WHICH goal the vanilla economy
 desires. Movement, routing, markers, combat micro and the gap-jump play are
 untouched. Reads only self-state, map facts and the static table -- no
 omniscient enemy reads.

 Tunables (read on demand, TA cvar-refresh pattern):
   k_kbot_rp_w             bias strength 0..1 (default 0.6)
   k_kbot_rp_cap           max desire multiplier (default 3.0)
   k_kbot_rp_open_boost    opening-goal multiplier (default 3.0)
   k_kbot_rp_radius        visit radius in qu (default 96)
   k_kbot_rp_weapon_boost  desire multiplier for a weapon node the bot lacks
                           (default 1.5; the transition bias never applies
                           there -- Milton's matrix is conditioned on his
                           loadout, so it suppresses re-acquisition for a
                           bot that has nothing)

 Expects g_local.h to have been included first (KTX header convention).
 */

#ifdef BOT_SUPPORT

#include "g_local.h"
#include "kbot.h"
#include "hm.h"
#include "kbot_routepolicy_dm3.h"
#include "kbot_flowchart_dm3.h"

static int rp_map_ok;
static gedict_t *rp_node_ent[RP_DM3_NUM_NODES];
static float rp_cvar_next;
static int rp_tier;
static float rp_w, rp_cap, rp_open_boost, rp_radius2, rp_weapon_boost;
static float rp_quad_boost;
static float rp_pathnoise;
static qbool rp_debug;

typedef struct rp_bot_s
{
	int last_node;          // last visited resource node, -1 = none since spawn
	int opening_node;       // current opening-leg goal, -1 = none
	float opening_deadline; // g_globalvars.time limit for the current leg
	int spawn_cluster;      // -1 when unclassified
	// tier 3 (flowchart): the full opening sequence; opening_node mirrors
	// seq[seq_idx] so the tier-2 boost/track machinery applies unchanged
	int seq[RP_FLOW_MAX_LEGS];
	float win[RP_FLOW_MAX_LEGS];
	int seq_len;
	int seq_idx;
} rp_bot_t;

static rp_bot_t rp_bots[MAX_CLIENTS + 1];

static void RP_RefreshCvars(void)
{
	float v;

	if (g_globalvars.time < rp_cvar_next)
	{
		return;
	}
	rp_cvar_next = g_globalvars.time + 0.5f;

	rp_tier = (int)cvar("k_kbot_routepolicy");
	v = cvar("k_kbot_rp_w");
	rp_w = ((v > 0) && (v <= 1)) ? v : 0.6f;
	v = cvar("k_kbot_rp_cap");
	rp_cap = (v > 0) ? v : 3.0f;
	v = cvar("k_kbot_rp_open_boost");
	rp_open_boost = (v > 0) ? v : 3.0f;
	v = cvar("k_kbot_rp_radius");
	v = (v > 0) ? v : 96.0f;
	rp_radius2 = v * v;
	v = cvar("k_kbot_rp_weapon_boost");
	rp_weapon_boost = (v > 0) ? v : 1.5f;
	v = cvar("k_kbot_rp_quad_boost");
	rp_quad_boost = (v > 0) ? v : 1.0f;
	v = cvar("k_kbot_rp_pathnoise");
	rp_pathnoise = (v > 0) ? v : 1.0f;
	rp_debug = cvar("k_hm_debug") != 0;
}

static qbool RP_Enabled(gedict_t *self)
{
	return rp_map_ok && (rp_tier >= 1) && self->isBot && self->fb.kbot;
}

static int RP_NodeForEnt(gedict_t *ent)
{
	int i;

	for (i = 0; i < RP_DM3_NUM_NODES; i++)
	{
		if (rp_node_ent[i] == ent)
		{
			return i;
		}
	}

	return -1;
}

static float RP_Dist2(const float *a, const float *b)
{
	float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];

	return dx * dx + dy * dy + dz * dz;
}

// Bind each node id to its live item edict: filter by the node's classname
// family, then nearest to the header origin (map fact; droptofloor z drift is
// why nearest-match beats exact-match).
static const char* RP_NodeClassname(int node)
{
	switch (node)
	{
	case RP_DM3_PENT:     return "item_artifact_invulnerability";
	case RP_DM3_QUAD:     return "item_artifact_super_damage";
	case RP_DM3_RING:     return "item_artifact_invisibility";
	case RP_DM3_RA:       return "item_armorInv";
	case RP_DM3_YA_BOX:   return "item_armor2";
	case RP_DM3_RL:       return "weapon_rocketlauncher";
	case RP_DM3_SNG:      return "weapon_supernailgun";
	case RP_DM3_WATER_GL: return "weapon_grenadelauncher";
	case RP_DM3_WATER_LG: return "weapon_lightning";
	default:              return "item_health"; // the three megas
	}
}

void KBot_RoutePolicyMapInit(void)
{
	int i;
	gedict_t *p;

	memset(rp_bots, 0, sizeof(rp_bots));
	for (i = 0; i <= MAX_CLIENTS; i++)
	{
		int j;

		rp_bots[i].last_node = -1;
		rp_bots[i].opening_node = -1;
		rp_bots[i].spawn_cluster = -1;
		for (j = 0; j < RP_FLOW_MAX_LEGS; j++)
		{
			rp_bots[i].seq[j] = -1;
		}
	}
	memset(rp_node_ent, 0, sizeof(rp_node_ent));
	rp_cvar_next = 0;
	rp_map_ok = false;

	if (!streq(mapname, "dm3"))
	{
		return;
	}

	for (i = 0; i < RP_DM3_NUM_NODES; i++)
	{
		const char *want = RP_NodeClassname(i);
		float best = 160.0f * 160.0f;

		for (p = world; (p = nextent(p));)
		{
			if (!p->classname || !streq(p->classname, want))
			{
				continue;
			}
			if (streq(want, "item_health") && !((int)p->s.v.spawnflags & 2))
			{
				continue; // megas only
			}
			if (RP_Dist2(p->s.v.origin, rp_node_org_dm3[i]) < best)
			{
				best = RP_Dist2(p->s.v.origin, rp_node_org_dm3[i]);
				rp_node_ent[i] = p;
			}
		}
		if (!rp_node_ent[i])
		{
			G_cprint("[kb-route-init] map=%s node=%s UNBOUND -> policy inert\n",
						mapname, rp_node_name_dm3[i]);
			return; // fail-safe: rp_map_ok stays false, module fully inert
		}
	}

	rp_map_ok = true;
	if (cvar("k_hm_debug"))
	{
		G_cprint("[kb-route-init] map=%s nodes=%d spawnpts=%d ok\n", mapname,
					RP_DM3_NUM_NODES, RP_DM3_NUM_SPAWNPTS);
	}
}

// Arm the current flowchart leg, skipping legs whose item the bot BELIEVES
// (HMode_ItemRespawnTime, perception-based) is down beyond the leg window --
// the flowchart's own conditionals ("Got pent? NO -> steal the mega") say a
// downed objective is bypassed, not camped. A leg whose item respawns within
// the window gets deadline = respawn + window (travel there and time it).
static void RP_ArmLeg(gedict_t *self, rp_bot_t *b)
{
	while (b->seq_idx < b->seq_len)
	{
		int node = b->seq[b->seq_idx];
		float win = b->win[b->seq_idx];
		float resp = HMode_ItemRespawnTime(self, rp_node_ent[node]);

		if (resp <= g_globalvars.time + win)
		{
			b->opening_node = node;
			b->opening_deadline = ((resp > g_globalvars.time) ? resp : g_globalvars.time)
					+ win;

			return;
		}
		if (rp_debug)
		{
			G_cprint("[kb-route] bot=%s ev=leg-unavail idx=%d node=%s resp=%.1f t=%.1f\n",
						self->netname, b->seq_idx, rp_node_name_dm3[node], resp,
						g_globalvars.time);
		}
		b->seq_idx++;
	}
	b->opening_node = -1;
}

// At spawn: reset the route chain; tier 2 samples an opening resource from
// the reference distribution, conditioned on "reached a resource" (combat /
// none columns excluded -- outcomes, not intent).
void KBot_RoutePolicySpawnEvent(gedict_t *self, gedict_t *spawn_pos)
{
	int slot = NUM_FOR_EDICT(self);
	rp_bot_t *b;
	int i, cluster = -1;
	float r, mass;

	if ((slot < 1) || (slot > MAX_CLIENTS))
	{
		return;
	}
	b = &rp_bots[slot];
	b->last_node = -1;
	b->opening_node = -1;
	b->opening_deadline = 0;
	b->spawn_cluster = -1;
	b->seq_len = 0;
	b->seq_idx = 0;
	for (i = 0; i < RP_FLOW_MAX_LEGS; i++)
	{
		b->seq[i] = -1;
	}

	RP_RefreshCvars();
	if (!RP_Enabled(self) || (rp_tier < 2) || !spawn_pos)
	{
		return;
	}

	{
		float best = 160.0f * 160.0f;

		for (i = 0; i < RP_DM3_NUM_SPAWNPTS; i++)
		{
			float d = RP_Dist2(spawn_pos->s.v.origin, rp_spawnpt_org_dm3[i]);

			if (d < best)
			{
				best = d;
				cluster = rp_spawnpt_cluster_dm3[i];
			}
		}
	}
	b->spawn_cluster = cluster;
	if (cluster < 0)
	{
		return;
	}

	// tier 3: deterministic flowchart openings (owner spec, dm3spawns.png)
	if (rp_tier >= 3)
	{
		const int *seq = rp_flow_seq_dm3[cluster];
		const float *win = rp_flow_win_dm3[cluster];

		// SNG.tele split: if a live teammate kbot from the same cluster is
		// already opening on ring, this spawner takes the quad branch
		// ("Split ring+RA and quad between the two players").
		if ((cluster == 0) && (seq[0] == RP_DM3_RING))
		{
			for (i = 1; i <= MAX_CLIENTS; i++)
			{
				gedict_t *other = &g_edicts[i];

				if ((i == slot) || (rp_bots[i].spawn_cluster != 0)
						|| (rp_bots[i].seq_len < 1) || (rp_bots[i].seq[0] != RP_DM3_RING)
						|| (rp_bots[i].opening_node < 0))
				{
					continue;
				}
				if (!other->isBot || ISDEAD(other) || !SameTeam(other, self))
				{
					continue;
				}
				seq = rp_flow_seq_sng_alt_dm3;
				win = rp_flow_win_sng_alt_dm3;
				break;
			}
		}

		// RL branch #2: quad believed down -> take the pent mega and prepare
		// for the second quad (do not force the long cross-map RA walk)
		if ((cluster == 1)
				&& (HMode_ItemRespawnTime(self, rp_node_ent[RP_DM3_QUAD])
						> g_globalvars.time + rp_flow_win_dm3[1][0]))
		{
			seq = rp_flow_seq_rl_alt_dm3;
			win = rp_flow_win_rl_alt_dm3;
		}

		for (i = 0; (i < RP_FLOW_MAX_LEGS) && (seq[i] >= 0); i++)
		{
			b->seq[i] = seq[i];
			b->win[i] = win[i];
		}
		b->seq_len = i;
		b->seq_idx = 0;
		RP_ArmLeg(self, b);
		if (rp_debug)
		{
			G_cprint("[kb-route] bot=%s ev=open3 spawn=%s seq=%s%s%s%s%s t=%.1f\n",
						self->netname, rp_spawn_name_dm3[cluster],
						rp_node_name_dm3[b->seq[0]],
						(b->seq_len > 1) ? ">" : "",
						(b->seq_len > 1) ? rp_node_name_dm3[b->seq[1]] : "",
						(b->seq_len > 2) ? ">" : "",
						(b->seq_len > 2) ? rp_node_name_dm3[b->seq[2]] : "",
						g_globalvars.time);
		}

		return;
	}

	if (rp_open_total_dm3[cluster] < RP_MIN_ROW_SUPPORT)
	{
		return;
	}

	mass = 0;
	for (i = 0; i < RP_DM3_NUM_NODES; i++)
	{
		mass += rp_open_prob_dm3[cluster][i];
	}
	if (mass <= 0.001f)
	{
		return;
	}
	r = g_random() * mass;
	for (i = 0; i < RP_DM3_NUM_NODES; i++)
	{
		r -= rp_open_prob_dm3[cluster][i];
		if (r <= 0)
		{
			break;
		}
	}
	if (i >= RP_DM3_NUM_NODES)
	{
		i = RP_DM3_NUM_NODES - 1;
	}
	b->opening_node = i;
	b->opening_deadline = g_globalvars.time + RP_OPENING_WINDOW_S;
	if (rp_debug)
	{
		G_cprint("[kb-route] bot=%s ev=open spawn=%s pick=%s t=%.1f\n",
					self->netname, rp_spawn_name_dm3[cluster], rp_node_name_dm3[i],
					g_globalvars.time);
	}
}

// Per-frame (from KBot_Frame): record resource visits by proximity to the
// bound item edicts. Gives the conditioning state for the transition bias and
// the [kb-route] visit log the bench metrics recompute the empirical matrix
// from.
void KBot_RoutePolicyTrack(gedict_t *self)
{
	int slot = NUM_FOR_EDICT(self);
	rp_bot_t *b;
	int i, node = -1;
	float best;

	RP_RefreshCvars();
	if ((slot < 1) || (slot > MAX_CLIENTS) || !RP_Enabled(self))
	{
		return;
	}
	b = &rp_bots[slot];
	if (ISDEAD(self))
	{
		return;
	}

	// tier 3: a stale opening leg is skipped, not abandoned -- the flowchart
	// chain continues with the next leg (ev=leg-skip marks the miss)
	if ((rp_tier >= 3) && (b->opening_node >= 0) && (b->seq_len > 0)
			&& (g_globalvars.time > b->opening_deadline))
	{
		if (rp_debug)
		{
			G_cprint("[kb-route] bot=%s ev=leg-skip idx=%d node=%s t=%.1f\n",
						self->netname, b->seq_idx, rp_node_name_dm3[b->opening_node],
						g_globalvars.time);
		}
		b->seq_idx++;
		b->opening_node = -1;
		RP_ArmLeg(self, b);
	}

	best = rp_radius2;
	for (i = 0; i < RP_DM3_NUM_NODES; i++)
	{
		float d = RP_Dist2(self->s.v.origin, rp_node_ent[i]->s.v.origin);

		if (d < best)
		{
			best = d;
			node = i;
		}
	}
	if ((node < 0) || (node == b->last_node))
	{
		return;
	}

	if (rp_debug)
	{
		G_cprint("[kb-route] bot=%s ev=visit from=%s to=%s t=%.1f\n", self->netname,
					(b->last_node >= 0) ? rp_node_name_dm3[b->last_node] : "spawn",
					rp_node_name_dm3[node], g_globalvars.time);
	}
	b->last_node = node;
	if ((b->opening_node == node) && (g_globalvars.time <= b->opening_deadline))
	{
		if ((rp_tier >= 3) && (b->seq_len > 0))
		{
			if (rp_debug)
			{
				G_cprint("[kb-route] bot=%s ev=leg-done idx=%d node=%s t=%.1f\n",
							self->netname, b->seq_idx, rp_node_name_dm3[node],
							g_globalvars.time);
			}
			b->seq_idx++;
			b->opening_node = -1;
			RP_ArmLeg(self, b);

			return;
		}
		if (rp_debug)
		{
			G_cprint("[kb-route] bot=%s ev=open-done pick=%s t=%.1f\n", self->netname,
						rp_node_name_dm3[node], g_globalvars.time);
		}
		b->opening_node = -1;
	}
}

// Death aborts the active opening (the flowchart's RIP branch). Called from
// BotPlayerDeathEvent -- dead bots never reach KBot_Frame/Track, so the abort
// must ride the death event itself for the telemetry to see it.
void KBot_RoutePolicyDeathEvent(gedict_t *self)
{
	int slot = NUM_FOR_EDICT(self);
	rp_bot_t *b;

	RP_RefreshCvars();
	if ((slot < 1) || (slot > MAX_CLIENTS) || !RP_Enabled(self))
	{
		return;
	}
	b = &rp_bots[slot];
	if ((rp_tier >= 3) && (b->opening_node >= 0) && (b->seq_len > 0) && rp_debug)
	{
		G_cprint("[kb-route] bot=%s ev=open-abort reason=death idx=%d node=%s t=%.1f\n",
					self->netname, b->seq_idx, rp_node_name_dm3[b->opening_node],
					g_globalvars.time);
	}
	b->opening_node = -1;
	b->seq_len = 0;
	b->last_node = -1;
}

// Path/look noise scale for the two flat "+ g_random()" terms in route
// scoring (bot_routing.c PathScore, bot_botpath.c EvalLook -- both marked
// FIXME: Skill upstream). 1.0 = vanilla noise; k_kbot_rp_pathnoise < 1
// makes kbot route choice greedier/more deliberate (Milton does not wander).
float KBot_RoutePolicyPathNoise(gedict_t *self)
{
	RP_RefreshCvars();
	if (!RP_Enabled(self))
	{
		return 1.0f;
	}

	return rp_pathnoise;
}

// EvalGoal consumer: scale a kbot's desire for a route resource by the
// reference policy. Identity for stock bots, cvar 0, non-resource goals,
// unknown conditioning and sparse rows.
float KBot_RoutePolicyDesireBias(gedict_t *self, gedict_t *goal_entity, float desire)
{
	int slot = NUM_FOR_EDICT(self);
	rp_bot_t *b;
	int from, to;
	float factor;

	RP_RefreshCvars();
	if ((slot < 1) || (slot > MAX_CLIENTS) || !RP_Enabled(self) || (desire <= 0))
	{
		return desire;
	}
	to = RP_NodeForEnt(goal_entity);
	if (to < 0)
	{
		return desire;
	}

	b = &rp_bots[slot];

	// tier 3: the active flowchart leg dominates everything else -- the
	// owner's spawn openings are meant to be unmistakable. Leg advance and
	// timeout live in KBot_RoutePolicyTrack. Need-scaled desires (armor on a
	// stocked bot) are floored so the mandated leg cannot be outbid by a
	// nearby snack.
	if ((rp_tier >= 3) && (b->opening_node >= 0) && (to == b->opening_node)
			&& (g_globalvars.time <= b->opening_deadline))
	{
		if (desire < RP_FLOW_MIN_DESIRE)
		{
			desire = RP_FLOW_MIN_DESIRE;
		}

		return desire * rp_open_boost;
	}

	// A weapon node the bot LACKS is exempt from the Milton conditioning:
	// the reference matrix is conditioned on Milton's loadout (he holds RL
	// near-constantly, so his to-weapon rates are low) and would suppress
	// exactly the pickup a naked bot needs most (R5 diagnosis: komo held RL
	// 1.8% of player-time vs fbots 25.8%). Boost instead, never scale down.
	{
		int held = (int)self->s.v.items;
		qbool lacks = ((to == RP_DM3_RL) && !(held & IT_ROCKET_LAUNCHER))
				|| ((to == RP_DM3_WATER_LG) && !(held & IT_LIGHTNING))
				|| ((to == RP_DM3_SNG) && !(held & IT_SUPER_NAILGUN))
				|| ((to == RP_DM3_WATER_GL) && !(held & IT_GRENADE_LAUNCHER));

		if (lacks)
		{
			return desire * rp_weapon_boost;
		}
	}

	if ((rp_tier == 2) && (b->opening_node >= 0))
	{
		if (g_globalvars.time > b->opening_deadline)
		{
			b->opening_node = -1; // window expired
		}
		else if (to == b->opening_node)
		{
			return desire * rp_open_boost;
		}
	}

	from = b->last_node;
	if ((from < 0) || (rp_row_total_dm3[from] < RP_MIN_ROW_SUPPORT))
	{
		return desire;
	}
	factor = (1.0f - rp_w) + rp_w * rp_trans_prob_dm3[from][to] * RP_DM3_NUM_NODES;
	if (factor > rp_cap)
	{
		factor = rp_cap;
	}
	// Experimental dial (R5 diagnosis: quad takes 10 vs 20, quad frags 19 vs
	// 62): extra weight on the quad node on top of the Milton bias. 1.0 = off.
	if (to == RP_DM3_QUAD)
	{
		factor *= rp_quad_boost;
	}

	return desire * factor;
}

#endif // BOT_SUPPORT
