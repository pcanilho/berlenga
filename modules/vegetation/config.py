def can_build(env, platform):
    env.module_add_dependencies("vegetation", ["terrain_deform", "weather", "world"])
    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "Biome",
        "GrassField3D",
        "LeafFall3D",
        "PlantGenerator",
        "PlantSpecies",
        "SeedDown3D",
        "PollenCloud3D",
        "SodLipFall3D",
        "TreeGenerator",
        "TreeWind3D",
        "VegetationField3D",
        "PlacedPlant3D",
    ]


def get_doc_path():
    return "doc_classes"
