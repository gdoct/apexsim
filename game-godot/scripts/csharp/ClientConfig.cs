using Godot;
using System;
using System.IO;
using System.Collections.Generic;

namespace ApexSim;

public class ClientConfig
{
    public string ContentDirectory { get; set; } = "../content";
    // If true, use the alternative car selection UI for A/B testing.
    public bool UseAltCarSelection { get; set; } = false;

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

        // Try to load base config from several candidate locations.
        // Common locations: executable directory, project res:// path, current working directory.
        var candidates = GetCandidateConfigPaths(ConfigFileName);
        var loaded = false;
        foreach (var path in candidates)
        {
            if (File.Exists(path))
            {
                try
                {
                    var json = File.ReadAllText(path);
                    var loadedConfig = System.Text.Json.JsonSerializer.Deserialize<ClientConfig>(json);
                    if (loadedConfig != null)
                    {
                        config = loadedConfig;
                        loaded = true;
                        GD.Print($"ClientConfig: loaded base config from {path}");
                        break;
                    }
                }
                catch (Exception ex)
                {
                    GD.PrintErr($"Failed to load base config from {path}: {ex.Message}");
                }
            }
        }

        if (!loaded)
        {
            // Create default config file at the first candidate (executable dir) for visibility
            try
            {
                var basePath = candidates[0];
                SaveDefaultConfig(basePath);
                GD.Print($"ClientConfig: wrote default config to {basePath}");
            }
            catch (Exception ex)
            {
                GD.PrintErr($"Failed to save default config: {ex.Message}");
            }
        }

        // Try to load override config (takes precedence) from same candidate locations
        var overrideCandidates = GetCandidateConfigPaths(ConfigOverrideFileName);
        foreach (var path in overrideCandidates)
        {
            if (File.Exists(path))
            {
                try
                {
                    var json = File.ReadAllText(path);
                    var overrideConfig = System.Text.Json.JsonSerializer.Deserialize<ClientConfig>(json);
                    if (overrideConfig != null)
                    {
                        config = overrideConfig;
                        GD.Print($"ClientConfig: loaded override config from {path}");
                        break;
                    }
                }
                catch (Exception ex)
                {
                    GD.PrintErr($"Failed to load override config from {path}: {ex.Message}");
                }
            }
        }

        return config;
    }

    private static string GetConfigPath(string fileName)
    {
        // Backwards-compatible single-path helper - prefer executable dir
        var executablePath = System.AppContext.BaseDirectory;
        return Path.Combine(executablePath, fileName);
    }

    private static string[] GetCandidateConfigPaths(string fileName)
    {
        var exeDir = System.AppContext.BaseDirectory;
        string projectDir = "";
        try
        {
            projectDir = ProjectSettings.GlobalizePath("res://");
        }
        catch
        {
            projectDir = "";
        }

        var cwd = Directory.GetCurrentDirectory();
        var list = new List<string>();
        // Prefer project-level config (res://), then working directory, then executable dir.
        if (!string.IsNullOrEmpty(projectDir)) list.Add(Path.Combine(projectDir, fileName));
        if (!string.IsNullOrEmpty(cwd)) list.Add(Path.Combine(cwd, fileName));
        if (!string.IsNullOrEmpty(exeDir)) list.Add(Path.Combine(exeDir, fileName));
        return list.ToArray();
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
