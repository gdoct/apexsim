extends Node

# Scene paths
const LOADING_SCREEN = "res://scenes/loading_screen.tscn"
const MAIN_MENU = "res://scenes/main_menu.tscn"

# Current scene reference
var current_scene = null

func _ready():
	var root = get_tree().root
	current_scene = root.get_child(root.get_child_count() - 1)

# Change scene with optional loading screen
func change_scene(target_scene_path: String, use_loading: bool = false):
	if use_loading:
		# Deferred call to avoid conflicts
		call_deferred("_deferred_change_with_loading", target_scene_path)
	else:
		call_deferred("_deferred_change_scene", target_scene_path)

# Direct scene change
func _deferred_change_scene(target_scene_path: String):
	print("SceneManager: _deferred_change_scene called for: ", target_scene_path)
	print("SceneManager: current_scene = ", current_scene)

	var root = get_tree().root

	# Debug: Print all children
	print("SceneManager: Root has ", root.get_child_count(), " children:")
	for i in range(root.get_child_count()):
		var child = root.get_child(i)
		print("  [", i, "] ", child.name, " (", child, ")")

	# Free ALL non-autoload scenes (keep SceneManager and Network)
	for child in root.get_children():
		if child != self and child.name != "Network":
			print("SceneManager: Freeing scene: ", child.name)
			child.queue_free()

	print("SceneManager: Loading new scene...")
	var new_scene = load(target_scene_path).instantiate()
	get_tree().root.add_child(new_scene)
	get_tree().current_scene = new_scene
	current_scene = new_scene
	print("SceneManager: New scene loaded: ", new_scene.name)

# Change scene with loading screen
func _deferred_change_with_loading(target_scene_path: String):
	if current_scene:
		current_scene.free()

	var loading_scene = load(LOADING_SCREEN).instantiate()
	get_tree().root.add_child(loading_scene)
	get_tree().current_scene = loading_scene
	current_scene = loading_scene

	# Tell loading screen what to load
	if loading_scene.has_method("start_loading"):
		loading_scene.start_loading(target_scene_path)
