### Fixed

- Path tracing: a MultiMesh surface whose vertex shader is captured is left out until its first capture is built, instead of being traced as its bare mesh (grass blades showed as flat 1 m strips until the dynamic build budget reached them).
