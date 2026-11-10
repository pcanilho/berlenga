def can_build(env, platform):
    env.module_add_dependencies("vegetation", ["terrain_deform", "weather"])
    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "Biome",
        "GrassField3D",
        "PlantSpecies",
        "VegetationField3D",
    ]


def get_doc_path():
    return "doc_classes"
