using Godot;

namespace ApexSim;

public partial class MainMenu : Control
{
    // UI Elements
    private Label? _subtitleLabel;
    private Button? _btnConnect;
    private Button? _btnCreateSession;
    private Button? _btnJoinSession;
    private Button? _btnLeaveSession;
    private Button? _btnStartSession;
    private Button? _btnQuit;
    private Label? _connectionStatus;

    // Scene paths
    private const string ConnectionDialogScene = "res://scenes/connection_dialog.tscn";
    private const string SessionBrowserScene = "res://scenes/session_browser.tscn";
    private const string SessionCreationScene = "res://scenes/session_creation.tscn";

    private NetworkClient? _network;
    private bool _inSession = false;

    public override void _Ready()
    {
        // Get UI elements
        _subtitleLabel = GetNode<Label>("VBoxContainer/Subtitle");
        _btnConnect = GetNode<Button>("VBoxContainer/ButtonConnect");
        _btnCreateSession = GetNode<Button>("VBoxContainer/ButtonCreateSession");
        _btnJoinSession = GetNode<Button>("VBoxContainer/ButtonJoinSession");
        _btnLeaveSession = GetNode<Button>("VBoxContainer/ButtonLeaveSession");
        _btnStartSession = GetNode<Button>("VBoxContainer/ButtonStartSession");
        _btnQuit = GetNode<Button>("VBoxContainer/ButtonQuit");
        _connectionStatus = GetNode<Label>("ConnectionStatus");

        // Connect signals
        _btnConnect.Pressed += OnConnectPressed;
        _btnCreateSession.Pressed += OnCreateSessionPressed;
        _btnJoinSession.Pressed += OnJoinSessionPressed;
        _btnLeaveSession.Pressed += OnLeaveSessionPressed;
        _btnStartSession.Pressed += OnStartSessionPressed;
        _btnQuit.Pressed += OnQuitPressed;

        // Get network singleton
        _network = GetNode<NetworkClient>("/root/Network");

        // Connect to network signals
        _network.AuthenticationSuccess += OnAuthenticationSuccess;
        _network.DisconnectedFromServer += OnDisconnected;
        _network.SessionJoined += OnSessionJoined;
        _network.SessionLeft += OnSessionLeft;
        _network.ErrorReceived += OnErrorReceived;

        // Set initial state
        UpdateUIState();

        // Add to group for communication
        AddToGroup("main_menu");
    }

    private void UpdateUIState()
    {
        bool connected = _network!.IsConnected;

        _btnConnect.Visible = !connected;
        _btnCreateSession.Visible = connected && !_inSession;
        _btnJoinSession.Visible = connected && !_inSession;
        _btnLeaveSession.Visible = connected && _inSession;
        _btnStartSession.Visible = connected && _inSession;

        if (connected)
        {
            var playerId = _network.PlayerId;
            var shortId = playerId.Length > 8 ? playerId.Substring(0, 8) : playerId;
            _connectionStatus!.Text = $"Connected | Player: {_network.PlayerName} ({shortId}...)";
            _connectionStatus.Modulate = Colors.Green;
        }
        else
        {
            _connectionStatus!.Text = "Not connected";
            _connectionStatus.Modulate = Colors.Gray;
        }
    }

    private void OnConnectPressed()
    {
        var dialogScene = GD.Load<PackedScene>(ConnectionDialogScene);
        var dialog = dialogScene.Instantiate();
        GetTree().Root.AddChild(dialog);
    }

    private void OnCreateSessionPressed()
    {
        var dialogScene = GD.Load<PackedScene>(SessionCreationScene);
        var dialog = dialogScene.Instantiate();
        GetTree().Root.AddChild(dialog);
    }

    private void OnJoinSessionPressed()
    {
        var dialogScene = GD.Load<PackedScene>(SessionBrowserScene);
        var dialog = dialogScene.Instantiate();
        GetTree().Root.AddChild(dialog);
    }

    private async void OnLeaveSessionPressed()
    {
        _subtitleLabel!.Text = "Leaving session...";
        await _network!.LeaveSessionAsync();
    }

    private async void OnStartSessionPressed()
    {
        _subtitleLabel!.Text = "Starting session...";
        await _network!.StartSessionAsync();
    }

    private void OnQuitPressed()
    {
        _network!.DisconnectFromServer();
        GetTree().Quit();
    }

    // Network event handlers
    private void OnAuthenticationSuccess(string playerId, uint serverVersion)
    {
        _subtitleLabel!.Text = $"Connected! Server v{serverVersion}";
        UpdateUIState();
    }

    private void OnDisconnected()
    {
        _inSession = false;
        _subtitleLabel!.Text = "Disconnected from server";
        UpdateUIState();
    }

    private void OnSessionJoined(string sessionId, byte gridPosition)
    {
        _inSession = true;
        _subtitleLabel!.Text = $"Joined session! Grid position: {gridPosition}";
        UpdateUIState();
    }

    private void OnSessionLeft()
    {
        _inSession = false;
        _subtitleLabel!.Text = "Left session";
        UpdateUIState();
    }

    private void OnErrorReceived(ushort code, string message)
    {
        _subtitleLabel!.Text = $"Error [{code}]: {message}";
    }

    // Called by ConnectionDialog when successfully connected
    public void ShowLobbyInterface()
    {
        UpdateUIState();
    }
}
