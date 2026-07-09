/*
 test_navmesh.c -- standalone unit test for the nano navmesh (S1b/S2).

 Mirrors rtx's RTX_TEST_BSP approach: parse a real .bsp, build the land navmesh,
 and sanity-check the result -- cell/link counts, a link-kind histogram, sample
 A* paths, the S2 Dijkstra flood vs A* costs, nearest-reachable queries, and a
 land-spawn reachable-cell smoke check for S2a.

 Compile (from repo root):
   gcc -O2 -DNANO_SUPPORT -I test/shim -I include -I src \
       test/test_navmesh.c src/nano_bsp.c src/nano_navmesh.c \
       -lm -o test/test_navmesh
 Run:
   ./test/test_navmesh /path/to/qw/maps/dm3.bsp
*/
#define NANO_SUPPORT 1
#include "g_local.h"	// shim
#include "nano.h"

#include <stdio.h>
#include <time.h>

static unsigned long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)ts.tv_nsec / 1000000UL;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "dm3.bsp";
	FILE *f;
	long len;
	byte *buf;
	unsigned long t0, t1, t2;
	nano_bsp_t *bsp;
	nano_navgraph_t *g;

	f = fopen(path, "rb");
	if (!f)
	{
		fprintf(stderr, "cannot open %s\n", path);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0)
	{
		fprintf(stderr, "empty %s\n", path);
		return 1;
	}
	buf = (byte *)malloc((size_t)len);
	if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len)
	{
		fprintf(stderr, "short read\n");
		return 1;
	}
	fclose(f);

	t0 = now_ms();
	bsp = Nano_BspParse(buf, (int)len);
	t1 = now_ms();
	if (!bsp)
	{
		fprintf(stderr, "BSP parse FAILED\n");
		return 1;
	}
	printf("BSP parsed: %d planes, %d clipnodes, hull1_headnode=%d  (%lu ms)\n", bsp->num_planes,
		bsp->num_clipnodes, bsp->hull1_headnode, t1 - t0);

	g = Nano_NavBuild(bsp);
	t2 = now_ms();
	if (!g)
	{
		fprintf(stderr, "navmesh build FAILED\n");
		return 1;
	}
	printf("navmesh built: %d cells, %d links  (%lu ms)\n", Nano_NavNumCells(g),
		Nano_NavNumLinks(g), t2 - t1);

	// link-kind histogram
	{
		int hist[10] = {0};
		int i;
		const char *names[10] = {"walk", "step", "drop", "jump", "djmp", "sjmp", "plat", "tele", "hook",
									"rjmp"};
		for (i = 0; i < Nano_NavNumLinks(g); i++)
		{
			int k = Nano_NavLinkKind(g, i);
			if (k >= 0 && k < 10)
			{
				hist[k]++;
			}
		}
		printf("links by kind:");
		for (i = 0; i < 10; i++)
		{
			if (hist[i])
			{
				printf(" %s=%d", names[i], hist[i]);
			}
		}
		printf("\n");
	}

	// reachability probe: a sample A* path between two far-apart cells. This is
	// the brain's actual query, so it's the right thing to exercise.
	{
		int n = Nano_NavNumCells(g);
		int route[512];
		int rl = Nano_NavFindPath(g, 0, n - 1, route, 512);
		printf("path cell0 -> cell%d: %s (len=%d)\n", n - 1, rl < 0 ? "NO ROUTE" : "ok", rl);

		// and a mid-mesh hop to confirm A* returns short routes too
		if (n > 200)
		{
			int rl2 = Nano_NavFindPath(g, n / 3, n / 3 + 50, route, 512);
			printf("path cell%d -> cell%d: %s (len=%d)\n", n / 3, n / 3 + 50,
				rl2 < 0 ? "NO ROUTE" : "ok", rl2);
		}
	}

	// nearest-cell lookups for a few world points
	{
		vec3_t p;
		int c;
		VectorSet(p, 0.0f, 0.0f, 0.0f);
		c = Nano_NavNearest(g, p);
		printf("nearest (0,0,0) -> cell %d\n", c);
		VectorSet(p, 512.0f, 512.0f, 0.0f);
		c = Nano_NavNearest(g, p);
		printf("nearest (512,512,0) -> cell %d\n", c);
	}

	// S2 query API: cost flood + nearest-reachable
	{
		int n = Nano_NavNumCells(g);
		float *costs = (float *)malloc((size_t)n * sizeof(float));
		int i, errors = 0;
		if (!costs)
		{
			fprintf(stderr, "malloc costs FAILED\n");
			return 1;
		}
		if (!Nano_NavCostsFrom(g, 0, costs, n))
		{
			fprintf(stderr, "Nano_NavCostsFrom FAILED\n");
			free(costs);
			return 1;
		}
		printf("cost flood from cell0 computed\n");

		// compare flood cost to A* cost on a sample of cells
		for (i = 1; i < n; i += n / 8 + 1)
		{
			int route[512];
			int rl = Nano_NavFindPath(g, 0, i, route, 512);
			float a_cost = (rl < 0) ? NANO_NAV_UNREACHABLE : 0.0f;
			int j;
			if (rl >= 0)
			{
				for (j = 0; j < rl; j++)
				{
					a_cost += Nano_NavLinkCost(g, route[j]);
				}
			}
			if (fabsf(costs[i] - a_cost) > 0.01f &&
				!(costs[i] >= NANO_NAV_UNREACHABLE && a_cost >= NANO_NAV_UNREACHABLE))
			{
				printf("COST MISMATCH cell0-%d: flood=%.3f a*=%.3f\n", i, costs[i], a_cost);
				errors++;
			}
		}
		if (errors == 0)
		{
			printf("cost flood matches A* on all sampled cells\n");
		}
		else
		{
			fprintf(stderr, "cost flood had %d mismatch(es)\n", errors);
			free(costs);
			return 1;
		}

		// nearest-reachable: a point in the world should map to a reachable cell
		{
			vec3_t p;
			int c;
			VectorSet(p, 512.0f, 512.0f, 0.0f);
			c = Nano_NavNearestReachable(g, p, costs, n);
			printf("nearest-reachable (512,512,0) -> cell %d (cost %.3f)\n",
				   c, c >= 0 ? costs[c] : -1.0f);
			if (c < 0 || costs[c] >= NANO_NAV_UNREACHABLE)
			{
				fprintf(stderr, "nearest-reachable FAILED for a known reachable point\n");
				free(costs);
				return 1;
			}
		}

		// nearest-reachable fallback: a point far outside the mesh should still
		// return the nearest reachable cell (full-scan fallback), not -1.
		{
			vec3_t p;
			int c;
			VectorSet(p, 10000.0f, 10000.0f, 0.0f);
			c = Nano_NavNearestReachable(g, p, costs, n);
			printf("nearest-reachable fallback (10000,10000,0) -> cell %d (cost %.3f)\n",
				   c, c >= 0 ? costs[c] : -1.0f);
			if (c < 0 || costs[c] >= NANO_NAV_UNREACHABLE)
			{
				fprintf(stderr, "nearest-reachable fallback FAILED\n");
				free(costs);
				return 1;
			}
		}

		// S2a smoke: a real land cell origin has a nearest cell with a broad
		// reachable cost flood.
		{
			const float *p = Nano_NavCellOrigin(g, 0);
			int c, reachable = 0;
			vec3_t pv;
			if (!p)
			{
				fprintf(stderr, "S2a smoke FAILED: cell 0 has no origin\n");
				free(costs);
				return 1;
			}
			VectorCopy(p, pv);
			c = Nano_NavNearest(g, pv);
			if (c < 0)
			{
				fprintf(stderr, "S2a smoke FAILED: no nearest cell for cell0 origin\n");
				free(costs);
				return 1;
			}
			if (!Nano_NavCostsFrom(g, c, costs, n))
			{
				fprintf(stderr, "S2a smoke FAILED: cost flood from nearest cell\n");
				free(costs);
				return 1;
			}
			for (i = 0; i < n; i++)
			{
				if (costs[i] < NANO_NAV_UNREACHABLE)
				{
					reachable++;
				}
			}
			printf("S2a smoke: nearest cell0 origin = cell %d, reachable cells = %d/%d\n",
				   c, reachable, n);
			if (reachable < n / 4)
			{
				fprintf(stderr, "S2a smoke FAILED: too few reachable cells\n");
				free(costs);
				return 1;
			}
		}

		free(costs);
	}

	Nano_NavFree(g);
	Nano_BspFree(bsp);
	free(buf);
	return 0;
}
