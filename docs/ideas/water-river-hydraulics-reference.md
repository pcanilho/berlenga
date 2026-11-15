# River hydraulics and buoyancy: what a UE5 water plugin does that we do not (15 Nov)

Reference read: Dynamic Real Water 3.0 (Natural Level, a paid Unreal Engine 5 plugin; public docs at
https://naturallevel.dev/assets/dynamic-real-water/). Nothing of it is used; this note keeps the
techniques its docs describe that the fork lacks, with the published sources behind them.

## Where the fork already matches or passes it

FFT ocean with cascades, swell and runtime sea states (ours: TMA, measured spreading, Monahan
whitecaps, storm spray); shore surf with shoaling, breaking and run-up (ours adds wave groups and
plunging lips); river depth and speed from discharge and Manning roughness; rocks parting the
current; joins to rivers, lakes and the sea with a plume; carved channels; camera under water with
caustics. It has no path tracer support, no waterfalls, no live water over terrain and no wetness
(all on its roadmap). Our live window already gives eddies, jumps and wakes near the camera.

## Gaps worth taking

1. **Analytic river surface features in the far field.** Away from the live window our baked river
   is a diffusive wave (step 4c): no inertia, so no standing waves, cross-waves or jumps. The plugin
   draws them analytically along the whole river from the section's Froude number, which suits an
   open world where most river is outside any simulation window:
   - Gradually varied flow by the standard step method (Chow 1959, ch. 10) instead of normal depth
     plus level pool: water ponds upstream of bed bumps and lake levels, and supercritical reaches
     end in a hydraulic jump where the momentum function matches the downstream depth (Belanger's
     sequent depth `y2/y1 = (sqrt(1 + 8 Fr1^2) - 1) / 2`). Jump roller and foam keyed on `Fr1`
     (Chanson's air entrainment, already cited).
   - Standing wave trains in subcritical flow below slope breaks, jumps and rapids: stationary when
     the wave's phase speed equals the current, `U^2 = (g/k) tanh(k h)`, which sets the wavelength;
     undular jumps for `Fr1` about 1 to 1.7 (Fawer 1937; Chanson 1995). Height a fraction of depth,
     decaying over a few wavelengths.
   - Cross-waves in supercritical flow: oblique standing waves at the wave angle `sin(beta) = 1/Fr`
     from width changes and bends, reflecting between banks into diamonds (Ippen 1951, Knapp 1951,
     "Mechanics of supercritical flow", ASCE Trans. 116). The same angle gives a rock's stationary
     V wake when the water shoots.
   - Bend superelevation: the surface tilts up to the outer bank by about `U^2 w / (g r)` (Chow
     1959), with a secondary surface drift outward that carries foam and floaters. We shift the
     carved floor at bends but keep the surface level across.
   - A hydraulics overlay (depth, speed, Froude, regime, jump markers) for tuning.
   Owner: water (`PhysXRiver3D` bake and shader). Cost: bake-time only plus a few shader terms.
2. **Lake outlets that drain the lake.** Our outlet river does not take its flow from the lake
   (step 4e, "Not yet"). The plugin sets discharge from the head of the lake over the outlet sill:
   the broad-crested weir law `Q = Cd b sqrt(g) H^(3/2)` (Cd about 0.85 times (2/3)^(3/2); Henderson
   1966, Open Channel Flow, ch. 6). With inflow from rivers this also gives lakes a level budget.
   Directly useful for hanging-valley lakes with a fall at the outlet. Owner: water.
3. **Buoyancy from shapes with densities.** Floaters built from simple shapes, each with a
   material density (wood floats, steel sinks), plus air shapes that remove mass inside a hull so a
   steel ship floats; skeletal bodies take their shapes from the ragdoll. Submerged volume per shape
   (clipped boxes, spheres, capsules) instead of fixed probes; the exact per-triangle waterline
   (Kerner 2015) stays for hero hulls. Planing hulls rise by speed (Savitsky 1964). Owner: water
   (`PhysXBuoyancy3D`), low priority.
4. **Light beams from lamps under water** in raster (the path tracer has them): point and spot
   lights scatter into visible beams inside the water fog. Belongs with the raster light shafts
   already queued (godrays stage 2).

## Not worth taking

Its eight FFT layers and preset blending (ours are at least as good), shoreline obstacles and the
"small coast" surf fade (a heuristic stand-in for diffraction), multiplayer replication of the
water clock (game side, not engine), its 16 m depth map (coarser than ours).
