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
