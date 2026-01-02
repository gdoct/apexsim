using Godot;
using System;
using System.IO;

namespace ApexSim;

public class ClientConfig
{
    public string ContentDirectory { get; set; } = "../content";

    private static ClientConfig? _instance;
    private static readonly string ConfigFileName = "client_config.json";
    private static readonly string ConfigOverrideFileName = "client_config.override.json";

    public static ClientConfig Instance
    {
        get
        {
            if (_instance == null)
            {
                _instance = Load();
            }
            return _instance;
        }
    }

    private static ClientConfig Load()
    {
        var config = new ClientConfig();

        // Try to load base config
        var baseConfigPath = GetConfigPath(ConfigFileName);
        if (File.Exists(baseConfigPath))
        {
            try
            {
                var json = File.ReadAllText(baseConfigPath);
                var loadedConfig = System.Text.Json.JsonSerializer.Deserialize<ClientConfig>(json);
                if (loadedConfig != null)
                {
                    config = loadedConfig;
                    GD.Print($"Loaded client config from: {baseConfigPath}");
                }
            }
            catch (Exception ex)
            {
                GD.PrintErr($"Failed to load base config: {ex.Message}");
            }
        }
        else
        {
            GD.Print($"No base config found at {baseConfigPath}, using defaults");
            // Create default config file
            SaveDefaultConfig(baseConfigPath);
        }

        // Try to load override config (takes precedence)
        var overrideConfigPath = GetConfigPath(ConfigOverrideFileName);
        if (File.Exists(overrideConfigPath))
        {
            try
            {
                var json = File.ReadAllText(overrideConfigPath);
                var overrideConfig = System.Text.Json.JsonSerializer.Deserialize<ClientConfig>(json);
                if (overrideConfig != null)
                {
                    config = overrideConfig;
                    GD.Print($"Loaded client config override from: {overrideConfigPath}");
                }
            }
            catch (Exception ex)
            {
                GD.PrintErr($"Failed to load override config: {ex.Message}");
            }
        }

        GD.Print($"Using content directory: {config.ContentDirectory}");
        return config;
    }

    private static string GetConfigPath(string fileName)
    {
        // Get the directory where the executable is located
        var executablePath = System.AppContext.BaseDirectory;
        return Path.Combine(executablePath, fileName);
    }

    private static void SaveDefaultConfig(string path)
    {
        try
        {
            var defaultConfig = new ClientConfig();
            var json = System.Text.Json.JsonSerializer.Serialize(defaultConfig, new System.Text.Json.JsonSerializerOptions
            {
                WriteIndented = true
            });
            File.WriteAllText(path, json);
            GD.Print($"Created default config at: {path}");
        }
        catch (Exception ex)
        {
            GD.PrintErr($"Failed to save default config: {ex.Message}");
        }
    }

    public string GetCarsDirectory()
    {
        return Path.Combine(ContentDirectory, "cars");
    }

    public string GetCarModelPath(string carFolder, string modelFile)
    {
        // Return absolute path for file access
        var absolutePath = Path.GetFullPath(Path.Combine(ContentDirectory, "cars", carFolder, modelFile));

        // Convert to Godot res:// or user:// path for loading
        // We need to check if the file is in the project directory or external
        var projectPath = ProjectSettings.GlobalizePath("res://");

        if (absolutePath.StartsWith(projectPath))
        {
            // File is inside project, use res:// path
            var relativePath = Path.GetRelativePath(projectPath, absolutePath);
            return $"res://{relativePath.Replace("\\", "/")}";
        }
        else
        {
            // File is outside project, we'll need to load it differently
            // For now, return the absolute path - GLB loading will need special handling
            return absolutePath;
        }
    }
}
