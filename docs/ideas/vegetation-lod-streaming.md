# Vegetation: cheaper plants and streamed placement

Two performance steps for VegetationField3D and PlantGenerator, so that dense woods and meadows
scale from a bench map to an open world. Both have to keep the look: no visible pop, and no
detail lost under path tracing.

## 1. Cheaper plants

### Where we are

PlantGenerator builds distance LODs with meshoptimizer (`ImporterMesh.generate_lods`, as the
scene importer does). Measured per surface (triangles at LOD 0, then the smallest LOD reached):

| Preset | Stems and blades | Leaf cards / leaves |
| --- | --- | --- |
| Bramble | 19,796 to 168 | 6,560 to 20 |
| Heather | 6,200 to 1,048 | 4,160 to 1,040 |
| Woodruff | 4,816 to 270 | 1,088 to 272 |
| Bracken | 9,896 to 170 | 1,720 to 10 |
| Ivy | 2,576 to 390 | 2,800 to 140 |

Edge-collapse simplification works on connected stems but stalls at about a quarter on surfaces
made of many separate small leaves and cards: each card is its own island with a border, and
meshoptimizer keeps borders. Heather and woodruff are exactly that, so a heath of them stays at
thousands of triangles a plant at any distance.

### What others do

- **SpeedTree** (used by UE, HDRP, CryEngine titles): LODs *reduce leaf count* and scale the
  surviving leaves up so the crown keeps its silhouette and coverage, then a billboard last.
- **Unreal Engine 5 Nanite foliage**: "Preserve Area" grows the remaining triangles as a
  cluster simplifies, for the same reason: thin and separate parts otherwise vanish or lose
  coverage at a distance.
- **CryEngine** (Kingdom Come: Deliverance 2): vegetation LODs with dissolve between them and
  sprites far away; grass as merged instanced meshes.
- **Frostbite** (Battlefield): leaf cards fade out by distance; far trees as impostors.
- **Horizon Zero Dawn** (van Muijden, GDC 2017): per-asset LODs with dithered transitions.
- **Octahedral impostors** (Brucks, Fortnite, 2018): a plant baked into views on an atlas,
  drawn as one quad from any angle. Right for shrubs over about a metre at mid distance.
- **Unity HDRP**: LODGroup with dithered crossfade; leaves the reduction itself to the asset.

### Plan

1. **Leaf and card reduction with area kept.** PlantGenerator gives every leaf and card a
   stable rank (stored per vertex, beside the organ pivot it already keeps in UV2.y for
   bending). Its LOD chain drops cards by rank, highest first, for leaf and card surfaces
   (index lists only, so no extra vertex memory); stems keep meshoptimizer LODs. The plant
   shader shrinks a card to its pivot as the camera nears the distance where its LOD drops it,
   and grows the survivors to keep the covered area (SpeedTree, Nanite Preserve Area). The
   card is gone before the index list loses it, so there is no pop. This is the same rank
   and grow scheme the far-distance stage uses for whole plants.
2. **Impostors for the larger shrubs** (bramble, bracken, broom, heather clumps): octahedral
   impostors baked by PlantGenerator, drawn in raster beyond a set distance with a dithered
   crossfade. The path tracer keeps the smallest mesh LOD (a flat impostor would lose depth
   and shadows under path tracing, against the rule that path tracing must not remove detail).
   **Built (1a).** Leaf surfaces with 24 or more cards (petals excluded: dropping them dims
   the flowers) keep the pivot and a Weyl-sequence rank per card in CUSTOM0; each LOD level
   keeps the cards ranked under its share and then lets meshoptimizer simplify the survivors
   within the same error, so cards and their outlines both shrink. Triangles drawn by a
   2,600-plant heath bench (raster, 640 x 360 view across it): heather 112k to 70k, woodruff
   42k to 17k, bramble unchanged (its leaves were already reduced by meshoptimizer). Raster
   crops match; path traced frames before and after differ by 0.15 of 255 on average (the
   path tracer draws the full mesh and the shader keeps every card at full size up close).
   **Built (1b).** `PlantGenerator.make_impostor()` bakes on the CPU (all cores, about 0.1 s a
   plant): 8 x 8 hemi-octahedral views with frame directions at the cell centres (the lowest
   ring looks about 7 degrees down, so flat leaves do not vanish edge-on), each 64 texels fitted
   to the plant's extent from that side, 4 x 4 samples a texel. Atlas A: albedo (as the plant
   shader colours it, wear included) and coverage, raised by half so leaves under a texel still
   pass the alpha test, with mips that keep the share passing (Castaño 2010). Atlas B: normal
   (octahedral), depth, back light. The shader blends the three nearest views (Brucks),
   reprojects each through the view ray, writes depth from the baked depth and lights at that
   surface (`LIGHT_VERTEX`); shadow casters sit 8% of the radius behind it so the camera's views
   do not shadow themselves. `add_impostors(species, range)` moves the meshes to near meshes and
   puts impostors beyond; `make_floor()` does it for bramble and bracken at 15 m. Under path
   tracing VegetationField3D swaps its impostor cells back to the meshes (checked four times a
   second, and at populate), so path tracing loses nothing.
   Oak wood view in the grass bench (`--forest=in --stand=oak --prims`): 3.91M to 2.98M
   triangles a frame; `make_floor` 0.9 s to 1.5 s; about 17 MB of textures for the six
   impostors. Single bramble at 20 and 35 m: impostor and mesh alike.
3. **Measure**: triangles per hectare in the grass bench's heath and beech floors before and
   after, raster and path traced at 100% crop, GPU time on the PC.

## 2. Placement in the background, streamed around the camera

### Where we are

`VegetationField3D.populate()` places the whole rect at once on the main thread: about 0.3 s
for the grass bench's 96 m square with all its species. That grows with area, so a 10 km
square would take minutes and hold every plant in memory.

### What others do

- **Horizon Zero Dawn** (van Muijden, GDC 2017): placement runs on the GPU in compute shaders,
  tile by tile around the camera, from density maps; nothing is stored.
- **Ghost of Tsushima** (Wohllaib, GDC 2021): grass generated per tile in compute each frame.
- **Unreal Engine PCG**: runtime generation on a partitioned grid, with a generation radius
  and a cleanup radius per grid size, time-sliced on worker threads.
- **Frostbite** (Andersson, "Terrain Rendering in Frostbite", SIGGRAPH 2007): a decorator
  system places ground cover per terrain patch as CPU jobs, near the camera only.
- **CryEngine**: procedural vegetation generated per terrain sector at load.

### Plan

1. **Tiles generated on WorkerThreadPool around a centre** (the camera by default), nearest
   first. Placement is already per tile (a Poisson pattern per tile, ranks from hashes), so a
   tile depends only on its own cell and the larger species in the cells around it. A tile
   computes those neighbours' trees and shrubs itself (cheap, deterministic) for footprints and
   shade, so tiles never wait on each other.
2. **A radius per species from its visibility range**: herbs only near, trees much farther, so
   most tiles hold few species. Tiles past the cleanup radius are freed.
3. **Main thread only commits**: MultiMesh creation and node insertion with a per-frame time
   budget, as GrassField3D already does for its tiles (`max_tiles_per_frame`).
4. **Maps**: the canopy, forest floor and far colour maps are written per tile into textures
   that cover the streamed area and are recentred as the camera moves (as the trample field
   does), so ground shaders read the same globals.
5. **populate() stays** for benches and small scenes: it runs the same tile code for the whole
   rect and waits.
6. GPU placement (Horizon) only if the CPU workers do not keep up at a run's speed: the CPU path
   reuses all the rule code and the trees' and snow's readers of the placements.

## References

- SpeedTree documentation, "Level of detail" (leaf reduction and scaling).
- Epic Games, Nanite foliage and "Preserve Area", Unreal Engine 5 documentation.
- Brucks, "Octahedral Impostors", 2018.
- van Muijden, "GPU-Based Procedural Placement in Horizon Zero Dawn", GDC 2017.
- Wohllaib, "Procedural Grass Rendering in Ghost of Tsushima", GDC 2021.
- Andersson, "Terrain Rendering in Frostbite Using Procedural Shader Splatting", SIGGRAPH 2007.
- Epic Games, Procedural Content Generation framework, runtime generation (UE 5.2+).
