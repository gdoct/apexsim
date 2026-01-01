extends Node

# Network client for connecting to ApexSim server
# Uses TCP with length-prefixed JSON messages

signal connected_to_server
signal disconnected_from_server
signal authentication_success(player_id: String)
signal authentication_failed(reason: String)
signal lobby_state_received(lobby_data: Dictionary)
signal session_joined(session_id: String, grid_position: int)
signal session_left
signal error_received(code: int, message: String)

var tcp_client: StreamPeerTCP
var server_address: String = "127.0.0.1"
var server_port: int = 9000
var is_connected: bool = false
var player_id: String = ""
var player_name: String = "Player"
var auth_token: String = "dev-token"

# Message buffer for incomplete messages
var receive_buffer: PackedByteArray = PackedByteArray()

func _ready():
	tcp_client = StreamPeerTCP.new()

# Connect to server
func connect_to_server(address: String, port: int) -> void:
	server_address = address
	server_port = port

	print("Connecting to %s:%d..." % [server_address, server_port])
	var error = tcp_client.connect_to_host(server_address, server_port)

	if error != OK:
		print("Failed to initiate connection: ", error)
		emit_signal("disconnected_from_server")
		return

	# Wait for connection in _process

# Disconnect from server
func disconnect_from_server() -> void:
	if is_connected:
		send_message({
			"type": "Disconnect"
		})
	tcp_client.disconnect_from_host()
	is_connected = false
	player_id = ""
	emit_signal("disconnected_from_server")

# Authenticate with server
func authenticate(name: String, token: String) -> void:
	player_name = name
	auth_token = token

	send_message({
		"type": "Authenticate",
		"token": token,
		"player_name": name
	})

# Request lobby state
func request_lobby_state() -> void:
	send_message({
		"type": "RequestLobbyState"
	})

# Select a car
func select_car(car_id: String) -> void:
	send_message({
		"type": "SelectCar",
		"car_config_id": car_id
	})

# Create a new session
func create_session(track_id: String, max_players: int, ai_count: int, lap_limit: int) -> void:
	send_message({
		"type": "CreateSession",
		"track_config_id": track_id,
		"max_players": max_players,
		"ai_count": ai_count,
		"lap_limit": lap_limit
	})

# Join an existing session
func join_session(session_id: String) -> void:
	send_message({
		"type": "JoinSession",
		"session_id": session_id
	})

# Leave current session
func leave_session() -> void:
	send_message({
		"type": "LeaveSession"
	})

# Start the session (host only)
func start_session() -> void:
	send_message({
		"type": "StartSession"
	})

# Send heartbeat
func send_heartbeat(client_tick: int) -> void:
	send_message({
		"type": "Heartbeat",
		"client_tick": client_tick
	})

# Send a message to the server (JSON with length prefix)
func send_message(data: Dictionary) -> void:
	if not is_connected and data.get("type") != "Authenticate":
		print("Not connected to server")
		return

	# Serialize to JSON
	var json_str = JSON.stringify(data)
	var json_bytes = json_str.to_utf8_buffer()

	# Create length-prefixed message (4 bytes big-endian length + data)
	var length = json_bytes.size()
	var message = PackedByteArray()
	message.append((length >> 24) & 0xFF)
	message.append((length >> 16) & 0xFF)
	message.append((length >> 8) & 0xFF)
	message.append(length & 0xFF)
	message.append_array(json_bytes)

	# Send
	tcp_client.put_data(message)
	print("Sent: ", data.get("type", "Unknown"))

# Process incoming messages
func _process(delta):
	# Check connection status
	var status = tcp_client.get_status()

	if status == StreamPeerTCP.STATUS_CONNECTING:
		return
	elif status == StreamPeerTCP.STATUS_CONNECTED:
		if not is_connected:
			is_connected = true
			tcp_client.set_no_delay(true)
			emit_signal("connected_to_server")
			print("Connected to server!")
			# Auto-authenticate
			authenticate(player_name, auth_token)
	elif status == StreamPeerTCP.STATUS_ERROR or status == StreamPeerTCP.STATUS_NONE:
		if is_connected:
			is_connected = false
			emit_signal("disconnected_from_server")
			print("Disconnected from server")
		return

	# Read available data
	if is_connected:
		var available = tcp_client.get_available_bytes()
		if available > 0:
			var data = tcp_client.get_partial_data(available)
			if data[0] == OK:
				receive_buffer.append_array(data[1])
				_process_received_data()

# Process received data buffer
func _process_received_data() -> void:
	while receive_buffer.size() >= 4:
		# Read length prefix (4 bytes, big-endian)
		var length = (receive_buffer[0] << 24) | (receive_buffer[1] << 16) | (receive_buffer[2] << 8) | receive_buffer[3]

		# Check if we have the complete message
		if receive_buffer.size() < 4 + length:
			break

		# Extract message
		var message_bytes = receive_buffer.slice(4, 4 + length)
		receive_buffer = receive_buffer.slice(4 + length)

		# Parse JSON
		var json_str = message_bytes.get_string_from_utf8()
		var json = JSON.new()
		var parse_result = json.parse(json_str)

		if parse_result == OK:
			var message = json.data
			_handle_message(message)
		else:
			print("Failed to parse JSON: ", json_str)

# Handle received message
func _handle_message(msg: Dictionary) -> void:
	var msg_type = msg.get("type", "")
	print("Received: ", msg_type)

	match msg_type:
		"AuthSuccess":
			player_id = msg.get("player_id", "")
			var server_version = msg.get("server_version", 0)
			print("Authenticated! Player ID: ", player_id, " Server version: ", server_version)
			emit_signal("authentication_success", player_id)
			# Auto-request lobby state
			request_lobby_state()

		"AuthFailure":
			var reason = msg.get("reason", "Unknown error")
			print("Authentication failed: ", reason)
			emit_signal("authentication_failed", reason)

		"LobbyState":
			emit_signal("lobby_state_received", msg)

		"SessionJoined":
			var session_id = msg.get("session_id", "")
			var grid_pos = msg.get("your_grid_position", 0)
			print("Joined session ", session_id, " at grid position ", grid_pos)
			emit_signal("session_joined", session_id, grid_pos)

		"SessionLeft":
			print("Left session")
			emit_signal("session_left")

		"Error":
			var code = msg.get("code", 0)
			var error_msg = msg.get("message", "Unknown error")
			print("Server error [", code, "]: ", error_msg)
			emit_signal("error_received", code, error_msg)

		"HeartbeatAck":
			pass  # Silently handle heartbeats

		_:
			print("Unknown message type: ", msg_type)
