### Added
- PhysXHair3D `deep_shadows` (on, with `hair_shading`) and `deep_shadow_light`: the raster strands shade themselves from a directional light with a deep opacity map (Yuksel and Keyser 2008), smooth instead of grainy
- Spatial shader fragment output `DIRECTIONAL_SHADOW_OFFSET`: moves the fragment's directional shadow lookups toward each light by that many metres

### Changed
- PhysXHair3D strands cast into shadow maps with their own noise threshold each (stochastic transparency), so the shadow behind several strands builds up as their product
