extends Node

func _ready():
    var scene_path := "res://scenes/car_selection_alt.tscn"
    print("Checking scene: ", scene_path)
    print("ResourceLoader.exists: ", ResourceLoader.exists(scene_path))
    var global_path = ProjectSettings.globalize_path(scene_path)
    print("Global path: ", global_path)
    print("FileAccess.file_exists: ", FileAccess.file_exists(global_path))

    var res = ResourceLoader.load(scene_path)
    if res == null:
        print("ResourceLoader.load returned: NULL")
    else:
        print("ResourceLoader.load returned: ", res, " class=", res.get_class())

    var res2 = ResourceLoader.load(scene_path, "PackedScene")
    print("ResourceLoader.load as PackedScene: ", res2)

    print("Done.")
    get_tree().quit()
