### Added
- `WeatherSkyMaterial.atmosphere_model` SCATTERING (default): the Hillaire 2020 atmosphere (transmittance and multiple-scattering tables, Rayleigh, Mie and ozone) lit by both the sun and the moon, with twilight and the Earth's shadow. The clouds reach the horizon through aerial perspective. `atmo_far_water` shows open sea past the scene. `PHYSICAL_SKY` keeps the older model.
- `SkyClock.drive_light_energy` and `energy_per_lux`: the sun and moon lights take their colour and strength from the same transmittance and their brightness above the air, and drive the sky.

### Fixed
- The flat grey band between a cloud deck and a sea horizon.
