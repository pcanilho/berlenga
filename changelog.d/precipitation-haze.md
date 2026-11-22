### Added

- Weather: `WeatherMap3D.set_precipitation_extinction()` lets snow and rain nodes thin the air. Uniform haze drives the environment's fog, coloured by the sky. A layer near the ground becomes lit volumetric fog. Both renderers read it, and the environment is put back as it was when the last source clears.
