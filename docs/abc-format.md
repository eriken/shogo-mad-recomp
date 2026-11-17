# .ABC model format (LithTech 1.0, as used by Shogo)

Worked out from the game's own model files (`MODELS/**/*.ABC` in `SHOGO.REZ`).
All 550 models parse with this layout. All values are little endian:
`str16` is a u16 length followed by that many bytes (no terminator), and
`vec3` is three f32.

`engine/model.cpp` reads all of it except the level-of-detail data; the
engine draws the full-detail mesh. `shogo-modelview` renders any model to a
PNG for checking.

## Sections

The file is a chain of sections:

```
str16 name
u32   absolute file offset of the next section (0xFFFFFFFF = this is the last)
...   section data, up to the next section's offset
```

Shogo's models have `Header`, `Geometry`, `Nodes`, `Animation` and `AnimDims`
in that order. Look sections up by name, not by position.

## Header

```
str16 version                 "MonolithExport Model File v6"
str16 command string          usually empty; "DrawIndexedDist 3000", or weapon
                              timings ("0.5 1.0 3.0 5.0") the game parses
                              (GetModelCommandString)
```

## Geometry

```
24 bytes  bounding box
u32       n
u16 × (n + 1)                 level-of-detail table (not used)
u32       nTris
per triangle (33 bytes):
  f32 × 6   u, v for each corner, 0..1 across the skin
  u16 × 3   vertex indices
  3 bytes   packed normal
u32       nVerts              all vertices, including lower-detail ones
u32       nUsed               the full-detail mesh uses vertices 0 .. nUsed-1
per vertex (20 bytes):
  vec3      position, in its node's space
  3 bytes   normal, signed, /127
  u8        node (the node's index field)
  u32       level-of-detail link (not used)
```

For nodes with vertex animation (below), their vertices' positions here are
placeholders (0..255 values); the animation supplies them.

## Nodes

The node tree is stored depth first, starting with the root:

```
24 bytes  bounding box
str16     name
u16       index
u8        flags
u32       nMDVerts            vertices this node moves
u16       vertex index × nMDVerts
u32       nChildren           followed by the children, recursively
```

`nMDVerts` is needed to step over the animation data.

## Animation

```
u32 count
per animation:
  str16 name                  e.g. STATIC_MODEL, RR (run), IR1 (idle)
  u32   length                ms
  6×f32 bounding box
  u32   nKeyframes
  per keyframe:
    u32   time                ms
    6×f32 bounding box
    str16 string              usually empty; otherwise a command for
                              MID_MODELSTRINGKEY / OnModelKey ("fire", footsteps...)
  per node (tree order), the node's track:
    per keyframe:
      vec3   position          relative to the parent node
      f32×4  rotation          quaternion x y z w, relative to the parent,
                               stored inverted (conjugate it)
      nMDVerts × 3 bytes       vertex animation: position = byte × scale + translate
    vec3 scale, vec3 translate for the vertex animation
```

Six models use vertex animation (the cat's tail, CothiNeal, the squeaky toy,
three explosions). Node lists may name a vertex more than once.

## AnimDims

One `vec3` per animation, in animation order: the animation's "user dims", the
half-extents the game uses for the object's collision box (sanjuro's
`STATIC_MODEL`: 20, 41, 20). `GetModelAnimUserDims` returns these, and the
engine sets them as an object's dims when it is created or changes animation,
unless the object has `CF_DONTSETDIMS`.

## Space

Posed as above (node transforms composed root first, vertices placed by their
node), the data is mirrored relative to the world: characters face -z and the
triangles wind the other way from the world's polygons (3ds Max is
right-handed). Mirroring z gives the engine's space: models face +z, and the
first-person weapons point away from the camera.
