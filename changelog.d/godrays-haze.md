### Added

- Volumetric fog takes the cloud shadow projected on the sun, so cloud shafts show in the fog.
- WeatherMap3D `aerosol_haze`, `haze_visibility`, `haze_height`: background haze from visibility and humidity, lit by the volumetric fog.

### Fixed

- Volumetric fog no longer drops fog volumes thinner than 0.001 /m: density is stored at 1/65536 /m with stochastic rounding.
