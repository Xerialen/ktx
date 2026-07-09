/*
 nano_nav_build.c -- navmesh build lifecycle (port of rtx src/nav_build.rs).

 The KTX integration of the (pure) nano BSP reader + navmesh builder: on first
 need it reads maps/<mapname>.bsp via the engine filesystem, parses the player
 clip hull, builds the land mesh, and caches it for the map. Built once per map,
 lazily; freed on map change (Nano_NavMapReset from worldspawn).

 rtx builds on a worker thread to avoid a frame hitch. KTX game logic is single-
 threaded and a map's build runs once at warmup (where a hitch is harmless) and
 is ~150 ms on dm3, so the simpler synchronous build is used here.

 S1c scope: the land mesh + lifecycle only. The entity-derived splices (plats,
 teleports, gates), the lava/slime hazard surcharge (needs trap_PointContents),
 and the item-goal catalog (collect_goals, for the S2 brain) are added later --
 dm3 is navigable without them.
*/
#ifdef NANO_SUPPORT

#include "g_local.h"
#include "nano.h"

// Per-map cached navmesh state (module-static; reset by Nano_NavMapReset).
static nano_bsp_t *g_nano_bsp = NULL;		// kept for later hazard-surcharge
static nano_navgraph_t *g_nano_nav = NULL;
static qbool g_nano_attempted = false;

void Nano_NavMapReset(void)
{
	if (g_nano_nav)
	{
		Nano_NavFree(g_nano_nav);
		g_nano_nav = NULL;
	}
	if (g_nano_bsp)
	{
		Nano_BspFree(g_nano_bsp);
		g_nano_bsp = NULL;
	}
	g_nano_attempted = false;
}

const nano_navgraph_t *Nano_NavEnsure(void)
{
	char path[80];
	fileHandle_t fh;
	int len, nread;
	byte *buf;

	if (g_nano_nav)
	{
		return g_nano_nav;
	}
	if (g_nano_attempted)
	{
		return NULL;	// a prior read/parse/build failed; don't retry this map
	}
	g_nano_attempted = true;

	// Slurp maps/<mapname>.bsp through the engine filesystem (resolves gamedir/
	// paks, like rtx's read_file). mvdsv gate reads maps/dm3.bsp here.
	snprintf(path, sizeof(path), "maps/%s.bsp", mapname);
	len = (int)trap_FS_OpenFile(path, &fh, FS_READ_BIN);
	if (len <= 0)
	{
		G_cprint("[nano] navmesh: could not open %s\n", path);
		return NULL;
	}
	buf = (byte *)malloc((size_t)len);
	if (!buf)
	{
		trap_FS_CloseFile(fh);
		G_cprint("[nano] navmesh: alloc failed (%d bytes)\n", len);
		return NULL;
	}
	nread = (int)trap_FS_ReadFile((char *)buf, (intptr_t)len, fh);
	trap_FS_CloseFile(fh);
	if (nread < len)
	{
		free(buf);
		G_cprint("[nano] navmesh: short read %s (%d/%d)\n", path, nread, len);
		return NULL;
	}

	g_nano_bsp = Nano_BspParse(buf, len);
	free(buf);
	if (!g_nano_bsp)
	{
		G_cprint("[nano] navmesh: BSP parse failed (%s)\n", path);
		return NULL;
	}

	g_nano_nav = Nano_NavBuild(g_nano_bsp);
	if (!g_nano_nav)
	{
		Nano_BspFree(g_nano_bsp);
		g_nano_bsp = NULL;
		G_cprint("[nano] navmesh: build failed (%s)\n", path);
		return NULL;
	}

	G_cprint("[nano] navmesh: %s -> %d planes, %d clipnodes -> %d cells, %d links\n", mapname,
		g_nano_bsp->num_planes, g_nano_bsp->num_clipnodes, Nano_NavNumCells(g_nano_nav),
		Nano_NavNumLinks(g_nano_nav));
	return g_nano_nav;
}

const nano_navgraph_t *Nano_NavGraph(void)
{
	return g_nano_nav;
}

#endif // NANO_SUPPORT
