using System.Collections.Generic;

namespace ApexSim;

public class TrackData
{
	public string? TrackId { get; set; }
	public string Name { get; set; } = "";
	public List<TrackNode> Nodes { get; set; } = new();
	public List<Dictionary<string, object>>? Checkpoints { get; set; }
	public List<Dictionary<string, object>>? SpawnPoints { get; set; }
	public float? DefaultWidth { get; set; }
	public bool? ClosedLoop { get; set; }
	public List<TrackNode>? Raceline { get; set; }
	public TrackMetadata? Metadata { get; set; }
}
