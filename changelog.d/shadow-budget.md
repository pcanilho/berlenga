### Added

- Positional shadows go to the most important lights first (energy times size on screen), so a full shadow atlas drops the least important; a budget caps how many keep shadows (project setting `rendering/lights_and_shadows/positional_shadow/max_shadowed_lights`, 0 = as many as fit).
