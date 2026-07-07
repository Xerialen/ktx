// kbot_flowchart_dm3.h -- owner-authored dm3 spawn openings (NOT generated).
// Source: the owner's "dm3 spawns colorised" flowchart (Downloads/dm3spawns.png,
// drawio 2026-07-07). Each spawn cluster gets a deterministic opening sequence;
// legs advance on visit or on RP_FLOW_LEG_S timeout. The SNG.tele cluster is
// split between simultaneous spawners per the flowchart ("Split ring+RA and
// quad between the two players").
//
// Cluster indices follow rp_spawn_name_dm3 in kbot_routepolicy_dm3.h:
//   0 SNG.tele  1 RL  2 RA.tunnel  3 YA.box  4 lifts
#ifndef KBOT_FLOWCHART_DM3_H
#define KBOT_FLOWCHART_DM3_H

#define RP_FLOW_MAX_LEGS 3
#define RP_FLOW_LEG_S    12.0f

// primary opening per spawn cluster (-1 pads unused legs)
static const int rp_flow_seq_dm3[RP_DM3_NUM_SPAWNS][RP_FLOW_MAX_LEGS] = {
	{ RP_DM3_RING, RP_DM3_RA, -1 },                    // SNG.tele: ring -> RA
	{ RP_DM3_QUAD, RP_DM3_RA, -1 },                    // RL: high bridge -> window -> quad -> RA
	{ RP_DM3_HILL, RP_DM3_WATER_LG, RP_DM3_RA },       // RA.tunnel: center mega -> first LG -> RA
	{ RP_DM3_QUAD, -1, -1 },                           // YA.box: get quad
	{ RP_DM3_PENT, RP_DM3_PENT_MH, RP_DM3_WATER_GL },  // lifts: pent fast -> mega -> GL
};

// SNG.tele alternative when a teammate already opened on ring (the split)
static const int rp_flow_seq_sng_alt_dm3[RP_FLOW_MAX_LEGS] = {
	RP_DM3_QUAD, -1, -1
};

#endif // KBOT_FLOWCHART_DM3_H
