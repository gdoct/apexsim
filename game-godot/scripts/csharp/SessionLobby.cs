using Godot;
using System;
using System.Collections.Generic;

namespace ApexSim;

public partial class SessionLobby : Control
{
    // UI Elements
    private Label? _sessionInfoLabel;
    private Label? _gameModeLabel;
    private VBoxContainer? _playersContainer;
    private TrackCard? _trackCard;
    private CarCard? _carCard;
    private Button? _selectTrackButton;
    private Button? _selectCarButton;
    private Button? _sandboxButton;
    private Button? _demoLapButton;
    private Button? _freePracticeButton;
    private Button? _leaveButton;
    private Label? _statusLabel;

    // Network and state
    private NetworkClient? _network;
    private PackedScene? _trackSelectionScene;
    private PackedScene? _carSelectionScene;
    private PackedScene? _trackViewScene;
    private bool _isHost = false;
    private GameMode _currentGameMode = GameMode.Lobby;
    private TrackConfigSummary? _currentTrack;
    private CarConfigSummary? _currentCar;

    public override void _Ready()
    {
        // Get UI elements
        _sessionInfoLabel = GetNode<Label>("MainContainer/VBox/Header/SessionInfo");
        _gameModeLabel = GetNode<Label>("MainContainer/VBox/Header/GameModeLabel");
        _playersContainer = GetNode<VBoxContainer>("MainContainer/VBox/HSplit/LeftPanel/PlayersList/PlayersContainer");
        _trackCard = GetNode<TrackCard>("MainContainer/VBox/HSplit/RightPanel/CardsHBox/TrackCardContainer/TrackCard");
        _carCard = GetNode<CarCard>("MainContainer/VBox/HSplit/RightPanel/CardsHBox/CarCardContainer/CarCard");
        _selectTrackButton = GetNode<Button>("MainContainer/VBox/HSplit/RightPanel/CardsHBox/TrackCardContainer/SelectTrackButton");
        _selectCarButton = GetNode<Button>("MainContainer/VBox/HSplit/RightPanel/CardsHBox/CarCardContainer/SelectCarButton");
        _sandboxButton = GetNode<Button>("MainContainer/VBox/HSplit/RightPanel/GameModeSection/ModeButtonsGrid/SandboxButton");
        _demoLapButton = GetNode<Button>("MainContainer/VBox/HSplit/RightPanel/GameModeSection/ModeButtonsGrid/DemoLapButton");
        _freePracticeButton = GetNode<Button>("MainContainer/VBox/HSplit/RightPanel/GameModeSection/ModeButtonsGrid/FreePracticeButton");
        _leaveButton = GetNode<Button>("MainContainer/VBox/BottomBar/LeaveButton");
        _statusLabel = GetNode<Label>("MainContainer/VBox/BottomBar/StatusLabel");

        // Connect signals
        _selectTrackButton.Pressed += OnSelectTrackPressed;
        _selectCarButton.Pressed += OnSelectCarPressed;
        _sandboxButton.Pressed += OnSandboxPressed;
        _demoLapButton.Pressed += OnDemoLapPressed;
        _freePracticeButton.Pressed += OnFreePracticePressed;
        _leaveButton.Pressed += OnLeavePressed;

        // Get network client
        _network = GetNode<NetworkClient>("/root/Network");
        _network.LobbyStateReceived += OnLobbyStateReceived;
        _network.GameModeChanged += OnGameModeChanged;
        _network.SessionLeft += OnSessionLeft;
        _network.ErrorReceived += OnErrorReceived;

        // Load selection scenes
        _trackSelectionScene = GD.Load<PackedScene>("res://scenes/track_selection.tscn");
        _carSelectionScene = GD.Load<PackedScene>("res://scenes/car_selection.tscn");
        _trackViewScene = GD.Load<PackedScene>("res://scenes/track_view.tscn");

        // Initial state update
        UpdateUI();
        RequestLobbyState();
    }

    private async void RequestLobbyState()
    {
        if (_network != null && _network.IsConnected)
        {
            await _network.RequestLobbyStateAsync();
        }
    }

    private void OnLobbyStateReceived()
    {
        if (!IsInsideTree()) return;

        var lobbyState = _network?.LastLobbyState;
        if (lobbyState == null) return;

        // Update session info and determine if we're host
        UpdateSessionInfo(lobbyState);

        // Update players list
        UpdatePlayersList(lobbyState);

        // Update track card if we found the session
        if (_currentTrack != null && _trackCard != null)
        {
            _trackCard.SetupCard(_currentTrack);
        }

        // Update car card with player's selected car
        UpdatePlayerCar(lobbyState);

        UpdateUI();
    }

    private void UpdateSessionInfo(LobbyStateMessage lobbyState)
    {
        if (_network?.CurrentSessionId == null) return;

        foreach (var session in lobbyState.AvailableSessions)
        {
            if (session.Id == _network.CurrentSessionId)
            {
                // Find the track config
                foreach (var track in lobbyState.TrackConfigs)
                {
                    if (track.Name == session.TrackName)
                    {
                        _currentTrack = track;
                        break;
                    }
                }

                _sessionInfoLabel!.Text = $"Track: {session.TrackName} | Players: {session.PlayerCount}/{session.MaxPlayers}";

                // Check if we're the host
                // For now, we'll enable host controls - the server will validate
                _isHost = true;
                return;
            }
        }
    }

    private void UpdatePlayerCar(LobbyStateMessage lobbyState)
    {
        if (_network?.PlayerId == null)
        {
            GD.PrintErr("UpdatePlayerCar: PlayerId is null!");
            return;
        }

        // Find our player in the lobby
        foreach (var player in lobbyState.PlayersInLobby)
        {
            if (player.Id == _network.PlayerId)
            {
                // Find the car config if the player has selected one
                if (player.SelectedCar != null)
                {
                    foreach (var car in lobbyState.CarConfigs)
                    {
                        // Compare as strings, case-insensitive
                        if (string.Equals(car.Id, player.SelectedCar, StringComparison.OrdinalIgnoreCase))
                        {
                            _currentCar = car;

                            if (_carCard != null)
                            {
                                // Use car.Id as the model path - it's the car folder name
                                _carCard.SetupCard(car, car.Id);
                            }
                            else
                            {
                                GD.PrintErr("UpdatePlayerCar: _carCard is NULL!");
                            }
                            break;
                        }
                    }

                    if (_currentCar == null)
                    {
                        GD.PrintErr($"UpdatePlayerCar: No matching car found for ID {player.SelectedCar}!");
                    }
                }
                else
                {
                    GD.PrintErr("UpdatePlayerCar: Player has no selected car!");
                }
                break;
            }
        }
    }

    private void UpdatePlayersList(LobbyStateMessage lobbyState)
    {
        // Clear existing player labels
        foreach (Node child in _playersContainer!.GetChildren())
        {
            child.QueueFree();
        }

        // Add players in this session
        foreach (var player in lobbyState.PlayersInLobby)
        {
            if (player.InSession == _network?.CurrentSessionId)
            {
                var playerLabel = new Label
                {
                    Text = $"{player.Name} {(player.SelectedCar != null ? "✓" : "○")}",
                    ThemeTypeVariation = "HeaderMedium"
                };
                playerLabel.AddThemeColorOverride("font_color",
                    player.SelectedCar != null ? new Color(0, 1, 0.4f) : new Color(0.7f, 0.7f, 0.7f));
                playerLabel.AddThemeFontSizeOverride("font_size", 16);
                _playersContainer.AddChild(playerLabel);
            }
        }
    }

    private void TransitionToTrackView()
    {
        GD.Print("TransitionToTrackView called");

        // Use SceneManager for proper scene cleanup
        var sceneManager = GetNode("/root/SceneManager");
        if (sceneManager != null && sceneManager.HasMethod("change_scene"))
        {
            GD.Print("Using SceneManager to change scene...");
            sceneManager.Call("change_scene", "res://scenes/track_view.tscn", false);
        }
        else
        {
            GD.PrintErr("SceneManager not found or missing change_scene method!");
        }
    }

    private void OnGameModeChanged(int modeInt)
    {
        GameMode mode = (GameMode)modeInt;
        GD.Print($"OnGameModeChanged called with mode: {mode} (int: {modeInt})");

        _currentGameMode = mode;
        UpdateGameModeLabel();
        UpdateUI();

        // If we've transitioned to a driving mode, load the track view scene
        if (mode == GameMode.Sandbox || mode == GameMode.DemoLap || mode == GameMode.FreePractice)
        {
            GD.Print($"Mode is {mode}, transitioning to track view...");
            _statusLabel!.Text = $"Starting {mode} mode...";
            _statusLabel.Modulate = new Color(0, 1, 0.4f);

            // Transition to the driving view scene
            TransitionToTrackView();
        }
        else if (mode != GameMode.Lobby)
        {
            GD.Print($"Mode is {mode}, not transitioning to track view");
            _statusLabel!.Text = $"Session started in {mode} mode!";
            _statusLabel.Modulate = new Color(0, 1, 0.4f);
        }
    }

    private void UpdateGameModeLabel()
    {
        string modeText = _currentGameMode switch
        {
            GameMode.Lobby => "Mode: Lobby",
            GameMode.Sandbox => "Mode: Sandbox",
            GameMode.Countdown => "Mode: Countdown",
            GameMode.DemoLap => "Mode: Demo Lap",
            GameMode.FreePractice => "Mode: Free Practice",
            GameMode.Replay => "Mode: Replay",
            GameMode.Qualification => "Mode: Qualification",
            GameMode.Race => "Mode: Race",
            _ => "Mode: Unknown"
        };

        _gameModeLabel!.Text = modeText;
    }

    private void UpdateUI()
    {
        // Host controls - only enabled in Lobby mode and if host
        bool canStartSession = _isHost && _currentGameMode == GameMode.Lobby;
        _selectTrackButton!.Disabled = !_isHost || _currentGameMode != GameMode.Lobby;
        _sandboxButton!.Disabled = !canStartSession;
        _demoLapButton!.Disabled = !canStartSession;
        _freePracticeButton!.Disabled = !canStartSession;
    }

    private void OnSelectTrackPressed()
    {
        if (_trackSelectionScene == null) return;

        var trackSelectionDialog = _trackSelectionScene.Instantiate<TrackSelectionDialog>();
        trackSelectionDialog.TrackSelected += OnTrackSelected;
        GetTree().Root.AddChild(trackSelectionDialog);
    }

    private void OnTrackSelected(string trackId, string trackName)
    {
        // TODO: Send track selection to server
        // Note: Current protocol doesn't support changing track after session creation
        // This would require a new message type
        _statusLabel!.Text = "Track selection after session creation not yet supported";
        _statusLabel.Modulate = Colors.Yellow;
    }

    private void OnSelectCarPressed()
    {
        if (_carSelectionScene == null) return;

        var carSelectionDialog = _carSelectionScene.Instantiate<CarSelectionDialog>();
        carSelectionDialog.CarSelected += OnCarSelected;
        GetTree().Root.AddChild(carSelectionDialog);
    }

    private async void OnCarSelected(string carId, string carName)
    {
        _statusLabel!.Text = $"Selecting car: {carName}...";
        _statusLabel.Modulate = Colors.Yellow;

        if (_network != null)
        {
            await _network.SelectCarAsync(carId);
            _statusLabel.Text = $"Car selected: {carName}";
            _statusLabel.Modulate = new Color(0, 1, 0.4f);

            // Refresh lobby state to update player list and car card
            RequestLobbyState();
        }
    }

    private async void OnSandboxPressed()
    {
        if (_network == null || !_isHost)
        {
            GD.PrintErr($"Cannot start sandbox: network={_network != null}, isHost={_isHost}");
            return;
        }

        _statusLabel!.Text = "Starting Sandbox mode...";
        _statusLabel.Modulate = Colors.Yellow;
        _sandboxButton!.Disabled = true;

        await _network.SetGameModeAsync(GameMode.Sandbox);
    }

    private async void OnDemoLapPressed()
    {
        if (_network == null || !_isHost)
        {
            GD.PrintErr($"Cannot start demo lap: network={_network != null}, isHost={_isHost}");
            return;
        }

        _statusLabel!.Text = "Starting Demo Lap...";
        _statusLabel.Modulate = Colors.Yellow;
        _demoLapButton!.Disabled = true;

        await _network.SetGameModeAsync(GameMode.DemoLap);
    }

    private async void OnFreePracticePressed()
    {
        if (_network == null || !_isHost) return;

        _statusLabel!.Text = "Starting Free Practice...";
        _statusLabel.Modulate = Colors.Yellow;
        _freePracticeButton!.Disabled = true;

        await _network.SetGameModeAsync(GameMode.FreePractice);
    }

    private async void OnLeavePressed()
    {
        if (_network == null) return;

        _statusLabel!.Text = "Leaving session...";
        _statusLabel.Modulate = Colors.Yellow;
        _leaveButton!.Disabled = true;

        await _network.LeaveSessionAsync();
    }

    private void OnSessionLeft()
    {
        // Return to main menu or session browser
        // For now, we'll just free this scene
        _statusLabel!.Text = "Left session";
        _statusLabel.Modulate = new Color(0, 1, 0.4f);

        // TODO: Transition back to main menu
        GetTree().CreateTimer(0.5).Timeout += QueueFree;
    }

    private void OnErrorReceived(ushort code, string message)
    {
        _statusLabel!.Text = $"Error: {message}";
        _statusLabel.Modulate = Colors.Red;

        // Re-enable buttons
        UpdateUI();
    }

    public override void _ExitTree()
    {
        // Unsubscribe from events
        if (_network != null)
        {
            _network.LobbyStateReceived -= OnLobbyStateReceived;
            _network.GameModeChanged -= OnGameModeChanged;
            _network.SessionLeft -= OnSessionLeft;
            _network.ErrorReceived -= OnErrorReceived;
        }
    }
}
