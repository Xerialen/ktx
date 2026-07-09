/*
 g_local.h -- MINIMAL SHIM for the standalone nano navmesh unit test ONLY.

 This is NOT the real KTX g_local.h. It provides just the types/macros that
 nano_bsp.c and nano_navmesh.c pull in from it, so the (engine-free) navmesh
 build can be compiled and exercised outside the server. The real navmesh code
 calls no trap_* / gedict_t * APIs -- it is pure over a byte buffer -- so the
 shim needs only the type aliases and vector macros.

 Used by: test/test_navmesh.c  (+ the two nano .c files it links).
 Never compiled into qwprogs.so.
*/
#ifndef KTX_SHIM_G_LOCAL_H
#define KTX_SHIM_G_LOCAL_H

#include <string.h>
#include <stdlib.h>
#include <math.h>

typedef unsigned char byte;
typedef float vec_t;
typedef vec_t vec3_t[3];

typedef int qbool;
#ifndef true
#define true 1
#endif
#ifndef false
#define false 0
#endif

#define DotProduct(x, y)        ((x)[0] * (y)[0] + (x)[1] * (y)[1] + (x)[2] * (y)[2])
#define VectorSubtract(a, b, c) \
	{ \
		(c)[0] = (a)[0] - (b)[0]; \
		(c)[1] = (a)[1] - (b)[1]; \
		(c)[2] = (a)[2] - (b)[2]; \
	}
#define VectorAdd(a, b, c)      \
	{ \
		(c)[0] = (a)[0] + (b)[0]; \
		(c)[1] = (a)[1] + (b)[1]; \
		(c)[2] = (a)[2] + (b)[2]; \
	}
#define VectorCopy(a, b) \
	{ \
		(b)[0] = (a)[0]; \
		(b)[1] = (a)[1]; \
		(b)[2] = (a)[2]; \
	}
#define VectorClear(a) ((a)[0] = (a)[1] = (a)[2] = 0)
#define VectorSet(v, x, y, z) ((v)[0] = (x), (v)[1] = (y), (v)[2] = (z))

// gedict_t is referenced only by nano.h *declarations* the navmesh test never
// calls (Nano_MarkBot / Nano_Frame). A forward declaration satisfies them.
struct gedict_s;
typedef struct gedict_s gedict_t;

#endif // KTX_SHIM_G_LOCAL_H
