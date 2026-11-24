### Added
- StreetWater3D.frame_path, prefill and get_depth_at; RoadRelief3D.relief_frame, storm-drain inlets (inlet_spacing, inlets) and get_pond_depth_at: water that rides a sliding street, puddles already filled as it comes into view, gutters that drain.
- RoadRelief3D.height_map and roughness class None: a street's own relief (its potholes, manholes, gutter grades) under the mesh, the ponding and the water. StreetWater3D.height_group: streets streamed in and out are found by group.
