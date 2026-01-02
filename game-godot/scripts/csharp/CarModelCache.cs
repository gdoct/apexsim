using Godot;
using System;
using System.Collections.Generic;

namespace ApexSim;

/// <summary>
/// Singleton that loads and caches car 3D models at startup
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
                _instance = new CarModelCache();
            }
            return _instance;
        }
    }

    private Dictionary<string, PackedScene> _modelCache = new();
    private bool _isLoading = false;

    public override void _Ready()
    {
        _instance = this;
    }

    /// <summary>
    /// Preload all car models from the content directory
    /// </summary>
    public async void PreloadAllModels()
    {
        if (_isLoading) return;
        _isLoading = true;

        GD.Print("=== Preloading car models ===");
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

            // Parse the TOML to get model filename
            var tomlContent = System.IO.File.ReadAllText(carTomlPath);
            string? modelFilename = null;

            foreach (var line in tomlContent.Split('\n'))
            {
                var trimmed = line.Trim();
                if (trimmed.StartsWith("model ="))
                {
                    modelFilename = trimmed.Substring(7).Trim().Trim('"');
                    break;
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

            if (!string.IsNullOrEmpty(modelFilename))
            {
                var modelPath = System.IO.Path.Combine(carDir, modelFilename);
                await LoadAndCacheModel(carFolderName, modelPath);
            }
        }

        GD.Print($"=== Preloaded {_modelCache.Count} car models ===");
        _isLoading = false;
    }

    private async System.Threading.Tasks.Task LoadAndCacheModel(string carFolderName, string modelPath)
    {
        if (_modelCache.ContainsKey(carFolderName))
            return;

        try
        {
            if (!System.IO.File.Exists(modelPath))
            {
                GD.PrintErr($"Model file not found: {modelPath}");
                return;
            }

            GD.Print($"Loading model for {carFolderName}: {modelPath}");

            var gltfDocument = new GltfDocument();
            var gltfState = new GltfState();

            var error = gltfDocument.AppendFromFile(modelPath, gltfState);
            if (error == Error.Ok)
            {
                var scene = gltfDocument.GenerateScene(gltfState);
                if (scene != null)
                {
                    // Create a PackedScene from the loaded scene
                    var packedScene = new PackedScene();
                    packedScene.Pack(scene);
                    _modelCache[carFolderName] = packedScene;

                    // Clean up the temporary scene
                    scene.QueueFree();

                    GD.Print($"  ✓ Cached model for '{carFolderName}' (key stored in cache)");
                }
            }
            else
            {
                GD.PrintErr($"Failed to load GLTF: {modelPath}, Error: {error}");
            }

            // Yield to prevent blocking
            await ToSignal(GetTree(), SceneTree.SignalName.ProcessFrame);
        }
        catch (Exception ex)
        {
            GD.PrintErr($"Error loading model {modelPath}: {ex.Message}");
        }
    }

    /// <summary>
    /// Get a cached model scene by car folder name
    /// </summary>
    public PackedScene? GetModel(string carFolderName)
    {
        var result = _modelCache.GetValueOrDefault(carFolderName);
        if (result == null)
        {
            GD.Print($"Cache miss for '{carFolderName}'. Available keys: {string.Join(", ", _modelCache.Keys)}");
        }
        return result;
    }

    /// <summary>
    /// Check if a model is cached
    /// </summary>
    public bool HasModel(string carFolderName)
    {
        return _modelCache.ContainsKey(carFolderName);
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
