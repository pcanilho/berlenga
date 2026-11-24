### Added
- StreetWater3D: GPU shallow water over a street's relief in a camera-following window (2.5 cm cells). Rain runs off as a film into ruts, hollows and gutters; tyres and feet (submit_sphere) push the water aside into bow waves and V wakes and it runs onto dry road; soaking, evaporation and damp that dries. The road material draws it from one map (get_shader_include), so raster and path tracing match.
- RoadRelief3D.get_surface_height_at: the built mesh's height, cheap enough for many samples.
- StreetWater3D.frame_path, prefill and get_depth_at; RoadRelief3D.relief_frame, storm-drain inlets (inlet_spacing, inlets) and get_pond_depth_at: water that rides a sliding street, puddles already filled as it comes into view, gutters that drain.
