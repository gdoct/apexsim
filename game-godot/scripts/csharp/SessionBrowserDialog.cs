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
    private NetworkClient? _network;
    private List<SessionSummary> _sessions = new();

    public override void _Ready()
    {
        _sessionList = GetNode<VBoxContainer>("Panel/VBox/ScrollContainer/SessionList");
        _refreshButton = GetNode<Button>("Panel/VBox/ButtonBar/RefreshButton");
        _closeButton = GetNode<Button>("Panel/VBox/ButtonBar/CloseButton");
        _statusLabel = GetNode<Label>("Panel/VBox/StatusLabel");

        _refreshButton.Pressed += OnRefreshPressed;
        _closeButton.Pressed += OnClosePressed;

        _network = GetNode<NetworkClient>("/root/Network");
        _network.LobbyStateReceived += OnLobbyStateReceived;

        // Request initial lobby state
        RefreshSessions();
    }

    private async void RefreshSessions()
    {
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

    private void OnLobbyStateReceived()
    {
        var lobbyState = _network!.LastLobbyState;
        if (lobbyState == null) return;

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
        if (session.State == SessionState.Lobby && session.PlayerCount < session.MaxPlayers)
        {
            var joinButton = new Button();
            joinButton.Text = "Join";
            joinButton.CustomMinimumSize = new Vector2(100, 40);
            joinButton.Pressed += () => OnJoinSession(session);
            hbox.AddChild(joinButton);
        }
        else
        {
            var disabledLabel = new Label();
            disabledLabel.Text = session.State != SessionState.Lobby ? "In Progress" : "Full";
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

    private async void OnJoinSession(SessionSummary session)
    {
        _statusLabel!.Text = $"Joining {session.TrackName}...";
        _statusLabel.Modulate = Colors.Yellow;
        await _network!.JoinSessionAsync(session.Id);
        QueueFree(); // Close browser after joining
    }
}
