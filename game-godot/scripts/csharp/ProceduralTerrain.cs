using Godot;
using System;
using System.Collections.Generic;

namespace ApexSim;

/// <summary>
/// Generates and renders procedural terrain meshes from heightmap data.
/// Handles coordinate conversion from server space to Godot space.
/// </summary>
public partial class ProceduralTerrain : Node3D
{
	private ProceduralWorldData? _worldData;
	private MeshInstance3D? _terrainMesh;
	private const float SCALE_FACTOR = 50.0f; // Server units to Godot units

	/// <summary>
	/// Generate terrain mesh from procedural world data.
	/// </summary>
	public void GenerateTerrain(ProceduralWorldData worldData)
	{
		_worldData = worldData;

		if (worldData.Heightmap == null)
		{
			GD.PrintErr("ProceduralTerrain: No heightmap data available");
			return;
		}

		var heightmap = worldData.Heightmap;
		GD.Print($"Generating terrain mesh: {heightmap.Width}x{heightmap.Height} cells, " +
				 $"cell size: {heightmap.CellSizeM}m, environment: {worldData.EnvironmentType}");

		// Create mesh instance
		_terrainMesh = new MeshInstance3D();
		AddChild(_terrainMesh);

		// Generate the terrain mesh
		var mesh = GenerateTerrainMesh(heightmap, worldData.Preset);
		_terrainMesh.Mesh = mesh;
		_terrainMesh.CastShadow = GeometryInstance3D.ShadowCastingSetting.Off;

		GD.Print($"Terrain mesh generated successfully with {heightmap.Heights.Count} height values");
	}

	/// <summary>
	/// Generate terrain mesh from heightmap data.
	/// </summary>
	private ArrayMesh GenerateTerrainMesh(TerrainHeightmap heightmap, EnvironmentPreset preset)
	{
		var surfaceTool = new SurfaceTool();
		surfaceTool.Begin(Mesh.PrimitiveType.Triangles);

		// Get ground color from preset
		var groundColor = new Color(
			preset.GroundColor[0],
			preset.GroundColor[1],
			preset.GroundColor[2]
		);

		// Generate quad grid from heightmap
		// Skip the last row and column (we need pairs of vertices for quads)
		for (int y = 0; y < heightmap.Height - 1; y++)
		{
			for (int x = 0; x < heightmap.Width - 1; x++)
			{
				// Get the 4 corner heights
				float h00 = heightmap.GetHeight(x, y);
				float h10 = heightmap.GetHeight(x + 1, y);
				float h01 = heightmap.GetHeight(x, y + 1);
				float h11 = heightmap.GetHeight(x + 1, y + 1);

				// Calculate world positions (server coordinates)
				float worldX0 = heightmap.OriginX + x * heightmap.CellSizeM;
				float worldX1 = heightmap.OriginX + (x + 1) * heightmap.CellSizeM;
				float worldY0 = heightmap.OriginY + y * heightmap.CellSizeM;
				float worldY1 = heightmap.OriginY + (y + 1) * heightmap.CellSizeM;

				// Convert to Godot coordinates
				// Server: X, Y (horizontal), Z (elevation)
				// Godot: X (same), Y (elevation), Z (-Y flipped)
				var v00 = ServerToGodot(worldX0, worldY0, h00);
				var v10 = ServerToGodot(worldX1, worldY0, h10);
				var v01 = ServerToGodot(worldX0, worldY1, h01);
				var v11 = ServerToGodot(worldX1, worldY1, h11);

				// Calculate smooth normals for lighting
				var normal00 = CalculateNormal(heightmap, x, y);
				var normal10 = CalculateNormal(heightmap, x + 1, y);
				var normal01 = CalculateNormal(heightmap, x, y + 1);
				var normal11 = CalculateNormal(heightmap, x + 1, y + 1);

				// First triangle (v00, v10, v01)
				surfaceTool.SetColor(groundColor);
				surfaceTool.SetNormal(normal00);
				surfaceTool.AddVertex(v00);

				surfaceTool.SetColor(groundColor);
				surfaceTool.SetNormal(normal10);
				surfaceTool.AddVertex(v10);

				surfaceTool.SetColor(groundColor);
				surfaceTool.SetNormal(normal01);
				surfaceTool.AddVertex(v01);

				// Second triangle (v10, v11, v01)
				surfaceTool.SetColor(groundColor);
				surfaceTool.SetNormal(normal10);
				surfaceTool.AddVertex(v10);

				surfaceTool.SetColor(groundColor);
				surfaceTool.SetNormal(normal11);
				surfaceTool.AddVertex(v11);

				surfaceTool.SetColor(groundColor);
				surfaceTool.SetNormal(normal01);
				surfaceTool.AddVertex(v01);
			}
		}

		// Commit the mesh
		var mesh = surfaceTool.Commit();

		// Create material
		var material = new StandardMaterial3D();
		material.VertexColorUseAsAlbedo = true;
		material.Roughness = 0.95f;
		material.Metallic = 0.0f;
		material.CullMode = BaseMaterial3D.CullModeEnum.Back;

		mesh.SurfaceSetMaterial(0, material);

		return mesh;
	}

	/// <summary>
	/// Calculate smooth normal at a grid point by averaging adjacent face normals.
	/// </summary>
	private Vector3 CalculateNormal(TerrainHeightmap heightmap, int x, int y)
	{
		// Sample heights around this point
		float h = heightmap.GetHeight(x, y);
		float hLeft = x > 0 ? heightmap.GetHeight(x - 1, y) : h;
		float hRight = x < heightmap.Width - 1 ? heightmap.GetHeight(x + 1, y) : h;
		float hUp = y > 0 ? heightmap.GetHeight(x, y - 1) : h;
		float hDown = y < heightmap.Height - 1 ? heightmap.GetHeight(x, y + 1) : h;

		// Calculate tangent vectors
		float cellSize = heightmap.CellSizeM * SCALE_FACTOR;
		Vector3 tangentX = new Vector3(cellSize, (hRight - hLeft) * SCALE_FACTOR * 0.5f, 0).Normalized();
		Vector3 tangentZ = new Vector3(0, (hDown - hUp) * SCALE_FACTOR * 0.5f, cellSize).Normalized();

		// Cross product gives normal
		Vector3 normal = tangentZ.Cross(tangentX).Normalized();

		return normal;
	}

	/// <summary>
	/// Convert server coordinates to Godot coordinates with scaling.
	/// Server: X, Y (horizontal), Z (elevation)
	/// Godot: X (same), Y (elevation), Z (-Y flipped)
	/// </summary>
	private Vector3 ServerToGodot(float serverX, float serverY, float serverZ)
	{
		return new Vector3(
			serverX * SCALE_FACTOR,      // X stays same, scaled
			serverZ * SCALE_FACTOR,      // Z (elevation) becomes Y
			-serverY * SCALE_FACTOR      // Y becomes -Z (flipped)
		);
	}

	public override void _ExitTree()
	{
		_terrainMesh?.QueueFree();
		base._ExitTree();
	}
}
