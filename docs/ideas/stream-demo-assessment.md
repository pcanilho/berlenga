# Assessment: the "Stream" web demo (18 Nov)

Link Paulo shared: https://stream.mulualem.com/ . Read from its shipped bundle (no write-up, no source
repository and no licence found).

## What it is

A browser demo of a small creek, about 3 m x 3 m: water enters on one side, runs between boulders over a
pebble bed and leaves on the other. three.js WebGPU renderer with TSL compute; it refuses to start without
WebGPU. Textures and sky are Poly Haven (ganges_river_pebbles, rock_05, rock_08, autumn_field_puresky).

## How it works

- **Shallow-water solver**: Chentanez and Müller 2010 on a staggered grid (512 x 512 desktop, 256 mobile,
  8 substeps of 2 ms). Three compute passes a substep: semi-Lagrangian self-advection of face velocities
  (bilinear), height from upwind face fluxes with the `h_avg - 2 dx / (g dt)` overshoot cap, then gravity,
  a depth-weighted drag term and the `0.5 dx / dt` velocity clamp. Dry-face rule as in the paper.
- **Boundaries**: an inflow strip of fixed depth and speed, an absorbing (sponge) layer on the outflow side.
- **Terrain**: the boulder and bed meshes are drawn top-down into an orthographic height buffer once.
- **Foam** (one texture, decays exponentially): three sources, Froude number above 1 in thin water,
  convergence (negative divergence past a threshold, where water piles into itself), and stagnation (velocity
  pointing at a dry cell within 3 cells: the pillow on a rock's upstream face). Plus a wetness channel that
  dries over about 2 minutes.
- **Surface detail**: advected texture coordinates (Neyret 2003): each cell carries a UV that the simulated
  velocity moves every step, two copies reset half a period apart and cross-faded, so ripple normals follow
  eddies exactly instead of the local-velocity offset of a flow map.
- **Shading**: screen refraction, Beer-Lambert absorption, sky reflection, sparkle; caustics from a 1024
  texture where each surface texel's refracted ray lands on the bed (the area-ratio caustic mesh).
  Post: depth of field, bloom, grade.

## Against the fork

Almost everything is already in the fork, usually further along:
- The same solver is the live window (`water_ripple.glsl`, step 3c), with monotone Catmull-Rom advection
  instead of bilinear, wall friction, wet and dry fronts, and a baked current underneath, over whole
  rivers and terrain (`PhysXLiveWater3D`).
- Caustics: the fork's ray-guided caustics map feeds the path tracer with cascades (steps 2, raster caustics).
- Rock terrain capture: the contact foam map and seabed rays do the same job.
- Wetness: "wet where water has been" (step 4a).

What the demo has that the fork's live window does not:
1. **Convergence foam**: our `flow_foam_source` uses shear and Froude only.
2. **Stagnation foam on upstream rock faces** in the live grid: the baked river has a pillow term, and the sea
   has distance-based contact foam, but live water meeting a rock face makes no white water there.
3. **Advected texture coordinates** for the live window's ripples and foam, in place of two-phase flow
   mapping from the live velocity (Neyret 2003; Yu, Neyret, Bruneton and Holzschuch 2011 for the
   Lagrangian version). Gains most in tight eddies behind rocks, where flow-mapped ripples smear.

## Licence

No licence in the bundle or the page, so no code may be taken. All three ideas above come from published
papers and can be built from those.

## Recommendation

Borrow ideas: add items 1 and 2 to `flow_foam_source` (small, a few lines in one shader, no new
texture) and treat item 3 as a separate, optional step for the live window, with a raster and a path-traced
check at 100% crop. Owner: the Valence thread (water).

## As built (18 Nov, Paulo's pick: foam ideas plus advected UVs)

Both on by default on `PhysXRiver3D` and `PhysXWaterSurface3D`; benches and tests switch them off with the
properties below (off is the old behaviour exactly).

- **`flow_foam_impact`**: two more white-water sources in the live window (`water_ripple.glsl`,
  `impact_foam_source`). Convergence: the speed lost over 25 cm (negative divergence times 0.25 m) between
  0.15 and 0.6 m/s, gated by the faster face's speed (0.5 to 1.5 m/s), so it marks a jet meeting slack water or
  a jump's toe, not slow deepening. Stagnation: the nearest dry cell along each axis within about 0.75 m (at
  most 3 cells) gives the wall's direction; the approach speed is read that far upstream, where the flow has
  not yet turned aside; 0.3 to 1.2 m/s gives the pillow. Joins the existing shear and Froude sources by max,
  so it carries, fades and renders as the other flow foam does. Params grew a fifth vec4 (`flow_extra`).
- **`live_advected_ripples`**: `water_flow_offset.glsl`, one pass and a copy per solver step over the ripple
  grid (RGBA32F, 16 bytes a cell, plus a scratch). It stores, for each of the two flow-map phases, how far the
  advected coordinate has drifted from plain flow mapping, so zero is the old look and the materials just add
  it (river: `live_offset_tex`; sea and lakes: `flow_offset_tex` inside the simulated square, fading over its
  edge as the flow does). Each phase restarts at zero when its own phase wraps, on the materials' clock
  (`WaterSolver::set_flow_clock`, the same `water_time` and phase noise), so the restart is unseen. A uniform
  current gives zero exactly; the correction only shows where the flow turns. Re-centring clears it.
- River bench: the generated granite boulders and `--view=creek` from Valence's shelved draft.

Cost: one extra small compute pass on a 160 to 192 square grid and one texture read per pixel of live water;
the foam terms add about a dozen neighbour reads per cell to a pass that already runs. Not timed on a GPU
yet (PC queue).
