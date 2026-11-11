### Added
- PhysXHair3D `deep_shadow_local_lights` (default 3): the omni and spot lights that light the hair most get their own deep opacity map (a perspective view from the light), so hair shadows itself smoothly under lamps as under the sun
- Spatial shader fragment output `POSITIONAL_SHADOW_OFFSET`: moves the fragment's omni and spot shadow lookups toward each light by that many metres
