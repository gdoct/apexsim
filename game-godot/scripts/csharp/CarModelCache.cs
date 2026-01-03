using Godot;
using System;
using System.Collections.Generic;

namespace ApexSim;

/// <summary>
/// Singleton that provides car model loading functionality.
/// Instead of caching scene instances (which causes duplication issues),
/// this class caches metadata and loads fresh models on demand from disk.
/// </summary>
public partial class CarModelCache : Node
{
    private static CarModelCache? _instance;
    public static CarModelCache Instance
    {
        get
        {
            if (_instance == null)
            {
                GD.PrintErr("CarModelCache accessed before initialization!");
                _instance = new CarModelCache();
            }
            return _instance;
        }
    }

    public static CarModelCache GetOrCreateInstance()
    {
        if (_instance == null)
        {
            _instance = new CarModelCache();
        }
        return _instance;
    }

    /// <summary>
    /// Cached car model metadata - stores paths, not scene instances
    /// </summary>
    private class CarModelInfo
    {
        public string ModelPath { get; set; } = "";
        public string CarFolderName { get; set; } = "";
    }

    private Dictionary<string, CarModelInfo> _modelInfo = new(); // Key: UUID
    private bool _isLoading = false;

    public bool IsLoading => _isLoading;

    public CarModelCache()
    {
        if (_instance == null)
        {
            _instance = this;
        }
    }

    public override void _Ready()
    {
        _instance = this;
    }

    /// <summary>
    /// Scan all car directories and cache their model paths
    /// </summary>
    public async void PreloadAllModels()
    {
        if (_isLoading) return;
        _isLoading = true;

        var config = ClientConfig.Instance;
        var carsPath = config.GetCarsDirectory();
        var absoluteCarsPath = System.IO.Path.GetFullPath(carsPath);

        if (!System.IO.Directory.Exists(absoluteCarsPath))
        {
            GD.PrintErr($"Cars directory does not exist: {absoluteCarsPath}");
            _isLoading = false;
            return;
        }

        var carDirectories = System.IO.Directory.GetDirectories(absoluteCarsPath);
        foreach (var carDir in carDirectories)
        {
            var carFolderName = System.IO.Path.GetFileName(carDir);
            var carTomlPath = System.IO.Path.Combine(carDir, "car.toml");

            if (!System.IO.File.Exists(carTomlPath))
                continue;

            // Parse the TOML to get UUID and model filename
            var tomlContent = System.IO.File.ReadAllText(carTomlPath);
            string? uuid = null;
            string? modelFilename = null;

            foreach (var line in tomlContent.Split('\n'))
            {
                var trimmed = line.Trim();
                if (trimmed.StartsWith("id ="))
                {
                    uuid = trimmed.Substring(4).Trim().Trim('"');
                }
                else if (trimmed.StartsWith("model ="))
                {
                    modelFilename = trimmed.Substring(7).Trim().Trim('"');
                }
            }

            if (string.IsNullOrEmpty(modelFilename))
            {
                // Try to find a .glb file
                var glbFiles = System.IO.Directory.GetFiles(carDir, "*.glb");
                if (glbFiles.Length > 0)
                {
                    modelFilename = System.IO.Path.GetFileName(glbFiles[0]);
                }
            }

            if (!string.IsNullOrEmpty(modelFilename) && !string.IsNullOrEmpty(uuid))
            {
                var modelPath = System.IO.Path.Combine(carDir, modelFilename);
                _modelInfo[uuid] = new CarModelInfo
                {
                    ModelPath = modelPath,
                    CarFolderName = carFolderName
                };
            }

            // Yield to prevent blocking
            if (IsInsideTree())
            {
                await ToSignal(GetTree(), SceneTree.SignalName.ProcessFrame);
            }
        }

        _isLoading = false;
    }

    /// <summary>
    /// Load a fresh model instance from disk for the given car UUID.
    /// Each call loads a new instance - no caching of scene instances.
    /// </summary>
    public Node3D? GetModel(string carUuid)
    {
        if (!_modelInfo.TryGetValue(carUuid, out var info))
        {
            GD.PrintErr($"  No model info for '{carUuid}'. Available: {string.Join(", ", _modelInfo.Keys)}");
            return null;
        }

        return LoadModelFromFile(info.ModelPath, info.CarFolderName);
    }

    private static int _loadCounter = 0;

    /// <summary>
    /// Load a model directly from a file path
    /// </summary>
    private Node3D? LoadModelFromFile(string modelPath, string carFolderName)
    {
        var loadId = ++_loadCounter;
        try
        {
            if (!System.IO.File.Exists(modelPath))
            {
                GD.PrintErr($"  [{loadId}] Model file not found: {modelPath}");
                return null;
            }

            var gltfDocument = new GltfDocument();
            var gltfState = new GltfState();

            // Set the base path so textures can be found relative to the GLB file
            var basePath = System.IO.Path.GetDirectoryName(modelPath);
            if (!string.IsNullOrEmpty(basePath))
            {
                gltfState.BasePath = basePath;
            }

            var error = gltfDocument.AppendFromFile(modelPath, gltfState);
            if (error != Error.Ok)
            {
                GD.PrintErr($"  [{loadId}] Failed to load GLTF: {error}");
                return null;
            }

            var scene = gltfDocument.GenerateScene(gltfState);
            if (scene is Node3D node3D)
            {
                // Give each loaded model a unique name for debugging
                node3D.Name = $"{carFolderName}_model_{loadId}";

                // Ensure all materials are visible (fix for black/invisible materials)
                EnsureMaterialsVisible(node3D);

                return node3D;
            }
            else
            {
                GD.PrintErr($"  [{loadId}] Generated scene is not Node3D");
                return null;
            }
        }
        catch (Exception ex)
        {
            GD.PrintErr($"  [{loadId}] Error loading model: {ex.Message}");
            return null;
        }
    }

    /// <summary>
    /// Ensure all materials in a model are visible by fixing common issues
    /// </summary>
    private void EnsureMaterialsVisible(Node node)
    {
        if (node is MeshInstance3D meshInstance)
        {
            var mesh = meshInstance.Mesh;
            if (mesh != null)
            {
                for (int i = 0; i < mesh.GetSurfaceCount(); i++)
                {
                    var material = meshInstance.GetActiveMaterial(i);
                    if (material is StandardMaterial3D stdMat)
                    {
                        // Make material double-sided (disable backface culling)
                        stdMat.CullMode = BaseMaterial3D.CullModeEnum.Disabled;

                        // Check if albedo color is too dark (black or near-black)
                        var albedo = stdMat.AlbedoColor;
                        bool hasTexture = stdMat.AlbedoTexture != null;
                        bool isDark = albedo.R < 0.1f && albedo.G < 0.1f && albedo.B < 0.1f;

                        if (isDark && !hasTexture)
                        {
                            // Create a new material with a visible gray color
                            var newMat = new StandardMaterial3D();
                            newMat.AlbedoColor = new Color(0.6f, 0.6f, 0.6f);
                            newMat.Roughness = 0.5f;
                            newMat.Metallic = 0.2f;
                            newMat.CullMode = BaseMaterial3D.CullModeEnum.Disabled;
                            meshInstance.SetSurfaceOverrideMaterial(i, newMat);
                        }
                    }
                }
            }
        }

        foreach (Node child in node.GetChildren())
        {
            EnsureMaterialsVisible(child);
        }
    }

    /// <summary>
    /// Check if a model is registered
    /// </summary>
    public bool HasModel(string carUuid)
    {
        return _modelInfo.ContainsKey(carUuid);
    }

    /// <summary>
    /// Get the car folder name from a model path
    /// </summary>
    public static string GetCarFolderFromPath(string modelPath)
    {
        // Extract folder name from path like "C:\...\content\cars\bmw-m4-prototype\model.glb"
        var parts = modelPath.Replace('\\', '/').Split('/');
        for (int i = 0; i < parts.Length - 1; i++)
        {
            if (parts[i] == "cars" && i + 1 < parts.Length)
            {
                return parts[i + 1];
            }
        }
        return "";
    }
}
