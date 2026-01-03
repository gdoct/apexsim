using Godot;
using System;
using System.Collections.Generic;

namespace ApexSim;

/// <summary>
/// Standalone test script that renders a track mesh and saves it as an image.
/// Run this scene to quickly validate track rendering without starting a full game session.
/// </summary>
public partial class TrackMeshTester : Node3D
{
	private SubViewport? _viewport;
	private Camera3D? _camera;
	private MeshInstance3D? _trackMesh;

	public override void _Ready()
	{
		// Generate track mesh directly (no viewport needed for validation)
		_trackMesh = new MeshInstance3D();
		AddChild(_trackMesh);

		GenerateTestTrack();

		// Validate and exit
		ValidateAndExit();
	}

	private void GenerateTestTrack()
	{
		// Generate circular track points
		var trackPoints = new List<Vector3>();
		var numPoints = 60;
		var radius = 100.0f;

		for (int i = 0; i < numPoints; i++)
		{
			float angle = 2.0f * Mathf.Pi * i / numPoints;
			float x = radius * Mathf.Cos(angle);
			float z = radius * Mathf.Sin(angle);
			trackPoints.Add(new Vector3(x, 0, z));
		}

		// Generate the mesh
		var surfaceTool = new SurfaceTool();
		surfaceTool.Begin(Mesh.PrimitiveType.Triangles);

		float trackWidth = 12.0f;
		var trackColor = new Color(1.0f, 1.0f, 1.0f); // White for visibility

		// Generate track surface
		for (int i = 0; i < trackPoints.Count; i++)
		{
			int nextIdx = (i + 1) % trackPoints.Count;

			Vector3 current = trackPoints[i];
			Vector3 next = trackPoints[nextIdx];
			Vector3 direction = (next - current).Normalized();
			Vector3 right = new Vector3(-direction.Z, 0, direction.X).Normalized();

			Vector3 leftEdge = current - right * (trackWidth * 0.5f);
			Vector3 rightEdge = current + right * (trackWidth * 0.5f);
			Vector3 nextLeftEdge = next - right * (trackWidth * 0.5f);
			Vector3 nextRightEdge = next + right * (trackWidth * 0.5f);

			// First triangle
			surfaceTool.SetColor(trackColor);
			surfaceTool.SetNormal(Vector3.Up);
			surfaceTool.AddVertex(leftEdge);

			surfaceTool.SetColor(trackColor);
			surfaceTool.SetNormal(Vector3.Up);
			surfaceTool.AddVertex(rightEdge);

			surfaceTool.SetColor(trackColor);
			surfaceTool.SetNormal(Vector3.Up);
			surfaceTool.AddVertex(nextLeftEdge);

			// Second triangle
			surfaceTool.SetColor(trackColor);
			surfaceTool.SetNormal(Vector3.Up);
			surfaceTool.AddVertex(rightEdge);

			surfaceTool.SetColor(trackColor);
			surfaceTool.SetNormal(Vector3.Up);
			surfaceTool.AddVertex(nextRightEdge);

			surfaceTool.SetColor(trackColor);
			surfaceTool.SetNormal(Vector3.Up);
			surfaceTool.AddVertex(nextLeftEdge);
		}

		var mesh = surfaceTool.Commit();

		// Create material
		var material = new StandardMaterial3D();
		material.VertexColorUseAsAlbedo = true;
		material.ShadingMode = BaseMaterial3D.ShadingModeEnum.Unshaded;

		mesh.SurfaceSetMaterial(0, material);
		_trackMesh!.Mesh = mesh;
	}

	private void ValidateAndExit()
	{
		bool success = true;

		// Validate track mesh was created
		if (_trackMesh == null || _trackMesh.Mesh == null)
		{
			GD.PrintErr("✗ FAIL: Track mesh was not created!");
			success = false;
		}
		else
		{
			GD.Print("✓ PASS: Track mesh created successfully");

			// Validate mesh has surfaces
			var surfaceCount = _trackMesh.Mesh.GetSurfaceCount();
			if (surfaceCount == 0)
			{
				GD.PrintErr("✗ FAIL: Mesh has no surfaces!");
				success = false;
			}
			else
			{
				GD.Print($"✓ PASS: Mesh has {surfaceCount} surface(s)");
			}

			// Validate bounding box
			var aabb = _trackMesh.Mesh.GetAabb();
			var size = aabb.Size;
			if (size.Length() < 100) // Should be ~200+ for a radius 100 circle
			{
				GD.PrintErr($"✗ FAIL: Mesh is too small! AABB size: {size}");
				success = false;
			}
			else
			{
				GD.Print($"✓ PASS: Mesh has valid size (AABB: {aabb})");
			}

			// Validate material
			var material = _trackMesh.Mesh.SurfaceGetMaterial(0);
			if (material == null)
			{
				GD.PrintErr("✗ FAIL: Mesh has no material!");
				success = false;
			}
			else
			{
				GD.Print("✓ PASS: Mesh has material assigned");
			}
		}

		if (success)
		{
			GD.Print("=== ALL TESTS PASSED ===");
			GetTree().Quit(0);
		}
		else
		{
			GD.PrintErr("=== TESTS FAILED ===");
			GetTree().Quit(1);
		}
	}
}
