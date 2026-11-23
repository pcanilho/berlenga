### Fixed
- Path tracing: a blended (alpha) layer's own bounce light is no longer lost at some alpha values, which left blended surfaces near black where only sky and bounce light reach them (#321).
- Path tracing: the medium scratch image starts cleared, so a stray read can't pick up another surface's leftover colours (#400).
