using Godot;
using System;
using System.Collections.Generic;
using System.Linq;

namespace ApexSim;

public partial class TrackSelectionDialog : Control
{
    [Signal]
    public delegate void TrackSelectedEventHandler(string trackId, string trackName);

    private GridContainer? _gridContainer;
    private LineEdit? _searchInput;
    private Label? _statusLabel;
    private Button? _selectButton;
    private Button? _cancelButton;
    private NetworkClient? _network;

    private List<TrackConfigSummary> _allTracks = new();
    private List<TrackCard> _trackCards = new();
    private TrackCard? _selectedCard = null;
    private PackedScene? _trackCardScene;

    public override void _Ready()
    {
        _gridContainer = GetNode<GridContainer>("Panel/VBox/ScrollContainer/GridContainer");
        _searchInput = GetNode<LineEdit>("Panel/VBox/SearchBar/SearchInput");
        _statusLabel = GetNode<Label>("Panel/VBox/StatusLabel");
        _selectButton = GetNode<Button>("Panel/VBox/ButtonBar/SelectButton");
        _cancelButton = GetNode<Button>("Panel/VBox/ButtonBar/CancelButton");

        _selectButton.Pressed += OnSelectPressed;
        _cancelButton.Pressed += OnCancelPressed;
        _searchInput.TextChanged += OnSearchTextChanged;

        _network = GetNode<NetworkClient>("/root/Network");
        _network.LobbyStateReceived += OnLobbyStateReceived;

        // Load the track card scene
        _trackCardScene = GD.Load<PackedScene>("res://scenes/track_card.tscn");

        // Request lobby state to get available tracks
        RequestTrackData();
    }

    private async void RequestTrackData()
    {
        _statusLabel!.Text = "Loading tracks...";
        GD.Print("[TrackSelection] RequestTrackData started");

        // Use cached data if available
        if (_network!.LastLobbyState != null)
        {
            GD.Print("[TrackSelection] Using cached lobby state");
            PopulateTracks(_network.LastLobbyState);
            return; // Don't request again if we already have data
        }

        GD.Print("[TrackSelection] Requesting lobby state from server");

        if (!_network.IsConnected)
        {
            _statusLabel!.Text = "Not connected to server";
            _statusLabel.Modulate = Colors.Red;
            return;
        }

        await _network.RequestLobbyStateAsync();

        // Timeout guard
        GD.Print("[TrackSelection] After RequestLobbyStateAsync, _allTracks.Count = " + _allTracks.Count);
        await ToSignal(GetTree().CreateTimer(3.0), SceneTreeTimer.SignalName.Timeout);
        if (_allTracks.Count == 0)
        {
            _statusLabel!.Text = "No tracks available";
            _statusLabel.Modulate = Colors.Red;
        }
    }

    private void OnLobbyStateReceived()
    {
        if (!IsInsideTree()) return;

        var lobbyState = _network!.LastLobbyState;
        if (lobbyState == null) return;

        PopulateTracks(lobbyState);
    }

    private void PopulateTracks(LobbyStateMessage lobbyState)
    {
        if (!IsInsideTree()) return;
        if (_gridContainer == null) return;

        GD.Print($"[TrackSelection] PopulateTracks called with {lobbyState.TrackConfigs.Length} tracks from server");

        _allTracks.Clear();

        // Sort tracks by name
        var sortedTracks = new List<TrackConfigSummary>(lobbyState.TrackConfigs);
        sortedTracks.Sort((a, b) => string.Compare(a.Name, b.Name, StringComparison.Ordinal));

        foreach (var track in sortedTracks)
        {
            GD.Print($"[TrackSelection]   Server track: {track.Name} (ID: {track.Id})");
        }

        _allTracks.AddRange(sortedTracks);

        if (_allTracks.Count == 0)
        {
            GD.Print("[TrackSelection] No tracks available!");
            _statusLabel!.Text = "No tracks available";
            _statusLabel.Modulate = Colors.Red;
            return;
        }

        _statusLabel!.Text = $"{_allTracks.Count} track(s) available";
        _statusLabel.Modulate = Colors.Green;

        // Create track list items
        CreateTrackCards();
    }

    private void CreateTrackCards(string filter = "")
    {
        if (_gridContainer == null || _trackCardScene == null) return;

        GD.Print($"[TrackSelection] CreateTrackCards called with {_allTracks.Count} total tracks");

        // Clear existing cards
        foreach (var card in _trackCards)
        {
            card.QueueFree();
        }
        _trackCards.Clear();

        // Filter tracks based on search
        var filteredTracks = _allTracks;
        if (!string.IsNullOrWhiteSpace(filter))
        {
            filteredTracks = _allTracks.Where(t => t.Name.ToLower().Contains(filter.ToLower())).ToList();
        }

        if (filteredTracks.Count == 0)
        {
            GD.Print("[TrackSelection] No tracks match filter");
            _statusLabel!.Text = "No tracks match your search";
            _statusLabel.Modulate = Colors.Yellow;
            return;
        }

        _statusLabel!.Text = $"{filteredTracks.Count} track(s) available";
        _statusLabel.Modulate = Colors.Green;

        GD.Print($"[TrackSelection] Adding {filteredTracks.Count} cards to GridContainer");

        // Create cards for filtered tracks
        foreach (var track in filteredTracks)
        {
            var cardInstance = _trackCardScene.Instantiate<TrackCard>();
            _gridContainer.AddChild(cardInstance);
            _trackCards.Add(cardInstance);

            GD.Print($"[TrackSelection]   Adding track card: {track.Name} (ID: {track.Id})");
            cardInstance.SetupCard(track);
            cardInstance.CardClicked += OnCardClicked;
        }
        
        GD.Print($"[TrackSelection] GridContainer now has {_gridContainer.GetChildCount()} children");
    }

    private void OnCardClicked(TrackCard clickedCard)
    {
        // Deselect previous card
        if (_selectedCard != null && _selectedCard != clickedCard)
        {
            _selectedCard.IsSelected = false;
        }

        // Select new card
        _selectedCard = clickedCard;
        _selectedCard.IsSelected = true;

        // Emit signal and close immediately
        if (_selectedCard.TrackConfig != null)
        {
            EmitSignal(SignalName.TrackSelected, _selectedCard.TrackConfig.Id, _selectedCard.TrackConfig.Name);
            QueueFree();
        }
    }

    private void OnSearchTextChanged(string newText)
    {
        CreateTrackCards(newText);
        _selectedCard = null;
    }

    private void OnSelectPressed()
    {
        // This is now handled in OnCardClicked, but keep this for safety
        if (_selectedCard?.TrackConfig != null)
        {
            QueueFree();
        }
    }

    private void OnCancelPressed()
    {
        QueueFree();
    }

    public override void _ExitTree()
    {
        if (_network != null)
        {
            _network.LobbyStateReceived -= OnLobbyStateReceived;
        }

        // Clean up card event handlers
        foreach (var card in _trackCards)
        {
            if (IsInstanceValid(card))
            {
                card.CardClicked -= OnCardClicked;
            }
        }
    }
}
