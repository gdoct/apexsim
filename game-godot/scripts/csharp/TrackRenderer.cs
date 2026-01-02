using Godot;
using System;
using System.Collections.Generic;
using System.IO;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;

namespace ApexSim;

// Track data structures for YAML deserialization
public class TrackNode
{
	public float X { get; set; }
	public float Y { get; set; }
	public float Z { get; set; }
	public float? Width { get; set; }
	public float? WidthLeft { get; set; }
	public float? WidthRight { get; set; }
	public float? Banking { get; set; }
	public float? Friction { get; set; }
	public string? SurfaceType { get; set; }
}

public class TrackMetadata
{
	public string? Country { get; set; }
	public string? City { get; set; }
	public float? LengthM { get; set; }
	public string? Description { get; set; }
	public int? YearBuilt { get; set; }
	public string? Category { get; set; }
}

public class TrackData
{
	public string Name { get; set; } = "";
	public List<TrackNode> Nodes { get; set; } = new();
	public List<Dictionary<string, object>>? Checkpoints { get; set; }
	public List<Dictionary<string, object>>? SpawnPoints { get; set; }
	public float? DefaultWidth { get; set; }
	public bool? ClosedLoop { get; set; }
	public List<TrackNode>? Raceline { get; set; }
	public TrackMetadata? Metadata { get; set; }
}

public partial class TrackRenderer : Node3D
{
	private MeshInstance3D? _trackMesh;
	private NetworkClient? _network;
	private string? _currentSessionId;
	private Camera3D? _camera;
	private Button? _backButton;
	private float _cameraAngle = 0.785f; // Start at 45 degrees (PI/4) for better initial view

	// Content path configuration - relative to game-godot directory
	private string _contentBasePath = "../content";
	private float _cameraDistance = 250.0f; // Start a bit farther away
	private const float MinCameraDistance = 50.0f;
	private const float MaxCameraDistance = 500.0f;
	private const float ZoomSpeed = 20.0f;

	// Free camera controls
	private bool _isMouseDragging = false;
	private float _cameraYaw = 0.0f;
	private float _cameraPitch = -45.0f; // Start looking down
	private Vector3 _cameraPosition = new Vector3(0, 5, 5); // Close to track surface
	private bool _useFreeCam = true;

	public override void _Ready()
	{
		GD.Print("========================================");
		GD.Print("TrackRenderer _Ready called");
		GD.Print("========================================");

		try
		{
			_network = GetNode<NetworkClient>("/root/Network");
			GD.Print("✓ Network client found");
			_network.SessionJoined += OnSessionJoined;
			GD.Print("✓ SessionJoined event subscribed");
		}
		catch (System.Exception ex)
		{
			GD.PrintErr($"✗ Error getting Network: {ex.Message}");
		}

		try
		{
			// Get camera reference (sibling node)
			_camera = GetNode<Camera3D>("../Camera3D");
			GD.Print($"✓ Camera found at position: {_camera?.Position}");
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
			GD.Print("✓ Back button found and connected");
		}
		catch (System.Exception ex)
		{
			GD.PrintErr($"✗ Error getting BackButton: {ex.Message}");
		}

		try
		{
			// Create the track mesh instance
			_trackMesh = new MeshInstance3D();
			AddChild(_trackMesh);
			GD.Print("✓ Track mesh instance created and added as child");
		}
		catch (System.Exception ex)
		{
			GD.PrintErr($"✗ Error creating track mesh: {ex.Message}");
		}

		try
		{
			// Check if we're already in a session (signal may have fired before we subscribed)
			var lobbyState = _network?.LastLobbyState;
			if (lobbyState != null)
			{
				// Find any session we might be in
				foreach (var session in lobbyState.AvailableSessions)
				{
					// Check if any player in the session is us (would be in player_count but not visible here)
					// For now, assume if we're on this scene, we should load the first/only session
					GD.Print($"Found session in lobby: {session.TrackName}");
					if (!string.IsNullOrEmpty(session.TrackFile))
					{
						GD.Print($"Session track file available: {session.TrackFile}");
						_currentSessionId = session.Id;
						LoadAndRenderTrack(session.TrackFile);
						break;
					}
				}
			}
		}
		catch (System.Exception ex)
		{
			GD.PrintErr($"✗ Error checking for existing session: {ex.Message}");
			GD.PrintErr($"Stack trace: {ex.StackTrace}");
		}

		GD.Print("========================================");
		GD.Print("TrackRenderer _Ready finished");
		GD.Print("========================================");
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
		if (@event is InputEventMouseButton mouseEvent)
		{
			// Handle mousewheel zoom
			if (mouseEvent.ButtonIndex == MouseButton.WheelUp && mouseEvent.Pressed)
			{
				_cameraDistance = Mathf.Max(MinCameraDistance, _cameraDistance - ZoomSpeed);
			}
			else if (mouseEvent.ButtonIndex == MouseButton.WheelDown && mouseEvent.Pressed)
			{
				_cameraDistance = Mathf.Min(MaxCameraDistance, _cameraDistance + ZoomSpeed);
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
			float moveSpeed = 50.0f * (float)delta;

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

			// Debug output for first few frames
			if (_frameCount < 3)
			{
				GD.Print($"FreeCam Frame {_frameCount}: Camera at {_cameraPosition}, yaw: {_cameraYaw:F1}, pitch: {_cameraPitch:F1}");
				GD.Print("Controls: Right-click + drag to look, WASD to move, QE to move up/down");
				_frameCount++;
			}
		}
		else
		{
			// Orbital camera mode (original behavior)
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

	private void OnSessionJoined(string sessionId, byte gridPosition)
	{
		_currentSessionId = sessionId;
		GD.Print($"Joined session {sessionId}, grid position {gridPosition}");

		// Get track name from lobby state
		var lobbyState = _network?.LastLobbyState;
		if (lobbyState == null)
		{
			GD.PrintErr("No lobby state available, cannot determine track");
			GenerateCircularTrack();
			return;
		}

		// Find the session to get track name and file
		var session = Array.Find(lobbyState.AvailableSessions, s => s.Id == sessionId);
		if (session == null)
		{
			GD.PrintErr($"Session {sessionId} not found in lobby state");
			GenerateCircularTrack();
			return;
		}

		GD.Print($"Loading track: {session.TrackName} from {session.TrackFile}");
		LoadAndRenderTrack(session.TrackFile);
	}

	private void GenerateTestTrack()
	{
		// For testing in sandbox mode without joining a session
		// This should only be called when NOT in a session (direct scene load)
		GD.Print("Sandbox mode: Generating circular test track...");
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
			GD.Print($"✓ Loaded {trackData.Nodes.Count} track nodes from {trackPath}");

			// Convert to track points and widths
			var centerline = new List<Vector3>();
			var widths = new List<float>();

			foreach (var node in trackData.Nodes)
			{
				// YAML format: x, y are horizontal plane, z is elevation
				// Godot: X=x, Y=z (elevation/height), Z=y (horizontal)
				// Flip Y-axis to match server/world orientation (track previously mirrored)
				centerline.Add(new Vector3(node.X, node.Z, -node.Y));

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
				widths.Add(width);
			}

			GD.Print($"✓ Converted {centerline.Count} track points");
			GenerateGroundPlane(centerline, widths);
			GenerateTrackMeshSpline(centerline, widths);
			GenerateWhiteBorderArea(centerline, widths);
			GenerateDottedOutlineSpline(centerline, widths);
			GD.Print($"✓ Track mesh generation completed for {trackFile}");

			// Add debug spheres at track points to see where they are
			AddDebugMarkers(centerline);
		}
		catch (Exception ex)
		{
			GD.PrintErr($"Failed to load track {trackFile}: {ex.Message}");
			GD.PrintErr($"Stack trace: {ex.StackTrace}");

			// Fallback to simple circular track
			GD.Print("Falling back to circular test track...");
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
		GD.Print("Generating circular test track...");
		var trackPoints = new List<Vector3>();
		var numPoints = 60;
		var radius = 100.0f;

		for (int i = 0; i < numPoints; i++)
		{
			float angle = 2.0f * Mathf.Pi * i / numPoints;
			float x = radius * Mathf.Cos(angle);
			float y = 0.0f; // Flat for now
			float z = radius * Mathf.Sin(angle);
			trackPoints.Add(new Vector3(x, y, z));
		}

		GD.Print($"Generated {trackPoints.Count} track points");
		var widths = new List<float>();
		for (int i = 0; i < trackPoints.Count; i++)
		{
			widths.Add(12.0f);
		}
		GenerateGroundPlane(trackPoints, widths);
		GenerateTrackMesh(trackPoints, 12.0f);
		GD.Print("Track mesh generation completed");

		// Add debug spheres at track points to see where they are
		AddDebugMarkers(trackPoints);
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
		GD.Print($"✓ Added {points.Count / 10} red debug spheres at track centerline");
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
			Vector3 righht = new Vector3(-dir.Z, 0, dir.X).Normalized();

			leftRaw.Add(current - righht * (width * 0.5f));
			rightRaw.Add(current + righht * (width * 0.5f));
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

		var asphaltColor = new Color(0f, 0f, 0f);
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

		GD.Print($"Spline track mesh assigned, visible: {_trackMesh.Visible}, position: {_trackMesh.Position}");
		GD.Print($"Track mesh AABB: {mesh.GetAabb()}");
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
		GD.Print($"Mesh committed with {mesh.GetSurfaceCount()} surfaces");

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

		GD.Print($"Track mesh assigned, visible: {_trackMesh.Visible}, position: {_trackMesh.Position}");
		GD.Print($"Track mesh AABB: {mesh.GetAabb()}");

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

		var outlineMesh = new MeshInstance3D();
		AddChild(outlineMesh);

		var surfaceTool = new SurfaceTool();
		surfaceTool.Begin(Mesh.PrimitiveType.Triangles);

		float stripeWidth = 0.35f;
		float yOffset = 0.5f;  // Raise outline above track surface
		int segCount = left.Count;

		for (int i = 0; i < segCount; i++)
		{
			int nextIdx = (i + 1) % segCount;

			// Fully opaque red and white colors
			Color color = (i % 2 == 0) ? new Color(1.0f, 0.0f, 0.0f, 1.0f) : new Color(1.0f, 1.0f, 1.0f, 1.0f);

			Vector3 l0 = left[i];
			Vector3 r0 = rightEdge[i];
			Vector3 l1 = left[nextIdx];
			Vector3 r1 = rightEdge[nextIdx];

			Vector3 dir = (r0 + r1) * 0.5f - (l0 + l1) * 0.5f;
			Vector3 offsetDir = (dir.LengthSquared() < 1e-4f)
				? new Vector3(1, 0, 0)
				: dir.Normalized();

			// Left stripe
			Vector3 leftInner0 = l0 + Vector3.Up * yOffset;
			Vector3 leftOuter0 = l0 - offsetDir * stripeWidth + Vector3.Up * yOffset;
			Vector3 leftInner1 = l1 + Vector3.Up * yOffset;
			Vector3 leftOuter1 = l1 - offsetDir * stripeWidth + Vector3.Up * yOffset;

			AddQuad(surfaceTool, leftOuter0, leftInner0, leftOuter1, leftInner1, color);

			// Right stripe
			Vector3 rightInner0 = r0 + Vector3.Up * yOffset;
			Vector3 rightOuter0 = r0 + offsetDir * stripeWidth + Vector3.Up * yOffset;
			Vector3 rightInner1 = r1 + Vector3.Up * yOffset;
			Vector3 rightOuter1 = r1 + offsetDir * stripeWidth + Vector3.Up * yOffset;

			AddQuad(surfaceTool, rightInner0, rightOuter0, rightInner1, rightOuter1, color);
		}

		var mesh = surfaceTool.Commit();
		var material = new StandardMaterial3D();
		material.VertexColorUseAsAlbedo = true;
		material.Roughness = 0.6f;
		material.ShadingMode = BaseMaterial3D.ShadingModeEnum.Unshaded;
		material.CullMode = BaseMaterial3D.CullModeEnum.Disabled;
		material.Transparency = BaseMaterial3D.TransparencyEnum.Disabled;  // Fully opaque
		mesh.SurfaceSetMaterial(0, material);
		outlineMesh.Mesh = mesh;
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

		float borderWidth = 3.0f;  // Width of the white border area
		float yOffset = 0.3f;  // Raised above the track for visibility from top
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

		float markingWidth = 0.3f;
		float yOffset = 0.01f; // Slightly above track surface to avoid z-fighting

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

		float groundWidth = 50.0f;  // Wide ground plane extending beyond track
		float yOffset = -1.0f;  // Below track surface
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
		GD.Print("✓ Added bright cyan test cube at center (0, 10, 0)");
	}
}
