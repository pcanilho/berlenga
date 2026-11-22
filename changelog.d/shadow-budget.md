### Added

- Positional shadows go to the most important lights first (energy times size on screen, a light keeping its shadow until another is clearly more important), so a full shadow atlas drops the least important; a budget caps how many keep shadows (project setting `rendering/lights_and_shadows/positional_shadow/max_shadowed_lights`, 0 = as many as fit).
