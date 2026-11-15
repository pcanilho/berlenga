# Dark bark furrows under path tracing (shadow terminator)

## Problem (15 Nov)

Bark furrows read darker under path tracing than in raster when the sun grazes the trunk.
Bench `docs/bench/trees --preset=0 --seed=1 --stand=forest --view=trunk --sun=30,-60`.

- With the sun from the front, raster and PT match. With sky light only, the means match.
- With the grazing sun, PT showed whole rectangles of black on the lit flank, cut along the mesh's
  rings and columns. Turning off the bark relief geometry (`bark_relief:0`) and the moss shell
  (`moss_cover:0`) did not remove them.
- The blocks follow triangles, not texels, so the normal map is not the cause.

## Two known terminator problems

1. **Smooth-shading (shadow) terminator.** The interpolated vertex normal faces the light while the
   flat triangle under it already faces away. The shadow ray starts on the flat triangle, below the
   surface the normals describe, and hits the neighbouring triangles. Whole triangles go black.
   Raster hides it with shadow-map bias. Fixes:
   - Hanika, "Hacking the Shadow Terminator", Ray Tracing Gems II (2021), ch. 4. Moves the shadow
     ray origin onto the curved surface the vertex normals imply. Used by Cycles (per-object
     "shadow terminator geometry offset") and other production renderers.
   - Chiang, Li and Burley, "Taming the Shadow Terminator" (SIGGRAPH 2019 talk) treats the
     shading-normal side instead.
2. **Bump terminator.** A normal map tilts the shading normal towards a light past the geometric
   terminator. Fixes:
   - Conty Estevez, Lecocq and Stein, "A Microfacet-Based Shadowing Function to Solve the Bump
     Terminator Problem", Ray Tracing Gems (2019), ch. 12: scale the light by
     `G = min(1, Ng.L / (Ns.L * Ng.Ns))`, smoothed as `-G^3 + G^2 + G`.
   - Schussler et al., "Microfacet-based Normal Mapping for Robust Monte Carlo Path Tracing"
     (SIGGRAPH Asia 2017). Exact, but more BSDF work and a changed look on every normal map.

## What was tried

- Conty Estevez term on positional and directional lights. Lit-flank mean fell from 31 to 21
  (raster 33) and the blocks stayed. It darkens light the normal map already shades, which loses
  detail compared with raster. Dropped.
- Hanika lift. Lit-flank mean 33.4 against raster 33.2 (was 31.6), the black triangle blocks on
  the lit flank are gone. Kept.

## As built

`rt_terminator_shadow_point()` in `raytracing_closest_hit_common_inc.glsl`. For each vertex of the
hit triangle, where the hit lies below that vertex's tangent plane, it is projected onto the plane;
the three lifts are blended by the barycentrics (Hanika's formula). Points above every tangent
plane do not move, so flat and convex-faceted surfaces lit head-on are unchanged. The lift is
capped at a quarter of the triangle's longest edge so a broken vertex normal cannot throw the
origin away from the surface. Only the front-side shadow origin moves; the back origin (lights
behind a translucent surface) is as before. Procedural and swept-sphere hits skip it.

Cost: three vertex position and normal fetches per shaded hit. No extra rays.

## What remains

On the flank facing away from the sun, PT is still darker than raster (mean 15 against 19, sky
only 12 against 12). There the triangles themselves face away from the sun. Raster still lights
normal-mapped ridges there, because its N.L uses the normal map and the shadow map's normal bias
lets the light through. PT shadows them, which is what the real trunk would do. Matching raster
there would mean lighting surfaces the sun cannot reach, so it is left as is.
