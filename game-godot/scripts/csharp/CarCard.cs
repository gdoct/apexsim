using Godot;
using System;

namespace ApexSim;

public partial class CarCard : PanelContainer
{
    [Signal]
    public delegate void CardClickedEventHandler(CarCard card);

    private Label? _nameLabel;
    private Label? _massLabel;
    private Label? _powerLabel;
    private Node3D? _carModelParent;
    private ColorRect? _selectionIndicator;
    private ColorRect? _selectionBorder;
    private ColorRect? _hoverEffect;
    private SubViewport? _viewport;
    private Camera3D? _camera;

    private CarConfigSummary? _carConfig;
    private Node3D? _loadedModel;
    private bool _isSelected = false;
    private bool _isHovered = false;
    private float _rotationAngle = 0.0f;

    public CarConfigSummary? CarConfig => _carConfig;
    public bool IsSelected
    {
        get => _isSelected;
        set
        {
            _isSelected = value;
            UpdateSelectionVisuals();
        }
    }

    public override void _Ready()
    {
        _nameLabel = GetNode<Label>("VBox/InfoSection/CarName");
        _massLabel = GetNode<Label>("VBox/InfoSection/CarSpecs/Mass");
        _powerLabel = GetNode<Label>("VBox/InfoSection/CarSpecs/Power");
        _carModelParent = GetNode<Node3D>("VBox/ModelViewport/SubViewport/CarModel");
        _selectionIndicator = GetNode<ColorRect>("SelectionIndicator");
        _selectionBorder = GetNode<ColorRect>("SelectionBorder");
        _hoverEffect = GetNode<ColorRect>("HoverEffect");
        _viewport = GetNode<SubViewport>("VBox/ModelViewport/SubViewport");
        _camera = GetNode<Camera3D>("VBox/ModelViewport/SubViewport/Camera3D");

        // Set up mouse interaction
        MouseEntered += OnMouseEntered;
        MouseExited += OnMouseExited;

        // Make the panel clickable
        GuiInput += OnGuiInput;

        // Enable viewport rendering when card becomes visible
        if (_viewport != null)
        {
            _viewport.RenderTargetUpdateMode = SubViewport.UpdateMode.Always;
        }
    }

    public void SetupCard(CarConfigSummary carConfig, string modelPath)
    {
        _carConfig = carConfig;

        if (_nameLabel != null)
            _nameLabel.Text = carConfig.Name;

        // Display physics data if available
        if (carConfig.MassKg > 0 && _massLabel != null)
            _massLabel.Text = $"Mass: {carConfig.MassKg:F0} kg";

        if (carConfig.MaxEngineForceN > 0 && _powerLabel != null)
            _powerLabel.Text = $"Power: {carConfig.MaxEngineForceN:F0} N";

        // Load the 3D model
        LoadCarModel(modelPath);
    }

    private void LoadCarModel(string modelPath)
    {
        if (_carModelParent == null) return;

        // Clear any existing model
        if (_loadedModel != null)
        {
            _loadedModel.QueueFree();
            _loadedModel = null;
        }

        try
        {
            // Get car folder name from path
            var carFolder = CarModelCache.GetCarFolderFromPath(modelPath);

            GD.Print($"Loading model for {_carConfig?.Name}: path={modelPath}, extracted folder={carFolder}");

            if (string.IsNullOrEmpty(carFolder))
            {
                GD.PrintErr($"Could not extract car folder from path: {modelPath}");
                CreateFallbackModel();
                return;
            }

            // Try to get cached model - this now returns a fresh Node3D instance
            var cache = CarModelCache.Instance;
            var modelInstance = cache.GetModel(carFolder);
            GD.Print($"  Cache lookup for '{carFolder}': {(modelInstance != null ? "FOUND" : "NOT FOUND")}");

            if (modelInstance != null)
            {
                // Use the model instance directly
                _loadedModel = modelInstance;
                _carModelParent.AddChild(_loadedModel);
                CenterAndScaleModel(_loadedModel);
                GD.Print($"  ✓ Successfully loaded model for {_carConfig?.Name}");
            }
            else
            {
                // Model not in cache yet, show fallback
                GD.Print($"Model not yet cached for {carFolder}, using fallback");
                CreateFallbackModel();
            }
        }
        catch (Exception ex)
        {
            GD.PrintErr($"Error loading car model {modelPath}: {ex.Message}");
            CreateFallbackModel();
        }
    }

    private void CenterAndScaleModel(Node3D model)
    {
        // Calculate the model's bounding box
        var aabb = CalculateAABB(model);
        var center = aabb.GetCenter();
        var size = aabb.Size;

        // Center the model
        model.Position = new Vector3(-center.X, -aabb.Position.Y, -center.Z);

        // Scale to fit nicely in the viewport (increased from 2.0 to 3.5 for better visibility)
        float maxDimension = Mathf.Max(size.X, Mathf.Max(size.Y, size.Z));
        if (maxDimension > 0)
        {
            float scale = 3.5f / maxDimension;
            model.Scale = new Vector3(scale, scale, scale);
            GD.Print($"  Applied scale: {scale} (max dimension was {maxDimension})");
        }
        else
        {
            GD.PrintErr($"  Model has zero size! Using default scale");
            model.Scale = Vector3.One;
        }
    }

    private Aabb CalculateAABB(Node3D node)
    {
        var aabb = new Aabb();
        bool first = true;

        if (node is MeshInstance3D meshInstance && meshInstance.Mesh != null)
        {
            aabb = meshInstance.Mesh.GetAabb();
            first = false;
        }

        foreach (Node child in node.GetChildren())
        {
            if (child is Node3D childNode3D)
            {
                var childAabb = CalculateAABB(childNode3D);
                if (childAabb.Size.LengthSquared() > 0)
                {
                    childAabb.Position += childNode3D.Position;
                    if (first)
                    {
                        aabb = childAabb;
                        first = false;
                    }
                    else
                    {
                        aabb = aabb.Merge(childAabb);
                    }
                }
            }
        }

        return first ? new Aabb(Vector3.Zero, Vector3.One) : aabb;
    }

    private void CreateFallbackModel()
    {
        // Create a simple box as fallback
        var mesh = new MeshInstance3D();
        var boxMesh = new BoxMesh();
        boxMesh.Size = new Vector3(2, 0.8f, 4);
        mesh.Mesh = boxMesh;

        var material = new StandardMaterial3D();
        material.AlbedoColor = new Color(0.5f, 0.5f, 0.5f);
        mesh.SetSurfaceOverrideMaterial(0, material);

        _loadedModel = mesh;
        _carModelParent?.AddChild(_loadedModel);
    }

    public override void _Process(double delta)
    {
        // Rotate the car model slowly for a nice effect
        if (_loadedModel != null)
        {
            _rotationAngle += (float)delta * 0.5f;
            _loadedModel.RotationDegrees = new Vector3(0, _rotationAngle * 30.0f, 0);
        }
    }

    private void OnGuiInput(InputEvent @event)
    {
        if (@event is InputEventMouseButton mouseButton)
        {
            if (mouseButton.ButtonIndex == MouseButton.Left && mouseButton.Pressed)
            {
                EmitSignal(SignalName.CardClicked, this);
            }
        }
    }

    private void OnMouseEntered()
    {
        _isHovered = true;
        UpdateSelectionVisuals();
    }

    private void OnMouseExited()
    {
        _isHovered = false;
        UpdateSelectionVisuals();
    }

    private void UpdateSelectionVisuals()
    {
        if (_selectionIndicator == null || _selectionBorder == null || _hoverEffect == null)
            return;

        // Show selection indicator if selected
        _selectionIndicator.Visible = _isSelected;

        // Add a subtle border when selected
        if (_isSelected)
        {
            _selectionBorder.Visible = true;
            // Create border effect by setting color on edges
            var styleBox = new StyleBoxFlat();
            styleBox.BorderColor = new Color(0, 0.8f, 1, 1);
            styleBox.BorderWidthLeft = 3;
            styleBox.BorderWidthRight = 3;
            styleBox.BorderWidthTop = 3;
            styleBox.BorderWidthBottom = 3;
            AddThemeStyleboxOverride("panel", styleBox);
        }
        else
        {
            _selectionBorder.Visible = false;
            // Reset to default panel style
            RemoveThemeStyleboxOverride("panel");
        }

        // Show hover effect when mouse is over (but less visible than selection)
        _hoverEffect.Visible = _isHovered && !_isSelected;

        // Add scale effect on hover
        var targetScale = (_isHovered || _isSelected) ? 1.05f : 1.0f;
        var tween = CreateTween();
        tween.TweenProperty(this, "scale", new Vector2(targetScale, targetScale), 0.2);
    }
}
