extends Control

@onready var title_label = $VBoxContainer/Title
@onready var subtitle_label = $VBoxContainer/Subtitle
@onready var btn_refresh_lobby = $VBoxContainer/ButtonRefreshLobby
@onready var btn_select_car = $VBoxContainer/ButtonSelectCar
@onready var btn_create_session = $VBoxContainer/ButtonCreateSession
@onready var btn_join_session = $VBoxContainer/ButtonJoinSession
@onready var btn_quit = $VBoxContainer/ButtonQuit

func _ready():
	# Connect button signals programmatically
	btn_refresh_lobby.pressed.connect(_on_button_refresh_lobby_pressed)
	btn_select_car.pressed.connect(_on_button_select_car_pressed)
	btn_create_session.pressed.connect(_on_button_create_session_pressed)
	btn_join_session.pressed.connect(_on_button_join_session_pressed)
	btn_quit.pressed.connect(_on_button_quit_pressed)

	# TODO: Initialize network client and authenticate when implemented

# Menu button handlers
func _on_button_refresh_lobby_pressed():
	subtitle_label.text = "Refresh Lobby - Not yet implemented"
	# TODO: Send RequestLobbyState message to server
	# TODO: Display lobby state (players, sessions, cars, tracks)

func _on_button_select_car_pressed():
	subtitle_label.text = "Select Car - Not yet implemented"
	# TODO: Show car selection dialog
	# TODO: Send SelectCar message to server

func _on_button_create_session_pressed():
	subtitle_label.text = "Create Session - Not yet implemented"
	# TODO: Show session creation dialog (track, max players, AI count, laps)
	# TODO: Send CreateSession message to server

func _on_button_join_session_pressed():
	subtitle_label.text = "Join Session - Not yet implemented"
	# TODO: Show available sessions dialog
	# TODO: Send JoinSession message to server

func _on_button_quit_pressed():
	get_tree().quit()
