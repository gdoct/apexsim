using Godot;
using System.Collections.Generic;

namespace ApexSim;

public partial class SessionCreationDialog : Control
{
    private OptionButton? _trackSelector;
    private Button? _carSelectorButton;
    private OptionButton? _sessionTypeSelector;
    private SpinBox? _maxPlayersSpinBox;
    private SpinBox? _aiCountSpinBox;
    private SpinBox? _lapLimitSpinBox;
    private Button? _createButton;
    private Button? _cancelButton;
    private Label? _statusLabel;
    private NetworkClient? _network;
    private Dictionary<int, TrackConfigSummary> _tracks = new();
    private CarConfigSummary? _selectedCar = null;
    private PackedScene? _carSelectionScene;

    public override void _Ready()
    {
        _trackSelector = GetNode<OptionButton>("Panel/VBox/TrackSelection/OptionButton");
        _carSelectorButton = GetNode<Button>("Panel/VBox/CarSelection/CarSelectorButton");
        _sessionTypeSelector = GetNode<OptionButton>("Panel/VBox/SessionType/SessionTypeOptionButton");
        _maxPlayersSpinBox = GetNode<SpinBox>("Panel/VBox/MaxPlayers/SpinBox");
        _aiCountSpinBox = GetNode<SpinBox>("Panel/VBox/AICount/SpinBox");
        _lapLimitSpinBox = GetNode<SpinBox>("Panel/VBox/LapLimit/SpinBox");
        _createButton = GetNode<Button>("Panel/VBox/ButtonBar/CreateButton");
        _cancelButton = GetNode<Button>("Panel/VBox/ButtonBar/CancelButton");
        _statusLabel = GetNode<Label>("Panel/VBox/StatusLabel");

        // Populate session type selector
        _sessionTypeSelector.AddItem("Multiplayer", (int)SessionKind.Multiplayer);
        _sessionTypeSelector.AddItem("Practice", (int)SessionKind.Practice);
        _sessionTypeSelector.AddItem("Sandbox", (int)SessionKind.Sandbox);
        _sessionTypeSelector.Selected = 0;

        _createButton.Pressed += OnCreatePressed;
        _cancelButton.Pressed += OnCancelPressed;
        _carSelectorButton.Pressed += OnCarSelectorPressed;

        _network = GetNode<NetworkClient>("/root/Network");
        _network.LobbyStateReceived += OnLobbyStateReceived;
        _network.SessionJoined += OnSessionJoined;
        _network.ErrorReceived += OnErrorReceived;

        // Load car selection scene
        _carSelectionScene = GD.Load<PackedScene>("res://scenes/car_selection.tscn");

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
        if (_tracks.Count == 0)
        {
            _statusLabel!.Text = "Lobby data unavailable";
            _statusLabel.Modulate = Colors.Red;
        }
    }

    private void OnCarSelectorPressed()
    {
        if (_carSelectionScene == null) return;

        var carSelectionDialog = _carSelectionScene.Instantiate<CarSelectionDialog>();
        carSelectionDialog.CarSelected += OnCarSelected;
        GetTree().Root.AddChild(carSelectionDialog);
    }

    private void OnCarSelected(string carId, string carName)
    {
        // Find the car in the lobby state
        if (_network?.LastLobbyState != null)
        {
            foreach (var car in _network.LastLobbyState.CarConfigs)
            {
                if (car.Id.ToString() == carId)
                {
                    _selectedCar = car;
                    break;
                }
            }
        }

        if (_carSelectorButton != null)
        {
            _carSelectorButton.Text = carName;
            _carSelectorButton.AddThemeColorOverride("font_color", new Color(0, 1, 0.4f));
        }

        // Update create button state
        UpdateCreateButtonState();
    }

    private void OnLobbyStateReceived()
    {
        // Check if dialog is still valid (not queued for deletion)
        if (!IsInsideTree()) return;

        var lobbyState = _network!.LastLobbyState;
        if (lobbyState == null) return;

        PopulateFromLobbyState(lobbyState);
    }

    private void PopulateFromLobbyState(LobbyStateMessage lobbyState)
    {
        // Check if dialog is still valid (not queued for deletion)
        if (!IsInsideTree()) return;
        if (_trackSelector == null) return;

        // Remember previous selections
        int previousTrackSelection = _trackSelector.Selected >= 0 ? _trackSelector.Selected : -1;

        _tracks.Clear();
        _trackSelector!.Clear();

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

        // Sort tracks by name
        var sortedTracks = new System.Collections.Generic.List<TrackConfigSummary>(lobbyState.TrackConfigs);
        sortedTracks.Sort((a, b) => string.Compare(a.Name, b.Name, System.StringComparison.Ordinal));

        for (int i = 0; i < sortedTracks.Count; i++)
        {
            var track = sortedTracks[i];
            _tracks[i] = track;
            _trackSelector.AddItem(track.Name, i);
        }

        // Restore previous selections, or default to 0 if first time
        _trackSelector.Selected = previousTrackSelection >= 0 && previousTrackSelection < _tracks.Count ? previousTrackSelection : 0;

        _statusLabel!.Text = $"{lobbyState.TrackConfigs.Length} track(s), {lobbyState.CarConfigs.Length} car(s) available";
        _statusLabel.Modulate = Colors.Green;

        UpdateCreateButtonState();
    }

    private void UpdateCreateButtonState()
    {
        if (_createButton == null) return;

        // Enable create button only if both track and car are selected
        _createButton.Disabled = _tracks.Count == 0 || _selectedCar == null;
    }

    private async void OnCreatePressed()
    {
        if (_tracks.Count == 0 || _selectedCar == null) return;

        var selectedIndex = _trackSelector!.Selected;
        if (!_tracks.ContainsKey(selectedIndex)) return;

        var track = _tracks[selectedIndex];
        var maxPlayers = (byte)_maxPlayersSpinBox!.Value;
        var aiCount = (byte)_aiCountSpinBox!.Value;
        var lapLimit = (byte)_lapLimitSpinBox!.Value;

        var sessionTypeIndex = _sessionTypeSelector!.Selected;
        var sessionKind = (SessionKind)sessionTypeIndex;

        _statusLabel!.Text = $"Creating session on {track.Name} with {_selectedCar.Name}...";
        _statusLabel.Modulate = Colors.Yellow;
        _createButton!.Disabled = true;

        await _network!.SelectCarAsync(_selectedCar.Id);
        await _network!.CreateSessionAsync(track.Id, maxPlayers, aiCount, lapLimit, sessionKind);
    }

    private void OnSessionJoined(string sessionId, byte gridPosition)
    {
        _statusLabel!.Text = $"Session created! Grid position: {gridPosition}";
        _statusLabel.Modulate = Colors.Green;

        // Close dialog and return to main menu where user can start the session
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

    public override void _ExitTree()
    {
        // Unsubscribe from events to prevent accessing disposed objects
        if (_network != null)
        {
            _network.LobbyStateReceived -= OnLobbyStateReceived;
            _network.SessionJoined -= OnSessionJoined;
            _network.ErrorReceived -= OnErrorReceived;
        }
    }
}
