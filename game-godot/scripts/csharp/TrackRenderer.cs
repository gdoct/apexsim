using Godot;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;
using MessagePack;
using MessagePack.Resolvers;

namespace ApexSim;

public partial class TrackRenderer : Node3D
{
	private MeshInstance3D? _trackMesh;
	private ProceduralTerrain? _proceduralTerrain;
	private NetworkClient? _network;
	private string? _currentSessionId;
	private Camera3D? _camera;
	private Button? _backButton;
	private Label? _telemetryLabel;
	private float _cameraAngle = 0.785f; // Start at 45 degrees (PI/4) for better initial view

	// Terrain heightmap for track elevation
	private TerrainHeightmap? _currentHeightmap;

	// Car rendering for demo mode
	private Node3D? _demoCarModel;
	private string? _selectedCarId;

	// MessagePack options for terrain data deserialization
	// Use StandardResolver for array-based MessagePack (matches Rust rmp_serde::to_vec)
	private static readonly MessagePackSerializerOptions MsgPackOptions =
		MessagePackSerializerOptions.Standard.WithResolver(StandardResolver.Instance);

	// Telemetry tracking
	private int _telemetryCount = 0;
	private double _lastTelemetryTime = 0;

	// Content path configuration - relative to game-godot directory
	private string _contentBasePath = "../content";
	private float _cameraDistance = 250.0f * 50.0f; // Start a bit farther away
	private const float MinCameraDistance = 50.0f * 50.0f;
	private const float MaxCameraDistance = 500.0f * 50.0f;
	private const float ZoomSpeed = 20.0f * 50.0f;

	// Free camera controls
	private bool _isMouseDragging = false;
	private float _cameraYaw = 0.0f;
	private float _cameraPitch = -45.0f; // Start looking down
	private Vector3 _cameraPosition = new Vector3(0, 5, 5) * 50.0f; // Close to track surface
	private bool _useFreeCam = false; // Start with follow cam in demo mode

	// Camera follow settings
	private Vector3 _cameraFollowOffset = new Vector3(0, 1.4f, 0.2f) * 50.0f; // Cockpit: inside the car
	private float _cameraFollowSmoothness = 5.0f;

	private CameraViewMode _currentViewMode = CameraViewMode.Cockpit;

	// Camera offset presets for each view mode
	private readonly Dictionary<CameraViewMode, Vector3> _cameraViewOffsets = new Dictionary<CameraViewMode, Vector3>
	{
		{ CameraViewMode.Chase, new Vector3(0, 3, 4) * 50.0f },  // Chase: behind and above
		{ CameraViewMode.Hood, new Vector3(0, 1, 2) * 50.0f },    // Hood: low and close
		{ CameraViewMode.Cockpit, new Vector3(0, 1.4f, 0.2f) * 50.0f }  // Cockpit: inside the car (driver eye height)
	};

	public override void _Ready()
	{
		GD.Print("=== TrackRenderer._Ready() called - track_view scene is loading ===");

		try
		{
			_network = GetNode<NetworkClient>("/root/Network");
			_network.SessionJoined += OnSessionJoined;
			_network.TelemetryReceived += OnTelemetryReceived;
			GD.Print("✓ Network client connected successfully");

			// Get the player's selected car from lobby state
			var lobbyState = _network.LastLobbyState;
			if (lobbyState != null)
			{
				var player = Array.Find(lobbyState.PlayersInLobby, p => p.Id == _network.PlayerId);
				if (player != null && !string.IsNullOrEmpty(player.SelectedCar))
				{
					_selectedCarId = player.SelectedCar;
					GD.Print($"✓ Player selected car: {_selectedCarId}");
				}
			}
		}
		catch (System.Exception ex)
		{
			GD.PrintErr($"✗ Error getting Network: {ex.Message}");
		}

		try
		{
			// Get camera reference (sibling node)
			_camera = GetNode<Camera3D>("../Camera3D");
			if (_camera != null)
			{
				_camera.Far = 200000.0f; // Increase draw distance for 50x scale
			}
		}
		catch (System.Exception ex)
		{
			GD.PrintErr($"✗ Error getting Camera3D: {ex.Message}");
		}

		try
		{
			// Setup back button (in UI CanvasLayer)
			_backButton = GetNode<Button>("../UI/BackButton");
			_backButton.Pressed += OnBackButtonPressed;
		}
		catch (System.Exception ex)
		{
			GD.PrintErr($"✗ Error getting BackButton: {ex.Message}");
		}

		try
		{
			// Setup telemetry label
			_telemetryLabel = GetNode<Label>("../UI/TelemetryLabel");
		}
		catch (System.Exception ex)
		{
			GD.PrintErr($"✗ Error getting TelemetryLabel: {ex.Message}");
		}

		try
		{
			// Create the track mesh instance
			_trackMesh = new MeshInstance3D();
			AddChild(_trackMesh);
		}
		catch (System.Exception ex)
		{
			GD.PrintErr($"✗ Error creating track mesh: {ex.Message}");
		}

		try
		{
			// Check if we're already in a session (signal may have fired before we subscribed)
			var currentSessionId = _network?.CurrentSessionId;
			if (!string.IsNullOrEmpty(currentSessionId))
			{
				_currentSessionId = currentSessionId;

				// Find the session in lobby state to get track file
				var lobbyState = _network?.LastLobbyState;
				if (lobbyState != null)
				{
					foreach (var session in lobbyState.AvailableSessions)
					{
						if (session.Id == currentSessionId)
						{
							if (!string.IsNullOrEmpty(session.TrackFile))
							{
								LoadAndRenderTrack(session.TrackFile);
								// Also load the car model if we're already in a session
								LoadDemoCarModel();
							}
							break;
						}
					}
				}
			}
		}
		catch (System.Exception ex)
		{
			GD.PrintErr($"✗ Error checking for existing session: {ex.Message}");
			GD.PrintErr($"Stack trace: {ex.StackTrace}");
		}
	}

	private void OnBackButtonPressed()
	{
		// Leave session if in one
		if (_currentSessionId != null)
		{
			_ = _network!.LeaveSessionAsync();
		}

		// Return to main menu
		GetTree().ChangeSceneToFile("res://scenes/main_menu.tscn");
	}

	public override void _Input(InputEvent @event)
	{
		if (@event is InputEventKey keyEvent && keyEvent.Pressed)
		{
			// Toggle camera mode with Tab key
			if (keyEvent.Keycode == Key.Tab)
			{
				_useFreeCam = !_useFreeCam;
				GD.Print($"Camera mode: {(_useFreeCam ? "Free Cam" : "Follow Cam")}");
			}
			// Cycle camera view with F1
			else if (keyEvent.Keycode == Key.F1)
			{
				_currentViewMode = (CameraViewMode)(((int)_currentViewMode + 1) % ((int)CameraViewMode.Cockpit + 1));
				_cameraFollowOffset = _cameraViewOffsets[_currentViewMode];

				string viewName = _currentViewMode switch
				{
					CameraViewMode.Chase => "Chase Camera",
					CameraViewMode.Hood => "Hood Camera",
					CameraViewMode.Cockpit => "Cockpit Camera",
					_ => "Unknown"
				};
				GD.Print($"Camera view: {viewName}");
			}
		}

		if (@event is InputEventMouseButton mouseEvent)
		{
			// Handle mousewheel zoom
			if (mouseEvent.ButtonIndex == MouseButton.WheelUp && mouseEvent.Pressed)
			{
				_cameraDistance = Mathf.Max(MinCameraDistance, _cameraDistance - ZoomSpeed);
				// Adjust follow offset distance
				_cameraFollowOffset.Z = Mathf.Max(10, _cameraFollowOffset.Z - 5);
			}
			else if (mouseEvent.ButtonIndex == MouseButton.WheelDown && mouseEvent.Pressed)
			{
				_cameraDistance = Mathf.Min(MaxCameraDistance, _cameraDistance + ZoomSpeed);
				// Adjust follow offset distance
				_cameraFollowOffset.Z = Mathf.Min(50, _cameraFollowOffset.Z + 5);
			}
			// Right-click to look around
			else if (mouseEvent.ButtonIndex == MouseButton.Right)
			{
				_isMouseDragging = mouseEvent.Pressed;
				if (mouseEvent.Pressed)
				{
					Input.MouseMode = Input.MouseModeEnum.Captured;
				}
				else
				{
					Input.MouseMode = Input.MouseModeEnum.Visible;
				}
			}
		}
		else if (@event is InputEventMouseMotion motionEvent && _isMouseDragging)
		{
			// Rotate camera with mouse movement
			_cameraYaw -= motionEvent.Relative.X * 0.2f;
			_cameraPitch -= motionEvent.Relative.Y * 0.2f;
			_cameraPitch = Mathf.Clamp(_cameraPitch, -89.0f, 89.0f);
		}
	}

	private int _frameCount = 0;

	public override void _Process(double delta)
	{
		if (_camera == null) return;

		if (_useFreeCam)
		{
			// Free camera mode - WASD movement and mouse look
			float moveSpeed = 50.0f * 50.0f * (float)delta;

			// WASD movement in camera's local space
			Vector3 forward = -_camera.Transform.Basis.Z; // Camera's forward direction
			Vector3 right = _camera.Transform.Basis.X;    // Camera's right direction

			if (Input.IsKeyPressed(Key.W))
				_cameraPosition += forward * moveSpeed;
			if (Input.IsKeyPressed(Key.S))
				_cameraPosition -= forward * moveSpeed;
			if (Input.IsKeyPressed(Key.A))
				_cameraPosition -= right * moveSpeed;
			if (Input.IsKeyPressed(Key.D))
				_cameraPosition += right * moveSpeed;
			if (Input.IsKeyPressed(Key.E))
				_cameraPosition += Vector3.Up * moveSpeed;
			if (Input.IsKeyPressed(Key.Q))
				_cameraPosition -= Vector3.Up * moveSpeed;

			// Apply position
			_camera.Position = _cameraPosition;

			// Apply rotation from mouse look
			_camera.RotationDegrees = new Vector3(_cameraPitch, _cameraYaw, 0);
		}
		else
		{
			// Follow camera mode - follows the car
			if (_demoCarModel != null && _demoCarModel.Visible)
			{
				// Get car's position and rotation
				Vector3 carPosition = _demoCarModel.Position;
				Vector3 carRotation = _demoCarModel.Rotation;

				// Calculate camera position behind and above the car
				// Rotate the offset by the car's yaw to follow behind
				float carYaw = carRotation.Y;
				Vector3 rotatedOffset = new Vector3(
					_cameraFollowOffset.X * Mathf.Cos(carYaw) - _cameraFollowOffset.Z * Mathf.Sin(carYaw),
					_cameraFollowOffset.Y,
					_cameraFollowOffset.X * Mathf.Sin(carYaw) + _cameraFollowOffset.Z * Mathf.Cos(carYaw)
				);

				Vector3 targetPosition = carPosition + rotatedOffset;

				// Smoothly interpolate camera position
				if (_currentViewMode == CameraViewMode.Cockpit || _currentViewMode == CameraViewMode.Hood)
				{
					// For cockpit/hood, move instantly and match rotation
					_camera.Position = targetPosition;
					
					// Match car rotation but stay level? Or matches pitch/roll? 
					// For now, simpler implementation: Look at a point far ahead of the car
					Vector3 forwardOffset = new Vector3(
						-Mathf.Sin(carYaw),
						0, // Keep level for now or pitch with car?
						-Mathf.Cos(carYaw)
					) * 1000.0f; // Look far ahead
					
					// Note: Car model is rotated 180 degrees in previous logic (LookAt(back)). 
					// Let's rely on carRotation again.
					// If carRotation.Y is the yaw.
					
					_camera.Rotation = new Vector3(0, carRotation.Y + Mathf.Pi, 0); // +PI because model is flipped?
					
					// Re-verify the flip logic from OnTelemetryReceived:
					// _demoCarModel.LookAt(nextPosition, Vector3.Up);
					// nextPosition = carPosition - ... (direction negated).
					// So car model +Z axis points "Forward" in world space? No, LookAt makes -Z point to target.
					// If target is "behind" (velocity negated), then -Z points backwards. +Z points forwards.
					// So the model is BACKWARDS.
					
					// If we want Camera to look Forward. Forward is direction of velocity.
					// Velocity direction has angle `carState.YawRad`.
					// Godot 0 angle is South? (Z+)
					// Math.Cos/Sin usages suggests: X = Cos, Y(Z) = Sin? No, usage is `Speed * Cos(Yaw)`
					
					// Better approach: Look at where the car is heading!
					// In OnTelemetryReceived calculated `nextPosition` (which was actually previous/behind position?)
					
					// Let's just use the rotation that looks "Forward" relative to the car.
					// If the car model is flipped 180, we rotate camera 180 relative to it.
					// Or just LookAt(carPosition + CarForwardVector * 1000)
					
					// Get the car's basis
					var carBasis = _demoCarModel.GlobalTransform.Basis;
					// The car model forward (-Z) is actually pointing BACKWARDS due to the fix in OnTelemetry.
					// So the car's "real forward" is +Z.
					Vector3 realForward = carBasis.Z; 
					
					_camera.LookAt(_camera.Position + realForward * 100.0f, Vector3.Up);
				}
				else
				{
					_camera.Position = _camera.Position.Lerp(targetPosition, _cameraFollowSmoothness * (float)delta);
					// Look at the car
					_camera.LookAt(carPosition, Vector3.Up);
				}
			}
			else
			{
				// Fallback to orbital camera mode if no car
				float rotationSpeed = 0.3f;
				_cameraAngle += rotationSpeed * (float)delta;

				float radius = _cameraDistance * 0.5f;
				float height = _cameraDistance * 1.2f;

				Vector3 cameraPos = new Vector3(
					radius * Mathf.Cos(_cameraAngle),
					height,
					radius * Mathf.Sin(_cameraAngle)
				);

				_camera.Position = cameraPos;
				_camera.LookAt(Vector3.Zero, Vector3.Up);
			}
		}
	}

	private void OnSessionJoined(string sessionId, byte gridPosition)
	{
		_currentSessionId = sessionId;

		// Get track name from lobby state
		var lobbyState = _network?.LastLobbyState;
		if (lobbyState == null)
		{
			GD.PrintErr("No lobby state available, cannot determine track");
			return;
		}

		// Find the session to get track name and file
		var session = Array.Find(lobbyState.AvailableSessions, s => s.Id == sessionId);
		if (session == null)
		{
			GD.PrintErr($"Session {sessionId} not found in lobby state");
			return;
		}

		LoadAndRenderTrack(session.TrackFile);
		LoadDemoCarModel();
	}

	private void LoadDemoCarModel()
	{
		// In demo mode, if no car is selected, use the first available car from lobby state
		if (string.IsNullOrEmpty(_selectedCarId))
		{
			var lobbyState = _network?.LastLobbyState;
			if (lobbyState?.CarConfigs != null && lobbyState.CarConfigs.Length > 0)
			{
				_selectedCarId = lobbyState.CarConfigs[0].Id;
				GD.Print($"No car selected, using first available car: {_selectedCarId}");
			}
			else
			{
				GD.PrintErr("No car selected and no cars available in lobby state");
				return;
			}
		}

		var carCache = CarModelCache.Instance;
		if (carCache == null)
		{
			GD.PrintErr("CarModelCache not available");
			return;
		}

		_demoCarModel = carCache.GetModel(_selectedCarId);
		if (_demoCarModel == null)
		{
			GD.PrintErr($"Failed to load car model for {_selectedCarId}");
			return;
		}

		// Scale up the car model to make it more visible (car models might be too small)
		_demoCarModel.Scale = new Vector3(50.0f, 50.0f, 50.0f);

		// Add the car model to the scene but initially invisible
		AddChild(_demoCarModel);
		_demoCarModel.Visible = false;
		GD.Print($"✓ Loaded demo car model: {_demoCarModel.Name}");
	}

	private void OnTelemetryReceived()
	{
		_telemetryCount++;
		_lastTelemetryTime = Time.GetTicksMsec() / 1000.0;

		var telemetry = _network?.LastTelemetry;
		if (telemetry == null)
		{
			if (_telemetryLabel != null)
			{
				_telemetryLabel.Text = $"Telemetry: Received {_telemetryCount}, but data is null!";
			}
			return;
		}

		// Update telemetry display
		if (_telemetryLabel != null)
		{
			string statusText = $"Mode: {telemetry.GameMode}\n";

			if (_useFreeCam)
			{
				statusText += "Camera: Free (Tab to toggle)\n";
			}
			else
			{
				string viewName = _currentViewMode switch
				{
					CameraViewMode.Chase => "Chase",
					CameraViewMode.Hood => "Hood",
					CameraViewMode.Cockpit => "Cockpit",
					_ => "Unknown"
				};
				statusText += $"Camera: {viewName} (Tab/F1)\n";
			}

			if (telemetry.CarStates.Length > 0)
			{
				// In demo lap mode, show the AI driver's data (not the local player's)
				var state = telemetry.CarStates[0];
				if (telemetry.GameMode == GameMode.DemoLap)
				{
					// Filter out the local player's car and find the AI driver
					var aiCarStates = telemetry.CarStates.Where(cs => cs.PlayerId != _network?.PlayerId).ToArray();
					if (aiCarStates.Length > 0)
					{
						// Use the AI car with the most progress
						state = aiCarStates[0];
						foreach (var car in aiCarStates)
						{
							if (car.CurrentLap > state.CurrentLap ||
								(car.CurrentLap == state.CurrentLap && car.TrackProgress > state.TrackProgress))
							{
								state = car;
							}
						}
					}
				}

				// Display current lap time (from server)
				if (state.CurrentLap > 0 && state.CurrentLapTimeMs > 0)
				{
					float currentLapSeconds = state.CurrentLapTimeMs / 1000.0f;
					int minutes = (int)(currentLapSeconds / 60);
					float seconds = currentLapSeconds % 60;
					statusText += $"Current Lap: {minutes}:{seconds:00.000}\n";
				}
				else
				{
					statusText += "Current Lap: --:--.---\n";
				}

				// Display previous lap time
				if (state.LastLapTimeMs.HasValue)
				{
					float lastLapSeconds = state.LastLapTimeMs.Value / 1000.0f;
					int minutes = (int)(lastLapSeconds / 60);
					float seconds = lastLapSeconds % 60;
					statusText += $"Previous Lap: {minutes}:{seconds:00.000}\n";
				}
				else
				{
					statusText += "Previous Lap: --:--.---\n";
				}

				// Convert speed from m/s to km/h
				float speedKmh = state.SpeedMps * 3.6f;
				statusText += $"Speed: {speedKmh:F0} km/h";
			}
			else
			{
				statusText += "No car data!";
			}

			_telemetryLabel.Text = statusText;
		}

		// Only render car in demo mode
		if (_demoCarModel == null)
		{
			GD.Print($"[Telemetry #{_telemetryCount}] Car model is null!");
			return;
		}

		if (telemetry.GameMode != GameMode.DemoLap)
		{
			_demoCarModel.Visible = false;
			return;
		}

		// Find the car state (in demo mode there should be exactly one AI driver)
		if (telemetry.CarStates.Length == 0)
		{
			GD.Print($"[Telemetry #{_telemetryCount}] No car states in telemetry!");
			return;
		}

		// In demo lap mode, show the AI driver's car (not the local player's car)
		// Filter out the local player's car and find the AI driver
		var aiCars = telemetry.CarStates.Where(cs => cs.PlayerId != _network?.PlayerId).ToArray();

		if (aiCars.Length == 0)
		{
			if (_telemetryCount <= 5)
			{
				GD.Print($"[Telemetry #{_telemetryCount}] No AI cars found in telemetry! Local player ID: {_network?.PlayerId}");
			}
			_demoCarModel.Visible = false;
			return;
		}

		// Use the first AI car (there should only be one in demo mode)
		var carState = aiCars[0];

		// Debug: Log player info to verify this is the AI
		if (_telemetryCount <= 5)
		{
			GD.Print($"[Telemetry #{_telemetryCount}] Showing demo car for AI player: {carState.PlayerId} (excluding local player: {_network?.PlayerId})");
		}

		// Log first few telemetry updates for debugging
		if (_telemetryCount <= 5)
		{
			GD.Print($"[Telemetry #{_telemetryCount}] Pos: ({carState.PosX:F2}, {carState.PosY:F2}, {carState.PosZ:F2}), " +
					 $"Yaw: {carState.YawRad:F2} rad ({carState.YawRad * 180.0f / Mathf.Pi:F1}°), Speed: {carState.SpeedMps:F1} m/s, Progress: {carState.TrackProgress:F2}");
		}

		// Additional debug logging every 60 frames (quarter second at 240Hz)
		// if (_telemetryCount % 60 == 0)
		// {
		// 	GD.Print($"[Debug #{_telemetryCount}] Server yaw: {carState.YawRad:F2} rad, Godot yaw will be: {-carState.YawRad:F2} rad");
		// }

		// Update car position and orientation
		// Server coordinates: X, Y, Z
		// Godot coordinates: X (same), Y (height/Z), Z (-Y)
		var carPosition = new Vector3(
			carState.PosX,
			carState.PosZ,  // Z becomes Y (height)
			-carState.PosY  // Y becomes -Z (flipped)
		) * 50.0f;

		_demoCarModel.Position = carPosition;

		// Calculate direction from position change (use previous position if available)
		// This ensures the car faces the direction it's actually moving
		if (_telemetryCount > 1)
		{
			// Use LookAt to orient the car toward where it's going
			// Get the direction vector from velocity or calculate it
			// Negate the direction because the car model's "forward" is actually its back
			var nextPosition = carPosition - new Vector3(
				carState.SpeedMps * Mathf.Cos(carState.YawRad) * 0.1f,
				0,
				-carState.SpeedMps * Mathf.Sin(carState.YawRad) * 0.1f
			) * 50.0f;

			// Look at the next position to face forward
			_demoCarModel.LookAt(nextPosition, Vector3.Up);
		}

		// Make car visible
		_demoCarModel.Visible = true;
	}

	private void GenerateTestTrack()
	{
		// For testing in sandbox mode without joining a session
		// This should only be called when NOT in a session (direct scene load)
		GenerateCircularTrack();
	}

	private void LoadAndRenderTrack(string trackFile)
	{
		// trackFile is relative to content folder, e.g. "tracks/real/Austin.yaml"
		string trackPath = Path.Combine(_contentBasePath, trackFile);

		// Convert to absolute path
		trackPath = ProjectSettings.GlobalizePath("res://").PathJoin(trackPath);

		try
		{
			var trackData = LoadTrackFromYaml(trackPath);

			// Try to load procedural terrain data
			var terrainData = LoadProceduralTerrainData(trackPath);
			if (terrainData != null)
			{
				GD.Print($"✅ Loaded procedural terrain data for: {trackData.Name}");
				GD.Print($"   Using procedural world rendering (terrain only)");
				RenderProceduralTerrain(terrainData);

				// Store heightmap for track elevation
				_currentHeightmap = terrainData.Heightmap;

				// Still need to render track surface and markings on top of terrain
				GD.Print($"   Rendering track surface on top of procedural terrain");
				// Fall through to render track mesh
			}
			else
			{
				GD.Print($"ℹ️  No procedural terrain data found for: {trackData.Name}");
				GD.Print($"   Using legacy rendering (flat ground plane)");
				_currentHeightmap = null;
			}

			// Convert to track points and widths
			var centerline = new List<Vector3>();
			var widths = new List<float>();

			float minElevation = float.MaxValue;
			float maxElevation = float.MinValue;
			int elevationAdjustedCount = 0;

			foreach (var node in trackData.Nodes)
			{
				// YAML format: x, y are horizontal plane, z is elevation
				// Godot: X=x, Y=z (elevation/height), Z=y (horizontal)
				// Flip Y-axis to match server/world orientation (track previously mirrored)
				float worldX = node.X;
				float worldY = node.Y;
				float trackElevation = node.Z;

				// Sample terrain height if available
				float terrainHeight = 0.0f;
				if (_currentHeightmap != null)
				{
					terrainHeight = _currentHeightmap.Sample(worldX, worldY);
				}

				// Use the higher of terrain height or track elevation
				// This ensures the track sits on or above the terrain
				float finalElevation = Mathf.Max(terrainHeight, trackElevation);

				if (terrainHeight > trackElevation)
				{
					elevationAdjustedCount++;
				}

				minElevation = Mathf.Min(minElevation, finalElevation);
				maxElevation = Mathf.Max(maxElevation, finalElevation);

				centerline.Add(new Vector3(worldX, finalElevation, -worldY) * 50.0f);

				// Get track width
				float width;
				if (trackData.DefaultWidth.HasValue)
				{
					width = trackData.DefaultWidth.Value;
				}
				else if (node.Width.HasValue)
				{
					width = node.Width.Value;
				}
				else
				{
					width = (node.WidthLeft ?? 5.0f) + (node.WidthRight ?? 5.0f);
				}
				width = Mathf.Max(width, 8.0f);
				widths.Add(width * 50.0f);
			}

			// Log elevation summary
			if (_currentHeightmap != null)
			{
				GD.Print($"   Track elevation range: {minElevation:F1}m to {maxElevation:F1}m");
				GD.Print($"   Adjusted {elevationAdjustedCount}/{centerline.Count} points to match terrain");
			}

			// Only generate ground plane if no procedural terrain
			if (terrainData == null)
			{
				GenerateGroundPlane(centerline, widths);
			}

			GenerateTrackMeshSpline(centerline, widths);
			// GenerateWhiteBorderArea(centerline, widths);  // Disabled - kerbs look better
			GenerateDottedOutlineSpline(centerline, widths);  // This is now proper kerbs
			GenerateStartFinishGrid(centerline, widths);

			// Add debug spheres at track points to see where they are
			// AddDebugMarkers(centerline);
		}
		catch (Exception ex)
		{
			GD.PrintErr($"Failed to load track {trackFile}: {ex.Message}");
			GD.PrintErr($"Stack trace: {ex.StackTrace}");

			// Fallback to simple circular track
			GenerateCircularTrack();
		}
	}

	private TrackData LoadTrackFromYaml(string filePath)
	{
		var yaml = File.ReadAllText(filePath);
		var deserializer = new DeserializerBuilder()
			.WithNamingConvention(UnderscoredNamingConvention.Instance)
			.Build();
		return deserializer.Deserialize<TrackData>(yaml);
	}

	private void GenerateCircularTrack()
	{
		var trackPoints = new List<Vector3>();
		var numPoints = 60;
		var radius = 100.0f * 50.0f;

		for (int i = 0; i < numPoints; i++)
		{
			float angle = 2.0f * Mathf.Pi * i / numPoints;
			float x = radius * Mathf.Cos(angle);
			float y = 0.0f; // Flat for now
			float z = radius * Mathf.Sin(angle);
			trackPoints.Add(new Vector3(x, y, z));
		}

		var widths = new List<float>();
		for (int i = 0; i < trackPoints.Count; i++)
		{
			widths.Add(12.0f * 50.0f);
		}
		GenerateGroundPlane(trackPoints, widths);
		GenerateTrackMesh(trackPoints, 12.0f * 50.0f);

		// Add debug spheres at track points to see where they are
		// AddDebugMarkers(trackPoints);
	}

	private void AddDebugMarkers(List<Vector3> points)
	{
		for (int i = 0; i < points.Count; i += 10) // Every 10th point
		{
			var sphere = new MeshInstance3D();
			var sphereMesh = new SphereMesh();
			sphereMesh.Radius = 2.0f;
			sphereMesh.Height = 4.0f;
			sphere.Mesh = sphereMesh;

			var material = new StandardMaterial3D();
			material.AlbedoColor = new Color(1, 0, 0, 0.1f); // Red, 90% transparent
			material.EmissionEnabled = true;
			material.Emission = new Color(1, 0, 0);
			material.EmissionEnergyMultiplier = 2.0f;

			sphere.Mesh.SurfaceSetMaterial(0, material);
			sphere.Position = points[i] + new Vector3(0, 2, 0); // 2 units above track

			AddChild(sphere);
		}
	}

	private List<Vector3> CatmullRom(List<Vector3> pts, int samplesPerSeg, bool closed)
	{
		var result = new List<Vector3>();
		if (pts.Count < 2) return result;

		int count = pts.Count;
		for (int i = 0; i < count; i++)
		{
			Vector3 p0 = pts[(i - 1 + count) % count];
			Vector3 p1 = pts[i];
			Vector3 p2 = pts[(i + 1) % count];
			Vector3 p3 = pts[(i + 2) % count];

			for (int s = 0; s < samplesPerSeg; s++)
			{
				float t = (float)s / samplesPerSeg;
				float t2 = t * t;
				float t3 = t2 * t;

				// Catmull-Rom spline basis
				Vector3 point = 0.5f * (
					(2f * p1) +
					(-p0 + p2) * t +
					(2f * p0 - 5f * p1 + 4f * p2 - p3) * t2 +
					(-p0 + 3f * p1 - 3f * p2 + p3) * t3
				);
				result.Add(point);
			}
		}

		if (!closed)
		{
			// Append last point explicitly for open curves
			result.Add(pts[count - 1]);
		}

		return result;
	}

	private void GenerateTrackMeshSpline(List<Vector3> centerline, List<float> widths)
	{
		if (centerline.Count < 2 || centerline.Count != widths.Count)
		{
			GD.PrintErr("Invalid centerline/width data for spline track generation");
			return;
		}

		bool closed = true;
		int samplesPerSeg = 8;

		// Build raw edges
		var leftRaw = new List<Vector3>();
		var rightRaw = new List<Vector3>();
		for (int i = 0; i < centerline.Count; i++)
		{
			int nextIdx = (i + 1) % centerline.Count;
			Vector3 current = centerline[i];
			Vector3 next = centerline[nextIdx];
			float width = widths[i];

			Vector3 dir = (next - current).Normalized();
			Vector3 right = new Vector3(-dir.Z, 0, dir.X).Normalized();

			leftRaw.Add(current - right * (width * 0.5f));
			rightRaw.Add(current + right * (width * 0.5f));
		}

		// Resample edges with Catmull-Rom for smoothness
		var left = CatmullRom(leftRaw, samplesPerSeg, closed);
		var rightResampled = CatmullRom(rightRaw, samplesPerSeg, closed);

		if (left.Count != rightResampled.Count || left.Count < 2)
		{
			GD.PrintErr("Resampled edge counts mismatch or too small");
			return;
		}

		var surfaceTool = new SurfaceTool();
		surfaceTool.Begin(Mesh.PrimitiveType.Triangles);

		var asphaltColor = new Color(0.3f, 0.3f, 0.3f); // Dark gray asphalt
		int segCount = left.Count;
		for (int i = 0; i < segCount; i++)
		{
			int nextIdx = (i + 1) % segCount;

			Vector3 l0 = left[i];
			Vector3 r0 = rightResampled[i];
			Vector3 l1 = left[nextIdx];
			Vector3 r1 = rightResampled[nextIdx];

			// Triangle 1
			surfaceTool.SetColor(asphaltColor);
			surfaceTool.SetNormal(Vector3.Up);
			surfaceTool.AddVertex(l0);

			surfaceTool.SetColor(asphaltColor);
			surfaceTool.SetNormal(Vector3.Up);
			surfaceTool.AddVertex(r0);

			surfaceTool.SetColor(asphaltColor);
			surfaceTool.SetNormal(Vector3.Up);
			surfaceTool.AddVertex(l1);

			// Triangle 2
			surfaceTool.SetColor(asphaltColor);
			surfaceTool.SetNormal(Vector3.Up);
			surfaceTool.AddVertex(r0);

			surfaceTool.SetColor(asphaltColor);
			surfaceTool.SetNormal(Vector3.Up);
			surfaceTool.AddVertex(r1);

			surfaceTool.SetColor(asphaltColor);
			surfaceTool.SetNormal(Vector3.Up);
			surfaceTool.AddVertex(l1);
		}

		var mesh = surfaceTool.Commit();
		if (mesh.GetSurfaceCount() == 0)
		{
			GD.PrintErr("ERROR: Mesh has no surfaces after commit!");
			return;
		}

		var material = new StandardMaterial3D();
		material.VertexColorUseAsAlbedo = true;
		material.Roughness = 0.9f;
		material.Metallic = 0.0f;
		material.ShadingMode = BaseMaterial3D.ShadingModeEnum.Unshaded;
		material.CullMode = BaseMaterial3D.CullModeEnum.Disabled;

		mesh.SurfaceSetMaterial(0, material);
		_trackMesh!.Mesh = mesh;
		_trackMesh.Visible = true;
		_trackMesh.CastShadow = GeometryInstance3D.ShadowCastingSetting.Off;
	}

	private void GenerateTrackMesh(List<Vector3> centerline, float trackWidth)
	{
		if (centerline.Count < 3)
		{
			GD.PrintErr("Not enough track points to generate mesh");
			return;
		}

		var surfaceTool = new SurfaceTool();
		surfaceTool.Begin(Mesh.PrimitiveType.Triangles);

		// Generate track surface
		for (int i = 0; i < centerline.Count; i++)
		{
			int nextIdx = (i + 1) % centerline.Count;

			Vector3 current = centerline[i];
			Vector3 next = centerline[nextIdx];

			// Calculate track direction
			Vector3 direction = (next - current).Normalized();

			// Calculate perpendicular vector (right direction)
			Vector3 right = new Vector3(-direction.Z, 0, direction.X).Normalized();

			// Calculate left and right edge points
			Vector3 leftEdge = current - right * (trackWidth * 0.5f);
			Vector3 rightEdge = current + right * (trackWidth * 0.5f);
			Vector3 nextLeftEdge = next - right * (trackWidth * 0.5f);
			Vector3 nextRightEdge = next + right * (trackWidth * 0.5f);

			// Add track surface color (BLACK for visibility against green background)
			var asphaltColor = new Color(0.0f, 0.0f, 0.0f);

			// First triangle (left-current, right-current, left-next)
			surfaceTool.SetColor(asphaltColor);
			surfaceTool.SetNormal(Vector3.Up);
			surfaceTool.SetUV(new Vector2(0, (float)i / centerline.Count));
			surfaceTool.AddVertex(leftEdge);

			surfaceTool.SetColor(asphaltColor);
			surfaceTool.SetNormal(Vector3.Up);
			surfaceTool.SetUV(new Vector2(1, (float)i / centerline.Count));
			surfaceTool.AddVertex(rightEdge);

			surfaceTool.SetColor(asphaltColor);
			surfaceTool.SetNormal(Vector3.Up);
			surfaceTool.SetUV(new Vector2(0, (float)nextIdx / centerline.Count));
			surfaceTool.AddVertex(nextLeftEdge);

			// Second triangle (right-current, right-next, left-next)
			surfaceTool.SetColor(asphaltColor);
			surfaceTool.SetNormal(Vector3.Up);
			surfaceTool.SetUV(new Vector2(1, (float)i / centerline.Count));
			surfaceTool.AddVertex(rightEdge);

			surfaceTool.SetColor(asphaltColor);
			surfaceTool.SetNormal(Vector3.Up);
			surfaceTool.SetUV(new Vector2(1, (float)nextIdx / centerline.Count));
			surfaceTool.AddVertex(nextRightEdge);

			surfaceTool.SetColor(asphaltColor);
			surfaceTool.SetNormal(Vector3.Up);
			surfaceTool.SetUV(new Vector2(0, (float)nextIdx / centerline.Count));
			surfaceTool.AddVertex(nextLeftEdge);
		}

		// Generate the mesh
		var mesh = surfaceTool.Commit();

		if (mesh.GetSurfaceCount() == 0)
		{
			GD.PrintErr("ERROR: Mesh has no surfaces after commit!");
			return;
		}

		// Create material with vertex colors and make it unlit/emissive so we can see it
		var material = new StandardMaterial3D();
		material.VertexColorUseAsAlbedo = true;
		material.Roughness = 0.9f;
		material.Metallic = 0.0f;
		material.ShadingMode = BaseMaterial3D.ShadingModeEnum.Unshaded; // Make it always visible
		material.CullMode = BaseMaterial3D.CullModeEnum.Disabled; // Disable backface culling for debugging

		mesh.SurfaceSetMaterial(0, material);
		_trackMesh!.Mesh = mesh;
		_trackMesh.Visible = true;
		_trackMesh.CastShadow = GeometryInstance3D.ShadowCastingSetting.Off;

		// Add edge markings
		GenerateTrackMarkings(centerline, trackWidth);
	}

	private void GenerateDottedOutlineSpline(List<Vector3> centerline, List<float> widths)
	{
		if (centerline.Count < 2 || centerline.Count != widths.Count)
			return;

		bool closed = true;
		int samplesPerSeg = 8;

		// Build raw edges
		var leftRaw = new List<Vector3>();
		var rightRaw = new List<Vector3>();
		for (int i = 0; i < centerline.Count; i++)
		{
			int nextIdx = (i + 1) % centerline.Count;
			Vector3 current = centerline[i];
			Vector3 next = centerline[nextIdx];
			float width = widths[i];

			Vector3 dir = (next - current).Normalized();
			Vector3 right = new Vector3(-dir.Z, 0, dir.X).Normalized();

			leftRaw.Add(current - right * (width * 0.5f));
			rightRaw.Add(current + right * (width * 0.5f));
		}

		// Resample edges
		var left = CatmullRom(leftRaw, samplesPerSeg, closed);
		var rightEdge = CatmullRom(rightRaw, samplesPerSeg, closed);

		if (left.Count != rightEdge.Count || left.Count < 2)
			return;

		// Calculate curvature at each point to determine where kerbs should be placed
		var curvatures = new List<float>();
		for (int i = 0; i < centerline.Count; i++)
		{
			int prevIdx = (i - 1 + centerline.Count) % centerline.Count;
			int nextIdx = (i + 1) % centerline.Count;

			Vector3 prev = centerline[prevIdx];
			Vector3 current = centerline[i];
			Vector3 next = centerline[nextIdx];

			// Calculate curvature using the angle between direction vectors
			Vector3 v1 = (current - prev).Normalized();
			Vector3 v2 = (next - current).Normalized();

			float dotProduct = Mathf.Clamp(v1.Dot(v2), -1.0f, 1.0f);
			float angle = Mathf.Acos(dotProduct);
			float distance = (next - current).Length();

			// Curvature = angle / distance (radians per meter)
			float curvature = distance > 0.01f ? angle / distance : 0.0f;
			curvatures.Add(curvature);

			// Determine turn direction (left or right)
			Vector3 cross = v1.Cross(v2);
			bool isLeftTurn = cross.Y > 0;
		}

		// Threshold for placing kerbs (only at corners with significant curvature)
		float curvatureThreshold = 0.0005f; // Adjust this to control where kerbs appear

		var kerbMesh = new MeshInstance3D();
		AddChild(kerbMesh);

		var surfaceTool = new SurfaceTool();
		surfaceTool.Begin(Mesh.PrimitiveType.Triangles);

		// Kerb dimensions (realistic FIA kerbs)
		float kerbWidth = 0.3f * 50.0f;     // 30cm wide kerbs (placed at track edge)
		float kerbHeight = 0.1f * 50.0f;    // 10cm high (raised kerb)
		float kerbYOffset = 0.2f * 50.0f;   // Slightly raised above track surface

		// Stripe pattern
		float stripeLength = 1.0f * 50.0f;
		float leftAccumDist = 0.0f;
		float rightAccumDist = 0.0f;
		int leftStripeIdx = 0;
		int rightStripeIdx = 0;

		int segCount = left.Count;
		int kerbSegmentsPlaced = 0;
		int samplesPerRawSeg = samplesPerSeg;

		for (int i = 0; i < segCount; i++)
		{
			int nextIdx = (i + 1) % segCount;

			// Map back to original centerline index to check curvature
			int rawIdx = i / samplesPerRawSeg;
			if (rawIdx >= curvatures.Count) rawIdx = curvatures.Count - 1;

			float curvature = curvatures[rawIdx];

			// Determine turn direction at this point
			int prevRawIdx = (rawIdx - 1 + centerline.Count) % centerline.Count;
			int nextRawIdx = (rawIdx + 1) % centerline.Count;
			Vector3 v1 = (centerline[rawIdx] - centerline[prevRawIdx]).Normalized();
			Vector3 v2 = (centerline[nextRawIdx] - centerline[rawIdx]).Normalized();
			Vector3 cross = v1.Cross(v2);
			bool isLeftTurn = cross.Y > 0;

			// Only place kerbs at corners with sufficient curvature
			if (curvature < curvatureThreshold)
				continue;

			Vector3 l0 = left[i];
			Vector3 r0 = rightEdge[i];
			Vector3 l1 = left[nextIdx];
			Vector3 r1 = rightEdge[nextIdx];

			Vector3 dir = (r0 + r1) * 0.5f - (l0 + l1) * 0.5f;
			Vector3 offsetDir = (dir.LengthSquared() < 1e-4f)
				? new Vector3(1, 0, 0)
				: dir.Normalized();

			// Place kerbs on the outside of the corner (where racing line would use them)
			if (isLeftTurn)
			{
				// Left turn - place kerb on RIGHT side (outside of turn)
				float segmentLength = r0.DistanceTo(r1);
				rightAccumDist += segmentLength;

				if (rightAccumDist >= stripeLength)
				{
					rightStripeIdx = (rightStripeIdx + 1) % 2;
					rightAccumDist = 0.0f;
				}

				Color kerbColor = (rightStripeIdx == 0) ? new Color(0.9f, 0.0f, 0.0f, 1.0f) : new Color(0.95f, 0.95f, 0.95f, 1.0f);

				// Place kerb partially on track edge (inner) and partially outside (outer)
				Vector3 rightInner0 = r0 - offsetDir * (kerbWidth * 0.3f) + Vector3.Up * kerbYOffset;
				Vector3 rightOuter0 = r0 + offsetDir * (kerbWidth * 0.7f) + Vector3.Up * kerbYOffset;
				Vector3 rightInner1 = r1 - offsetDir * (kerbWidth * 0.3f) + Vector3.Up * kerbYOffset;
				Vector3 rightOuter1 = r1 + offsetDir * (kerbWidth * 0.7f) + Vector3.Up * kerbYOffset;

				Vector3 rightInner0Top = rightInner0 + Vector3.Up * kerbHeight;
				Vector3 rightOuter0Top = rightOuter0 + Vector3.Up * kerbHeight;
				Vector3 rightInner1Top = rightInner1 + Vector3.Up * kerbHeight;
				Vector3 rightOuter1Top = rightOuter1 + Vector3.Up * kerbHeight;

				AddQuad(surfaceTool, rightInner0Top, rightOuter0Top, rightInner1Top, rightOuter1Top, kerbColor);
				AddQuad(surfaceTool, rightInner0Top, rightInner0, rightInner1Top, rightInner1, kerbColor * 0.7f);
				AddQuad(surfaceTool, rightOuter0, rightOuter0Top, rightOuter1, rightOuter1Top, kerbColor * 0.7f);
				kerbSegmentsPlaced++;
			}
			else
			{
				// Right turn - place kerb on LEFT side (outside of turn)
				float segmentLength = l0.DistanceTo(l1);
				leftAccumDist += segmentLength;

				if (leftAccumDist >= stripeLength)
				{
					leftStripeIdx = (leftStripeIdx + 1) % 2;
					leftAccumDist = 0.0f;
				}

				Color kerbColor = (leftStripeIdx == 0) ? new Color(0.9f, 0.0f, 0.0f, 1.0f) : new Color(0.95f, 0.95f, 0.95f, 1.0f);

				// Place kerb partially on track edge (inner) and partially outside (outer)
				Vector3 leftInner0 = l0 + offsetDir * (kerbWidth * 0.3f) + Vector3.Up * kerbYOffset;
				Vector3 leftOuter0 = l0 - offsetDir * (kerbWidth * 0.7f) + Vector3.Up * kerbYOffset;
				Vector3 leftInner1 = l1 + offsetDir * (kerbWidth * 0.3f) + Vector3.Up * kerbYOffset;
				Vector3 leftOuter1 = l1 - offsetDir * (kerbWidth * 0.7f) + Vector3.Up * kerbYOffset;

				Vector3 leftInner0Top = leftInner0 + Vector3.Up * kerbHeight;
				Vector3 leftOuter0Top = leftOuter0 + Vector3.Up * kerbHeight;
				Vector3 leftInner1Top = leftInner1 + Vector3.Up * kerbHeight;
				Vector3 leftOuter1Top = leftOuter1 + Vector3.Up * kerbHeight;

				AddQuad(surfaceTool, leftOuter0Top, leftInner0Top, leftOuter1Top, leftInner1Top, kerbColor);
				AddQuad(surfaceTool, leftInner0, leftInner0Top, leftInner1, leftInner1Top, kerbColor * 0.7f);
				AddQuad(surfaceTool, leftOuter0Top, leftOuter0, leftOuter1Top, leftOuter1, kerbColor * 0.7f);
				kerbSegmentsPlaced++;
			}
		}

		if (kerbSegmentsPlaced > 0)
		{
			var mesh = surfaceTool.Commit();
			var material = new StandardMaterial3D();
			material.VertexColorUseAsAlbedo = true;
			material.Roughness = 0.7f;
			material.Metallic = 0.0f;
			material.ShadingMode = BaseMaterial3D.ShadingModeEnum.Unshaded;
			material.CullMode = BaseMaterial3D.CullModeEnum.Disabled;
			mesh.SurfaceSetMaterial(0, material);
			kerbMesh.Mesh = mesh;

			GD.Print($"✅ Generated 3D kerbs at corners with {kerbSegmentsPlaced} segments");
		}
		else
		{
			kerbMesh.QueueFree();
			GD.Print("ℹ️  No corners detected for kerb placement");
		}
	}

	private void GenerateWhiteBorderArea(List<Vector3> centerline, List<float> widths)
	{
		if (centerline.Count < 2 || centerline.Count != widths.Count)
			return;

		bool closed = true;
		int samplesPerSeg = 8;

		// Build raw edges
		var leftRaw = new List<Vector3>();
		var rightRaw = new List<Vector3>();
		for (int i = 0; i < centerline.Count; i++)
		{
			int nextIdx = (i + 1) % centerline.Count;
			Vector3 current = centerline[i];
			Vector3 next = centerline[nextIdx];
			float width = widths[i];

			Vector3 dir = (next - current).Normalized();
			Vector3 right = new Vector3(-dir.Z, 0, dir.X).Normalized();

			leftRaw.Add(current - right * (width * 0.5f));
			rightRaw.Add(current + right * (width * 0.5f));
		}

		// Resample edges
		var left = CatmullRom(leftRaw, samplesPerSeg, closed);
		var rightEdge = CatmullRom(rightRaw, samplesPerSeg, closed);

		if (left.Count != rightEdge.Count || left.Count < 2)
			return;

		var borderMesh = new MeshInstance3D();
		AddChild(borderMesh);

		var surfaceTool = new SurfaceTool();
		surfaceTool.Begin(Mesh.PrimitiveType.Triangles);

		float borderWidth = 3.0f * 50.0f;  // Width of the white border area
		float yOffset = 0.3f * 50.0f;  // Raised above the track for visibility from top
		var whiteColor = new Color(1, 1, 1);  // White
		int segCount = left.Count;

		for (int i = 0; i < segCount; i++)
		{
			int nextIdx = (i + 1) % segCount;

			Vector3 l0 = left[i];
			Vector3 r0 = rightEdge[i];
			Vector3 l1 = left[nextIdx];
			Vector3 r1 = rightEdge[nextIdx];

			Vector3 dir = (r0 + r1) * 0.5f - (l0 + l1) * 0.5f;
			Vector3 offsetDir = (dir.LengthSquared() < 1e-4f)
				? new Vector3(1, 0, 0)
				: dir.Normalized();

			// Left border
			Vector3 leftInner0 = l0 + Vector3.Up * yOffset;
			Vector3 leftOuter0 = l0 - offsetDir * borderWidth + Vector3.Up * yOffset;
			Vector3 leftInner1 = l1 + Vector3.Up * yOffset;
			Vector3 leftOuter1 = l1 - offsetDir * borderWidth + Vector3.Up * yOffset;

			AddQuad(surfaceTool, leftOuter0, leftInner0, leftOuter1, leftInner1, whiteColor);

			// Right border
			Vector3 rightInner0 = r0 + Vector3.Up * yOffset;
			Vector3 rightOuter0 = r0 + offsetDir * borderWidth + Vector3.Up * yOffset;
			Vector3 rightInner1 = r1 + Vector3.Up * yOffset;
			Vector3 rightOuter1 = r1 + offsetDir * borderWidth + Vector3.Up * yOffset;

			AddQuad(surfaceTool, rightInner0, rightOuter0, rightInner1, rightOuter1, whiteColor);
		}

		var mesh = surfaceTool.Commit();
		var material = new StandardMaterial3D();
		material.VertexColorUseAsAlbedo = true;
		material.Roughness = 0.8f;
		material.ShadingMode = BaseMaterial3D.ShadingModeEnum.Unshaded;
		material.CullMode = BaseMaterial3D.CullModeEnum.Disabled;
		material.Transparency = BaseMaterial3D.TransparencyEnum.Disabled;
		mesh.SurfaceSetMaterial(0, material);
		borderMesh.Mesh = mesh;
	}

	private void GenerateTrackMarkings(List<Vector3> centerline, float trackWidth)
	{
		var markingsMesh = new MeshInstance3D();
		AddChild(markingsMesh);

		var surfaceTool = new SurfaceTool();
		surfaceTool.Begin(Mesh.PrimitiveType.Triangles);

		float markingWidth = 0.3f * 50.0f;
		float yOffset = 0.01f * 50.0f; // Slightly above track surface to avoid z-fighting

		for (int i = 0; i < centerline.Count; i++)
		{
			int nextIdx = (i + 1) % centerline.Count;

			Vector3 current = centerline[i];
			Vector3 next = centerline[nextIdx];

			Vector3 direction = (next - current).Normalized();
			Vector3 right = new Vector3(-direction.Z, 0, direction.X).Normalized();

			// White edge markings
			var whiteColor = new Color(0.95f, 0.95f, 0.95f);

			// Left edge marking
			Vector3 leftInner = current - right * (trackWidth * 0.5f) + Vector3.Up * yOffset;
			Vector3 leftOuter = current - right * (trackWidth * 0.5f + markingWidth) + Vector3.Up * yOffset;
			Vector3 nextLeftInner = next - right * (trackWidth * 0.5f) + Vector3.Up * yOffset;
			Vector3 nextLeftOuter = next - right * (trackWidth * 0.5f + markingWidth) + Vector3.Up * yOffset;

			// Left marking triangles
			AddQuad(surfaceTool, leftOuter, leftInner, nextLeftOuter, nextLeftInner, whiteColor);

			// Right edge marking
			Vector3 rightInner = current + right * (trackWidth * 0.5f) + Vector3.Up * yOffset;
			Vector3 rightOuter = current + right * (trackWidth * 0.5f + markingWidth) + Vector3.Up * yOffset;
			Vector3 nextRightInner = next + right * (trackWidth * 0.5f) + Vector3.Up * yOffset;
			Vector3 nextRightOuter = next + right * (trackWidth * 0.5f + markingWidth) + Vector3.Up * yOffset;

			// Right marking triangles
			AddQuad(surfaceTool, rightInner, rightOuter, nextRightInner, nextRightOuter, whiteColor);
		}

		var mesh = surfaceTool.Commit();
		var material = new StandardMaterial3D();
		material.VertexColorUseAsAlbedo = true;
		material.Roughness = 0.8f;
		mesh.SurfaceSetMaterial(0, material);
		markingsMesh.Mesh = mesh;
	}

	private void GenerateGroundPlane(List<Vector3> centerline, List<float> widths)
	{
		if (centerline.Count < 2 || centerline.Count != widths.Count)
			return;

		bool closed = true;
		int samplesPerSeg = 8;

		// Build raw edges
		var leftRaw = new List<Vector3>();
		var rightRaw = new List<Vector3>();
		for (int i = 0; i < centerline.Count; i++)
		{
			int nextIdx = (i + 1) % centerline.Count;
			Vector3 current = centerline[i];
			Vector3 next = centerline[nextIdx];
			float width = widths[i];

			Vector3 dir = (next - current).Normalized();
			Vector3 right = new Vector3(-dir.Z, 0, dir.X).Normalized();

			leftRaw.Add(current - right * (width * 0.5f));
			rightRaw.Add(current + right * (width * 0.5f));
		}

		// Resample edges
		var left = CatmullRom(leftRaw, samplesPerSeg, closed);
		var rightEdge = CatmullRom(rightRaw, samplesPerSeg, closed);

		if (left.Count != rightEdge.Count || left.Count < 2)
			return;

		var groundMesh = new MeshInstance3D();
		AddChild(groundMesh);

		var surfaceTool = new SurfaceTool();
		surfaceTool.Begin(Mesh.PrimitiveType.Triangles);

		float groundWidth = 50.0f * 50.0f;  // Wide ground plane extending beyond track
		float yOffset = -1.0f * 50.0f;  // Below track surface
		var groundColor = new Color(0.7f, 0.7f, 0.7f);  // Light grey
		int segCount = left.Count;

		for (int i = 0; i < segCount; i++)
		{
			int nextIdx = (i + 1) % segCount;

			Vector3 l0 = left[i];
			Vector3 r0 = rightEdge[i];
			Vector3 l1 = left[nextIdx];
			Vector3 r1 = rightEdge[nextIdx];

			Vector3 dir = (r0 + r1) * 0.5f - (l0 + l1) * 0.5f;
			Vector3 offsetDir = (dir.LengthSquared() < 1e-4f)
				? new Vector3(1, 0, 0)
				: dir.Normalized();

			// Left ground area
			Vector3 leftInner0 = l0 + Vector3.Up * yOffset;
			Vector3 leftOuter0 = l0 - offsetDir * groundWidth + Vector3.Up * yOffset;
			Vector3 leftInner1 = l1 + Vector3.Up * yOffset;
			Vector3 leftOuter1 = l1 - offsetDir * groundWidth + Vector3.Up * yOffset;

			AddQuad(surfaceTool, leftOuter0, leftInner0, leftOuter1, leftInner1, groundColor);

			// Right ground area
			Vector3 rightInner0 = r0 + Vector3.Up * yOffset;
			Vector3 rightOuter0 = r0 + offsetDir * groundWidth + Vector3.Up * yOffset;
			Vector3 rightInner1 = r1 + Vector3.Up * yOffset;
			Vector3 rightOuter1 = r1 + offsetDir * groundWidth + Vector3.Up * yOffset;

			AddQuad(surfaceTool, rightInner0, rightOuter0, rightInner1, rightOuter1, groundColor);
		}

		var mesh = surfaceTool.Commit();
		var material = new StandardMaterial3D();
		material.VertexColorUseAsAlbedo = true;
		material.Roughness = 0.9f;
		material.CullMode = BaseMaterial3D.CullModeEnum.Disabled;
		mesh.SurfaceSetMaterial(0, material);
		groundMesh.Mesh = mesh;
	}

	private void GenerateStartFinishGrid(List<Vector3> centerline, List<float> widths)
	{
		if (centerline.Count < 2 || centerline.Count != widths.Count)
			return;

		// Create grid at start/finish line (index 0)
		var gridMesh = new MeshInstance3D();
		AddChild(gridMesh);

		var surfaceTool = new SurfaceTool();
		surfaceTool.Begin(Mesh.PrimitiveType.Triangles);

		// Grid configuration
		float gridLength = 8.0f * 50.0f;    // 8 meters long
		float boxWidth = 0.5f * 50.0f;       // 50cm wide boxes
		float yOffset = 0.6f * 50.0f;        // Raised above track surface

		// Find start line position and direction
		Vector3 startPos = centerline[0];
		Vector3 nextPos = centerline[1];
		Vector3 forward = (nextPos - startPos).Normalized();
		Vector3 right = new Vector3(-forward.Z, 0, forward.X).Normalized();

		float trackWidth = widths[0];
		float boxHeight = boxWidth; // Square boxes

		// Calculate number of boxes based on actual track width
		int numBoxesAcross = Mathf.CeilToInt(trackWidth / boxWidth);

		// Generate checkered pattern
		for (int row = 0; row < 16; row++) // 16 rows = 8 meters at 0.5m per row
		{
			float rowOffset = row * boxWidth;
			Vector3 rowStart = startPos + forward * rowOffset;

			for (int col = 0; col < numBoxesAcross; col++)
			{
				// Checkered pattern: alternate colors
				bool isWhite = (row + col) % 2 == 0;
				Color boxColor = isWhite ? new Color(0.95f, 0.95f, 0.95f, 1.0f) : new Color(0.05f, 0.05f, 0.05f, 1.0f);

				// Calculate box position (centered on track)
				float colOffset = (col - numBoxesAcross / 2.0f + 0.5f) * boxHeight;
				Vector3 boxCenter = rowStart + right * colOffset + Vector3.Up * yOffset;

				// Box corners
				Vector3 bl = boxCenter - right * (boxWidth * 0.5f) - forward * (boxHeight * 0.5f);
				Vector3 br = boxCenter + right * (boxWidth * 0.5f) - forward * (boxHeight * 0.5f);
				Vector3 tl = boxCenter - right * (boxWidth * 0.5f) + forward * (boxHeight * 0.5f);
				Vector3 tr = boxCenter + right * (boxWidth * 0.5f) + forward * (boxHeight * 0.5f);

				AddQuad(surfaceTool, bl, br, tl, tr, boxColor);
			}
		}

		var mesh = surfaceTool.Commit();
		var material = new StandardMaterial3D();
		material.VertexColorUseAsAlbedo = true;
		material.Roughness = 0.8f;
		material.Metallic = 0.0f;
		material.ShadingMode = BaseMaterial3D.ShadingModeEnum.Unshaded;
		material.CullMode = BaseMaterial3D.CullModeEnum.Disabled;
		mesh.SurfaceSetMaterial(0, material);
		gridMesh.Mesh = mesh;

		GD.Print("✅ Generated start/finish grid");
	}

	private void AddQuad(SurfaceTool surfaceTool, Vector3 v1, Vector3 v2, Vector3 v3, Vector3 v4, Color color)
	{
		// First triangle (v1, v2, v3)
		surfaceTool.SetColor(color);
		surfaceTool.SetNormal(Vector3.Up);
		surfaceTool.AddVertex(v1);

		surfaceTool.SetColor(color);
		surfaceTool.SetNormal(Vector3.Up);
		surfaceTool.AddVertex(v2);

		surfaceTool.SetColor(color);
		surfaceTool.SetNormal(Vector3.Up);
		surfaceTool.AddVertex(v3);

		// Second triangle (v2, v4, v3)
		surfaceTool.SetColor(color);
		surfaceTool.SetNormal(Vector3.Up);
		surfaceTool.AddVertex(v2);

		surfaceTool.SetColor(color);
		surfaceTool.SetNormal(Vector3.Up);
		surfaceTool.AddVertex(v4);

		surfaceTool.SetColor(color);
		surfaceTool.SetNormal(Vector3.Up);
		surfaceTool.AddVertex(v3);
	}

	private void AddTestCube()
	{
		// Add a bright cyan cube at the center to test if 3D rendering works
		var cube = new MeshInstance3D();
		var boxMesh = new BoxMesh();
		boxMesh.Size = new Vector3(20, 20, 20);
		cube.Mesh = boxMesh;

		var material = new StandardMaterial3D();
		material.AlbedoColor = new Color(0, 1, 1); // Bright cyan
		material.EmissionEnabled = true;
		material.Emission = new Color(0, 1, 1); // Self-illuminated
		material.EmissionEnergyMultiplier = 2.0f; // Godot 4 property name

		cube.Mesh.SurfaceSetMaterial(0, material);
		cube.Position = new Vector3(0, 10, 0); // 10 units above the track center

		AddChild(cube);
	}

	/// <summary>
	/// Load procedural terrain data from .terrain.msgpack file.
	/// </summary>
	private ProceduralWorldData? LoadProceduralTerrainData(string trackYamlPath)
	{
		try
		{
			// Replace .yaml/.yml extension with .terrain.msgpack
			string terrainPath = Path.ChangeExtension(trackYamlPath, null);
			if (terrainPath.EndsWith(".yaml") || terrainPath.EndsWith(".yml"))
			{
				terrainPath = terrainPath.Substring(0, terrainPath.LastIndexOf('.'));
			}
			terrainPath += ".terrain.msgpack";

			if (!File.Exists(terrainPath))
			{
				return null;
			}

			GD.Print($"Loading terrain data from: {terrainPath}");
			byte[] data = File.ReadAllBytes(terrainPath);

			// Debug: Check what we're deserializing
			GD.Print($"Terrain file size: {data.Length} bytes");

			// Try deserializing with dynamic to see the structure
			try
			{
				var dynamic = MessagePackSerializer.Deserialize<dynamic>(data, MsgPackOptions);
				GD.Print($"Dynamic deserialization succeeded, type: {dynamic?.GetType().Name}");
			}
			catch (Exception ex)
			{
				GD.Print($"Dynamic deserialization failed: {ex.Message}");
			}

			var terrainData = MessagePackSerializer.Deserialize<ProceduralWorldData>(data, MsgPackOptions);

			if (terrainData?.Heightmap != null)
			{
				GD.Print($"Terrain loaded: {terrainData.Heightmap.Width}x{terrainData.Heightmap.Height} " +
						 $"cells, environment: {terrainData.EnvironmentType}");
			}

			return terrainData;
		}
		catch (Exception ex)
		{
			GD.PrintErr($"Failed to load procedural terrain data: {ex.Message}");
			GD.PrintErr($"Stack trace: {ex.StackTrace}");
			return null;
		}
	}

	/// <summary>
	/// Render procedural terrain mesh from terrain data.
	/// </summary>
	private void RenderProceduralTerrain(ProceduralWorldData terrainData)
	{
		// Clean up existing terrain if any
		if (_proceduralTerrain != null)
		{
			_proceduralTerrain.QueueFree();
			_proceduralTerrain = null;
		}

		// Create new terrain renderer
		_proceduralTerrain = new ProceduralTerrain();
		AddChild(_proceduralTerrain);

		// Generate terrain mesh
		_proceduralTerrain.GenerateTerrain(terrainData);

		GD.Print("✅ Procedural terrain rendered successfully");
	}
}
