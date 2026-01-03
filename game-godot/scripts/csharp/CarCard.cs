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

            // Set up environment with ambient lighting for the isolated world
            SetupViewportEnvironment();
        }
    }

    private void SetupViewportEnvironment()
    {
        if (_viewport == null) return;

        // Create a WorldEnvironment for this viewport's isolated world
        var worldEnv = new WorldEnvironment();
        var env = new Godot.Environment();

        // Set up ambient lighting so models are visible
        env.AmbientLightSource = Godot.Environment.AmbientSource.Color;
        env.AmbientLightColor = new Color(1.0f, 1.0f, 1.0f);
        env.AmbientLightEnergy = 0.5f;

        // Set background to transparent
        env.BackgroundMode = Godot.Environment.BGMode.Color;
        env.BackgroundColor = new Color(0, 0, 0, 0);

        worldEnv.Environment = env;
        _viewport.AddChild(worldEnv);
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

    private void LoadCarModel(string carUuid)
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
            // Load a fresh model instance for this car UUID
            var cache = CarModelCache.Instance;
            var modelInstance = cache.GetModel(carUuid);

            if (modelInstance != null)
            {
                // Use the model instance directly
                _loadedModel = modelInstance;
                _carModelParent.AddChild(_loadedModel);

                CenterAndScaleModel(_loadedModel);

                // Force visibility on all mesh instances
                ForceVisibility(_loadedModel);
            }
            else
            {
                // Model not in cache yet, show fallback
                CreateFallbackModel();
            }
        }
        catch (Exception ex)
        {
            GD.PrintErr($"Error loading car model {carUuid}: {ex.Message}");
            CreateFallbackModel();
        }
    }

    private void CenterAndScaleModel(Node3D model)
    {
        // First, calculate the global bounding box of all meshes
        var globalAabb = CalculateGlobalAABB(model);
        var center = globalAabb.GetCenter();
        var size = globalAabb.Size;

        // Scale first to normalize size
        float maxDimension = Mathf.Max(size.X, Mathf.Max(size.Y, size.Z));
        float scale = 1.0f;
        if (maxDimension > 0)
        {
            scale = 3.5f / maxDimension;
            model.Scale = new Vector3(scale, scale, scale);
        }
        else
        {
            GD.PrintErr($"  Model has zero size! Using default scale");
            model.Scale = Vector3.One;
        }

        // After scaling, recalculate the center and offset to place model at origin
        // The center needs to be scaled too
        var scaledCenter = center * scale;
        model.Position = new Vector3(-scaledCenter.X, -scaledCenter.Y, -scaledCenter.Z);
    }

    private Aabb CalculateGlobalAABB(Node3D root)
    {
        var aabb = new Aabb();
        bool first = true;

        CalculateGlobalAABBRecursive(root, ref aabb, ref first);

        return first ? new Aabb(Vector3.Zero, Vector3.One) : aabb;
    }

    private void CalculateGlobalAABBRecursive(Node node, ref Aabb aabb, ref bool first)
    {
        if (node is MeshInstance3D meshInstance && meshInstance.Mesh != null)
        {
            // Get the mesh's local AABB and transform it to global space
            var meshAabb = meshInstance.Mesh.GetAabb();
            var globalTransform = meshInstance.GlobalTransform;

            // Transform the 8 corners of the AABB to global space
            var corners = new Vector3[8];
            corners[0] = globalTransform * new Vector3(meshAabb.Position.X, meshAabb.Position.Y, meshAabb.Position.Z);
            corners[1] = globalTransform * new Vector3(meshAabb.Position.X + meshAabb.Size.X, meshAabb.Position.Y, meshAabb.Position.Z);
            corners[2] = globalTransform * new Vector3(meshAabb.Position.X, meshAabb.Position.Y + meshAabb.Size.Y, meshAabb.Position.Z);
            corners[3] = globalTransform * new Vector3(meshAabb.Position.X + meshAabb.Size.X, meshAabb.Position.Y + meshAabb.Size.Y, meshAabb.Position.Z);
            corners[4] = globalTransform * new Vector3(meshAabb.Position.X, meshAabb.Position.Y, meshAabb.Position.Z + meshAabb.Size.Z);
            corners[5] = globalTransform * new Vector3(meshAabb.Position.X + meshAabb.Size.X, meshAabb.Position.Y, meshAabb.Position.Z + meshAabb.Size.Z);
            corners[6] = globalTransform * new Vector3(meshAabb.Position.X, meshAabb.Position.Y + meshAabb.Size.Y, meshAabb.Position.Z + meshAabb.Size.Z);
            corners[7] = globalTransform * new Vector3(meshAabb.Position.X + meshAabb.Size.X, meshAabb.Position.Y + meshAabb.Size.Y, meshAabb.Position.Z + meshAabb.Size.Z);

            // Create AABB from transformed corners
            var globalMeshAabb = new Aabb(corners[0], Vector3.Zero);
            for (int i = 1; i < 8; i++)
            {
                globalMeshAabb = globalMeshAabb.Expand(corners[i]);
            }

            if (first)
            {
                aabb = globalMeshAabb;
                first = false;
            }
            else
            {
                aabb = aabb.Merge(globalMeshAabb);
            }
        }

        foreach (Node child in node.GetChildren())
        {
            CalculateGlobalAABBRecursive(child, ref aabb, ref first);
        }
    }

    private int CountMeshes(Node node)
    {
        int count = node is MeshInstance3D ? 1 : 0;
        foreach (Node child in node.GetChildren())
        {
            count += CountMeshes(child);
        }
        return count;
    }

    private int GetMaxDepth(Node node, int currentDepth)
    {
        int maxDepth = currentDepth;
        foreach (Node child in node.GetChildren())
        {
            int childDepth = GetMaxDepth(child, currentDepth + 1);
            if (childDepth > maxDepth) maxDepth = childDepth;
        }
        return maxDepth;
    }

    private void ForceVisibility(Node node)
    {
        if (node is Node3D node3D)
        {
            node3D.Visible = true;
        }
        if (node is MeshInstance3D meshInstance)
        {
            meshInstance.Visible = true;
            meshInstance.CastShadow = GeometryInstance3D.ShadowCastingSetting.Off;
        }
        foreach (Node child in node.GetChildren())
        {
            ForceVisibility(child);
        }
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
        // Rotate the parent node instead of the model itself to ensure rotation around viewport origin
        if (_carModelParent != null && _loadedModel != null)
        {
            _rotationAngle += (float)delta * 0.5f;
            _carModelParent.RotationDegrees = new Vector3(0, _rotationAngle * 30.0f, 0);
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
