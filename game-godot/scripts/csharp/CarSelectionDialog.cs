using Godot;
using System;
using System.Collections.Generic;
using System.Linq;

namespace ApexSim;

public partial class CarSelectionDialog : Control
{
    [Signal]
    public delegate void CarSelectedEventHandler(string carId, string carName);

    private GridContainer? _gridContainer;
    private LineEdit? _searchInput;
    private Label? _statusLabel;
    private Button? _selectButton;
    private Button? _cancelButton;
    private NetworkClient? _network;

    private List<CarConfigSummary> _allCars = new();
    private List<CarCard> _carCards = new();
    private CarCard? _selectedCard = null;
    private PackedScene? _carCardScene;
    private bool _carsPopulated = false;

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

        // Load the car card scene
        _carCardScene = GD.Load<PackedScene>("res://scenes/car_card.tscn");

        // Request lobby state to get available cars
        RequestCarData();
    }

    private async void RequestCarData()
    {
        _statusLabel!.Text = "Loading cars...";

        // Use cached data if available
        if (_network!.LastLobbyState != null)
        {
            PopulateCars(_network.LastLobbyState);
            return; // Don't request again if we already have data
        }

        if (!_network.IsConnected)
        {
            _statusLabel!.Text = "Not connected to server";
            _statusLabel.Modulate = Colors.Red;
            return;
        }

        await _network.RequestLobbyStateAsync();

        // Timeout guard
        await ToSignal(GetTree().CreateTimer(3.0), SceneTreeTimer.SignalName.Timeout);
        if (_allCars.Count == 0)
        {
            _statusLabel!.Text = "No cars available";
            _statusLabel.Modulate = Colors.Red;
        }
    }

    private void OnLobbyStateReceived()
    {
        if (!IsInsideTree()) return;
        if (_carsPopulated) return; // Don't repopulate if already done

        var lobbyState = _network!.LastLobbyState;
        if (lobbyState == null) return;

        PopulateCars(lobbyState);
    }

    private void PopulateCars(LobbyStateMessage lobbyState)
    {
        if (!IsInsideTree()) return;
        if (_gridContainer == null) return;

        _allCars.Clear();

        // Load all local cars from content/cars directory
        var localCars = LoadLocalCars();

        // Create a set of server car IDs for quick lookup
        var serverCarIds = new HashSet<string>();
        foreach (var serverCar in lobbyState.CarConfigs)
        {
            serverCarIds.Add(serverCar.Id.ToString());
        }

        // Filter local cars to only those available on the server
        var availableCars = new List<CarConfigSummary>();
        foreach (var localCar in localCars)
        {
            var localId = localCar.Id.ToString();
            var hasMatch = serverCarIds.Contains(localId);

            if (hasMatch)
            {
                // Find the matching server car to get the server-provided data
                foreach (var serverCar in lobbyState.CarConfigs)
                {
                    if (serverCar.Id.ToString() == localCar.Id.ToString())
                    {
                        // Use server's authoritative ID and stats, with local model path
                        availableCars.Add(new CarConfigSummary
                        {
                            Id = serverCar.Id, // Use server's ID (authoritative)
                            Name = serverCar.Name, // Use server's name (authoritative)
                            ModelPath = localCar.ModelPath, // Use local path for loading model
                            MassKg = serverCar.MassKg,
                            MaxEngineForceN = serverCar.MaxEngineForceN
                        });
                        break;
                    }
                }
            }
        }

        // Sort by name
        availableCars.Sort((a, b) => string.Compare(a.Name, b.Name, System.StringComparison.Ordinal));
        _allCars.AddRange(availableCars);

        if (_allCars.Count == 0)
        {
            _statusLabel!.Text = "No cars available! Make sure cars exist in both local content/cars folder and on server.";
            _statusLabel.Modulate = Colors.Red;
            return;
        }

        _statusLabel!.Text = $"{_allCars.Count} car(s) available";
        _statusLabel.Modulate = Colors.Green;

        // Create car cards
        CreateCarCards();

        // Mark as populated to prevent repeated population
        _carsPopulated = true;
    }

    private List<CarConfigSummary> LoadLocalCars()
    {
        var cars = new List<CarConfigSummary>();
        var config = ClientConfig.Instance;
        var carsPath = config.GetCarsDirectory();

        // Convert to absolute path
        var absoluteCarsPath = System.IO.Path.GetFullPath(carsPath);

        if (!System.IO.Directory.Exists(absoluteCarsPath))
        {
            GD.PrintErr($"Cars directory does not exist: {absoluteCarsPath}");
            return cars;
        }

        try
        {
            var carDirectories = System.IO.Directory.GetDirectories(absoluteCarsPath);

            foreach (var carDir in carDirectories)
            {
                var carFolderName = System.IO.Path.GetFileName(carDir);
                var carTomlPath = System.IO.Path.Combine(carDir, "car.toml");

                if (System.IO.File.Exists(carTomlPath))
                {
                    try
                    {
                        var car = LoadCarFromToml(carTomlPath, carFolderName);
                        if (car != null)
                        {
                            cars.Add(car);
                        }
                    }
                    catch (System.Exception ex)
                    {
                        GD.PrintErr($"Failed to load car from {carTomlPath}: {ex.Message}");
                    }
                }
            }
        }
        catch (System.Exception ex)
        {
            GD.PrintErr($"Error scanning cars directory: {ex.Message}");
        }

        return cars;
    }

    private CarConfigSummary? LoadCarFromToml(string tomlPath, string folderName)
    {
        if (!System.IO.File.Exists(tomlPath))
        {
            GD.PrintErr($"File does not exist: {tomlPath}");
            return null;
        }

        string content;
        try
        {
            content = System.IO.File.ReadAllText(tomlPath);
        }
        catch (System.Exception ex)
        {
            GD.PrintErr($"Failed to read file {tomlPath}: {ex.Message}");
            return null;
        }

        // Simple TOML parsing for car data
        var lines = content.Split('\n');
        string? id = null;
        string? name = null;
        string? model = null;
        float massKg = 0;
        float maxEngineForce = 0;

        foreach (var line in lines)
        {
            var trimmed = line.Trim();
            if (trimmed.StartsWith("id ="))
            {
                id = trimmed.Substring(4).Trim().Trim('"');
            }
            else if (trimmed.StartsWith("name ="))
            {
                name = trimmed.Substring(6).Trim().Trim('"');
            }
            else if (trimmed.StartsWith("model ="))
            {
                model = trimmed.Substring(7).Trim().Trim('"');
            }
            else if (trimmed.StartsWith("mass_kg ="))
            {
                float.TryParse(trimmed.Substring(9).Trim(), out massKg);
            }
            else if (trimmed.StartsWith("max_engine_force_n ="))
            {
                float.TryParse(trimmed.Substring(20).Trim(), out maxEngineForce);
            }
        }

        if (string.IsNullOrEmpty(id) || string.IsNullOrEmpty(name))
        {
            GD.PrintErr($"Invalid car.toml at {tomlPath}: missing required fields (id or name)");
            return null;
        }

        // If no model is specified, try to find a .glb file in the car folder
        if (string.IsNullOrEmpty(model))
        {
            var carDirectory = System.IO.Path.GetDirectoryName(tomlPath);
            if (carDirectory != null)
            {
                var glbFiles = System.IO.Directory.GetFiles(carDirectory, "*.glb");
                if (glbFiles.Length > 0)
                {
                    model = System.IO.Path.GetFileName(glbFiles[0]);
                }
                else
                {
                    GD.PrintErr($"No model specified in {tomlPath} and no .glb files found in directory");
                    return null;
                }
            }
        }

        var config = ClientConfig.Instance;
        var modelPath = config.GetCarModelPath(folderName, model!);

        return new CarConfigSummary
        {
            Id = id,
            Name = name,
            ModelPath = modelPath,
            MassKg = massKg,
            MaxEngineForceN = maxEngineForce
        };
    }

    private void CreateCarCards(string filter = "")
    {
        if (_gridContainer == null || _carCardScene == null) return;

        // Clear existing cards
        foreach (var card in _carCards)
        {
            card.QueueFree();
        }
        _carCards.Clear();

        // Filter cars based on search
        var filteredCars = _allCars;
        if (!string.IsNullOrWhiteSpace(filter))
        {
            filteredCars = _allCars.Where(c => c.Name.ToLower().Contains(filter.ToLower())).ToList();
        }

        if (filteredCars.Count == 0)
        {
            _statusLabel!.Text = "No cars match your search";
            _statusLabel.Modulate = Colors.Yellow;
            return;
        }

        _statusLabel!.Text = $"{filteredCars.Count} car(s) available";
        _statusLabel.Modulate = Colors.Green;

        // Create cards for filtered cars
        foreach (var car in filteredCars)
        {
            var cardInstance = _carCardScene.Instantiate<CarCard>();
            _gridContainer.AddChild(cardInstance);
            _carCards.Add(cardInstance);

            // Pass the car UUID to SetupCard - it will look it up in CarModelCache
            cardInstance.SetupCard(car, car.Id);
            cardInstance.CardClicked += OnCardClicked;
        }
    }

    private void OnCardClicked(CarCard clickedCard)
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
        EmitSignal(SignalName.CarSelected, _selectedCard.CarConfig.Id.ToString(), _selectedCard.CarConfig.Name);
        QueueFree();
    }

    private void OnSearchTextChanged(string newText)
    {
        CreateCarCards(newText);
    }

    private void OnSelectPressed()
    {
        // This is now handled in OnCardClicked, but keep this for safety
        if (_selectedCard?.CarConfig != null)
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
        foreach (var card in _carCards)
        {
            if (IsInstanceValid(card))
            {
                card.CardClicked -= OnCardClicked;
            }
        }
    }
}
