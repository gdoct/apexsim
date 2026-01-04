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
