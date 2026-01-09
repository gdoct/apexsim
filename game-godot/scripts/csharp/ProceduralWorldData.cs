using System;
using System.Collections.Generic;
using MessagePack;

namespace ApexSim;

/// <summary>
/// Terrain heightmap for procedural world generation.
/// Matches the Rust TerrainHeightmap structure.
/// NOTE: Rust uses rmp_serde::to_vec which serializes as array, not map
/// So we must use [MessagePackObject(false)] for array-based serialization
/// (false = keyAsPropertyName: false = use numeric keys for array)
/// </summary>
[MessagePackObject]
public class TerrainHeightmap
{
	[Key(0)]
	public int width { get; set; }
	[Key(1)]
	public int height { get; set; }
	[Key(2)]
	public float cell_size_m { get; set; }
	[Key(3)]
	public float origin_x { get; set; }
	[Key(4)]
	public float origin_y { get; set; }
	[Key(5)]
	public List<float> heights { get; set; } = new();

	// Convenience properties with PascalCase
	[IgnoreMember]
	public int Width => width;

	[IgnoreMember]
	public int Height => height;

	[IgnoreMember]
	public float CellSizeM => cell_size_m;

	[IgnoreMember]
	public float OriginX => origin_x;

	[IgnoreMember]
	public float OriginY => origin_y;

	[IgnoreMember]
	public List<float> Heights => heights;

	/// <summary>
	/// Sample height at world coordinates using bilinear interpolation.
	/// </summary>
	public float Sample(float worldX, float worldY)
	{
		// Convert world coords to grid coords
		float gridX = (worldX - OriginX) / CellSizeM;
		float gridY = (worldY - OriginY) / CellSizeM;

		// Clamp to valid range
		if (gridX < 0.0f || gridY < 0.0f
			|| gridX >= (Width - 1) || gridY >= (Height - 1))
		{
			return 0.0f;
		}

		// Get integer and fractional parts
		int x0 = (int)Math.Floor(gridX);
		int y0 = (int)Math.Floor(gridY);
		int x1 = Math.Min(x0 + 1, Width - 1);
		int y1 = Math.Min(y0 + 1, Height - 1);

		float fx = gridX - x0;
		float fy = gridY - y0;

		// Bilinear interpolation
		float h00 = GetHeight(x0, y0);
		float h10 = GetHeight(x1, y0);
		float h01 = GetHeight(x0, y1);
		float h11 = GetHeight(x1, y1);

		float h0 = h00 * (1.0f - fx) + h10 * fx;
		float h1 = h01 * (1.0f - fx) + h11 * fx;

		return h0 * (1.0f - fy) + h1 * fy;
	}

	/// <summary>
	/// Get height at grid coordinates (clamped to valid range).
	/// </summary>
	public float GetHeight(int x, int y)
	{
		if (x >= width || y >= height || x < 0 || y < 0)
		{
			return 0.0f;
		}
		return heights[y * width + x];
	}
}

/// <summary>
/// Environment preset configuration for procedural generation.
/// Matches the Rust EnvironmentPreset structure.
/// NOTE: Rust uses rmp_serde::to_vec which serializes as array, not map
/// </summary>
[MessagePackObject]
public class EnvironmentPreset
{
	[Key(0)]
	public float base_noise_freq { get; set; }
	[Key(1)]
	public float detail_noise_freq { get; set; }
	[Key(2)]
	public float max_height { get; set; }
	[Key(3)]
	public float object_density { get; set; }
	[Key(4)]
	public List<string> allowed_objects { get; set; } = new();
	[Key(5)]
	public float[] ground_color { get; set; } = new float[3];

	// Convenience properties with PascalCase
	[IgnoreMember]
	public float[] GroundColor => ground_color;
}

/// <summary>
/// Complete procedural world data containing terrain and environment settings.
/// Matches the Rust ProceduralWorldData structure.
/// NOTE: Rust uses rmp_serde::to_vec which serializes as array, not map
/// Field order MUST match the Rust struct definition exactly!
/// </summary>
[MessagePackObject]
public class ProceduralWorldData
{
	[Key(0)]
	public string environment_type { get; set; } = "";
	[Key(1)]
	public uint seed { get; set; }
	[Key(2)]
	public TerrainHeightmap? heightmap { get; set; }
	[Key(3)]
	public float blend_width { get; set; }
	[Key(4)]
	public float object_density { get; set; }
	[Key(5)]
	public string decal_profile { get; set; } = "";
	[Key(6)]
	public EnvironmentPreset preset { get; set; } = new();

	// Convenience properties with PascalCase
	[IgnoreMember]
	public string EnvironmentType => environment_type;

	[IgnoreMember]
	public TerrainHeightmap? Heightmap => heightmap;

	[IgnoreMember]
	public EnvironmentPreset Preset => preset;
}
