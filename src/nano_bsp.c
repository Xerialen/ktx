/*
 nano_bsp.c -- nano navmesh foundation: BSP clip-hull reader (port of rtx src/bsp.rs).

 Reads only the three lumps navigation needs from the PLAYER clip hull (hull 1):
 planes, clipnodes, and models[0]. Hull 1 was beveled to the standing player box
 at compile time, so a point test against it answers "would the player box
 collide here?" -- the walkability primitive the navmesh voxelize/classify steps
 need. Everything else in the BSP (render tree, faces, lightmaps, vis) is unread.

 Pure over a byte buffer: no engine syscalls, no global state, panic-free (every
 index/offset is bounds-checked; out-of-range resolves conservatively to
 CONTENTS_SOLID, never an out-of-bounds read). This is the self-contained,
 unit-testable kernel rtx's author identified as the portable part of the bot.

 Conventions: matches KTX (g_local.h pulls q_shared.h's vec3_t + mathlib.h's
 DotProduct/Vector* macros); the few extra vector ops bsp.rs uses are static
 inline helpers here. No header leaks -- only the Nano_Bsp* symbols in nano.h.
*/
#ifdef NANO_SUPPORT

#include "g_local.h"
#include "nano.h"

// Crossing point is placed this far onto the near side of a plane during a hull
// trace, so a bounce restart doesn't immediately re-collide with the surface it
// left (rtx DIST_EPSILON). Internal to the trace; not in the public header.
#define NANO_DIST_EPSILON 0.03125f

// ---------------------------------------------------------------------------
// small vec3 helpers (KTX mathlib has DotProduct + Vector{Add,Sub,Copy,Clear,
// Set}; bsp.rs also uses scalar scale, length, lerp, and negate)
// ---------------------------------------------------------------------------

static inline void nvec_sub(const vec3_t a, const vec3_t b, vec3_t out)
{
	VectorSubtract(a, b, out);
}
static inline void nvec_add(const vec3_t a, const vec3_t b, vec3_t out)
{
	VectorAdd(a, b, out);
}
static inline void nvec_copy(const vec3_t a, vec3_t out)
{
	VectorCopy(a, out);
}
// out = a + s*b
static inline void nvec_ma(const vec3_t a, float s, const vec3_t b, vec3_t out)
{
	out[0] = a[0] + s * b[0];
	out[1] = a[1] + s * b[1];
	out[2] = a[2] + s * b[2];
}
// out = p1 + t*(p2-p1)
static inline void nvec_lerp(const vec3_t p1, const vec3_t p2, float t, vec3_t out)
{
	out[0] = p1[0] + t * (p2[0] - p1[0]);
	out[1] = p1[1] + t * (p2[1] - p1[1]);
	out[2] = p1[2] + t * (p2[2] - p1[2]);
}
static inline void nvec_negate(const vec3_t a, vec3_t out)
{
	out[0] = -a[0];
	out[1] = -a[1];
	out[2] = -a[2];
}

// ---------------------------------------------------------------------------
// little-endian byte readers -- every read is bounds-checked against len and
// returns false (leaving *off unchanged) on overflow, so a malformed/truncated
// BSP can never read past the buffer. Matches rtx's read_le / .get() guards.
// ---------------------------------------------------------------------------

#define NANO_BSP_VERSION_BSP29 29u
#define NANO_BSP_VERSION_BSPHL 30u
#define NANO_BSP_VERSION_BSP2  0x32505342u	// "BSP2" as a little-endian u32

static qbool nano_rd_u32(const byte *b, int len, int *off, unsigned *out)
{
	if (*off < 0 || len - *off < 4)
	{
		return false;
	}
	*out = (unsigned)b[*off] | ((unsigned)b[*off + 1] << 8) | ((unsigned)b[*off + 2] << 16) |
			((unsigned)b[*off + 3] << 24);
	*off += 4;
	return true;
}

static qbool nano_rd_i32(const byte *b, int len, int *off, int *out)
{
	unsigned u;
	if (!nano_rd_u32(b, len, off, &u))
	{
		return false;
	}
	*out = (int)u;
	return true;
}

static qbool nano_rd_i16(const byte *b, int len, int *off, int *out)
{
	if (*off < 0 || len - *off < 2)
	{
		return false;
	}
	*out = (short)((unsigned short)b[*off] | ((unsigned short)b[*off + 1] << 8));
	*off += 2;
	return true;
}

// IEEE-754 bit cast of the next little-endian 4 bytes to a float.
static qbool nano_rd_f32(const byte *b, int len, int *off, float *out)
{
	unsigned u;
	if (!nano_rd_u32(b, len, off, &u))
	{
		return false;
	}
	// strict-aliasing-safe reinterpret of the bit pattern to a float.
	memcpy(out, &u, sizeof(float));
	return true;
}

// ---------------------------------------------------------------------------
// BSP lump layout (Quake / BSP2): u32 version, then 15 lumps of {u32 offset,
// u32 size}. The navmesh needs lumps 1 (planes), 9 (clipnodes), 14 (models).
// ---------------------------------------------------------------------------

#define NANO_LUMP_PLANES     1
#define NANO_LUMP_CLIPNODES  9
#define NANO_LUMP_MODELS     14
#define NANO_BSP_MAX_LUMPS   15

typedef struct
{
	unsigned offset;
	unsigned size;
} nano_lump_t;

// Read lump i from the directory (no bounds on lump contents yet). false if
// the directory entry itself is out of range.
static qbool nano_rd_lump(const byte *b, int len, int i, nano_lump_t *out)
{
	int off = 4 + i * 8;
	unsigned lo, sz;
	if (!nano_rd_u32(b, len, &off, &lo) || !nano_rd_u32(b, len, &off, &sz))
	{
		return false;
	}
	out->offset = lo;
	out->size = sz;
	return true;
}

void Nano_BspFree(nano_bsp_t *bsp)
{
	if (!bsp)
	{
		return;
	}
	if (bsp->planes && bsp->num_planes > 0)
	{
		free(bsp->planes);
	}
	if (bsp->clipnodes && bsp->num_clipnodes > 0)
	{
		free(bsp->clipnodes);
	}
	free(bsp);
}

// Parse planes (lump 1): each 20 bytes -- vec3 normal, f32 dist, i32 kind.
static qbool nano_parse_planes(const byte *b, int len, const nano_lump_t *lump, nano_bsp_t *out)
{
	const int plane_sz = 20;
	int count, i, off;
	nano_plane_t *planes;

	if (lump->size <= 0 || lump->offset > (unsigned)len)
	{
		return false;
	}
	count = (int)(lump->size / (unsigned)plane_sz);
	if (count <= 0)
	{
		return false;
	}
	planes = (nano_plane_t *)malloc(count * sizeof(nano_plane_t));
	if (!planes)
	{
		return false;
	}
	off = (int)lump->offset;
	for (i = 0; i < count; i++)
	{
		nano_plane_t *p = &planes[i];
		if (!nano_rd_f32(b, len, &off, &p->normal[0]) ||
				!nano_rd_f32(b, len, &off, &p->normal[1]) ||
				!nano_rd_f32(b, len, &off, &p->normal[2]) ||
				!nano_rd_f32(b, len, &off, &p->dist) || !nano_rd_i32(b, len, &off, &p->kind))
		{
			free(planes);
			return false;
		}
	}
	out->planes = planes;

	out->num_planes = count;
	return true;
}

// Parse clipnodes (lump 9): v29/HL store i16 children (8 bytes each); BSP2
// stores i32 (12 bytes). Both widen to the normalized nano_clipnode_t shape.
static qbool nano_parse_clipnodes(
	const byte *b, int len, const nano_lump_t *lump, unsigned version, nano_bsp_t *out)
{
	qbool wide = (version == NANO_BSP_VERSION_BSP2);
	int node_sz = wide ? 12 : 8;
	int count, i, off;
	nano_clipnode_t *nodes;

	if (lump->size <= 0 || lump->offset > (unsigned)len)
	{
		return false;
	}
	count = (int)(lump->size / (unsigned)node_sz);
	if (count <= 0)
	{
		return false;
	}
	nodes = (nano_clipnode_t *)malloc(count * sizeof(nano_clipnode_t));
	if (!nodes)
	{
		return false;
	}
	off = (int)lump->offset;
	for (i = 0; i < count; i++)
	{
		nano_clipnode_t *n = &nodes[i];
		unsigned plane;
		int c0, c1;
		// plane index is u32 in all formats; children are i16 (narrow) or i32 (wide).
		if (!nano_rd_u32(b, len, &off, &plane))
		{
			free(nodes);
			return false;
		}
		if (wide)
		{
			if (!nano_rd_i32(b, len, &off, &c0) || !nano_rd_i32(b, len, &off, &c1))
			{
				free(nodes);
				return false;
			}
		}
		else
		{
			if (!nano_rd_i16(b, len, &off, &c0) || !nano_rd_i16(b, len, &off, &c1))
			{
				free(nodes);
				return false;
			}
		}
		n->plane = (int)plane;
		n->children[0] = c0;
		n->children[1] = c1;
	}
	out->clipnodes = nodes;
	out->num_clipnodes = count;
	return true;
}

// Parse models[0] (lump 14, first record): mins(12) maxs(12) origin(12,skip)
// headnode[0](4,skip) headnode[1](4,=hull1_headnode). Only the world model is
// read; submodels (*N) are engine-managed and irrelevant to navmesh geometry.
static qbool nano_parse_world_model(const byte *b, int len, const nano_lump_t *lump, nano_bsp_t *out)
{
	int off;
	int clip1;

	if (lump->size < 44 || lump->offset > (unsigned)len)
	{
		return false;
	}
	off = (int)lump->offset;
	if (!nano_rd_f32(b, len, &off, &out->mins[0]) ||
			!nano_rd_f32(b, len, &off, &out->mins[1]) ||
			!nano_rd_f32(b, len, &off, &out->mins[2]) ||
			!nano_rd_f32(b, len, &off, &out->maxs[0]) ||
			!nano_rd_f32(b, len, &off, &out->maxs[1]) ||
			!nano_rd_f32(b, len, &off, &out->maxs[2]))
	{
		return false;
	}
	// skip origin (3 floats) + headnode[0] (1 int) = 16 bytes
	off += 16;
	if (!nano_rd_i32(b, len, &off, &clip1))
	{
		return false;
	}
	out->hull1_headnode = clip1;
	return true;
}

nano_bsp_t *Nano_BspParse(const byte *bytes, int len)
{
	unsigned version;
	nano_lump_t planes_lump, clipnodes_lump, models_lump;
	nano_bsp_t *bsp;

	if (!bytes || len < 4)
	{
		return NULL;
	}
	{
		int off = 0;
		if (!nano_rd_u32(bytes, len, &off, &version))
		{
			return NULL;
		}
	}
	if (version != NANO_BSP_VERSION_BSP29 && version != NANO_BSP_VERSION_BSPHL &&
			version != NANO_BSP_VERSION_BSP2)
	{
		return NULL;	// unsupported BSP version
	}
	if (!nano_rd_lump(bytes, len, NANO_LUMP_PLANES, &planes_lump) ||
			!nano_rd_lump(bytes, len, NANO_LUMP_CLIPNODES, &clipnodes_lump) ||
			!nano_rd_lump(bytes, len, NANO_LUMP_MODELS, &models_lump))
	{
		return NULL;
	}

	bsp = (nano_bsp_t *)malloc(sizeof(nano_bsp_t));
	if (!bsp)
	{
		return NULL;
	}
	memset(bsp, 0, sizeof(*bsp));

	if (!nano_parse_planes(bytes, len, &planes_lump, bsp) ||
			!nano_parse_clipnodes(bytes, len, &clipnodes_lump, version, bsp) ||
			!nano_parse_world_model(bytes, len, &models_lump, bsp))
	{
		Nano_BspFree(bsp);
		return NULL;
	}
	if (bsp->hull1_headnode < 0 || bsp->hull1_headnode >= bsp->num_clipnodes)
	{
		// degenerate/unsupported hull (a test cube has 0 clipnodes); refuse rather
		// than mis-navigate. rtx also requires a real hull-1 root.
		Nano_BspFree(bsp);
		return NULL;
	}
	return bsp;
}

// ---------------------------------------------------------------------------
// hull point test (SV_HullPointContents) -- iterative walk; O(tree depth),
// never recurses. Out-of-range indices resolve to CONTENTS_SOLID.
// ---------------------------------------------------------------------------

int Nano_HullContents(const nano_bsp_t *bsp, int headnode, const vec3_t p)
{
	int num = headnode;
	int guard = 0;

	// A malformed tree could cycle; bound the walk so we always terminate.
	while (num >= 0 && guard++ < (bsp->num_clipnodes + 1))
	{
		nano_clipnode_t *node;
		nano_plane_t *plane;
		float d;
		int k;

		if (num >= bsp->num_clipnodes)
		{
			return NANO_CONTENTS_SOLID;
		}
		node = &bsp->clipnodes[num];
		if (node->plane < 0 || node->plane >= bsp->num_planes)
		{
			return NANO_CONTENTS_SOLID;
		}
		plane = &bsp->planes[node->plane];
		if (plane->kind >= 0 && plane->kind < 3)
		{
			k = plane->kind;
			d = p[k] - plane->dist;
		}
		else
		{
			d = DotProduct(plane->normal, p) - plane->dist;
		}
		num = node->children[d < 0.0f ? 1 : 0];
	}
	return num;
}

int Nano_Hull1Contents(const nano_bsp_t *bsp, const vec3_t p)
{
	return Nano_HullContents(bsp, bsp->hull1_headnode, p);
}

qbool Nano_IsSolid(const nano_bsp_t *bsp, const vec3_t p)
{
	return Nano_Hull1Contents(bsp, p) == NANO_CONTENTS_SOLID;
}

// ---------------------------------------------------------------------------
// hull segment trace (port of SV_RecursiveHullCheck / rtx recursive_hull_check).
// Records fraction/endpos/plane_normal of the first solid impact, or a clear
// (fraction == 1) result. Depth-capped so a malformed tree can't blow the
// stack; the cap (>= any real Quake clipnode depth) resolves conservatively.
// ---------------------------------------------------------------------------

#define NANO_RHC_MAX_DEPTH 256

// Returns true while the segment stays out of solid; false once an impact is
// recorded (matches rtx's convention -- the caller stops on first false).
static qbool nano_recursive_hull_check(
	const nano_bsp_t *bsp, int num, float p1f, float p2f, const vec3_t p1, const vec3_t p2,
	int depth, nano_hulltrace_t *trace)
{
	nano_clipnode_t *node;
	nano_plane_t *plane;
	float t1, t2, frac, midf;
	vec3_t mid;
	int side;
	int k;

	if (depth > NANO_RHC_MAX_DEPTH)
	{
		trace->start_solid = true;
		return true;
	}

	// Leaf: a negative num is a CONTENTS_* value, not a node index.
	if (num < 0)
	{
		if (num != NANO_CONTENTS_SOLID)
		{
			trace->all_solid = false;
		}
		else
		{
			trace->start_solid = true;
		}
		return true;
	}
	if (num >= bsp->num_clipnodes)
	{
		trace->start_solid = true;
		return true;
	}
	node = &bsp->clipnodes[num];
	if (node->plane < 0 || node->plane >= bsp->num_planes)
	{
		trace->start_solid = true;
		return true;
	}
	plane = &bsp->planes[node->plane];
	if (plane->kind >= 0 && plane->kind < 3)
	{
		k = plane->kind;
		t1 = p1[k] - plane->dist;
		t2 = p2[k] - plane->dist;
	}
	else
	{
		t1 = DotProduct(plane->normal, p1) - plane->dist;
		t2 = DotProduct(plane->normal, p2) - plane->dist;
	}

	// Both points on the front side -> descend front only; both back -> back only.
	if (t1 >= 0.0f && t2 >= 0.0f)
	{
		return nano_recursive_hull_check(
			bsp, node->children[0], p1f, p2f, p1, p2, depth + 1, trace);
	}
	if (t1 < 0.0f && t2 < 0.0f)
	{
		return nano_recursive_hull_check(
			bsp, node->children[1], p1f, p2f, p1, p2, depth + 1, trace);
	}

	// The segment crosses this plane -- split it DIST_EPSILON onto the near side.
	{
		float denom = t1 - t2;
		if (denom == 0.0f)
		{
			frac = 0.0f;	// degenerate; shouldn't happen (t1,t2 straddle 0)
		}
		else if (t1 < 0.0f)
		{
			frac = (t1 + NANO_DIST_EPSILON) / denom;
		}
		else
		{
			frac = (t1 - NANO_DIST_EPSILON) / denom;
		}
		if (frac < 0.0f)
		{
			frac = 0.0f;
		}
		else if (frac > 1.0f)
		{
			frac = 1.0f;
		}
	}
	midf = p1f + (p2f - p1f) * frac;
	nvec_lerp(p1, p2, frac, mid);
	side = (t1 < 0.0f) ? 1 : 0;

	// Walk the near side first.
	if (!nano_recursive_hull_check(
			bsp, node->children[side], p1f, midf, p1, mid, depth + 1, trace))
	{
		return false;
	}

	// If the far side isn't solid at the crossing, keep going into it.
	if (Nano_HullContents(bsp, node->children[side ^ 1], mid) != NANO_CONTENTS_SOLID)
	{
		return nano_recursive_hull_check(
			bsp, node->children[side ^ 1], midf, p2f, mid, p2, depth + 1, trace);
	}

	if (trace->all_solid)
	{
		return false;	// never got out of the solid area
	}

	// Impact: the far side is solid. Record the (segment-facing) plane normal.
	if (side == 0)
	{
		nvec_copy(plane->normal, trace->plane_normal);
	}
	else
	{
		nvec_negate(plane->normal, trace->plane_normal);
	}

	// Back the impact point out of solid if the epsilon split left it just inside.
	while (Nano_Hull1Contents(bsp, mid) == NANO_CONTENTS_SOLID)
	{
		frac -= 0.1f;
		if (frac < 0.0f)
		{
			trace->fraction = midf;
			nvec_copy(mid, trace->endpos);
			return false;
		}
		midf = p1f + (p2f - p1f) * frac;
		nvec_lerp(p1, p2, frac, mid);
	}
	trace->fraction = midf;
	nvec_copy(mid, trace->endpos);
	return false;
}

void Nano_Hull1Trace(const nano_bsp_t *bsp, const vec3_t p1, const vec3_t p2, nano_hulltrace_t *out)
{
	if (!out)
	{
		return;
	}
	out->fraction = 1.0f;
	nvec_copy(p2, out->endpos);
	VectorClear(out->plane_normal);
	out->start_solid = false;
	out->all_solid = true;
	if (!bsp)
	{
		return;
	}
	(void)nano_recursive_hull_check(bsp, bsp->hull1_headnode, 0.0f, 1.0f, p1, p2, 0, out);
}

#endif // NANO_SUPPORT
