# Steep ranges, snow caps and falls off a face: audit and options (26 Nov 2026)

Question: can the fork make a basin ringed by steep mountain walls with snow on the tops, and a waterfall that leaves
a mountain face part way up and falls free? Answer from the code on main (3157985f), then the gaps with the
established technique for each. Nothing here is built yet.

## Works today

**Steep walls.** `TerrainGenerator3D` (modules/terrain_deform/terrain_generator_3d.h) makes relief from uplift and
erosion, so walls are where geology puts them:

- Where: `TerrainGuide3D` raise/lower/ridge/valley curves and `uplift_map` paint the ring and the basin; touch-up
  `TerrainBrush3D` after erosion, water re-solved (terrain-generation.md "Sculpting").
- How steep: hillslopes fail at `critical_slope` (40 degrees, settable to 80). Hard rock bands (`strata_enabled`, on)
  stand at `strata_cliff_angle` (75 degrees) and keep falls as vertical steps. Glacial stage (`glacial_enabled`, on;
  Herman et al. 2015 erosion on shallow-ice flow) wears U troughs with steep walls, cirques, riegels and hanging
  valleys; troughs fill flat after the ice (`outwash_age`), which is the flat floor a city sits on.
- Drawn: Landscape3D `cliff_relief` refines and displaces faces past ~50 degrees into beds and joint blocks, with the
  same relief per pixel in TerrainMaterial3D; `cliff_ledge_spacing` steps faces; RockField3D cliff kit pillars stand
  on faces from 50 degrees and the terrain recesses under them (`cliff_recess`, Far Cry 5 method, Carrier GDC 2018);
  ledge slabs, falling ledges, scree below. Raster and path traced match (the BLAS holds the displaced faces).

**Snow caps.** modules/snow: `SnowClimate` + `SnowCover3D.bake_climate()` run a WGEN season (Richardson 1981) over
the whole terrain: lapse-rate snow line, orographic precipitation (6%/100 m), slope-aspect melt, SnowSlide shedding
off steep ground (Bernhardt and Schulz 2010; a 60 degree face keeps a thin load), Winstral drift, cornices on
crests. The world tier carries it live and far materials read it (`snow_world_map`, rt_params 50 to 54), so a ring
of peaks shows its snow line from the city in raster and path traced. Thin snow on steep rock lies in patches
(follow-up 56). Avalanches, wind slab, icicles on eaves all exist.

**A fall off a face.** The generator emits falls where rivers cross hard bands or drop off a hanging valley's lip
(`get_waterfalls(min_area, min_height)`, fall lips, notches, plunge pools, gorges). PhysXWaterfalls3D
(modules/godot_physx/water/physx_waterfalls_3d.h) draws contact falls (sheet on the rock, self-aeration, Chanson
1993) and free falls (jet break-up after L_b = 6 q^0.32, Horeni 1956, then a column of clumps), with spray, mist that
wets the rock, the roar, wind drift, and the WorldWater contract. The landscape bench `--hanging` builds a lake in a
side bowl over a glacial trough that drops 316 m free off the lip, on three guides. Flow is per month
(`river_month`), with rain and snowmelt feeding the rivers.

## Partial

- A fall needs a river: `get_waterfalls` defaults to 1e5 m2 of catchment, so the thin ribbon falls off small high
  snowfields (Lauterbrunnen, Yosemite's ephemeral falls) only appear if the caller lowers `min_area`; not tested low.
- Karst springs (WorldCaves resurgences) get their own rivers but sit low on valley sides at the water table, and
  falls are indexed on the river list without karst: a spring never starts a fall.
- Snow caps in summer: the bake runs `start_day`/`days` (1 October for 182 days); a game day in August needs the
  bake run on to it, and nothing carries snow over from year to year (no firn), so high caps can thin or vanish in a
  warm summer where real ones stay.
- `glacial_ice` from the last glaciation is used for erosion and ice-worn rock only; no ice is drawn today.
- Scale: benches run 4 km at 16 m. A ring around a city wants a larger run; cost at that size is unmeasured.

## Gaps and the established technique for each (options)

1. **Falls that start on the face (spring-line and ribbon falls).** Water leaves a wall where a permeable bed
   (sandstone, jointed limestone) sits on an impermeable one (mudstone, shale): a contact spring or spring line
   (Ford and Williams 2007, ch. 7; Snake River's Thousand Springs; Hanging Gardens of the Colorado Plateau). The
   strata model already knows every bed and its hardness; a perched water table per hard band, fed by the band's
   outcrop catchment, gives the spring points along the contact on the face, flow by month. Emit them from
   `get_waterfalls` as a new type ("spring" lip on the face, its `discharge` by month) so PhysXWaterfalls3D draws them
   as it does now; plus a tested low `min_area` for snowmelt ribbon falls that dry in late summer.
2. **Frozen falls in winter.** Falls freeze from the edges inward as flow drops and spray freezes onto the rock,
   building ice curtains and pillars (Gauthier et al. 2013, thermodynamics of ice cascades; icicle growth by
   Makkonen 1988 and the morphogenetic accretion model of Szilder and Lozowski). The snow module's icicle growth and
   the falls' spray field are the two halves; the fall's flow by month and the air temperature drive it.
3. **Living glaciers and lasting caps.** Run the snow bake over more than one year with firn densification so snow
   above the equilibrium line carries over, and draw present-day glaciers with crevasses, seracs and icefalls from the
   ice's flow and strain (Argudo, Galin, Peytavie, Paris and Guérin 2020, "Simulation, modeling and authoring of
   glaciers", ACM TOG 39(6)). The generator's shallow-ice field is the input.
4. **Overhanging walls.** A heightfield cannot overhang; the cliff kit covers faces near vertical, not roofs. Add
   implicit 3D features over the heightfield only where needed (Paris, Galin, Peytavie, Guérin and Gain 2019,
   "Terrain amplification with implicit 3D features", ACM TOG 38(5)), meshed into the same chunks.
5. **A ring-basin bench at city scale.** A `--layout=ring` with guides (a raise ring, a valley basin, one hanging
   valley) at a larger world, to measure generation, memory and streaming cost before a game uses it.

Recommended order: 5 (proves the scale), then 1 (the asked-for fall), then 3, 2, 4.

## Sources

- Argudo, Galin, Peytavie, Paris and Guérin 2020, https://perso.liris.cnrs.fr/egalin/Articles/2020-glaciers.pdf
- Paris et al. 2019, https://perso.liris.cnrs.fr/aparis/public_html/projects/paris2019_3D.html
- Gauthier et al. 2013, https://hal.archives-ouvertes.fr/hal-01747132/file/Gauthier2013_IceCascadeThermo.pdf
- Makkonen 1988 / icicle growth, https://cambridge.org/core/journals/annals-of-glaciology/article/an-analytical-model-of-icicle-growth/C578E76767318E6AB3E9740F169B151D
- Spring line, https://en.wikipedia.com/wiki/Spring_horizon ; USGS Thousand Springs, https://water.usgs.gov/edu/gallery/gwdischarge-snakeriver.html
- Ford and Williams 2007, Karst Hydrogeology and Geomorphology; Carrier, GDC 2018, Procedural World Generation of Far Cry 5.
