def can_build(env, platform):
    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "TerrainBrush3D",
        "TerrainDataMap3D",
        "TerrainDeformer3D",
    ]


def get_doc_path():
    return "doc_classes"
