using Godot;
using System.Collections.Generic;
using System.Threading.Tasks;

namespace ApexSim;

public partial class SessionBrowserDialog : Control
{
    private VBoxContainer? _sessionList;
    private Button? _refreshButton;
    private Button? _closeButton;
    private Label? _statusLabel;
    private Button? _carSelectorButton;
    private NetworkClient? _network;
    private List<SessionSummary> _sessions = new();
    private CarConfigSummary? _selectedCar = null;
    private PackedScene? _carSelectionScene;

    public override void _Ready()
    {
        _sessionList = GetNode<VBoxContainer>("Panel/VBox/ScrollContainer/SessionList");
        _refreshButton = GetNode<Button>("Panel/VBox/ButtonBar/RefreshButton");
        _closeButton = GetNode<Button>("Panel/VBox/ButtonBar/CloseButton");
        _statusLabel = GetNode<Label>("Panel/VBox/StatusLabel");
        _carSelectorButton = GetNode<Button>("Panel/VBox/CarSelection/CarSelectorButton");

        _refreshButton.Pressed += OnRefreshPressed;
        _closeButton.Pressed += OnClosePressed;
        _carSelectorButton.Pressed += OnCarSelectorPressed;

        _network = GetNode<NetworkClient>("/root/Network");
        _network.LobbyStateReceived += OnLobbyStateReceived;

        // Load car selection scene
        _carSelectionScene = GD.Load<PackedScene>("res://scenes/car_selection.tscn");

        // Request initial lobby state
        RefreshSessions();

        // If we already have cached data, use it immediately so the dialog feels snappier.
        if (_network!.LastLobbyState != null)
        {
            ApplyLobbyState(_network.LastLobbyState);
        }
    }

    private async void RefreshSessions()
    {
        if (_network == null)
        {
            return;
        }

        if (!_network.IsConnected)
        {
            _statusLabel!.Text = "Not connected to server";
            _statusLabel.Modulate = Colors.Red;
            return;
        }

        if (_network.LastLobbyState != null)
        {
            ApplyLobbyState(_network.LastLobbyState);
        }

        _statusLabel!.Text = "Loading sessions...";
        _statusLabel.Modulate = Colors.Yellow;
        await _network!.RequestLobbyStateAsync();
    }

    private void OnRefreshPressed()
    {
        RefreshSessions();
    }

    private void OnClosePressed()
    {
        QueueFree();
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

        // Refresh session list to update join button states
        if (_network?.LastLobbyState != null)
        {
            ApplyLobbyState(_network.LastLobbyState);
        }
    }

    private void OnLobbyStateReceived()
    {
        var lobbyState = _network!.LastLobbyState;
        if (lobbyState == null) return;

        ApplyLobbyState(lobbyState);
    }

    private void ApplyLobbyState(LobbyStateMessage lobbyState)
    {
        if (!IsInsideTree()) return;

        _sessions.Clear();
        foreach (var child in _sessionList!.GetChildren())
        {
            child.QueueFree();
        }

        if (lobbyState.AvailableSessions.Length == 0)
        {
            _statusLabel!.Text = "No sessions available. Create one!";
            _statusLabel.Modulate = Colors.Gray;

            var noSessionsLabel = new Label();
            noSessionsLabel.Text = "No active sessions";
            noSessionsLabel.HorizontalAlignment = HorizontalAlignment.Center;
            noSessionsLabel.AddThemeColorOverride("font_color", Colors.Gray);
            _sessionList.AddChild(noSessionsLabel);
            return;
        }

        _statusLabel!.Text = $"{lobbyState.AvailableSessions.Length} session(s) available";
        _statusLabel.Modulate = Colors.Green;

        foreach (var session in lobbyState.AvailableSessions)
        {
            _sessions.Add(session);
            var sessionItem = CreateSessionItem(session);
            _sessionList.AddChild(sessionItem);
        }
    }

    private Control CreateSessionItem(SessionSummary session)
    {
        var panel = new PanelContainer();
        var hbox = new HBoxContainer();
        panel.AddChild(hbox);

        // Session info
        var vbox = new VBoxContainer();
        vbox.SizeFlagsHorizontal = Control.SizeFlags.ExpandFill;
        hbox.AddChild(vbox);

        var titleLabel = new Label();
        titleLabel.Text = session.TrackName;
        titleLabel.AddThemeFontSizeOverride("font_size", 18);
        titleLabel.AddThemeColorOverride("font_color", new Color(0, 0.8f, 1));
        vbox.AddChild(titleLabel);

        var infoLabel = new Label();
        infoLabel.Text = $"Host: {session.HostName} | Players: {session.PlayerCount}/{session.MaxPlayers}";
        infoLabel.AddThemeFontSizeOverride("font_size", 14);
        vbox.AddChild(infoLabel);

        var stateLabel = new Label();
        stateLabel.Text = $"State: {session.State}";
        stateLabel.AddThemeFontSizeOverride("font_size", 12);
        stateLabel.AddThemeColorOverride("font_color", GetStateColor(session.State));
        vbox.AddChild(stateLabel);

        // Join button
        var canJoin = session.State == SessionState.Lobby && session.PlayerCount < session.MaxPlayers && _selectedCar != null;

        if (canJoin)
        {
            var joinButton = new Button();
            joinButton.Text = "Join";
            joinButton.CustomMinimumSize = new Vector2(100, 40);
            joinButton.Pressed += () => OnJoinSession(session, joinButton);
            hbox.AddChild(joinButton);
        }
        else
        {
            var disabledLabel = new Label();
            if (_selectedCar == null)
            {
                disabledLabel.Text = "Select a car first";
            }
            else
            {
                disabledLabel.Text = session.State != SessionState.Lobby ? "In Progress" : "Full";
            }
            disabledLabel.CustomMinimumSize = new Vector2(100, 40);
            disabledLabel.HorizontalAlignment = HorizontalAlignment.Center;
            disabledLabel.VerticalAlignment = VerticalAlignment.Center;
            disabledLabel.AddThemeColorOverride("font_color", Colors.Gray);
            hbox.AddChild(disabledLabel);
        }

        return panel;
    }

    private Color GetStateColor(SessionState state)
    {
        return state switch
        {
            SessionState.Lobby => Colors.Green,
            SessionState.Countdown => Colors.Yellow,
            SessionState.Racing => Colors.Orange,
            SessionState.Finished => Colors.Gray,
            _ => Colors.White
        };
    }

    private async void OnJoinSession(SessionSummary session, Button? joinButton)
    {
        if (_selectedCar == null)
        {
            _statusLabel!.Text = "Select a car before joining";
            _statusLabel.Modulate = Colors.Red;
            return;
        }

        if (joinButton != null)
        {
            joinButton.Disabled = true;
        }

        _statusLabel!.Text = $"Joining {session.TrackName} with {_selectedCar.Name}...";
        _statusLabel.Modulate = Colors.Yellow;

        await _network!.SelectCarAsync(_selectedCar.Id);
        await _network!.JoinSessionAsync(session.Id);
        QueueFree(); // Close browser after joining
    }

    public override void _ExitTree()
    {
        if (_network != null)
        {
            _network.LobbyStateReceived -= OnLobbyStateReceived;
        }
    }
}
