extends Node

# Protocol definitions for ApexSim client-server communication
# This matches the Rust protocol.rs implementation

# Session States
enum SessionState {
	LOBBY,
	STARTING,
	RACING,
	FINISHED,
	CLOSED
}

# Message type identifiers (matches Rust bincode enum variants)
enum ClientMessageType {
	AUTHENTICATE = 0,
	HEARTBEAT = 1,
	SELECT_CAR = 2,
	REQUEST_LOBBY_STATE = 3,
	CREATE_SESSION = 4,
	JOIN_SESSION = 5,
	JOIN_AS_SPECTATOR = 6,
	LEAVE_SESSION = 7,
	START_SESSION = 8,
	DISCONNECT = 9,
	PLAYER_INPUT = 10,
}

enum ServerMessageType {
	AUTH_SUCCESS = 0,
	AUTH_FAILURE = 1,
	HEARTBEAT_ACK = 2,
	LOBBY_STATE = 3,
	SESSION_JOINED = 4,
	SESSION_LEFT = 5,
	SESSION_STARTING = 6,
	ERROR = 7,
	PLAYER_DISCONNECTED = 8,
	TELEMETRY = 9,
}

# Helper functions for working with UUIDs (as strings in Godot)
static func generate_uuid() -> String:
	var uuid = ""
	for i in range(16):
		uuid += "%02x" % (randi() % 256)
		if i == 3 or i == 5 or i == 7 or i == 9:
			uuid += "-"
	return uuid

# Parse session state from integer
static func parse_session_state(value: int) -> SessionState:
	match value:
		0: return SessionState.LOBBY
		1: return SessionState.STARTING
		2: return SessionState.RACING
		3: return SessionState.FINISHED
		4: return SessionState.CLOSED
		_: return SessionState.LOBBY

# Convert session state to string
static func session_state_to_string(state: SessionState) -> String:
	match state:
		SessionState.LOBBY: return "Lobby"
		SessionState.STARTING: return "Starting"
		SessionState.RACING: return "Racing"
		SessionState.FINISHED: return "Finished"
		SessionState.CLOSED: return "Closed"
		_: return "Unknown"
