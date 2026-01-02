extends Control

@onready var progress_bar = $ProgressBar
@onready var loading_label = $LoadingLabel

var target_scene_path: String = ""
var progress: float = 0.0
var loading_time: float = 0.0
const MIN_LOADING_TIME = .0  # Minimum time to show loading screen

func _ready():
	progress_bar.value = 0
	# Auto-load main menu after minimum loading time
	target_scene_path = "res://scenes/main_menu.tscn"

func _process(delta):
	loading_time += delta

	# Simulate loading progress
	if progress < 100:
		progress += delta * 50  # Load at 50% per second
		progress_bar.value = progress

	# Once we've loaded and shown the screen long enough, transition
	if progress >= 100 and loading_time >= MIN_LOADING_TIME:
		_finish_loading()

func start_loading(scene_path: String):
	target_scene_path = scene_path
	progress = 0.0
	loading_time = 0.0

func _finish_loading():
	if target_scene_path != "":
		SceneManager.change_scene(target_scene_path)
