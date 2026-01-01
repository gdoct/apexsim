using Godot;
using System.Collections.Generic;

namespace ApexSim;

public partial class SessionCreationDialog : Control
{
    private OptionButton? _trackSelector;
    private OptionButton? _carSelector;
    private SpinBox? _maxPlayersSpinBox;
    private SpinBox? _aiCountSpinBox;
    private SpinBox? _lapLimitSpinBox;
    private Button? _createButton;
    private Button? _cancelButton;
    private Label? _statusLabel;
    private NetworkClient? _network;
    private Dictionary<int, TrackConfigSummary> _tracks = new();
    private Dictionary<int, CarConfigSummary> _cars = new();

    public override void _Ready()
    {
        _trackSelector = GetNode<OptionButton>("Panel/VBox/TrackSelection/OptionButton");
        _carSelector = GetNode<OptionButton>("Panel/VBox/CarSelection/CarOptionButton");
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

        // If we already have cached data, use it immediately to avoid waiting on a roundtrip.
        if (_network!.LastLobbyState != null)
        {
            PopulateFromLobbyState(_network.LastLobbyState);
        }

        if (!_network.IsConnected)
        {
            _statusLabel!.Text = "Not connected to server";
            _statusLabel.Modulate = Colors.Red;
            return;
        }

        await _network.RequestLobbyStateAsync();

        // Simple timeout guard so we don't stay stuck on the loading message forever.
        await ToSignal(GetTree().CreateTimer(3.0), SceneTreeTimer.SignalName.Timeout);
        if (_tracks.Count == 0 && _cars.Count == 0)
        {
            _statusLabel!.Text = "Lobby data unavailable";
            _statusLabel.Modulate = Colors.Red;
        }
    }

    private void OnLobbyStateReceived()
    {
        var lobbyState = _network!.LastLobbyState;
        if (lobbyState == null) return;

        PopulateFromLobbyState(lobbyState);
    }

    private void PopulateFromLobbyState(LobbyStateMessage lobbyState)
    {
        if (_trackSelector == null || _carSelector == null) return;

        _tracks.Clear();
        _trackSelector!.Clear();
        _cars.Clear();
        _carSelector!.Clear();

        if (lobbyState.TrackConfigs.Length == 0)
        {
            _statusLabel!.Text = "No tracks available!";
            _statusLabel.Modulate = Colors.Red;
            _createButton!.Disabled = true;
            return;
        }

        if (lobbyState.CarConfigs.Length == 0)
        {
            _statusLabel!.Text = "No cars available!";
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

        for (int i = 0; i < lobbyState.CarConfigs.Length; i++)
        {
            var car = lobbyState.CarConfigs[i];
            _cars[i] = car;
            _carSelector.AddItem(car.Name, i);
        }

        _trackSelector.Selected = 0;
        _carSelector.Selected = 0;
        _statusLabel!.Text = $"{lobbyState.TrackConfigs.Length} track(s), {lobbyState.CarConfigs.Length} car(s) available";
        _statusLabel.Modulate = Colors.Green;
        _createButton!.Disabled = false;
    }

    private async void OnCreatePressed()
    {
        if (_tracks.Count == 0 || _cars.Count == 0) return;

        var selectedIndex = _trackSelector!.Selected;
        if (!_tracks.ContainsKey(selectedIndex)) return;

        var selectedCarIndex = _carSelector!.Selected;
        if (!_cars.ContainsKey(selectedCarIndex)) return;

        var track = _tracks[selectedIndex];
        var car = _cars[selectedCarIndex];
        var maxPlayers = (byte)_maxPlayersSpinBox!.Value;
        var aiCount = (byte)_aiCountSpinBox!.Value;
        var lapLimit = (byte)_lapLimitSpinBox!.Value;

        _statusLabel!.Text = $"Creating session on {track.Name} with {car.Name}...";
        _statusLabel.Modulate = Colors.Yellow;
        _createButton!.Disabled = true;

        await _network!.SelectCarAsync(car.Id);
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
