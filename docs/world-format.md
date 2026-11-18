# LithTech 1.0 file formats (as used by Shogo)

Worked out from the game's own data files: field sizes were confirmed by parsing
all 61 levels end to end, and geometry by checking that every polygon's
vertices lie on its surface's plane (all but 49 of 354,824 do; the rest are
warped by a few units in the source data). No engine source was used.

All values are little-endian. `vec3` = 3 × float32. `str16` = uint16 length + bytes.

## World (.DAT, version 56)

```
u32 version (56)
u32 objectDataPos
u32 renderDataPos
u32 infoLen, char info[infoLen]     "MasterPalette ...;WorldName \"...\";AmbientLight r g b"
```

### Objects (at objectDataPos)

```
u32 count
per object:
  u16 byteLength                    (of the rest of this object)
  str16 className                   e.g. "GameStartPoint", "Door", "SkyPointer"
  u32 propCount
  per property:
    str16 name
    u8  type                        PT_* from serverobj_de.h
    u32 flags
    u16 dataLength
    data: PT_STRING   str16
          PT_VECTOR / PT_COLOR      vec3
          PT_REAL / PT_LONGINT      float32 (long ints are stored as floats)
          PT_FLAGS                  u32
          PT_BOOL                   u8
          PT_ROTATION               4 × float32: euler pitch, yaw, roll (radians), then 1.0
```

### Geometry (at renderDataPos)

The main BSP, then at its `nextPos`: `u32 worldModelCount` and that many more BSP
blocks (doors, sky boxes, glass, props...), each tied to a level object by name.

```
BSP block:
  u32 nextPos                       absolute offset just past this block
  32 bytes reserved (zero)
  u32 flags
  str16 name                        "" for the main world
  u32 unknown (an offset)
  u32 nPoints, nPlanes, nSurfaces, nUserPortals, nPolies, nLeafs, nVerts,
      visListSize, nLeafLists, nNodes, unknown
  vec3 boxMin, boxMax, translation
  u32 namesLength, u32 nTextures, char names[namesLength]   NUL-separated texture paths
  per poly:  u8 numVerts, u8 extraVerts
  per leaf:  u16 nLists (0xFFFF: then u16 listIndex)
             else per list: u16 portal, u16 size, u8 data[size]
             u16 nPolyRefs, u32 polyRef[nPolyRefs], u32 unknown
  per plane: vec3 normal, float dist
  per surface:
             vec3 O, P, Q           texture mapping: u = dot(p - O, P), v = dot(p - O, Q), in texels
             vec3 lmP, lmQ          lightmap axes (P and Q normalized)
             vec3                   not identified (an RGB-like triple)
             u16 texture, u32 plane, u32 flags, u32 unknown
             u8 hasEffect; if set: str16 effect, str16 params   e.g. "pan" "8 8"
             u8, u8                 not identified
  per poly:  u16, u16               not identified
             8 bytes                runtime pointers (0xCD fill)
             u32 surface
             (numVerts + extraVerts) × { u16 point, u8 r, g, b }   baked vertex light;
                                    the first numVerts entries are the polygon
  per node:  u32 plane, u32 poly, u16 leaf, i32 child[2] (-1 in / -2 out),
             vec3 centre + float radius
  per user portal: str16 name, u32 flags, u16 index, vec3 centre, vec3 dims
  per point: vec3
  u32 nx, ny, nz; vec3 cellSize, gridMin   a grid over the block
  per cell (nx·ny·nz): u16 n, u16 root, n × { u16 poly, i16 child[2] }   a polygon tree (-1/-2 empty)
  u32 (0), u32 unknown count
  per poly:  vec3 lmOrigin          lightmap origin, on the polygon's plane
  u32 size                          bytes of lightmap data, which ends at nextPos
  per poly whose surface has flag 0x80, in polygon order:
             u8 w, u8 h, w·h × u16 RGB565   row-major
```

### Lightmaps

Sample (i, j) of a polygon's lightmap lies at lmOrigin + 20·i·lmP + 20·j·lmQ;
the origin is snapped to a 20-unit world grid along the axes, and w, h are
⌈max/20⌉ + 1 for the polygon's extent along each axis (off by one where an edge
lands on a grid line). The samples are the complete baked light, ambient
included (02_Quarters, `AmbientLight 60 60 70`: the darkest samples are about
57 60 65), so they replace the vertex light; drawn with bilinear filtering.
All 3632 blocks of the 61 levels decode exactly to their end.

### BSP nodes

The root is the one node no other node points to; usually node 0, but not
always (08_Airshafts: 7344). To find which side of a node a point is on, take
d = dot(normal, p) - dist with the node's plane: child[0] for d < 0, child[1]
otherwise. Walking down ends at -1 (open space: `GetPointStatus` says
DE_INSIDE) or -2 (solid, or outside the level: DE_OUTSIDE). Every level's
start point is inside. Three multiplayer maps (MCA_Avernus, MCA_MadSkills,
OF_Midnight) were built leaky: the space around their hull is "in" too.

### Surface types

The game asks `GetPolyTextureFlags` for a polygon and reads the answer as its
`SurfaceType` (Shared/SurfaceTypes.h: 20 metal, 30 wood, 40 glass, 80 cloth,
110 sky...). The value is the DTX header's second flags word (offset 20) of the
polygon's texture; for an animated `.spr` texture, of its first frame.

### Surface flags (observed)

| Bit | Meaning (from the textures that use it) |
|---|---|
| 0x1 | solid |
| 0x4 | invisible (`Textures\Invisible.dtx`, hull brushes) |
| 0x8 | translucent: drawn at half alpha with black texels see-through (glass has no black; barbed wire, girders, signs and sky moons are mostly black) |
| 0x10 | sky portal: the sky box shows through |
| 0x20 | fullbright (signs, lights, environment maps) |
| 0x80 | lightmapped |
| 0x8000 | takes the panning sky shadow (only in the levels whose `WorldProperties` turn `PanSky` on) |

### Sky

`SkyPointer` objects (implemented in the SDK's `engineobjects.c`) name a world
model to draw as the sky. The one with non-zero `SkyDims` defines the sky box:
the viewer's position within the main world's bounding box is mapped into the
small `InnerPercent` box around the pointer, giving a little parallax.

## Textures (.DTX, version -2)

```
u32 0, i32 -2
u16 width, u16 height, u16 mipCount, u16 0
u32 flags, u32 surfaceFlags, then zero padding to offset 44
256 × { u8 flag, u8 r, u8 g, u8 b }       palette
mip levels: width × height bytes, halving each level
alpha mask (optional): 4 bits per texel for every mip level, low nibble first
```

The alpha mask is there when the bytes after the mips are exactly half a byte
per texel of all levels (791 of the 4921 textures: sprite frames, particle
and effect textures). Effect textures often fill the whole square with color
(an impact flame is flat orange) and take their shape only from the mask. The
engine uses it for sprites, particles and other blended polygons; world
surfaces ignore it.

## Sprites (.SPR)

```
u32 frameCount, u32 framesPerSecond
u32 transparent      black texels are see-through
u32 translucent      drawn blended (the engine adds them)
u32 colorKey         always 0 in Shogo's files
frameCount × str16   DTX path per frame
```

Two shipped sprites are broken (`Signs\Pachinko.spr` is empty,
`BullgutMuzzleFlash1.spr` has no frames) and 61 frames name textures that
are not in the game's files.
