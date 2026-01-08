extends Node

func _ready():
    var scene_path = "res://scenes/car_selection_alt.tscn"
    print("Instantiating: ", scene_path)
    var s = ResourceLoader.load(scene_path)
    if s == null:
        print("Failed to load scene")
        get_tree().quit()
        return
    var inst = s.instantiate()
    add_child(inst)

    # Let the engine run a couple frames so C# _Ready can execute
    await get_tree().process_frame
    await get_tree().process_frame

    var n1 = inst.get_node_or_null("Panel/HBox/Right/Details/CarName")
    var car_name = "<missing>"
    if n1 != null:
        car_name = str(n1.text)

    var n2 = inst.get_node_or_null("Panel/HBox/Right/Details/EngineLabel")
    var engine_label = "<missing>"
    if n2 != null:
        engine_label = str(n2.text)
    print("CarName label text: ", car_name)
    print("EngineLabel text: ", engine_label)

    var model_root = inst.get_node_or_null("Panel/HBox/Right/TopRow/ModelViewport/SubViewport/ModelRoot")
    if model_root == null:
        print("ModelRoot missing")
    else:
        print("ModelRoot child count: ", model_root.get_child_count())

    print("Instantiated scene tree:")
    # call top-level helper
    dump_node(inst)

    # Try selecting second item to ensure selection updates UI
    var car_list = inst.get_node_or_null("Panel/HBox/Left/CarList")
    if car_list != null:
        # Programmatic selection does not always emit the signal, so emit it explicitly
        car_list.emit_signal("item_selected", 1)
        await get_tree().process_frame
        await get_tree().process_frame
        var new_name = "<missing>"
        var n3 = inst.get_node_or_null("Panel/HBox/Right/Details/CarName")
        if n3 != null:
            new_name = str(n3.text)
        print("After selecting index 1, CarName: ", new_name)

    get_tree().quit()

func dump_node(node, prefix=""):
    print(prefix, node.name, " (", node.get_class(), ")")
    for c in node.get_children():
        dump_node(c, prefix + "  ")
