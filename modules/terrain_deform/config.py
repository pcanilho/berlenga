def can_build(env, platform):
    env.module_add_dependencies("terrain_deform", ["noise", "world"])
    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "Landscape3D",
        "TerrainBrush3D",
        "TerrainSculptLayer3D",
        "TerrainDataMap3D",
        "TerrainDeformer3D",
        "TerrainGenerator3D",
        "TerrainGuide3D",
    ]


def get_doc_path():
    return "doc_classes"
