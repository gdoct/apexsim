using System;

namespace ApexSim;

public class TelemetryData
{
    public uint ServerTick { get; set; }
    public SessionState SessionState { get; set; }
    public GameMode GameMode { get; set; }
    public ushort? CountdownMs { get; set; }
    public CarStateTelemetry[] CarStates { get; set; } = Array.Empty<CarStateTelemetry>();
}
