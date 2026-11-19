### Added
- Rivers carry a load entering by one bank as a plume along that bank, spreading across by turbulent mixing over tens of channel widths, instead of mixing it at once through the whole channel.
- Rivers drop silt and ash where the water slackens (by the banks, in pools); `PhysXRiver3D.get_sediment_deposit_at()` and `WorldWater.sediment_deposit_at()` give what has settled, in kg/m².

### Fixed
- Path tracing: water surfaces (sea, lakes, pools, rivers) cast their shadows through the water, so sunlight reaching the bed is tinted by the depth it crossed on the way down as well as on the way back up; shallow beds no longer read pale and warm against raster.
- `add_sediment` docs: it is a rate held for 2 seconds after each call; a one-off mass goes to `add_sediment_mass`.
- SkyClock: with its clock held (a still), the sun now dims and reddens as fires' smoke is added or drifts over; it only followed the smoke while time moved. `get_smoke_transmittance()` is 1 with `smoke_haze` off.
