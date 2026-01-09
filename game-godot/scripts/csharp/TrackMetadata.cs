namespace ApexSim;

public class TrackMetadata
{
	public string? Country { get; set; }
	public string? City { get; set; }
	public float? LengthM { get; set; }
	public string? Description { get; set; }
	public int? YearBuilt { get; set; }
	public string? Category { get; set; }

	// Procedural generation parameters
	public string? EnvironmentType { get; set; }
	public uint? TerrainSeed { get; set; }
	public float? TerrainScale { get; set; }
	public float? TerrainDetail { get; set; }
	public float? TerrainBlendWidth { get; set; }
	public float? ObjectDensity { get; set; }
	public string? DecalProfile { get; set; }
}
