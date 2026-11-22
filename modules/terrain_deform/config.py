def can_build(env, platform):
    env.module_add_dependencies("terrain_deform", ["noise"])
    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "TerrainBrush3D",
        "TerrainDataMap3D",
        "TerrainDeformer3D",
        "TerrainGenerator3D",
    ]


def get_doc_path():
    return "doc_classes"
