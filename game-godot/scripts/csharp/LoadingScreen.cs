using Godot;

namespace ApexSim;

public partial class LoadingScreen : Control
{
    private ProgressBar? _progressBar;
    private Label? _loadingLabel;

    private string _targetScenePath = "";
    private float _loadingTime = 0.0f;
    private const float MinLoadingTime = 0.0f;  // Minimum time to show loading screen
    private bool _carModelsLoaded = false;
    private CarModelCache? _carModelCache;

    public override void _Ready()
    {
        _progressBar = GetNode<ProgressBar>("ProgressBar");
        _loadingLabel = GetNode<Label>("LoadingLabel");

        _progressBar.Value = 0;
        _targetScenePath = "res://scenes/main_menu.tscn";

        // Start preloading car models
        StartCarPreloading();
    }

    private void StartCarPreloading()
    {
        _loadingLabel!.Text = "Loading car models...";

        // Get or create the singleton instance
        _carModelCache = CarModelCache.GetOrCreateInstance();
        if (_carModelCache.GetParent() == null)
        {
            _carModelCache.Name = "CarModelCache";
            GetTree().Root.AddChild(_carModelCache);
        }

        // Start preloading and wait for completion
        WaitForCarPreloading();
    }

    private async void WaitForCarPreloading()
    {
        // Start the preloading process
        _carModelCache!.PreloadAllModels();

        // Wait for the loading to complete
        while (_carModelCache.IsLoading)
        {
            await ToSignal(GetTree(), SceneTree.SignalName.ProcessFrame);
        }

        _carModelsLoaded = true;
        _loadingLabel!.Text = "Loading complete!";
    }

    public override void _Process(double delta)
    {
        _loadingTime += (float)delta;

        // Update progress based on car loading status
        if (!_carModelsLoaded)
        {
            // Simulate progress while loading
            if (_progressBar!.Value < 90)
            {
                _progressBar.Value += delta * 30;  // Slower progress during actual loading
            }
        }
        else
        {
            // Quickly complete the progress bar
            if (_progressBar!.Value < 100)
            {
                _progressBar.Value = Mathf.Min(100, _progressBar.Value + (float)delta * 200);
            }
        }

        // Once we've loaded and shown the screen long enough, transition
        if (_carModelsLoaded && _progressBar!.Value >= 100 && _loadingTime >= MinLoadingTime)
        {
            FinishLoading();
        }
    }

    public void StartLoading(string scenePath)
    {
        _targetScenePath = scenePath;
        _loadingTime = 0.0f;
    }

    private void FinishLoading()
    {
        if (!string.IsNullOrEmpty(_targetScenePath))
        {
            var error = GetTree().ChangeSceneToFile(_targetScenePath);
            if (error != Error.Ok)
            {
                GD.PrintErr($"Failed to change scene to {_targetScenePath}: {error}");
            }
        }
    }
}
