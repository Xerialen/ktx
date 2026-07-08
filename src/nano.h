/*
 nano.h -- nano-bots: the rtx-port peer brain (qw-ctf/rtx bots in KTX).

 nano is a third server-side brain kind alongside frog and kbot. Unlike kbot
 (a decision-layer overlay that delegates movement/nav/combat 100% to stock
 frogbot logic), nano brings its OWN navigation stack -- a navmesh generated
 from the BSP clip hull, ported from qw-ctf/rtx -- plus rtx-style perception,
 combat and movement intent. It is a peer brain, not an overlay.

 Seating:  botcmd addnano -> FrogbotsAddNano_f -> Nano_MarkBot flags the bot.
 Dispatch: BotsThinkTime() routes a nano-flagged bot to Nano_Frame(); when that
           returns true the stock frogbot think is skipped entirely.

 Non-interference: the whole feature sits behind cmake NANO_SUPPORT (default
 OFF). With NANO_SUPPORT off, no nano source is compiled, no cvars register,
 no dispatch branch exists, and the gedict_t/fb struct is untouched -- the
 built qwprogs.so is byte-identical to the stock frogbot baseline. nano's
 per-bot flag lives in a nano-side runtime array (never in gedict_t), so even
 with NANO_SUPPORT on the shared bot structs are unchanged.

 Expects g_local.h to have been included first (KTX header convention).
*/
#ifndef KTX_NANO_H
#define KTX_NANO_H

#ifdef NANO_SUPPORT

#define NANO_VERSION "nano-df681334"	// rtx main @ df681334 (2026-07-08)

// nano flag states (nano-side array, indexed by edict number).
#define NANO_STATE_OFF     0	// stock frogbot (default)
#define NANO_STATE_MARKED  1	// nano-bot, identity not yet logged this match
#define NANO_STATE_ACTIVE  2	// nano-bot, one-time frame log emitted

// True iff this bot is flagged nano (any state > OFF).
qbool Nano_IsMarked(gedict_t *self);

// Flag a freshly added bot as a nano-bot: sets the nano flag, stamps identity
// markers (userinfo "nano" key + "nb:" name prefix) and logs to the console.
void Nano_MarkBot(gedict_t *bot);

// Clear a bot's nano flag (on slot reuse by a plain addbot). Wired in S4.
void Nano_ClearMark(gedict_t *bot);

// Per-frame brain entry point, called from BotsThinkTime() for nano-flagged
// bots. Returns true if the brain fully handled this frame's think
// (BotsThinkTime then skips the stock frogbot logic); false to fall through.
//
// S0 scaffolding: always returns false (pure delegation, like kbot WP2.1) --
// proves the seam with zero behavior change. The real rtx-port brain
// (navmesh, perception, combat, movement) arrives in later stages.
qbool Nano_Frame(gedict_t *self);

// ---------------------------------------------------------------------------
// S1: navmesh foundation -- BSP clip-hull reader (port of rtx src/bsp.rs).
//
// A minimal BSP parser that reads only the three lumps navigation needs from
// the PLAYER clip hull (hull 1): planes, clipnodes, and models[0]. Hull 1 was
// beveled to the standing player box at compile time, so a point test against
// it answers "would the player box collide here?" -- exactly the walkability
// primitive the navmesh voxelize/classify steps need. Everything else in the
// BSP (render tree, faces, lightmaps, textures, vis) is irrelevant and unread.
//
// Pure over a byte buffer: no engine syscalls, no global state, panic-free
// (every index/offset bounds-checked; out-of-range resolves conservatively to
// CONTENTS_SOLID). This is the self-contained, unit-testable kernel rtx's
// author identified as the portable part of the bot.
// ---------------------------------------------------------------------------

#define NANO_CONTENTS_SOLID (-2)	// CONTENTS_SOLID -- the only leaf value tested
#define NANO_CONTENTS_EMPTY (-1)	// CONTENTS_EMPTY (clip hulls are only SOLID/EMPTY)

// A BSP plane (dplane_t): the half-space normal*dist; kind < 3 is axis-aligned
// (test that single coordinate), >= 3 is a general plane (dot product).
typedef struct
{
	vec3_t normal;
	float dist;
	int kind;
} nano_plane_t;

// A clip-hull BSP node (normalized to the BSP2 shape). children[0] = front
// (d >= 0), children[1] = back; a negative child is a CONTENTS_* leaf.
typedef struct
{
	int plane;
	int children[2];
} nano_clipnode_t;

// The subset of a parsed BSP the navmesh consumes (heap-owned; Nano_BspFree).
typedef struct
{
	nano_plane_t *planes;
	int num_planes;
	nano_clipnode_t *clipnodes;
	int num_clipnodes;
	int hull1_headnode;		// models[0].headnode[1] -- the world's hull-1 root
	vec3_t mins;			// world model bbox (the volume the navmesh voxelizes)
	vec3_t maxs;
} nano_bsp_t;

// Result of a hull segment trace (Nano_Hull1Trace), port of rtx HullTrace.
typedef struct
{
	float fraction;			// fraction of the segment traversed before impact (1 = clear)
	vec3_t endpos;			// impact point (or p2 if clear)
	vec3_t plane_normal;	// surface normal at impact, against the incoming segment
	qbool start_solid;		// p1 started inside solid
	qbool all_solid;		// the whole segment was inside solid
} nano_hulltrace_t;

// Parse the lumps the navmesh needs from a whole-file byte buffer (the engine
// reads maps/<map>.bsp via trap_FS and hands the bytes here). Returns NULL on
// an unsupported version or a malformed/truncated lump. Caller frees with
// Nano_BspFree.
nano_bsp_t *Nano_BspParse(const byte *bytes, int len);
void Nano_BspFree(nano_bsp_t *bsp);

// Walk the hull rooted at headnode, returning the CONTENTS_* value at p
// (SV_HullPointContents). Out-of-range indices resolve to CONTENTS_SOLID.
int Nano_HullContents(const nano_bsp_t *bsp, int headnode, const vec3_t p);

// CONTENTS_* at p in the world's player hull (hull 1).
int Nano_Hull1Contents(const nano_bsp_t *bsp, const vec3_t p);

// True iff the player box centered at p would collide with world geometry.
qbool Nano_IsSolid(const nano_bsp_t *bsp, const vec3_t p);

// Trace the segment p1->p2 through the world's player hull (port of
// SV_RecursiveHullCheck). fraction==1 means the whole segment is clear;
// start_solid means p1 was already inside solid. Pure over planes/clipnodes.
void Nano_Hull1Trace(const nano_bsp_t *bsp, const vec3_t p1, const vec3_t p2,
						nano_hulltrace_t *out);

#endif // NANO_SUPPORT

#endif // KTX_NANO_H
