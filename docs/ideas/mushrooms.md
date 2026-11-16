# Woodland-floor mushrooms

Fruit bodies of soil fungi as real geometry: stalk, cap, gills (or pores), ring, warts, placed by
the ecology of each species. Fungi on wood (brackets, honey fungus at the trunk foot, sulphur tuft
on logs) are part of the tree meshes (trees module); everything coming up from the soil is here.

## Bar and references

- Kingdom Come: Deliverance II, Nyx I: mushrooms are hand-modelled props, few species, scattered
  without regard to trees; they read from a few metres only because each is a clean, solid shape
  with a pale underside. That is the minimum: no cards, real undersides.
- Unreal PCG forests (Megaplants, the UE 5.7 procedural vegetation tutorial) scatter scanned
  mushroom meshes by density masks; nothing ties them to host trees or season.
- Desbenoit, Vanderhaghe, Galin, Grosjean, "Interactive Modeling of Mushrooms", Eurographics 2004
  short papers: a silhouette split into stem, gills and cap, revolved round a bent skeleton,
  free-form deformation for irregularity, morphing between control shapes for variety. The
  geometry here follows it: each part is a surface of revolution of a parametric profile.
- Infinigen (Raistrick et al., CVPR 2023): the cap's underside as a star-shaped mesh whose points
  are the gills; the ring as a skirt below the cap. Here gills are separate thin fins (real depth
  in path tracing, short gills between long ones as in real agarics).
- itch.io devlogs (Vorxel "procedurally generated mushrooms"): caps from a few closed-form
  profiles; gills need their stripes to read.

## Species (European woods)

| Species | Host / substrate | Habit | Size (cap, stalk) | Season |
|---|---|---|---|---|
| Fly agaric, Amanita muscaria | birch, spruce, pine (ectomycorrhizal) | 1 to 3, scattered round a host | cap 8 to 20 cm red with white warts, stalk 10 to 20 cm, ring, bulb with volva rims | Aug to Nov |
| Cep, Boletus edulis | oak, beech, spruce, pine | 1 to 2 | cap 8 to 25 cm brown, pores not gills, stout bulbous stalk with a pale net | Aug to Oct |
| Chanterelle, Cantharellus cibarius | beech, oak, conifers, in moss | groups of 3 to 8 | cap 3 to 8 cm funnel, wavy margin, ridges running down the stalk, all egg-yellow | Jun to Nov |
| Brittlegill, Russula (emetica, nobilis) | beech, conifers | 1 to 2 | cap 4 to 9 cm red, white gills and stalk, no ring | Jul to Nov |
| Clouded agaric, Clitocybe nebularis | leaf and needle litter (saprotroph) | rings and arcs, 2 to 4 together | cap 6 to 15 cm grey, decurrent cream gills, stout stalk | Sep to Dec |
| Honey fungus, Armillaria | round stumps and dead roots (parasite, then saprotroph) | tufts of 6 to 15 fused at the base | cap 3 to 10 cm honey-brown with dark scales, ring | Sep to Nov |

## Ecology for placement

- Ectomycorrhizal fruit bodies come up within the host's root zone, mostly under the crown and a
  few metres from the trunk. Placement uses the canopy fields' shade (under crowns) and stand type
  (conifer share) as the host test; fly agaric takes birch and conifers, cep and chanterelle oak,
  beech and conifers, brittlegill both.
- Woodland fairy rings: the mycelium spreads outward and dies behind, leaving a living ring that
  fruits; woodland rings of Clitocybe nebularis are arcs a few metres across (Dowson, Rayner and
  Boddy, New Phytologist 1989). Meadow rings reach over 10 m; the oldest, Infundibulicybe geotropa
  near Belfort, about 300 m in radius.
- Honey fungus fruits in tufts at the base of stumps and over dead roots: next to deadwood.
- Fruiting needs moisture and mild weather: late summer to late autumn, after rain; frost ends it.
- Each mushroom lasts days; a flush leaves a few old, flattened, darker caps among fresh ones.

## Geometry

- Stalk: a surface of revolution along a slightly bent skeleton, radius by a profile (taper, bulb
  at the base for fly agaric and cep, volva rims as small ridges).
- Cap: upper surface and underside revolved from a profile (convex, flat, umbonate, funnel), the
  margin wavy for chanterelles; flattens with age.
- Gills: thin fins between the underside and a lower edge that bows down mid-way, long ones from
  stalk to margin with one or two ranks of short ones between; decurrent gills run down the stalk.
  Ceps have a tube layer instead, a thick pale cushion.
- Ring: a thin skirt hanging from the upper stalk (fly agaric, honey fungus).
- Warts: small raised patches on the fly agaric's cap.
- Surfaces: cap on the petal surface, gills, warts and ring on the centre surface, stalk on the
  stem surface; plain colours (no leaf texture), no light response, no wind.

## Honey fungus clumps from the wood (16 Nov)

A docs review read the first tufts as separate biscuits on straight white stalks in the litter.
Armillaria mellea (Phillips, "Mushrooms", 2006; Kibby, "Mushrooms and Toadstools of Britain and
Europe", 2017) fruits in dense clumps on stumps, logs and roots: stalks tough and fibrous, curved,
fused at a common base, pale yellow above a thick cottony ring (white with a yellow edge) and
darkening to olive brown at the foot; caps tawny honey, darker to the centre where small, erect,
dark brown fibrous scales crowd, slightly viscid when damp. Built:
- One fused base (a dark lumpy lathe) the stalks rise from, on its -X side against the wood; the
  stalks spread over the outer half, lean out low and curve back up, with a little sway each.
- Scales as geometry: 90 to 220 small pointed cones per cap, crowded at the centre and leaning out,
  in place of the darkened vertex speckle; caps darker to the centre and glossier (roughness 0.3).
- Stalk tint from the brown foot to pale yellow under the ring; a cream-yellow ring.
- VegetationField3D moves a wood feeder (PlantSpecies.deadwood_from_wood) onto the
  log's side where it meets the ground, 1.5 cm into the bark and moss, and turns it to face out of
  the wood, so the clump comes up from the log rather than beside it. Band 0.4 m (was 0.8).
- Plants beside a log (ferns, banked leaves, honey fungus) only where its underside is within
  15 cm of the ground: a windthrown beech propped on its root plate lies 1.5 m up at that end, and
  clumps had come up in the open under it.

## Honey fungus close-up, docs rounds 2 and 3 (16 Nov)

Docs' close-up faults and what real clumps show (Kibby 2017; Phillips 2006; First Nature and
Galloway Wild Foods field descriptions): clumps burst from cracks in the bark, often well up a
stump or log side, not from the litter beside it; stalks converge to one point and fuse in a
swollen, darker base, among small aborted primordia (buttons); caps honey yellow to tawny olive,
varying cap to cap with age and moisture, with innate radial fibrils, small dark erect scales
crowded at the centre and thinning out, a paler often striate margin, and a moist, slightly
greasy sheen. Built:
- Wood feeders root up the log's side, about 20 cm over the litter (at most the log's widest
  girth), tipped 15 degrees out of the bark (VegetationField3D::WOOD_FEEDER_HEIGHT, _LEAN_DEG).
- No separate base mesh: each stalk starts within 8 mm of the clump's centre with a swollen foot
  (the form's bulb), so the feet merge into one mass; 2 to 5 small buttons in the base.
- Caps: petal colour less orange (0.62, 0.47, 0.24); the brown centre reaches about half the cap;
  each cap its own shade from honey yellow to tawny olive; radial fibrils as vertex streaks on 32
  to 64 sectors; on half the caps a darker damp band inside the rim (hygrophanous drying from the
  centre); roughness 0.22 for the greasy sheen; scales dark brown (lighter ones read as pale grit from above) and kept to the inner 65%.
- Leaf litter lying underside up shows as a paler colour with its face still up: a litter leaf drawn
  from its back face read black or flat in close-ups.

## Cave mouths (16 Nov)

TerrainGenerator3D.add_hole() cuts the ground away behind a cave mouth (Landscape3D drops the
quads). VegetationField3D and GrassField3D find the generator through the data map (its geology
source, else its height source) and keep a copy of the hole boxes (vegetation_holes.h, read by the
placement workers): no stem in an opening or within the obstacle margin of its edge, no log over
one. On holes_changed a streaming field places again the tiles near the change (once no tile is
being placed), a whole field populates again, and the grass rebuilds its tiles there nearest first
with the old ones drawn until then. The far cover map is left as it is: the terrain is not drawn
in the opening, so it never shows there.
