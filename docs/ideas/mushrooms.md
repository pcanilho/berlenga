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
