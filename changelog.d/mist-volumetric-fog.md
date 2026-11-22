### Fixed
- Raster waterfall mist was invisible unless the scene turned on volumetric fog by hand: PhysXWaterMist3D now turns the Environment's volumetric fog on (density 0, sky-lit) while a mist is live and puts it back after (#239).
