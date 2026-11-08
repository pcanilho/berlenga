# Strain tearing for PhysXCloth3D

Research note, 8 Nov 2026, written before the code. `PhysXCloth3D` could already be cut by script
(`tear(from, to)`, `release_pins()`), but nothing tore by itself under load. This adds that.

## How others do it

- **PhysX 2.x cloth** (`NxClothDesc`, flag `NX_CLF_TEARABLE`, `tearFactor`): an edge stretched past
  `tearFactor` times its rest length breaks and the vertex there is split in two; the vertex buffers
  were sized up front for the extra vertices. Unity 3 to 4 exposed it as `InteractiveCloth.tearFactor`
  and UDK as the cloth's tear factor. It drove the tearing drapes and banners of Batman: Arkham Asylum
  (2009) and the PhysX cloth demos. PhysX 3 and later (APEX Clothing, NvCloth) dropped tearing.
- **Vertex splitting** comes from fracture: O'Brien and Hodgins, "Graphical Modeling and Animation of
  Brittle Fracture" (SIGGRAPH 1999), split a node along the plane across the largest tensile stress and
  hand each element to one side. Cloth papers apply the same split to triangle meshes: Metaaphanon,
  Bando, Chen and Nishita, "Simulation of Tearing Cloth with Frayed Edges" (Pacific Graphics 2009).
- **Removing elements** (deleting the triangles or springs that cross a line) is the cheaper option and
  is what `tear(from, to)` does, but under strain it leaves holes a triangle wide, which show on narrow
  cloth like a ribbon or a tape.
- **XPBD**: with compliance, a constraint's residual stretch is its force times its compliance, so a
  stretch threshold is a force threshold for a given stiffness (Macklin, Müller and Chentanez, "XPBD",
  2016; the small-steps form the solver already uses, Macklin et al. 2019).

## What is built

- `tear_factor` (0 = off) on `PhysXCloth3D`. After each step the solver lists the structural edges
  stretched past it, most stretched first, and splits up to `MAX_TEARS_PER_STEP` (4) vertices, at most
  one per edge and none twice in a step, so a rip travels over a few ticks instead of shattering.
- Split: the plane through the vertex, normal along the overloaded edge, decides which incident
  triangles move to the copy (those on the edge always do). A pinned end is split first, keeps its pin
  and gives every triangle on the far side to the free copy: an edge ripping off its tie. Otherwise the
  end with more triangles, so the rip runs into the cloth rather than along its border.
- Each constraint records what joins its ends through the mesh (an edge, a vertex for a bend, an edge
  for a shear or a bend across it). After a split every constraint through the vertex is kept, moved to
  the copy, or dropped when no triangles join its ends any more.
- No triangles are lost, texture coordinates follow the copies (grids now have UVs; source meshes keep
  theirs), and the vertex masses of both halves are recomputed from their triangles.
- The stretch compared with the factor is averaged over 50 ms (`TEAR_SMOOTH`). Measured on a 4 m,
  8 cm strip (28 rows, 3 across): with 12 substeps one Gauss-Seidel pass a substep leaves its edges up
  to 20 % over-long in a 3 m/s breeze as waves run down it, which tore it at once; with 40 substeps the
  peak stretch rises steadily with the wind (1.01 at 3 m/s, 1.04 at 8, 1.06 at 10, 1.09 at 14, 1.10 at
  20). So a thin strip needs its substeps raised before a factor means a load, and the factor is best
  chosen from `get_max_strain()` measured in the wind it should survive.
- Tie a strip at one vertex of each end, not a whole row: a clamped row bends the strip in its own plane
  at the tie and loads its outer edge there.
- `torn(position)` per split, for sound and dust. Project setting `physics/physx_3d/cloth/tearing`
  (default on) switches it off everywhere. CPU solver only: with `SIM_AUTO` a tearing cloth uses it.

## Not done

- The GPU deformable surface cannot change topology; tearing cloth stays on the CPU.
- No fraying or thread pull-out at the torn edge (Metaaphanon 2009 adds that with yarn geometry).
- Wind is still one vector per node, which is fine for flags, tape spans and banners up to a few metres.
