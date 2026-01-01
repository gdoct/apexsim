using Godot;
using System.Collections.Generic;

namespace ApexSim;

public partial class SessionCreationDialog : Control
{
    private OptionButton? _trackSelector;
    private SpinBox? _maxPlayersSpinBox;
    private SpinBox? _aiCountSpinBox;
    private SpinBox? _lapLimitSpinBox;
    private Button? _createButton;
    private Button? _cancelButton;
    private Label? _statusLabel;
    private NetworkClient? _network;
    private Dictionary<int, TrackConfigSummary> _tracks = new();

    public override void _Ready()
    {
        _trackSelector = GetNode<OptionButton>("Panel/VBox/TrackSelection/OptionButton");
        _maxPlayersSpinBox = GetNode<SpinBox>("Panel/VBox/MaxPlayers/SpinBox");
        _aiCountSpinBox = GetNode<SpinBox>("Panel/VBox/AICount/SpinBox");
        _lapLimitSpinBox = GetNode<SpinBox>("Panel/VBox/LapLimit/SpinBox");
        _createButton = GetNode<Button>("Panel/VBox/ButtonBar/CreateButton");
        _cancelButton = GetNode<Button>("Panel/VBox/ButtonBar/CancelButton");
        _statusLabel = GetNode<Label>("Panel/VBox/StatusLabel");

        _createButton.Pressed += OnCreatePressed;
        _cancelButton.Pressed += OnCancelPressed;

        _network = GetNode<NetworkClient>("/root/Network");
        _network.LobbyStateReceived += OnLobbyStateReceived;
        _network.SessionJoined += OnSessionJoined;
        _network.ErrorReceived += OnErrorReceived;

        // Request lobby state to get available tracks
        RequestLobbyState();
    }

    private async void RequestLobbyState()
    {
        _statusLabel!.Text = "Loading tracks...";
        await _network!.RequestLobbyStateAsync();
    }

    private void OnLobbyStateReceived()
    {
        var lobbyState = _network!.LastLobbyState;
        if (lobbyState == null) return;

        _tracks.Clear();
        _trackSelector!.Clear();

        if (lobbyState.TrackConfigs.Length == 0)
        {
            _statusLabel!.Text = "No tracks available!";
            _statusLabel.Modulate = Colors.Red;
            _createButton!.Disabled = true;
            return;
        }

        for (int i = 0; i < lobbyState.TrackConfigs.Length; i++)
        {
            var track = lobbyState.TrackConfigs[i];
            _tracks[i] = track;
            _trackSelector.AddItem(track.Name, i);
        }

        _trackSelector.Selected = 0;
        _statusLabel!.Text = $"{lobbyState.TrackConfigs.Length} track(s) available";
        _statusLabel.Modulate = Colors.Green;
        _createButton!.Disabled = false;
    }

    private async void OnCreatePressed()
    {
        if (_tracks.Count == 0) return;

        var selectedIndex = _trackSelector!.Selected;
        if (!_tracks.ContainsKey(selectedIndex)) return;

        var track = _tracks[selectedIndex];
        var maxPlayers = (byte)_maxPlayersSpinBox!.Value;
        var aiCount = (byte)_aiCountSpinBox!.Value;
        var lapLimit = (byte)_lapLimitSpinBox!.Value;

        _statusLabel!.Text = $"Creating session on {track.Name}...";
        _statusLabel.Modulate = Colors.Yellow;
        _createButton!.Disabled = true;

        await _network!.CreateSessionAsync(track.Id, maxPlayers, aiCount, lapLimit);
    }

    private void OnSessionJoined(string sessionId, byte gridPosition)
    {
        _statusLabel!.Text = $"Session created! Grid position: {gridPosition}";
        _statusLabel.Modulate = Colors.Green;

        // Close dialog after brief delay
        GetTree().CreateTimer(1.0).Timeout += QueueFree;
    }

    private void OnErrorReceived(ushort code, string message)
    {
        _statusLabel!.Text = $"Error: {message}";
        _statusLabel.Modulate = Colors.Red;
        _createButton!.Disabled = false;
    }

    private void OnCancelPressed()
    {
        QueueFree();
    }
}
