import struct, sys, collections
BSP = "/home/xerial/kbot/serverdir/qw/maps/dm3.bsp"
OUT = open("/mnt/c/Users/benya/kbot-e6/bsp_floor.txt", "w")
def P(*a): __builtins__.print(*a, file=OUT)
d = open(BSP, "rb").read()
ver = struct.unpack_from("<i", d, 0)[0]
P("bsp version", ver)
# 15 lumps, each (offset,length) int
lumps = [struct.unpack_from("<ii", d, 4+8*i) for i in range(15)]
def lump(i): o,l = lumps[i]; return d[o:o+l]
verts_b = lump(3); nverts = len(verts_b)//12
verts = [struct.unpack_from("<fff", verts_b, 12*i) for i in range(nverts)]
planes_b = lump(1); nplanes = len(planes_b)//20
planes = [struct.unpack_from("<ffff i", planes_b, 20*i) for i in range(nplanes)]
edges_b = lump(12); nedges = len(edges_b)//4
edges = [struct.unpack_from("<HH", edges_b, 4*i) for i in range(nedges)]
surf_b = lump(13); nsurf = len(surf_b)//4
surfedges = [struct.unpack_from("<i", surf_b, 4*i)[0] for i in range(nsurf)]
faces_b = lump(7); nfaces = len(faces_b)//20
P("verts",nverts,"planes",nplanes,"edges",nedges,"faces",nfaces)
floors = []  # (z, xmin,xmax,ymin,ymax, area)
for i in range(nfaces):
    plane_id, side, firstedge, numedges, texinfo = struct.unpack_from("<hh i h h", faces_b, 20*i)
    nx,ny,nz,dist = planes[plane_id][:4]
    if side: nx,ny,nz = -nx,-ny,-nz
    if nz < 0.85:  # only near-horizontal upward floors
        continue
    poly = []
    for k in range(numedges):
        se = surfedges[firstedge+k]
        if se >= 0: v = edges[se][0]
        else: v = edges[-se][1]
        poly.append(verts[v])
    xs=[p[0] for p in poly]; ys=[p[1] for p in poly]; zs=[p[2] for p in poly]
    zc = sum(zs)/len(zs)
    floors.append((round(zc), round(min(xs)), round(max(xs)), round(min(ys)), round(max(ys))))
# Region of interest for ring/quad/ra/ya lanes (game coords)
def show(name, xlo,xhi,ylo,yhi,zlo,zhi):
    P(f"\n=== {name}: floors in x[{xlo},{xhi}] y[{ylo},{yhi}] z[{zlo},{zhi}] ===")
    sel = [f for f in floors if xlo<=f[1] and f[2]<=xhi and ylo<=f[3] and f[4]<=yhi and zlo<=f[0]<=zhi]
    # merge/report sorted by x
    for f in sorted(set(sel)):
        P(f"   z={f[0]:5d}  x[{f[1]:5d},{f[2]:5d}]  y[{f[3]:5d},{f[4]:5d}]")
# ring/quad band (z~56), wide x
show("ring-quad band z40-70 y[-500,400]", 100, 1100, -500, 400, 40, 70)
show("ra-ya band z40-100 y[-1000,-350]", -450, 1300, -1000, -350, 30, 100)
OUT.close(); print("done")
