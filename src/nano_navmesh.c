/*
 nano_navmesh.c -- nano navigation graph (port of rtx src/navmesh/mod.rs + query.rs).

 Builds a land navmesh from a parsed BSP player clip hull (nano_bsp): it asks the
 hull-1 solidity oracle where a player can stand, drops a Cell at each floor, and
 classifies the moves between nearby cells into directed Links (walk/step/drop/
 jumpgap). Costs are travel times, so A* over the graph yields fast routes.

 Pure over a nano_bsp_t -- no engine syscalls, no global state, panic-free.
 This is rtx's offline build, ported to C and gated under NANO_SUPPORT.

 S1b scope: the LAND mesh (walk/step/drop/jumpgap + nearest + A*). The advanced
 movement links (double/speed/hook/rocket jump), the entity splices (plat/
 teleport/gate), and the lava/slime hazard surcharge are deferred to later
 stages -- dm3 is navigable without them for a first mesh.

 Conventions match nano_bsp.c (the reviewed template): vec3_t + mathlib macros,
 malloc/free, bounds-checked indexing, #ifdef NANO_SUPPORT.
*/
#ifdef NANO_SUPPORT

#include "g_local.h"
#include "nano.h"

// ---------------------------------------------------------------------------
// constants (rtx src/navmesh/mod.rs + qphys.rs). Player/movement physics are
// QuakeWorld-pmove literals; navmesh model constants are rtx's.
// ---------------------------------------------------------------------------

#define NANO_STEP_HEIGHT     18.0f	// pmove STEPSIZE: free steps up to this
#define NANO_WALK_DZ         8.0f	// height delta treated as flat (a Walk)
#define NANO_MAX_DROP        4096.0f	// largest one-way fall encoded as a landing
#define NANO_SAFE_FALL       88.0f	// fall height past which QW fall damage applies
#define NANO_JUMP_APEX       45.0f	// standing-jump apex = JUMP_VZ^2/(2*g) = 270^2/1600
#define NANO_JUMP_REACH      200.0f	// horizontal reach of a running jump (floored)
#define NANO_MAX_SPEED       320.0f	// sv_maxspeed default (cost denominator)
#define NANO_GRID            32.0f	// XY sampling step (one column per player body)
#define NANO_SCAN_DZ         8.0f	// vertical sweep step when scanning a column
#define NANO_JUMP_ELEV_SPAN  128.0f	// one jump-dedup elevation band ("storey")
#define NANO_JUMP_ELEV_BANDS 33		// (MAX_DROP/SPAN)+1
#define NANO_JUMP_VZ         270.0f	// jump impulse (velocity.z)
#define NANO_GRAVITY         800.0f	// nominal gravity for ballistic fall modeling
#define NANO_NEIGHBOR_CAP    8192	// scratch cap for neighbors_within (sized for R=7)
#define NANO_INF             NANO_NAV_UNREACHABLE
#define NANO_MAX_WORLD_SPAN  32768.0f	// per-axis bbox cap; any real QW map is far under this

// ---------------------------------------------------------------------------
// vec3 helpers (KTX mathlib has DotProduct + Vector*; add scale/len/len2/lerp).
// ---------------------------------------------------------------------------

static inline float nvec_dist2(const vec3_t a, const vec3_t b)
{
	float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return dx * dx + dy * dy + dz * dz;
}
static inline float nvec_dist(const vec3_t a, const vec3_t b)
{
	return sqrtf(nvec_dist2(a, b));
}
static inline float nvec_len2_xy(const vec3_t a, const vec3_t b)
{
	float dx = a[0] - b[0], dy = a[1] - b[1];
	return dx * dx + dy * dy;
}
static inline float nvec_len_xy(const vec3_t a, const vec3_t b)
{
	return sqrtf(nvec_len2_xy(a, b));
}
static inline void nvec_lerp(const vec3_t a, const vec3_t b, float t, vec3_t out)
{
	out[0] = a[0] + t * (b[0] - a[0]);
	out[1] = a[1] + t * (b[1] - a[1]);
	out[2] = a[2] + t * (b[2] - a[2]);
}

// ---------------------------------------------------------------------------
// data
// ---------------------------------------------------------------------------

typedef struct
{
	vec3_t origin;
	int gx, gy;
} nano_cell_t;

typedef struct
{
	int from, to;	// cell ids
	int kind;		// NANO_LINK_*
	float cost;		// travel-time (seconds)
} nano_link_t;

struct nano_navgraph_s
{
	nano_cell_t *cells;
	int num_cells;
	nano_link_t *links;
	int num_links;
	int links_cap;		// capacity of links (grows with the mesh; not a fixed per-cell cap)
	int *adj_offset;	// CSR row offsets, num_cells+1
	int *adj_links;		// CSR link indices, num_links
	// 2D grid index: per grid column (gx,gy) the contiguous [start,count) range
	// of cells in that column (cells are created column-major during carving,
	// so same-column cells are contiguous).
	int gx0, gy0, gxw, gyw;
	int *col_start;		// gxw*gyw, -1 if the column has no cells
	int *col_count;		// gxw*gyw
};

// growable buffer helper: returns the (possibly new) pointer, or NULL on
// failure (in which case the original pointer is still valid and the caller
// frees it). *cap is the current capacity in elements; need is elements wanted.
static void *nano_grow(void *p, int *cap, int need, size_t elemsz)
{
	int nc;
	void *np;
	if (need <= *cap)
	{
		return p;
	}
	nc = *cap ? *cap : 64;
	while (nc < need)
	{
		nc *= 2;
	}
	np = realloc(p, (size_t)nc * elemsz);
	if (np)
	{
		*cap = nc;
	}
	return np;
}

// Append a link to g->links, growing the array as needed (rtx uses an unbounded
// Vec; a fixed per-cell cap would silently drop links on tall/multi-level maps
// and change reachability). Returns false only on allocation failure (the build
// then aborts and frees everything).
static qbool nano_push_link(nano_navgraph_t *g, nano_link_t link)
{
	if (g->num_links >= g->links_cap)
	{
		nano_link_t *t = (nano_link_t *)nano_grow(
			g->links, &g->links_cap, g->num_links + 1, sizeof(nano_link_t));
		if (!t)
		{
			return false;
		}
		g->links = t;
	}
	g->links[g->num_links++] = link;
	return true;
}

// ---------------------------------------------------------------------------
// grid helpers
// ---------------------------------------------------------------------------

static int nano_floor_grid(float v)
{
	return (int)floorf(v / NANO_GRID);
}

// flat index into the grid 2D array for column (gx,gy), or -1 if out of range.
static int nano_col_index(const nano_navgraph_t *g, int gx, int gy)
{
	int ix, iy;
	if (gx < g->gx0 || gy < g->gy0)
	{
		return -1;
	}
	ix = gx - g->gx0;
	iy = gy - g->gy0;
	if (ix >= g->gxw || iy >= g->gyw)
	{
		return -1;
	}
	return ix * g->gyw + iy;
}

// The cell in column (gx,gy) within STEP_HEIGHT of height z, or -1 if none.
static int nano_cell_near(const nano_navgraph_t *g, int gx, int gy, float z)
{
	int ci = nano_col_index(g, gx, gy);
	int i, start, count;
	if (ci < 0)
	{
		return -1;
	}
	start = g->col_start[ci];
	count = g->col_count[ci];
	if (start < 0 || count <= 0)
	{
		return -1;
	}
	for (i = 0; i < count; i++)
	{
		int id = start + i;
		if (fabsf(g->cells[id].origin[2] - z) <= NANO_STEP_HEIGHT)
		{
			return id;
		}
	}
	return -1;
}

// Whether column (gx,gy) has a cell within STEP_HEIGHT of z (walkable ground
// continues there), so a jump to a target across it isn't warranted.
static qbool nano_has_ground_near(const nano_navgraph_t *g, int gx, int gy, float z)
{
	return nano_cell_near(g, gx, gy, z) >= 0;
}

// Collect every cell id in columns within Chebyshev radius of (gx,gy) into out
// (up to cap). Returns the count; a full buffer silently truncates (rare; only
// a pathologically tall map with a huge radius overflows, and the mesh stays
// valid -- some links just aren't generated).
static int nano_neighbors_within(
	const nano_navgraph_t *g, int gx, int gy, int radius, int *out, int cap)
{
	int dx, dy, n = 0;

	for (dx = -radius; dx <= radius; dx++)
	{
		for (dy = -radius; dy <= radius; dy++)
		{
			int ci = nano_col_index(g, gx + dx, gy + dy);
			int start, count, i;
			if (ci < 0)
			{
				continue;
			}
			start = g->col_start[ci];
			count = g->col_count[ci];
			if (start < 0 || count <= 0)
			{
				continue;
			}
			for (i = 0; i < count; i++)
			{
				if (n >= cap)
				{
					return n;
				}
				out[n++] = start + i;
			}
		}
	}
	return n;
}

// ---------------------------------------------------------------------------
// geometry primitives over the BSP solidity oracle (port of rtx free fns)
// ---------------------------------------------------------------------------

// Bisect the floor origin height between a solid sample below and an empty one.
static float nano_bisect_floor(const nano_bsp_t *bsp, float x, float y, float z_solid, float z_empty)
{
	float lo = z_solid, hi = z_empty;
	int i;
	for (i = 0; i < 8; i++)
	{
		float mid = (lo + hi) * 0.5f;
		vec3_t p;
		VectorSet(p, x, y, mid);
		if (Nano_IsSolid(bsp, p))
		{
			lo = mid;
		}
		else
		{
			hi = mid;
		}
	}
	return hi;
}

// Whether the straight segment between two standing origins is free of solid,
// sampled at the higher origin so a wall or low ceiling between cells blocks it.
static qbool nano_path_clear(const nano_bsp_t *bsp, const vec3_t a, const vec3_t b)
{
	float z = a[2] > b[2] ? a[2] : b[2];
	float xylen = nvec_len_xy(a, b);
	int steps = (int)fmaxf(1.0f, ceilf(xylen / 16.0f));
	int i;
	for (i = 0; i <= steps; i++)
	{
		float t = (float)i / (float)steps;
		vec3_t p;
		nvec_lerp(a, b, t, p);
		p[2] = z;
		if (Nano_IsSolid(bsp, p))
		{
			return false;
		}
	}
	return true;
}

// arc_clear with a caller-chosen apex height and step count: sample a parabola
// peaking `apex` above the higher endpoint and require every point to be open.
static qbool nano_arc_clear_peak(
	const nano_bsp_t *bsp, const vec3_t a, const vec3_t b, float apex, int steps)
{
	float peak = (a[2] > b[2] ? a[2] : b[2]) + apex;
	int i;
	for (i = 0; i <= steps; i++)
	{
		float t = (float)i / (float)steps;
		float z = a[2] + (b[2] - a[2]) * t + 4.0f * (peak - (a[2] > b[2] ? a[2] : b[2])) * t * (1.0f - t);
		vec3_t p;
		nvec_lerp(a, b, t, p);
		p[2] = z;
		if (Nano_IsSolid(bsp, p))
		{
			return false;
		}
	}
	return true;
}

// Whether a jump arc from a to b clears geometry: parabola peaking JUMP_APEX
// above the higher endpoint.
static qbool nano_arc_clear(const nano_bsp_t *bsp, const vec3_t a, const vec3_t b)
{
	return nano_arc_clear_peak(bsp, a, b, NANO_JUMP_APEX, 8);
}

// Airtime of a jump reaching dz above/below the takeoff, at gravity g (the
// descending root of JUMP_VZ*t - 0.5*g*t^2 = dz). 0 if dz is unreachable.
static float nano_jump_airtime(float dz, float gravity)
{
	float disc = NANO_JUMP_VZ * NANO_JUMP_VZ - 2.0f * gravity * dz;
	if (disc < 0.0f)
	{
		return 0.0f;
	}
	return (NANO_JUMP_VZ + sqrtf(disc)) / gravity;
}

// Clearance along the true ballistic path of a run-jump onto a target far below
// (constant horizontal speed, quadratic fall), for a deep plunge.
static qbool nano_ballistic_clear(const nano_bsp_t *bsp, const vec3_t a, const vec3_t b)
{
	float t_land = nano_jump_airtime(b[2] - a[2], NANO_GRAVITY);
	float dist;
	int steps, i;
	if (t_land <= 0.0f)
	{
		return false;
	}
	dist = nvec_dist(a, b);
	steps = (int)fmaxf(8.0f, fminf(48.0f, ceilf(dist / 64.0f)));
	for (i = 0; i <= steps; i++)
	{
		float f = (float)i / (float)steps;
		float t = t_land * f;
		float z = a[2] + NANO_JUMP_VZ * t - 0.5f * NANO_GRAVITY * t * t;
		vec3_t p;
		nvec_lerp(a, b, f, p);
		p[2] = z;
		if (Nano_IsSolid(bsp, p))
		{
			return false;
		}
	}
	return true;
}

// Travel-time cost of a link (rtx link_cost). Reserved kinds (added later)
// carry their rtx fallback cost so the formula stays whole.
static float nano_link_cost(int kind, float horiz, float dz)
{
	float base = (horiz > NANO_GRID ? horiz : NANO_GRID) / NANO_MAX_SPEED;
	float fall = (-dz > NANO_SAFE_FALL) ? sqrtf(2.0f * -dz / NANO_GRAVITY) + 0.4f : 0.0f;
	switch (kind)
	{
		case NANO_LINK_WALK:
			return base;
		case NANO_LINK_STEP:
			return base * 1.1f;
		case NANO_LINK_DROP:
			return base + 0.1f + fall;
		case NANO_LINK_JUMPGAP:
			return base + 0.3f + fall;
		case NANO_LINK_DJUMP:
			return base + 0.6f;
		case NANO_LINK_SJUMP:
			return base + 2.0f;
		case NANO_LINK_PLAT:
			return base + 1.0f;
		case NANO_LINK_TELEPORT:
			return 0.2f;
		case NANO_LINK_HOOK:
			return base + 1.2f;	// HOOK_OVERHEAD fallback
		case NANO_LINK_RJUMP:
			return base + 4.0f;
	}
	return base;
}

// Bucket a grid direction into a 3x3 compass cell (0..8; 4 is the center/unused).
static int nano_dir_bucket(int dgx, int dgy)
{
	int sx = (dgx > 0) - (dgx < 0);
	int sy = (dgy > 0) - (dgy < 0);
	return (sx + 1) + (sy + 1) * 3;
}

static int nano_sign(float v)
{
	return (v > 0.1f) - (v < -0.1f);
}

// Elevation band of a jump target's dz, 0..JUMP_ELEV_BANDS-1 (round, not floor).
static int nano_jump_elev_band(float dz)
{
	int b = (int)lroundf(dz / NANO_JUMP_ELEV_SPAN) + NANO_JUMP_ELEV_BANDS - 1;
	if (b < 0)
	{
		b = 0;
	}
	if (b >= NANO_JUMP_ELEV_BANDS)
	{
		b = NANO_JUMP_ELEV_BANDS - 1;
	}
	return b;
}

static int nano_jump_grid_radius(void)
{
	return (int)ceilf(NANO_JUMP_REACH / NANO_GRID);
}

// ---------------------------------------------------------------------------
// build
// ---------------------------------------------------------------------------

// Scan one column bottom-to-top; for each solid->empty transition (a floor with
// headroom) push a cell. prev_solid starts true (below the world is solid).
static qbool nano_column_floors(
	const nano_bsp_t *bsp, float x, float y, float zmin, float zmax,
	nano_cell_t **cells, int *num_cells, int *cells_cap)
{
	float z = zmin;
	qbool prev_solid = true;

	while (z <= zmax)
	{
		vec3_t p;
		qbool solid;
		VectorSet(p, x, y, z);
		solid = Nano_IsSolid(bsp, p);
		if (prev_solid && !solid)
		{
			float oz = nano_bisect_floor(bsp, x, y, z - NANO_SCAN_DZ, z);
			nano_cell_t *t = nano_grow(*cells, cells_cap, *num_cells + 1, sizeof(nano_cell_t));
			nano_cell_t *c;
			if (!t)
			{
				return false;
			}
			*cells = t;
			c = &(*cells)[*num_cells];
			VectorSet(c->origin, x, y, oz);
			c->gx = nano_floor_grid(x);
			c->gy = nano_floor_grid(y);
			(*num_cells)++;
		}
		prev_solid = solid;
		z += NANO_SCAN_DZ;
	}
	return true;
}

// A grounded move (walk/step/drop) or a short hop-up (jumpgap) to an adjacent
// column's cell, if the path/arc is clear. Returns true and fills *out if linked.
static qbool nano_classify_grounded(
	const nano_navgraph_t *g, const nano_bsp_t *bsp, int from, int to, nano_link_t *out)
{
	nano_cell_t a = g->cells[from];
	nano_cell_t b = g->cells[to];
	float dz = b.origin[2] - a.origin[2];
	float horiz;

	// Knee-high ledge (between step and jump apex): a hop-up jumpgap.
	if (dz > NANO_STEP_HEIGHT && dz <= NANO_JUMP_APEX)
	{
		if (!nano_arc_clear(bsp, a.origin, b.origin))
		{
			return false;
		}
		horiz = nvec_len_xy(a.origin, b.origin);
		out->from = from;
		out->to = to;
		out->kind = NANO_LINK_JUMPGAP;
		out->cost = nano_link_cost(NANO_LINK_JUMPGAP, horiz, dz);
		return true;
	}

	// Classify the rest.
	if (fabsf(dz) <= NANO_WALK_DZ)
	{
		out->kind = NANO_LINK_WALK;
	}
	else if (fabsf(dz) <= NANO_STEP_HEIGHT)
	{
		out->kind = NANO_LINK_STEP;
	}
	else if (dz >= -NANO_MAX_DROP && dz < -NANO_STEP_HEIGHT)
	{
		out->kind = NANO_LINK_DROP;
	}
	else
	{
		return false;	// up beyond a jump's apex -- needs the windowed ledge jumps
	}
	if (!nano_path_clear(bsp, a.origin, b.origin))
	{
		return false;
	}
	horiz = nvec_len_xy(a.origin, b.origin);
	out->from = from;
	out->to = to;
	out->cost = nano_link_cost(out->kind, horiz, dz);
	return true;
}

// One dedup slot: the nearest jump candidate found so far for a (compass octant,
// elevation band) pair.
typedef struct
{
	qbool used;
	float horiz;
	nano_link_t link;
} nano_jumpslot_t;

// Jump links out of `from`: from a ledge edge, within run-jump reach/apex, with a
// clear arc; deduped to the nearest target per (octant, elevation band). Pushes
// each winner onto g->links (grows as needed). Returns false on alloc failure.
static qbool nano_find_jumps(
	nano_navgraph_t *g, const nano_bsp_t *bsp, int from, int *neigh, int neigh_cap)
{
	nano_cell_t a = g->cells[from];
	nano_jumpslot_t best[9][NANO_JUMP_ELEV_BANDS];
	int radius = nano_jump_grid_radius();
	int n, i, oct, band;
	memset(best, 0, sizeof(best));
	n = nano_neighbors_within(g, a.gx, a.gy, radius, neigh, neigh_cap);
	for (i = 0; i < n; i++)
	{
		int to = neigh[i];
		nano_cell_t b;
		int dgx, dgy;
		float dz, horiz;
		qbool clear;
		nano_jumpslot_t *slot;

		if (to == from)
		{
			continue;
		}
		b = g->cells[to];
		dgx = b.gx - a.gx;
		dgy = b.gy - a.gy;
		if (dgx >= -1 && dgx <= 1 && dgy >= -1 && dgy <= 1)
		{
			continue;	// adjacent -- a grounded link if anything
		}
		dz = b.origin[2] - a.origin[2];
		if (dz < -NANO_MAX_DROP || dz > NANO_JUMP_APEX)
		{
			continue;
		}
		horiz = nvec_len_xy(a.origin, b.origin);
		if (horiz > NANO_JUMP_REACH)
		{
			continue;
		}
		// Take off from a ledge edge: the column one step toward B has no ground.
		if (nano_has_ground_near(g, a.gx + nano_sign((float)dgx), a.gy + nano_sign((float)dgy),
									a.origin[2]))
		{
			continue;
		}
		// Shallow crossings check the symmetric hop parabola; a deep plunge flies
		// a different path (out at run speed, then mostly straight down).
		clear = (dz < -NANO_JUMP_ELEV_SPAN) ? nano_ballistic_clear(bsp, a.origin, b.origin)
											: nano_arc_clear(bsp, a.origin, b.origin);
		if (!clear)
		{
			continue;
		}
		oct = nano_dir_bucket(dgx, dgy);
		band = nano_jump_elev_band(dz);
		slot = &best[oct][band];
		if (!slot->used || horiz < slot->horiz)
		{
			slot->used = true;
			slot->horiz = horiz;
			slot->link.from = from;
			slot->link.to = to;
			slot->link.kind = NANO_LINK_JUMPGAP;
			slot->link.cost = nano_link_cost(NANO_LINK_JUMPGAP, horiz, dz);
		}
	}
	// Emit the deduped winners.
	for (oct = 0; oct < 9; oct++)
	{
		for (band = 0; band < NANO_JUMP_ELEV_BANDS; band++)
		{
			if (best[oct][band].used)
			{
				if (!nano_push_link(g, best[oct][band].link))
				{
					return false;
				}
			}
		}
	}
	return true;
}

nano_navgraph_t *Nano_NavBuild(const nano_bsp_t *bsp)
{
	nano_navgraph_t *g;
	int gx0, gy0, gx1, gy1, gx, gy, cells_cap = 0;
	int *neigh = NULL;
	int i, j;

	if (!bsp)
	{
		return NULL;
	}

	// Validate the parsed world bbox: a corrupt/malformed BSP can carry NaN/Inf
	// or disordered/huge bounds that overflow the grid index or invoke UB in the
	// float->int casts of nano_floor_grid. Refuse rather than mis-navigate or
	// hang. The span cap also bounds the column z-scan iteration count. (Codex P1.)
	{
		int k;
		for (k = 0; k < 3; k++)
		{
			if (!isfinite(bsp->mins[k]) || !isfinite(bsp->maxs[k]) || bsp->mins[k] > bsp->maxs[k])
			{
				return NULL;
			}
		}
		if (bsp->maxs[0] - bsp->mins[0] > NANO_MAX_WORLD_SPAN ||
				bsp->maxs[1] - bsp->mins[1] > NANO_MAX_WORLD_SPAN ||
				bsp->maxs[2] - bsp->mins[2] > NANO_MAX_WORLD_SPAN)
		{
			return NULL;
		}
	}

	g = (nano_navgraph_t *)malloc(sizeof(nano_navgraph_t));
	if (!g)
	{
		return NULL;
	}
	memset(g, 0, sizeof(*g));

	// --- carve cells (column-major, so same-column cells are contiguous) ---
	gx0 = nano_floor_grid(bsp->mins[0]);
	gy0 = nano_floor_grid(bsp->mins[1]);
	gx1 = nano_floor_grid(bsp->maxs[0]);
	gy1 = nano_floor_grid(bsp->maxs[1]);
	g->gx0 = gx0;
	g->gy0 = gy0;
	g->gxw = gx1 - gx0 + 1;
	g->gyw = gy1 - gy0 + 1;
	for (gx = gx0; gx <= gx1; gx++)
	{
		for (gy = gy0; gy <= gy1; gy++)
		{
			float x = (float)gx * NANO_GRID;
			float y = (float)gy * NANO_GRID;
			if (!nano_column_floors(bsp, x, y, bsp->mins[2], bsp->maxs[2], &g->cells, &g->num_cells,
										&cells_cap))
			{
				Nano_NavFree(g);
				return NULL;
			}
		}
	}
	if (g->num_cells <= 0)
	{
		Nano_NavFree(g);
		return NULL;	// empty/degenerate mesh
	}

	// --- build the 2D grid index from the contiguous column runs ---
	{
		size_t gn = (size_t)g->gxw * (size_t)g->gyw;
		g->col_start = (int *)malloc(gn * sizeof(int));
		g->col_count = (int *)malloc(gn * sizeof(int));
		if (!g->col_start || !g->col_count)
		{
			Nano_NavFree(g);
			return NULL;
		}
		for (i = 0; i < (int)gn; i++)
		{
			g->col_start[i] = -1;
			g->col_count[i] = 0;
		}
		// Scan cells; same (gx,gy) are contiguous, so mark each column's first
		// cell and count the run length.
		i = 0;
		while (i < g->num_cells)
		{
			int cgx = g->cells[i].gx;
			int cgy = g->cells[i].gy;
			int ci = nano_col_index(g, cgx, cgy);
			int start = i;
			j = i;
			while (j < g->num_cells && g->cells[j].gx == cgx && g->cells[j].gy == cgy)
			{
				j++;
			}
			if (ci >= 0)
			{
				g->col_start[ci] = start;
				g->col_count[ci] = j - start;
			}
			i = j;
		}
	}

	// --- classify links: grounded moves to the 8 neighbors, then jumps ---
	// g->links grows on demand (rtx uses an unbounded Vec); a fixed cap would
	// silently drop valid links on tall/multi-level maps and change reachability.
	neigh = (int *)malloc(NANO_NEIGHBOR_CAP * sizeof(int));
	if (!neigh)
	{
		Nano_NavFree(g);
		return NULL;
	}
	g->links = NULL;
	g->num_links = 0;
	g->links_cap = 0;
	for (i = 0; i < g->num_cells; i++)
	{
		nano_cell_t c = g->cells[i];
		int nn = nano_neighbors_within(g, c.gx, c.gy, 1, neigh, NANO_NEIGHBOR_CAP);
		for (j = 0; j < nn; j++)
		{
			int to = neigh[j];
			nano_link_t link;
			if (to != i && nano_classify_grounded(g, bsp, i, to, &link))
			{
				if (!nano_push_link(g, link))
				{
					free(neigh);
					Nano_NavFree(g);
					return NULL;
				}
			}
		}
		if (!nano_find_jumps(g, bsp, i, neigh, NANO_NEIGHBOR_CAP))
		{
			free(neigh);
			Nano_NavFree(g);
			return NULL;
		}
	}
	free(neigh);

	// --- build CSR adjacency (adj_offset[num_cells+1], adj_links[num_links]) ---
	g->adj_offset = (int *)malloc((size_t)(g->num_cells + 1) * sizeof(int));
	g->adj_links = (int *)malloc((size_t)(g->num_links > 0 ? g->num_links : 1) * sizeof(int));
	if (!g->adj_offset || !g->adj_links)
	{
		Nano_NavFree(g);
		return NULL;
	}
	for (i = 0; i <= g->num_cells; i++)
	{
		g->adj_offset[i] = 0;
	}
	for (i = 0; i < g->num_links; i++)
	{
		g->adj_offset[g->links[i].from + 1]++;
	}
	for (i = 0; i < g->num_cells; i++)
	{
		g->adj_offset[i + 1] += g->adj_offset[i];
	}
	{
		int *cursor = (int *)malloc((size_t)g->num_cells * sizeof(int));
		if (!cursor)
		{
			Nano_NavFree(g);
			return NULL;
		}
		for (i = 0; i < g->num_cells; i++)
		{
			cursor[i] = g->adj_offset[i];
		}
		for (i = 0; i < g->num_links; i++)
		{
			int c = cursor[g->links[i].from]++;
			g->adj_links[c] = i;
		}
		free(cursor);
	}

	return g;
}

void Nano_NavFree(nano_navgraph_t *g)
{
	if (!g)
	{
		return;
	}
	if (g->cells)
	{
		free(g->cells);
	}
	if (g->links)
	{
		free(g->links);
	}
	if (g->adj_offset)
	{
		free(g->adj_offset);
	}
	if (g->adj_links)
	{
		free(g->adj_links);
	}
	if (g->col_start)
	{
		free(g->col_start);
	}
	if (g->col_count)
	{
		free(g->col_count);
	}
	free(g);
}

// ---------------------------------------------------------------------------
// query (port of rtx query.rs)
// ---------------------------------------------------------------------------

// Cell whose origin is nearest pos. Searches the home column then rings outward
// (up to 4 columns); stops once a candidate is found past the first ring.
int Nano_NavNearest(const nano_navgraph_t *g, const vec3_t pos)
{
	int gx = nano_floor_grid(pos[0]);
	int gy = nano_floor_grid(pos[1]);
	int *neigh;
	int radius, best = -1;
	int best_fm = 0;	// 0 = same floor (|dz|<=STEP), 1 = a different floor
	float best_xy = 0.0f;

	if (!g || g->num_cells <= 0)
	{
		return -1;
	}
	neigh = (int *)malloc(NANO_NEIGHBOR_CAP * sizeof(int));
	if (!neigh)
	{
		return -1;
	}
	// Floor-aware nearest: a same-floor cell (within STEP_HEIGHT of pos.z) always
	// beats a different-floor one, then nearest by XY. Pure 3D distance (rtx's
	// metric) can snap to a closer-3D cell on the wrong floor at a ledge and seed
	// the brain with an unreachable route. (Codex review P2.)
	for (radius = 0; radius <= 4; radius++)
	{
		int n = nano_neighbors_within(g, gx, gy, radius, neigh, NANO_NEIGHBOR_CAP);
		int i;
		for (i = 0; i < n; i++)
		{
			int id = neigh[i];
			const float *o = g->cells[id].origin;
			int fm = (fabsf(o[2] - pos[2]) <= NANO_STEP_HEIGHT) ? 0 : 1;
			float xy = nvec_len2_xy(o, pos);
			if (best < 0 || fm < best_fm || (fm == best_fm && xy < best_xy))
			{
				best = id;
				best_fm = fm;
				best_xy = xy;
			}
		}
		if (best >= 0 && radius >= 1)
		{
			break;
		}
	}
	free(neigh);
	return best;
}

// A min-heap node (used by A* and Dijkstra).
typedef struct
{
	float f;
	int cell;
} nano_heap_node_t;

static nano_heap_node_t *nano_heap_push(nano_heap_node_t *h, int *n, int *cap,
										 float f, int cell)
{
	int i = (*n)++;
	if (*n > *cap)
	{
		int nc = *cap ? *cap : 64;
		while (nc < *n)
		{
			nc *= 2;
		}
		h = (nano_heap_node_t *)realloc(h, (size_t)nc * sizeof(nano_heap_node_t));
		if (!h)
		{
			(*n)--;
			return NULL;
		}
		*cap = nc;
	}
	h[i].f = f;
	h[i].cell = cell;
	while (i > 0)
	{
		int parent = (i - 1) / 2;
		if (h[parent].f <= h[i].f)
		{
			break;
		}
		{
			nano_heap_node_t t = h[parent];
			h[parent] = h[i];
			h[i] = t;
		}
		i = parent;
	}
	return h;
}

static int nano_heap_pop(nano_heap_node_t *h, int *n)
{
	int top = h[0].cell;
	int i = 0;
	(*n)--;
	h[0] = h[*n];
	while (1)
	{
		int l = 2 * i + 1, r = 2 * i + 2, s = i;
		if (l < *n && h[l].f < h[s].f)
		{
			s = l;
		}
		if (r < *n && h[r].f < h[s].f)
		{
			s = r;
		}
		if (s == i)
		{
			break;
		}
		{
			nano_heap_node_t t = h[s];
			h[s] = h[i];
			h[i] = t;
		}
		i = s;
	}
	return top;
}

// A* from start to goal. Writes link indices into out_route (up to out_cap);
// returns the link count, or -1 if no route / buffer too small. S1b: static
// costs only (the gate/jitter/penalty dynamic layer is added with the brain).
int Nano_NavFindPath(const nano_navgraph_t *g, int start, int goal, int *out_route, int out_cap)
{
	float *g_cost;
	int *came_from;
	nano_heap_node_t *heap;
	int heap_n = 0, heap_cap = 0, route_len, i;

	if (!g || start < 0 || start >= g->num_cells || goal < 0 || goal >= g->num_cells)
	{
		return -1;
	}
	if (start == goal)
	{
		return 0;
	}

	g_cost = (float *)malloc((size_t)g->num_cells * sizeof(float));
	came_from = (int *)malloc((size_t)g->num_cells * sizeof(int));
	heap = NULL;
	if (!g_cost || !came_from)
	{
		free(g_cost);
		free(came_from);
		free(heap);
		return -1;
	}
	for (i = 0; i < g->num_cells; i++)
	{
		g_cost[i] = NANO_INF;
		came_from[i] = -1;
	}

	{
		// Heuristic = straight-line XY distance / MAX_SPEED: an admissible lower
		// bound (any path covers >= the XY distance at <= MAX_SPEED; vertical
		// drops are ~free, climbs cost via jump links whose cost already exceeds
		// their XY term). rtx uses 3D distance, which overestimates around deep
		// drops and is inadmissible -- XY keeps A* optimal. (Codex review P2.)
		float h0 = nvec_len_xy(g->cells[goal].origin, g->cells[start].origin) / NANO_MAX_SPEED;
		g_cost[start] = 0.0f;
		heap = nano_heap_push(heap, &heap_n, &heap_cap, h0, start);
		if (!heap)
		{
			free(g_cost);
			free(came_from);
			return -1;
		}
	}

	while (heap_n > 0)
	{
		int cell = nano_heap_pop(heap, &heap_n);
		int a, b;
		if (cell == goal)
		{
			break;	// settled goal -- reconstruct
		}
		a = g->adj_offset[cell];
		b = g->adj_offset[cell + 1];
		for (i = a; i < b; i++)
		{
			int li = g->adj_links[i];
			nano_link_t link = g->links[li];
			float ng = g_cost[cell] + link.cost;	// + link_extra (0, static costs)
			if (ng < g_cost[link.to])
			{
				float hf;
				g_cost[link.to] = ng;
				came_from[link.to] = li;
				hf = ng + nvec_len_xy(g->cells[goal].origin, g->cells[link.to].origin) / NANO_MAX_SPEED;
				heap = nano_heap_push(heap, &heap_n, &heap_cap, hf, link.to);
				if (!heap)
				{
					free(g_cost);
					free(came_from);
					return -1;
				}
			}
		}
	}

	if (g_cost[goal] >= NANO_INF)
	{
		// goal never reached.
		free(g_cost);
		free(came_from);
		free(heap);
		return -1;
	}

	// Reconstruct: walk came_from link indices back to start, then reverse.
	route_len = 0;
	{
		int c = goal;
		while (c != start && c >= 0)
		{
			int li = came_from[c];
			if (li < 0)
			{
				break;	// shouldn't happen if goal was reached
			}
			route_len++;
			c = g->links[li].from;
		}
	}
	if (out_route && out_cap < route_len)
	{
		free(g_cost);
		free(came_from);
		free(heap);
		return -1;	// buffer too small
	}
	if (out_route)
	{
		int c = goal, k = route_len;
		while (c != start && c >= 0 && k > 0)
		{
			int li = came_from[c];
			if (li < 0)
			{
				break;
			}
			out_route[--k] = li;
			c = g->links[li].from;
		}
	}

	free(g_cost);
	free(came_from);
	free(heap);
	return route_len;
}

// ---------------------------------------------------------------------------
// S2 query API (Codex P3): Dijkstra cost-flood + nearest-reachable.
// ---------------------------------------------------------------------------

// Dijkstra flood from `source` over the land mesh. Writes the minimum cost to
// reach each cell into out_costs[0..num_cells-1]; unreachable cells keep
// NANO_NAV_UNREACHABLE. The CSR adjacency and link costs are the same as A*.
qbool Nano_NavCostsFrom(const nano_navgraph_t *g, int source,
						float *out_costs, int out_cap)
{
	nano_heap_node_t *heap;
	int heap_n = 0, heap_cap = 0, i;

	if (!g || source < 0 || source >= g->num_cells || !out_costs ||
		out_cap < g->num_cells)
	{
		return false;
	}

	for (i = 0; i < g->num_cells; i++)
	{
		out_costs[i] = NANO_INF;
	}

	heap = NULL;
	out_costs[source] = 0.0f;
	heap = nano_heap_push(heap, &heap_n, &heap_cap, 0.0f, source);
	if (!heap)
	{
		return false;
	}

	while (heap_n > 0)
	{
		int cell = nano_heap_pop(heap, &heap_n);
		int a = g->adj_offset[cell];
		int b = g->adj_offset[cell + 1];

		for (i = a; i < b; i++)
		{
			int li = g->adj_links[i];
			nano_link_t link = g->links[li];
			float nc = out_costs[cell] + link.cost;

			if (nc < out_costs[link.to])
			{
				out_costs[link.to] = nc;
				heap = nano_heap_push(heap, &heap_n, &heap_cap, nc, link.to);
				if (!heap)
				{
					return false;
				}
			}
		}
	}

	free(heap);
	return true;
}

// Find the cell nearest to `pos` that is reachable according to `costs`
// (i.e. costs[id] < NANO_NAV_UNREACHABLE). Searches the home column and rings
// outward (same fast path as Nano_NavNearest), then falls back to a full scan
// if no reachable cell is found nearby. Returns -1 only if no cell is reachable.
int Nano_NavNearestReachable(const nano_navgraph_t *g, const vec3_t pos,
							const float *costs, int costs_cap)
{
	int gx = nano_floor_grid(pos[0]);
	int gy = nano_floor_grid(pos[1]);
	int *neigh;
	int radius, best = -1;
	int best_fm = 0;	// 0 = same floor (|dz| <= STEP_HEIGHT), 1 = different floor
	float best_xy = 0.0f;
	int i;

	if (!g || g->num_cells <= 0 || !costs || costs_cap < g->num_cells)
	{
		return -1;
	}

	neigh = (int *)malloc(NANO_NEIGHBOR_CAP * sizeof(int));
	if (!neigh)
	{
		return -1;
	}

	for (radius = 0; radius <= 4; radius++)
	{
		int n = nano_neighbors_within(g, gx, gy, radius, neigh, NANO_NEIGHBOR_CAP);

		for (i = 0; i < n; i++)
		{
			int id = neigh[i];
			const float *o = g->cells[id].origin;
			int fm;
			float xy;

			if (costs[id] >= NANO_INF)
			{
				continue;
			}

			fm = (fabsf(o[2] - pos[2]) <= NANO_STEP_HEIGHT) ? 0 : 1;
			xy = nvec_len2_xy(o, pos);
			if (best < 0 || fm < best_fm || (fm == best_fm && xy < best_xy))
			{
				best = id;
				best_fm = fm;
				best_xy = xy;
			}
		}

		if (best >= 0 && radius >= 1)
		{
			free(neigh);
			return best;
		}
	}

	// Fallback: scan every cell to honor the "nearest reachable" contract even
	// when the query point is far from any reachable cell.
	for (i = 0; i < g->num_cells; i++)
	{
		const float *o = g->cells[i].origin;
		int fm;
		float xy;

		if (costs[i] >= NANO_INF)
		{
			continue;
		}

		fm = (fabsf(o[2] - pos[2]) <= NANO_STEP_HEIGHT) ? 0 : 1;
		xy = nvec_len2_xy(o, pos);
		if (best < 0 || fm < best_fm || (fm == best_fm && xy < best_xy))
		{
			best = i;
			best_fm = fm;
			best_xy = xy;
		}
	}

	free(neigh);
	return best;
}

// ---------------------------------------------------------------------------
// accessors
// ---------------------------------------------------------------------------

int Nano_NavNumCells(const nano_navgraph_t *g)
{
	return g ? g->num_cells : 0;
}

int Nano_NavNumLinks(const nano_navgraph_t *g)
{
	return g ? g->num_links : 0;
}

const float *Nano_NavCellOrigin(const nano_navgraph_t *g, int cell)
{
	if (!g || cell < 0 || cell >= g->num_cells)
	{
		return NULL;
	}
	return g->cells[cell].origin;
}

int Nano_NavLinkTarget(const nano_navgraph_t *g, int link)
{
	if (!g || link < 0 || link >= g->num_links)
	{
		return -1;
	}
	return g->links[link].to;
}

int Nano_NavLinkKind(const nano_navgraph_t *g, int link)
{
	if (!g || link < 0 || link >= g->num_links)
	{
		return -1;
	}
	return g->links[link].kind;
}

float Nano_NavLinkCost(const nano_navgraph_t *g, int link)
{
	if (!g || link < 0 || link >= g->num_links)
	{
		return -1.0f;
	}
	return g->links[link].cost;
}

#endif // NANO_SUPPORT
