### Fixed

- Reprojected sky clouds no longer show a checker or dot pattern: each frame's march start is a hash of the frame, the history is variance-clipped to this frame's samples, and a pixel's first samples replace the upsampled fill.
