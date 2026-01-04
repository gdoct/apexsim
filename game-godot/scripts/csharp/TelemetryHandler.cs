using Godot;
using System;

namespace ApexSim;

public enum CameraViewMode
{
    FirstPerson,
    ThirdPerson,
    FreeCam,
    Chase,
    Hood,
    Cockpit
}

public class TelemetryData
{
    public uint ServerTick { get; set; }
    public SessionState SessionState { get; set; }
    public GameMode GameMode { get; set; }
    public ushort? CountdownMs { get; set; }
    public CarStateTelemetry[] CarStates { get; set; } = Array.Empty<CarStateTelemetry>();
}

public static class TelemetryHandler
{
    public static void UpdateTelemetryDisplay(Label telemetryLabel, TelemetryData telemetry, int telemetryCount, bool useFreeCam, CameraViewMode currentViewMode)
    {
        if (telemetryLabel == null)
            return;

        string statusText = $"Mode: {telemetry.GameMode}\n";

        if (useFreeCam)
        {
            statusText += "Camera: Free (Tab to toggle)\n";
        }
        else
        {
            string viewName = currentViewMode switch
            {
                CameraViewMode.Chase => "Chase",
                CameraViewMode.Hood => "Hood",
                CameraViewMode.Cockpit => "Cockpit",
                _ => "Unknown"
            };
            statusText += $"Camera: {viewName} (Tab/F1)\n";
        }

        statusText += $"Tick: {telemetry.ServerTick}\n";

        if (telemetry.CarStates.Length > 0)
        {
            var state = telemetry.CarStates[0];
            statusText += $"Progress: {(state.TrackProgress * 100):F1}%\n";
            statusText += $"Speed: {state.SpeedMps:F1} m/s\n";
            statusText += $"Position: ({state.PosX:F1}, {state.PosY:F1}, {state.PosZ:F1})";
        }
        else
        {
            statusText += "No car data!";
        }

        telemetryLabel.Text = statusText;
    }

    public static void HandleTelemetryUpdate(NetworkClient network, MeshInstance3D demoCarModel, int telemetryCount)
    {
        var telemetry = network?.LastTelemetry;
        if (telemetry == null || demoCarModel == null)
        {
            GD.Print($"[Telemetry #{telemetryCount}] No telemetry or car model is null!");
            return;
        }

        if (telemetry.GameMode != GameMode.DemoLap)
        {
            demoCarModel.Visible = false;
            return;
        }

        if (telemetry.CarStates.Length == 0)
        {
            GD.Print($"[Telemetry #{telemetryCount}] No car states in telemetry!");
            return;
        }

        var carState = telemetry.CarStates[0];

        if (telemetryCount <= 5)
        {
            GD.Print($"[Telemetry #{telemetryCount}] Pos: ({carState.PosX:F2}, {carState.PosY:F2}, {carState.PosZ:F2}), " +
                     $"Yaw: {carState.YawRad:F2} rad ({carState.YawRad * 180.0f / Mathf.Pi:F1}°), Speed: {carState.SpeedMps:F1} m/s, Progress: {carState.TrackProgress:F2}");
        }

        if (telemetryCount % 60 == 0)
        {
            GD.Print($"[Debug #{telemetryCount}] Server yaw: {carState.YawRad:F2} rad, Godot yaw will be: {-carState.YawRad:F2} rad");
        }

        var carPosition = new Vector3(
            carState.PosX,
            carState.PosZ,
            -carState.PosY
        );

        demoCarModel.Position = carPosition;

        if (telemetryCount > 1)
        {
            var nextPosition = carPosition - new Vector3(
                carState.SpeedMps * Mathf.Cos(carState.YawRad) * 0.1f,
                0,
                -carState.SpeedMps * Mathf.Sin(carState.YawRad) * 0.1f
            );

            demoCarModel.LookAt(nextPosition, Vector3.Up);
        }

        demoCarModel.Visible = true;
    }
}