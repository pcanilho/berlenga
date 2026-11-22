### Added

- Camera cut-out: a soft dithered hole through walls, roofs and trees between the camera and the player, the same in the path tracer (global shader parameters `camera_cutout` and `camera_cutout_floor`, `BaseMaterial3D.get_camera_cutout_shader_code()`); characters and anything else can stay in view (`BaseMaterial3D.camera_cutout`).

### Changed

- Camera cut-out: shadow maps of cut-out materials draw with the shared shadow material again, as before the cut-out (no setting).
