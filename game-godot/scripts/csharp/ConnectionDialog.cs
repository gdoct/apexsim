using Godot;

namespace ApexSim;

public partial class ConnectionDialog : Control
{
    private LineEdit? _serverAddressEdit;
    private LineEdit? _portEdit;
    private LineEdit? _playerNameEdit;
    private LineEdit? _tokenEdit;
    private Button? _connectButton;
    private Label? _statusLabel;
    private NetworkClient? _network;

    public override void _Ready()
    {
        _serverAddressEdit = GetNode<LineEdit>("Panel/VBox/ServerAddress/LineEdit");
        _portEdit = GetNode<LineEdit>("Panel/VBox/Port/LineEdit");
        _playerNameEdit = GetNode<LineEdit>("Panel/VBox/PlayerName/LineEdit");
        _tokenEdit = GetNode<LineEdit>("Panel/VBox/Token/LineEdit");
        _connectButton = GetNode<Button>("Panel/VBox/ConnectButton");
        _statusLabel = GetNode<Label>("Panel/VBox/StatusLabel");

        _connectButton.Pressed += OnConnectPressed;

        // Get network singleton
        _network = GetNode<NetworkClient>("/root/Network");

        // Connect to network signals
        _network.ConnectedToServer += OnConnectedToServer;
        _network.DisconnectedFromServer += OnDisconnectedFromServer;
        _network.AuthenticationSuccess += OnAuthenticationSuccess;
        _network.AuthenticationFailed += OnAuthenticationFailed;

        // Load defaults
        _serverAddressEdit.Text = "127.0.0.1";
        _portEdit.Text = "9000";
        _playerNameEdit.Text = "Player";
        _tokenEdit.Text = "dev-token";
    }

    private void OnConnectPressed()
    {
        if (_network == null) return;

        _statusLabel!.Text = "Connecting...";
        _statusLabel.Modulate = Colors.Yellow;
        _connectButton!.Disabled = true;

        _network.ServerAddress = _serverAddressEdit!.Text;
        _network.ServerPort = int.Parse(_portEdit!.Text);
        _network.PlayerName = _playerNameEdit!.Text;
        _network.AuthToken = _tokenEdit!.Text;

        _network.ConnectToServer();
    }

    private void OnConnectedToServer()
    {
        _statusLabel!.Text = "Authenticating...";
        _statusLabel.Modulate = Colors.Yellow;
    }

    private void OnAuthenticationSuccess(string playerId, uint serverVersion)
    {
        _statusLabel!.Text = $"Connected! Player ID: {playerId.Substring(0, 8)}...";
        _statusLabel.Modulate = Colors.Green;

        // Close dialog and show main menu
        GetTree().CallDeferred("call_group", "main_menu", "show_lobby_interface");
        QueueFree();
    }

    private void OnAuthenticationFailed(string reason)
    {
        _statusLabel!.Text = $"Auth failed: {reason}";
        _statusLabel.Modulate = Colors.Red;
        _connectButton!.Disabled = false;
    }

    private void OnDisconnectedFromServer()
    {
        if (_statusLabel!.Modulate != Colors.Green) // Only update if not successfully connected
        {
            _statusLabel.Text = "Connection failed";
            _statusLabel.Modulate = Colors.Red;
            _connectButton!.Disabled = false;
        }
    }
}
