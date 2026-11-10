### Fixed

- Weather: lightning casts shadows in raster. A far ground flash now lights the raster as a directional light from the channel's direction, with sharp cascaded shadows, instead of an omni light whose shadows were lost over kilometres. `raster_shadow_distance` sets how far its shadows reach.
