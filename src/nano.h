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

// ---------------------------------------------------------------------------
// S1: navmesh graph (port of rtx src/navmesh/mod.rs + query.rs).
//
// Auto-generated from the parsed BSP clip hull: voxelizes the walkable floor
// into standable cells (one per grid column per floor), classifies the moves
// between nearby cells into directed links (walk/step/drop/jumpgap), and
// answers nearest-cell + A* path queries. Pure over a nano_bsp_t -- no engine
// calls -- so it builds identically to rtx's offline worker build.
//
// S1b ships the land mesh (walk/step/drop/jumpgap). Double/speed/hook/rocket-
// jump, plat/teleport/gate splices, and the lava/slime hazard surcharge are
// added in later stages (dm3 needs none of them for a navigable first mesh).
// ---------------------------------------------------------------------------

// Link traversal kinds (mirror rtx LinkKind). S1b implements the first four;
// the rest are reserved for later stages so the enum matches rtx's ordering.
#define NANO_LINK_WALK       0
#define NANO_LINK_STEP       1
#define NANO_LINK_DROP       2
#define NANO_LINK_JUMPGAP    3
#define NANO_LINK_DJUMP      4	// (reserved, S-later)
#define NANO_LINK_SJUMP      5	// (reserved, S-later)
#define NANO_LINK_PLAT       6	// (reserved, S-later)
#define NANO_LINK_TELEPORT   7	// (reserved, S-later)
#define NANO_LINK_HOOK       8	// (reserved, S-later)
#define NANO_LINK_RJUMP      9	// (reserved, S-later)

// Opaque built graph (heap-owned; Nano_NavFree).
typedef struct nano_navgraph_s nano_navgraph_t;

// Build the land navmesh from a parsed BSP clip hull (pure; no engine calls).
// Caller frees with Nano_NavFree. Returns NULL on alloc failure or a mesh with
// no cells (an empty/degenerate map). Mirrors rtx NavGraph::build + link_cells.
nano_navgraph_t *Nano_NavBuild(const nano_bsp_t *bsp);
void Nano_NavFree(nano_navgraph_t *g);

// Cell whose standing origin is nearest pos (searches outward a few grid
// columns from pos's own column). Returns -1 if the graph is empty or nothing
// is found within range.
int Nano_NavNearest(const nano_navgraph_t *g, const vec3_t pos);

// A* from start to goal cell over the land mesh. Writes the route as a
// sequence of link indices into out_route[0..out_cap-1]; returns the link
// count (>= 0; 0 means start==goal), or -1 if no route exists or the buffer is
// too small. S1b: static link costs only (gate/jitter/penalty added later).
int Nano_NavFindPath(const nano_navgraph_t *g, int start, int goal,
						int *out_route, int out_cap);

// Accessors the brain (S2) reads routes through.
int Nano_NavNumCells(const nano_navgraph_t *g);
int Nano_NavNumLinks(const nano_navgraph_t *g);
const float *Nano_NavCellOrigin(const nano_navgraph_t *g, int cell);	// NULL if OOB
int Nano_NavLinkTarget(const nano_navgraph_t *g, int link);			// dest cell, -1 OOB
int Nano_NavLinkKind(const nano_navgraph_t *g, int link);				// NANO_LINK_*, -1 OOB
float Nano_NavLinkCost(const nano_navgraph_t *g, int link);			// base travel time, -1 OOB

// ---------------------------------------------------------------------------
// S2: navmesh query API (Codex P3).
//
// Dijkstra cost-flood and nearest-reachable helpers for the brain's goal
// selection. One flood per goal-pick cadence replaces many capped A* calls.
// ---------------------------------------------------------------------------

// Sentinel cost returned for unreachable cells by Nano_NavCostsFrom.
#define NANO_NAV_UNREACHABLE 1.0e30f

// Dijkstra flood from `source` cell. Writes the minimum travel-time cost to
// reach every cell into out_costs[0..out_cap-1]. Unreachable cells keep
// NANO_NAV_UNREACHABLE. Returns false if the graph is invalid, source is out of
// range, or out_cap < num_cells.
qbool Nano_NavCostsFrom(const nano_navgraph_t *g, int source,
						float *out_costs, int out_cap);

// Find the cell nearest to `pos` that has a finite cost in the precomputed
// `costs` array (i.e. is reachable from the source of the flood). Returns -1
// if no cell is reachable. costs_cap must be >= num_cells.
int Nano_NavNearestReachable(const nano_navgraph_t *g, const vec3_t pos,
							const float *costs, int costs_cap);

// ---------------------------------------------------------------------------
// S1c: navmesh build lifecycle (port of rtx nav_build.rs, synchronous).
//
// Reads maps/<mapname>.bsp via trap_FS, parses + builds the land mesh, caches
// it for the map. Built lazily on first need (Nano_Frame), at most once per map
// (a failed read isn't retried every frame). Synchronous -- rtx builds on a
// worker thread to avoid a frame hitch, but a map's build runs once at warmup
// (where a hitch is harmless) and is ~150ms on dm3, so the simpler synchronous
// build is used.
// ---------------------------------------------------------------------------

// Free this map's mesh + reset the attempted flag. Call from worldspawn so a
// new map starts clean. Idempotent; safe on a never-built map.
void Nano_NavMapReset(void);

// Ensure the map's navmesh is built (read + parse + build + cache + log). No-op
// if already built or already attempted this map. Returns the graph, or NULL if
// the map has no buildable navmesh (bots simply stay un-navigated, never fatal).
const nano_navgraph_t *Nano_NavEnsure(void);

// The cached graph for the current map (NULL until built / if it failed).
const nano_navgraph_t *Nano_NavGraph(void);

#endif // NANO_SUPPORT

#endif // KTX_NANO_H
