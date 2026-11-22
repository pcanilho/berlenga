### Fixed

- Flow gas drifts on the weather wind again: the wind reserves the blocks along the plume's path, swept downwind from the sources for as long as the smoke lasts (gas lives only in allocated blocks, and Flow does not allocate ahead of it).
- Flow gas keeps time below 60 frames a second: it catches up with up to eight steps a frame, as Godot's physics does (it took one, so at 30 frames a second it ran at half speed).
