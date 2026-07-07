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
#define RP_FLOW_LEG_S    15.0f
// desire floor for the active leg BEFORE the open boost: armor/health desire
// is need-scaled and near zero for a stocked bot, which let nearby megas
// outbid the mandated leg (F1: RA leg lost to hill mega)
#define RP_FLOW_MIN_DESIRE 150.0f

// primary opening per spawn cluster (-1 pads unused legs)
static const int rp_flow_seq_dm3[RP_DM3_NUM_SPAWNS][RP_FLOW_MAX_LEGS] = {
	{ RP_DM3_RING, RP_DM3_RA, -1 },                    // SNG.tele: ring -> RA
	{ RP_DM3_QUAD, RP_DM3_RA, -1 },                    // RL: high bridge -> window -> quad -> RA
	{ RP_DM3_HILL, RP_DM3_WATER_LG, RP_DM3_RA },       // RA.tunnel: center mega -> first LG -> RA
	{ RP_DM3_QUAD, -1, -1 },                           // YA.box: get quad
	{ RP_DM3_PENT, RP_DM3_PENT_MH, RP_DM3_WATER_GL },  // lifts: pent fast -> mega -> GL
};

// per-leg window seconds (travel-length calibrated, F1-F3 bench evidence:
// tele->ring runs ~3s but water.LG->RA and RL->quad run 15-25s)
static const float rp_flow_win_dm3[RP_DM3_NUM_SPAWNS][RP_FLOW_MAX_LEGS] = {
	{ 15.0f, 25.0f, 15.0f },   // SNG.tele
	{ 20.0f, 25.0f, 15.0f },   // RL
	{ 12.0f, 15.0f, 25.0f },   // RA.tunnel
	{ 20.0f, 15.0f, 15.0f },   // YA.box
	{ 15.0f, 12.0f, 15.0f },   // lifts
};

// SNG.tele alternative when a teammate already opened on ring (the split)
static const int rp_flow_seq_sng_alt_dm3[RP_FLOW_MAX_LEGS] = {
	RP_DM3_QUAD, -1, -1
};
static const float rp_flow_win_sng_alt_dm3[RP_FLOW_MAX_LEGS] = {
	20.0f, 15.0f, 15.0f
};

// RL alternative when quad is believed down (flowchart branch #2: high
// bridge -> lifts, take armor/mega, prepare for the second quad)
static const int rp_flow_seq_rl_alt_dm3[RP_FLOW_MAX_LEGS] = {
	RP_DM3_PENT_MH, RP_DM3_QUAD, -1
};
// quad leg horizon 40s: the chart mandates WAITING ("Wait patiently ...
// prepare for the second quad") -- the quad cycle is 60s, so a 25s window
// skipped the leg almost every time (series 7: 0/47 done)
static const float rp_flow_win_rl_alt_dm3[RP_FLOW_MAX_LEGS] = {
	15.0f, 40.0f, 15.0f
};

// RL branch #1 on TEAM QUAD: "window -> quad -> RA" is the movement line,
// not a quad pickup -- the teammate is CARRYING the quad, so the item leg
// can never complete (series 6: fired 3x, quad leg 0 done). The goal is RA.
static const int rp_flow_seq_rl_teamquad_dm3[RP_FLOW_MAX_LEGS] = {
	RP_DM3_RA, -1, -1
};
static const float rp_flow_win_rl_teamquad_dm3[RP_FLOW_MAX_LEGS] = {
	30.0f, 15.0f, 15.0f
};

#endif // KBOT_FLOWCHART_DM3_H
