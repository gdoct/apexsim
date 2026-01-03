using Godot;
using System;
using System.Collections.Generic;

namespace ApexSim;

public partial class TrackCard : PanelContainer
{
    [Signal]
    public delegate void CardClickedEventHandler(TrackCard card);

    // Static index of track IDs to track file names
    private static Dictionary<string, string> _trackFileNameIndex = new();
    private static bool _indexBuilt = false;

    private Label? _nameLabel;
    private Label? _lengthLabel;
    private TextureRect? _trackImage;
    private ColorRect? _selectionIndicator;
    private ColorRect? _selectionBorder;
    private ColorRect? _hoverEffect;

    private TrackConfigSummary? _trackConfig;
    private bool _isSelected = false;
    private bool _isHovered = false;

    public TrackConfigSummary? TrackConfig => _trackConfig;
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
        _nameLabel = GetNode<Label>("VBox/InfoSection/TrackName");
        _lengthLabel = GetNode<Label>("VBox/InfoSection/TrackInfo/Length");
        _trackImage = GetNode<TextureRect>("VBox/TrackImageContainer/TrackImage");
        _selectionIndicator = GetNode<ColorRect>("SelectionIndicator");
        _selectionBorder = GetNode<ColorRect>("SelectionBorder");
        _hoverEffect = GetNode<ColorRect>("HoverEffect");

        // Set up mouse interaction
        MouseEntered += OnMouseEntered;
        MouseExited += OnMouseExited;

        // Make the panel clickable
        GuiInput += OnGuiInput;

        // Build track file name index on first card creation
        if (!_indexBuilt)
        {
            BuildTrackFileNameIndex();
        }
    }

    public void SetupCard(TrackConfigSummary trackConfig)
    {
        _trackConfig = trackConfig;

        if (_nameLabel != null)
            _nameLabel.Text = trackConfig.Name;

        if (_lengthLabel != null)
            _lengthLabel.Text = $"Track ID: {trackConfig.Id.Substring(0, 8)}...";

        // Load track preview image
        LoadTrackImage();
    }

    private void LoadTrackImage()
    {
        if (_trackImage == null || _trackConfig == null) return;

        try
        {
            // Find the track file name by ID
            if (_trackFileNameIndex.TryGetValue(_trackConfig.Id, out var fileName))
            {
                // Remove extension and construct image path
                var baseName = System.IO.Path.GetFileNameWithoutExtension(fileName);
                var imagePath = $"res://assets/track_previews/{baseName}.png";

                if (ResourceLoader.Exists(imagePath))
                {
                    var texture = GD.Load<Texture2D>(imagePath);
                    if (texture != null)
                    {
                        _trackImage.Texture = texture;
                        return;
                    }
                }
                else
                {
                    GD.PrintErr($"[TrackCard]   Image not found: {imagePath}");
                }
            }
            else
            {
                GD.PrintErr($"[TrackCard]   Track ID not in index: {_trackConfig.Id}");
            }

            // Fallback: show placeholder
            CreatePlaceholderImage();
        }
        catch (Exception ex)
        {
            GD.PrintErr($"[TrackCard] Error loading track image: {ex.Message}");
            CreatePlaceholderImage();
        }
    }

    private void CreatePlaceholderImage()
    {
        // Create a simple placeholder image
        var image = Image.Create(400, 300, false, Image.Format.Rgb8);
        image.Fill(new Color(0.08f, 0.08f, 0.12f)); // Dark background

        var texture = ImageTexture.CreateFromImage(image);
        if (_trackImage != null)
        {
            _trackImage.Texture = texture;
        }
    }

    private static void BuildTrackFileNameIndex()
    {
        if (_indexBuilt) return;

        _trackFileNameIndex.Clear();

        // Use ClientConfig to get the tracks directory
        var config = ClientConfig.Instance;
        var contentDir = config.ContentDirectory;
        var tracksDir = System.IO.Path.Combine(contentDir, "tracks");
        var absoluteTracksPath = System.IO.Path.GetFullPath(tracksDir);

        if (!System.IO.Directory.Exists(absoluteTracksPath))
        {
            GD.PrintErr($"[TrackCard] Tracks directory does not exist: {absoluteTracksPath}");
            _indexBuilt = true;
            return;
        }

        // Recursively scan all subdirectories
        ScanDirectoryForTracks(absoluteTracksPath);

        _indexBuilt = true;
    }

    private static void ScanDirectoryForTracks(string directoryPath)
    {
        try
        {
            // Scan files in current directory
            var files = System.IO.Directory.GetFiles(directoryPath);
            foreach (var filePath in files)
            {
                var ext = System.IO.Path.GetExtension(filePath).ToLower();
                if (ext == ".yaml" || ext == ".yml" || ext == ".json")
                {
                    try
                    {
                        var content = System.IO.File.ReadAllText(filePath);
                        var lines = content.Split('\n');

                        if (lines.Length > 0)
                        {
                            var firstLine = lines[0].Trim();
                            if (firstLine.Contains("track_id:"))
                            {
                                var idValue = ExtractIdValue(firstLine);
                                if (!string.IsNullOrEmpty(idValue))
                                {
                                    var fileName = System.IO.Path.GetFileName(filePath);
                                    _trackFileNameIndex[idValue] = fileName;
                                }
                            }
                        }
                    }
                    catch (Exception ex)
                    {
                        GD.PrintErr($"[TrackCard] Error reading {filePath}: {ex.Message}");
                    }
                }
            }

            // Recursively scan subdirectories
            var subdirs = System.IO.Directory.GetDirectories(directoryPath);
            foreach (var subdir in subdirs)
            {
                ScanDirectoryForTracks(subdir);
            }
        }
        catch (Exception ex)
        {
            GD.PrintErr($"[TrackCard] Error scanning directory {directoryPath}: {ex.Message}");
        }
    }

    private static string ExtractIdValue(string line)
    {
        try
        {
            // Handle format: track_id: uuid-string
            var idx = line.IndexOf("track_id:");
            if (idx >= 0)
            {
                var valuePart = line.Substring(idx + 9).Trim();
                return valuePart;
            }
        }
        catch { }
        return "";
    }

    private void UpdateSelectionVisuals()
    {
        if (_selectionIndicator != null)
            _selectionIndicator.Visible = _isSelected;
        if (_selectionBorder != null)
            _selectionBorder.Visible = _isSelected;
    }

    private void OnMouseEntered()
    {
        _isHovered = true;
        if (_hoverEffect != null)
            _hoverEffect.Visible = !_isSelected;
    }

    private void OnMouseExited()
    {
        _isHovered = false;
        if (_hoverEffect != null)
            _hoverEffect.Visible = false;
    }

    private void OnGuiInput(InputEvent inputEvent)
    {
        if (inputEvent is InputEventMouseButton mouseEvent && mouseEvent.Pressed && mouseEvent.ButtonIndex == MouseButton.Left)
        {
            EmitSignal(SignalName.CardClicked, this);
        }
    }
}
