### Added

- Storms break and throw rigged trees: `TreeWind3D` checks each limb's wind moment against its wood (species MOR, knots) and each tree's against its root anchorage; broken limbs and thrown trees (with their root plate) fall as hinged PhysX bodies under the wind, and on landing become Blast destructibles that snap where they hit and settle under their own weight. `TreeWind3D.storm_*`, `TreeGenerator.storm_kit`, signal `storm_damage` with the root pit.
