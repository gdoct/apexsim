extends Control

# Button references will be set up via the scene connections

func _ready():
	print("ApexSim Main Menu loaded")
	# TODO: Initialize network client and authenticate when implemented

# Menu button handlers
func _on_button_refresh_lobby_pressed():
	print("Refresh Lobby State - Not yet implemented")
	# TODO: Send RequestLobbyState message to server
	# TODO: Display lobby state (players, sessions, cars, tracks)

func _on_button_select_car_pressed():
	print("Select Car - Not yet implemented")
	# TODO: Show car selection dialog
	# TODO: Send SelectCar message to server

func _on_button_create_session_pressed():
	print("Create New Session - Not yet implemented")
	# TODO: Show session creation dialog (track, max players, AI count, laps)
	# TODO: Send CreateSession message to server

func _on_button_join_session_pressed():
	print("Join Session - Not yet implemented")
	# TODO: Show available sessions dialog
	# TODO: Send JoinSession message to server

func _on_button_quit_pressed():
	print("Quitting ApexSim")
	get_tree().quit()
