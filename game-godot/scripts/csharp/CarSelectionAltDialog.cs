using Godot;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace ApexSim;

public partial class CarSelectionAltDialog : Control
{
    [Signal]
    public delegate void CarSelectedEventHandler(string carId, string carName);

    private ItemList? _carList;
    private Label? _statusLabel; // reuse CarName as status
    private Label? _carNameLabel;
    private Label? _brandLabel;
    private Label? _classLabel;
    private Label? _massLabel;
    private Label? _engineLabel;
    private Label? _transmissionLabel;
    private Label? _drivetrainLabel;
    private SubViewport? _viewport;
    private Node3D? _modelRoot;
    private Control? _gearGraphControl;
    private Button? _selectButton;
    private Button? _cancelButton;

    private List<CarConfigSummary> _allCars = new();
    private Dictionary<string, string> _tomlById = new(); // car id -> toml path
    private int _lastSelectedIndex = -1;

    // Model rotation state
    private float _autoRotationSpeed = 0.3f;
    private bool _isDragging = false;
    private Vector2 _lastMousePos;
    private float _manualRotationY = 0f;

    public override void _Ready()
    {
        // Try to find the UI nodes created in the scene file. If they are missing (e.g. scene couldn't be loaded),
        // build the UI programmatically so this dialog can still function when instantiated directly.
        _carList = GetNodeOrNull<ItemList>("Panel/HBox/Left/CarList");
        _carNameLabel = GetNodeOrNull<Label>("Panel/HBox/Right/Details/CarName");
        _brandLabel = GetNodeOrNull<Label>("Panel/HBox/Right/Details/CarInfo/BrandLabel");
        _classLabel = GetNodeOrNull<Label>("Panel/HBox/Right/Details/CarInfo/ClassLabel");
        _massLabel = GetNodeOrNull<Label>("Panel/HBox/Right/Details/CarInfo/MassLabel");
        _engineLabel = GetNodeOrNull<Label>("Panel/HBox/Right/Details/CarInfo/EngineLabel");
        _transmissionLabel = GetNodeOrNull<Label>("Panel/HBox/Right/Details/CarInfo/TransmissionLabel");
        _drivetrainLabel = GetNodeOrNull<Label>("Panel/HBox/Right/Details/CarInfo/DrivetrainLabel");
        _viewport = GetNodeOrNull<SubViewport>("Panel/HBox/Right/TopRow/ModelViewport/SubViewport");
        _modelRoot = GetNodeOrNull<Node3D>("Panel/HBox/Right/TopRow/ModelViewport/SubViewport/ModelRoot");
        _gearGraphControl = GetNodeOrNull<Control>("Panel/HBox/Right/Details/GearGraph");
        _selectButton = GetNodeOrNull<Button>("Panel/HBox/Right/ButtonBar/Select");
        _cancelButton = GetNodeOrNull<Button>("Panel/HBox/Right/ButtonBar/Cancel");

        if (_carList == null || _carNameLabel == null || _engineLabel == null || _viewport == null || _modelRoot == null || _gearGraphControl == null || _selectButton == null || _cancelButton == null)
        {
            BuildUiProgrammatically();

            // Attempt to re-fetch nodes after building
            _carList = GetNodeOrNull<ItemList>("Panel/HBox/Left/CarList");
            _carNameLabel = GetNodeOrNull<Label>("Panel/HBox/Right/Details/CarName");
            _brandLabel = GetNodeOrNull<Label>("Panel/HBox/Right/Details/CarInfo/BrandLabel");
            _classLabel = GetNodeOrNull<Label>("Panel/HBox/Right/Details/CarInfo/ClassLabel");
            _massLabel = GetNodeOrNull<Label>("Panel/HBox/Right/Details/CarInfo/MassLabel");
            _engineLabel = GetNodeOrNull<Label>("Panel/HBox/Right/Details/CarInfo/EngineLabel");
            _transmissionLabel = GetNodeOrNull<Label>("Panel/HBox/Right/Details/CarInfo/TransmissionLabel");
            _drivetrainLabel = GetNodeOrNull<Label>("Panel/HBox/Right/Details/CarInfo/DrivetrainLabel");
            _viewport = GetNodeOrNull<SubViewport>("Panel/HBox/Right/TopRow/ModelViewport/SubViewport");
            _modelRoot = GetNodeOrNull<Node3D>("Panel/HBox/Right/TopRow/ModelViewport/SubViewport/ModelRoot");
            _gearGraphControl = GetNodeOrNull<Control>("Panel/HBox/Right/Details/GearGraph");
            _selectButton = GetNodeOrNull<Button>("Panel/HBox/Right/ButtonBar/Select");
            _cancelButton = GetNodeOrNull<Button>("Panel/HBox/Right/ButtonBar/Cancel");
        }

        if (_carList != null) _carList.ItemSelected += OnCarSelected;
        if (_selectButton != null) _selectButton.Pressed += OnSelectPressed;
        if (_cancelButton != null) _cancelButton.Pressed += OnCancelPressed;

        // Try to load cars (and respect server lobby if present)
        RequestCarData();
    }

    public override void _Process(double delta)
    {
        base._Process(delta);

        if (_carList == null) return;
        var sel = _carList.GetSelectedItems();
        var idx = sel != null && sel.Length > 0 ? (int)sel[0] : -1;
        if (idx != _lastSelectedIndex)
        {
            _lastSelectedIndex = idx;
            ShowDetails(idx);
        }

        // Auto-rotate the model when not dragging
        if (_modelRoot != null && !_isDragging)
        {
            _manualRotationY += _autoRotationSpeed * (float)delta;
            _modelRoot.RotationDegrees = new Vector3(0, Mathf.RadToDeg(_manualRotationY), 0);
        }
    }

    public override void _Input(InputEvent @event)
    {
        base._Input(@event);

        if (_viewport == null) return;

        // Check if the mouse is over the viewport
        var viewportContainer = _viewport.GetParent<SubViewportContainer>();
        if (viewportContainer == null) return;

        var mousePos = viewportContainer.GetLocalMousePosition();
        var viewportRect = new Rect2(Vector2.Zero, viewportContainer.Size);

        if (@event is InputEventMouseButton mouseButton)
        {
            if (viewportRect.HasPoint(mousePos))
            {
                if (mouseButton.ButtonIndex == MouseButton.Left)
                {
                    _isDragging = mouseButton.Pressed;
                    _lastMousePos = mousePos;
                }
            }
        }
        else if (@event is InputEventMouseMotion)
        {
            if (_isDragging && viewportRect.HasPoint(mousePos))
            {
                var delta = mousePos - _lastMousePos;
                _manualRotationY += delta.X * 0.005f;
                if (_modelRoot != null)
                {
                    _modelRoot.RotationDegrees = new Vector3(0, Mathf.RadToDeg(_manualRotationY), 0);
                }
                _lastMousePos = mousePos;
            }
        }
    }

    private void BuildUiProgrammatically()
    {
        // Build a minimal UI structure that matches the expected node paths used by this script.
        var panel = GetNodeOrNull<PanelContainer>("Panel");
        if (panel == null)
        {
            panel = new PanelContainer();
            panel.Name = "Panel";
            AddChild(panel);
        }

        var hbox = panel.GetNodeOrNull<HBoxContainer>("HBox");
        if (hbox == null)
        {
            hbox = new HBoxContainer();
            hbox.Name = "HBox";
            panel.AddChild(hbox);
        }

        var left = hbox.GetNodeOrNull<VBoxContainer>("Left");
        if (left == null)
        {
            left = new VBoxContainer();
            left.Name = "Left";
            hbox.AddChild(left);
        }

        var title = new Label();
        title.Name = "Title";
        title.Text = "Cars";
        left.AddChild(title);

        var carList = new ItemList();
        carList.Name = "CarList";
        left.AddChild(carList);

        var right = hbox.GetNodeOrNull<VBoxContainer>("Right");
        if (right == null)
        {
            right = new VBoxContainer();
            right.Name = "Right";
            hbox.AddChild(right);
        }

        var topRow = new HBoxContainer();
        topRow.Name = "TopRow";
        right.AddChild(topRow);

        // Create a simple node container for model preview. Using full Viewport/SubViewport here can be
        // problematic in some runtime contexts; a Node3D placeholder is sufficient for UI fallback.
        var vpc = new SubViewportContainer();
        vpc.Name = "ModelViewport";
        topRow.AddChild(vpc);

        var vp = new SubViewport();
        vp.Name = "SubViewport";
        vp.RenderTargetUpdateMode = SubViewport.UpdateMode.Always;
        vpc.AddChild(vp);

        var modelRoot = new Node3D();
        modelRoot.Name = "ModelRoot";
        vp.AddChild(modelRoot);

        var details = new VBoxContainer();
        details.Name = "Details";
        right.AddChild(details);

        var carName = new Label();
        carName.Name = "CarName";
        carName.Text = "Select a car";
        details.AddChild(carName);

        var engineLabel = new Label();
        engineLabel.Name = "EngineLabel";
        details.AddChild(engineLabel);

        var gearGraph = new GearGraph();
        gearGraph.Name = "GearGraph";
        details.AddChild(gearGraph);

        var buttonBar = new HBoxContainer();
        buttonBar.Name = "ButtonBar";
        right.AddChild(buttonBar);

        var cancel = new Button();
        cancel.Name = "Cancel";
        cancel.Text = "Cancel";
        buttonBar.AddChild(cancel);

        var select = new Button();
        select.Name = "Select";
        select.Text = "Select";
        buttonBar.AddChild(select);
    }

    private void RequestCarData()
    {
        if (_carNameLabel != null)
            _carNameLabel.Text = "Loading cars...";

        // Try to use Network lobby data if available (root Network node)
        NetworkClient? network = null;
        try
        {
            network = GetNode<NetworkClient>("/root/Network");
        }
        catch
        {
            network = null;
        }

        // Populate from local cars and filter with server if possible
        var localCars = LoadLocalCars();

        if (network != null && network.LastLobbyState != null)
        {
            var serverIds = new HashSet<string>(network.LastLobbyState.CarConfigs.Select(c => c.Id));
            var available = new List<CarConfigSummary>();
            foreach (var l in localCars)
            {
                if (serverIds.Contains(l.Id))
                {
                    // find server's version
                    var server = network.LastLobbyState.CarConfigs.FirstOrDefault(c => c.Id == l.Id);
                    if (server != null)
                    {
                        available.Add(new CarConfigSummary
                        {
                            Id = server.Id,
                            Name = server.Name,
                            ModelPath = l.ModelPath,
                            MassKg = server.MassKg,
                            MaxEngineForceN = server.MaxEngineForceN
                        });
                    }
                }
            }
            _allCars = available.OrderBy(c => c.Name).ToList();
        }
        else
        {
            _allCars = localCars.OrderBy(c => c.Name).ToList();
        }

        if (_allCars.Count == 0)
        {
            if (_carNameLabel != null) _carNameLabel.Text = "No cars found";
            return;
        }

        PopulateList();
        // select first
        if (_carList != null)
        {
            _carList.Select(0);
            ShowDetails(0);
        }
    }

    private List<CarConfigSummary> LoadLocalCars()
    {
        var cars = new List<CarConfigSummary>();
        var config = ClientConfig.Instance;
        var carsPath = config.GetCarsDirectory();
        var absoluteCarsPath = Path.GetFullPath(carsPath);

        if (!Directory.Exists(absoluteCarsPath))
        {
            GD.PrintErr($"Cars directory does not exist: {absoluteCarsPath}");
            return cars;
        }

        try
        {
            var carDirectories = Directory.GetDirectories(absoluteCarsPath);
            foreach (var carDir in carDirectories)
            {
                var carFolderName = Path.GetFileName(carDir);
                var carTomlPath = Path.Combine(carDir, "car.toml");
                if (File.Exists(carTomlPath))
                {
                    var summary = LoadCarFromToml(carTomlPath, carFolderName);
                    if (summary != null)
                    {
                        cars.Add(summary);
                        _tomlById[summary.Id] = carTomlPath;
                    }
                }
            }
        }
        catch (Exception ex)
        {
            GD.PrintErr($"Error scanning cars directory: {ex.Message}");
        }

        return cars;
    }

    private CarConfigSummary? LoadCarFromToml(string tomlPath, string folderName)
    {
        if (!File.Exists(tomlPath)) return null;
        string content;
        try { content = File.ReadAllText(tomlPath); } catch { return null; }

        var lines = content.Split('\n');
        string? id = null;
        string? name = null;
        string? model = null;
        float massKg = 0;
        float maxEngineForce = 0;

        foreach (var line in lines)
        {
            var trimmed = line.Trim();
            if (trimmed.StartsWith("id =")) id = trimmed.Substring(4).Trim().Trim('"');
            else if (trimmed.StartsWith("name =")) name = trimmed.Substring(6).Trim().Trim('"');
            else if (trimmed.StartsWith("model =")) model = trimmed.Substring(7).Trim().Trim('"');
            else if (trimmed.StartsWith("mass_kg =")) float.TryParse(trimmed.Substring(9).Trim(), out massKg);
            else if (trimmed.StartsWith("max_engine_force_n =")) float.TryParse(trimmed.Substring(20).Trim(), out maxEngineForce);
        }

        if (string.IsNullOrEmpty(id) || string.IsNullOrEmpty(name)) return null;

        if (string.IsNullOrEmpty(model))
        {
            var glbFiles = Directory.GetFiles(Path.GetDirectoryName(tomlPath) ?? "", "*.glb");
            if (glbFiles.Length > 0) model = Path.GetFileName(glbFiles[0]);
            else
            {
                GD.PrintErr($"No model specified in {tomlPath} and no .glb found");
                model = "";
            }
        }

        var config = ClientConfig.Instance;
        var modelPath = config.GetCarModelPath(folderName, model ?? "");

        var summary = new CarConfigSummary
        {
            Id = id,
            Name = name,
            ModelPath = modelPath,
            MassKg = massKg,
            MaxEngineForceN = maxEngineForce
        };

        return summary;
    }

    private void PopulateList()
    {
        if (_carList == null) return;
        _carList.Clear();
        foreach (var car in _allCars)
        {
            _carList.AddItem(car.Name);
        }
    }

    private void OnCarSelected(long index)
    {
        ShowDetails((int)index);
    }

    private void ShowDetails(int index)
    {
        if (index < 0 || index >= _allCars.Count) return;
        var car = _allCars[index];
        _carNameLabel!.Text = car.Name;

        // Load toml for details
        if (_tomlById.TryGetValue(car.Id, out var tomlPath))
        {
            var details = ParseTomlDetails(tomlPath);
            if (details != null)
            {
                // Display brand and class
                if (_brandLabel != null)
                {
                    var brandText = !string.IsNullOrEmpty(details.Brand) ? $"Brand: {details.Brand}" : "";
                    if (!string.IsNullOrEmpty(details.Country))
                        brandText += $" ({details.Country})";
                    _brandLabel.Text = brandText;
                }

                if (_classLabel != null)
                {
                    var classText = !string.IsNullOrEmpty(details.Class) ? $"Class: {details.Class}" : "";
                    if (details.Year > 0)
                        classText += $" - {details.Year}";
                    _classLabel.Text = classText;
                }

                // Display mass
                if (_massLabel != null)
                {
                    _massLabel.Text = details.MassKg > 0 ? $"Mass: {details.MassKg:F0} kg" : "";
                }

                // Display engine info
                if (_engineLabel != null)
                {
                    var powerKw = details.EngineMaxPowerW / 1000.0f;
                    var powerHp = details.EngineMaxPowerW / 745.7f;
                    _engineLabel.Text = $"Engine: {powerKw:F0} kW ({powerHp:F0} hp) @ {details.RedlineRpm:F0} rpm";
                }

                // Display transmission info
                if (_transmissionLabel != null)
                {
                    var gearCount = details.GearRatios?.Count(r => r > 0) ?? 0;
                    _transmissionLabel.Text = $"Transmission: {details.TransmissionType} ({gearCount} gears)";
                }

                // Display drivetrain
                if (_drivetrainLabel != null)
                {
                    _drivetrainLabel.Text = !string.IsNullOrEmpty(details.Drivetrain) ? $"Drivetrain: {details.Drivetrain}" : "";
                }

                // Update gear graph if available
                if (_gearGraphControl is GearGraph gg && details.GearRatios?.Count > 0)
                {
                    gg.SetGearRatios(details.GearRatios, details.FinalDriveRatio);
                }
            }
        }

        // Load model into viewport
        LoadModelIntoViewport(car.ModelPath);
    }

    private void LoadModelIntoViewport(string modelPath)
    {
        // Clear children
        if (_modelRoot != null)
        {
            foreach (Node child in _modelRoot.GetChildren()) child.QueueFree();
        }

        if (string.IsNullOrEmpty(modelPath))
        {
            AddPlaceholderModel();
            return;
        }

        GD.Print($"LoadModelIntoViewport: modelPath={modelPath}");
        try
        {
            GD.Print($"ResourceLoader.Exists(modelPath) = {ResourceLoader.Exists(modelPath)}");
            // If the path is absolute, check file existence
            try
            {
                var gp = ProjectSettings.GlobalizePath("res://");
                GD.Print($"Project path: {gp}");
            }
            catch { }

            if (File.Exists(modelPath)) GD.Print($"System.IO.File.Exists absolute path: True");

            // Try generic load first to inspect returned type
            var generic = ResourceLoader.Load(modelPath);
            GD.Print($"ResourceLoader.Load (non-generic) returned: {generic}");

            var res = generic as PackedScene;
            if (res != null)
            {
                var inst = res.Instantiate();
                if (inst is Node3D node3d)
                {
                    if (_modelRoot != null) _modelRoot.AddChild(node3d);
                    EnsureViewportHelpers();
                    return;
                }
                else
                {
                    GD.PrintErr($"Model instantiated but is not Node3D: {inst.GetType()}");
                }
            }
            else
            {
                GD.Print($"Resource is not PackedScene (or null); attempting GLTF load if file exists");
                try
                {
                    if (System.IO.File.Exists(modelPath))
                    {
                        //var loadId = 0;
                        try
                        {
                            // Try to load GLB/GLTF directly from file using GltfDocument/GltfState
                            var doc = new GltfDocument();
                            var state = new GltfState();
                            var basePath = System.IO.Path.GetDirectoryName(modelPath);
                            if (!string.IsNullOrEmpty(basePath)) state.BasePath = basePath;

                            var err = doc.AppendFromFile(modelPath, state);
                            GD.Print($"GltfDocument.AppendFromFile returned: {err}");
                            if (err == Error.Ok)
                            {
                                var scene = doc.GenerateScene(state);
                                if (scene is Node3D nd)
                                {
                                    if (_modelRoot != null) _modelRoot.AddChild(nd);
                                    EnsureViewportHelpers();
                                    return;
                                }
                                else if (scene != null)
                                {
                                    GD.PrintErr($"GLTF generated scene not Node3D: {scene.GetType()}");
                                }
                            }
                        }
                        catch (Exception ex)
                        {
                            GD.PrintErr($"GLTF load attempt failed: {ex.Message}");
                        }
                    }
                }
                catch { }
            }
        }
        catch (Exception ex)
        {
            GD.PrintErr($"Failed to load model {modelPath}: {ex.Message}");
        }

        // fallback placeholder
        AddPlaceholderModel();
    }

    private void AddPlaceholderModel()
    {
        if (_modelRoot == null)
        {
            GD.PrintErr("AddPlaceholderModel: _modelRoot is null, cannot add placeholder");
            return;
        }
        var mesh = new MeshInstance3D();
        mesh.Mesh = new BoxMesh();
        _modelRoot.AddChild(mesh);
        EnsureViewportHelpers();
    }

    private void EnsureViewportHelpers()
    {
        // add camera and directional light if not present
        if (_viewport == null) return;
        if (_viewport.GetNodeOrNull<Camera3D>("Camera3D") == null)
        {
            var cam = new Camera3D();
            cam.Name = "Camera3D";
            // Position camera higher and angled slightly down to better frame the car
            cam.Position = new Vector3(0, 1.0f, 4);
            cam.LookAt(new Vector3(0, 0.5f, 0), Vector3.Up);
            _viewport.AddChild(cam);
            cam.Current = true;
        }
        if (_viewport.GetNodeOrNull<DirectionalLight3D>("Sun") == null)
        {
            var sun = new DirectionalLight3D();
            sun.Name = "Sun";
            sun.RotationDegrees = new Vector3(-45, 30, 0);
            _viewport.AddChild(sun);
        }
    }

    private void OnSelectPressed()
    {
        if (_carList == null) return;
        var sel = _carList.GetSelectedItems();
        if (!sel.Any()) return;
        var idx = System.Convert.ToInt32(sel[0]);
        if (idx >= 0 && idx < _allCars.Count)
        {
            var car = _allCars[idx];
            EmitSignal(SignalName.CarSelected, car.Id, car.Name);
            QueueFree();
        }
    }

    private void OnCancelPressed()
    {
        QueueFree();
    }

    private CarDetails? ParseTomlDetails(string tomlPath)
    {
        if (!File.Exists(tomlPath)) return null;
        var content = File.ReadAllText(tomlPath);
        var lines = content.Split('\n');

        var inEngine = false;
        var inTransmission = false;
        var inPhysics = false;
        var inDrivetrain = false;

        var brand = "";
        var country = "";
        var carClass = "";
        var year = 0;
        var massKg = 0f;
        var engineMaxPowerW = 0f;
        var engineMaxTorqueNm = 0f;
        var redlineRpm = 0f;
        var gearRatios = new List<float>();
        var finalDrive = 0f;
        var transmissionType = "";
        var drivetrain = "";

        foreach (var raw in lines)
        {
            var line = raw.Trim();
            if (line.StartsWith("[engine]")) { inEngine = true; inTransmission = false; inPhysics = false; inDrivetrain = false; continue; }
            if (line.StartsWith("[transmission]")) { inTransmission = true; inEngine = false; inPhysics = false; inDrivetrain = false; continue; }
            if (line.StartsWith("[physics]")) { inPhysics = true; inEngine = false; inTransmission = false; inDrivetrain = false; continue; }
            if (line.StartsWith("[drivetrain]")) { inDrivetrain = true; inEngine = false; inTransmission = false; inPhysics = false; continue; }
            if (line.StartsWith("[")) { inEngine = false; inTransmission = false; inPhysics = false; inDrivetrain = false; }

            if (inEngine)
            {
                if (line.StartsWith("max_power_w")) float.TryParse(line.Split('=')[1].Trim(), out engineMaxPowerW);
                if (line.StartsWith("max_torque_nm")) float.TryParse(line.Split('=')[1].Trim(), out engineMaxTorqueNm);
                if (line.StartsWith("redline_rpm")) float.TryParse(line.Split('=')[1].Trim(), out redlineRpm);
            }
            else if (inTransmission)
            {
                if (line.StartsWith("gear_ratios"))
                {
                    var start = line.IndexOf('[');
                    var end = line.IndexOf(']');
                    if (start >= 0 && end > start)
                    {
                        var inner = line.Substring(start + 1, end - start - 1);
                        var parts = inner.Split(',');
                        foreach (var p in parts)
                        {
                            if (float.TryParse(p.Trim(), out var r)) gearRatios.Add(r);
                        }
                    }
                }
                else if (line.StartsWith("final_drive_ratio")) float.TryParse(line.Split('=')[1].Trim(), out finalDrive);
                else if (line.StartsWith("transmission_type")) transmissionType = line.Split('=')[1].Trim().Trim('"');
            }
            else if (inPhysics)
            {
                if (line.StartsWith("mass_kg")) float.TryParse(line.Split('=')[1].Trim(), out massKg);
            }
            else if (inDrivetrain)
            {
                if (line.StartsWith("layout")) drivetrain = line.Split('=')[1].Trim().Trim('"');
            }
            else
            {
                // Top-level properties
                if (line.StartsWith("brand =")) brand = line.Substring(7).Trim().Trim('"');
                if (line.StartsWith("manufacturer_country =")) country = line.Substring(22).Trim().Trim('"');
                if (line.StartsWith("class =")) carClass = line.Substring(7).Trim().Trim('"');
                if (line.StartsWith("model_year =")) int.TryParse(line.Substring(12).Trim(), out year);
            }
        }

        return new CarDetails
        {
            Brand = brand,
            Country = country,
            Class = carClass,
            Year = year,
            MassKg = massKg,
            EngineMaxPowerW = engineMaxPowerW,
            EngineMaxTorqueNm = engineMaxTorqueNm,
            RedlineRpm = redlineRpm,
            GearRatios = gearRatios,
            FinalDriveRatio = finalDrive,
            TransmissionType = transmissionType,
            Drivetrain = drivetrain
        };
    }

    private class CarDetails
    {
        public string Brand { get; set; } = "";
        public string Country { get; set; } = "";
        public string Class { get; set; } = "";
        public int Year { get; set; }
        public float MassKg { get; set; }
        public float EngineMaxPowerW { get; set; }
        public float EngineMaxTorqueNm { get; set; }
        public float RedlineRpm { get; set; }
        public List<float> GearRatios { get; set; } = new();
        public float FinalDriveRatio { get; set; }
        public string TransmissionType { get; set; } = "";
        public string Drivetrain { get; set; } = "";
    }
}
