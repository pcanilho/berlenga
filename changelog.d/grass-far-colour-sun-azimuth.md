### Changed

- The far meadow's colour turns with the sun's heading from the view, brighter looking down-sun (the canopy hot spot), darker into the sun: GrassField3D's far colour bake renders the blades with the sun at four headings and fits the change (`get_far_color_lobes()`), and `grass_ground()` applies it with the sun from `sun_light` (or the scene's first DirectionalLight3D).
