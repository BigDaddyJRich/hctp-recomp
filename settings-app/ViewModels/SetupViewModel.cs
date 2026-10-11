using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Globalization;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using VendettaSettings.Services;

namespace VendettaSettings.ViewModels;

/// <summary>One line of the build's step list.</summary>
public partial class SetupStep : ObservableObject
{
    public SetupStep(string label, double weight, string note)
    {
        Label = label;
        Weight = weight;
        Note = note;
    }

    public string Label { get; }
    public string Note { get; }    // about how long it takes
    public double Weight { get; }  // its share of the whole
    [ObservableProperty] private string _state = "next"; // next | now | done
    public string Mark => State switch { "done" => "✓", "now" => "●", _ => "○" };
    public bool IsDone => State == "done";
    public bool IsNow => State == "now";
    public bool IsNext => State == "next";
    partial void OnStateChanged(string value)
    {
        OnPropertyChanged(nameof(Mark));
        OnPropertyChanged(nameof(IsDone));
        OnPropertyChanged(nameof(IsNow));
        OnPropertyChanged(nameof(IsNext));
    }
}

/// <summary>The first-run setup: 1 choose your disc, 2 build the game (tools\setup_game.py), 3 your settings.</summary>
public partial class SetupViewModel : ObservableObject
{
    private readonly GameFolder _folder;
    private readonly Action _finished;
    private Process? _process;
    private bool _cancelled;
    private readonly Stopwatch _clock = new();
    private StreamWriter? _log;

    public SetupViewModel(GameFolder folder, Action finished)
    {
        _folder = folder;
        _finished = finished;
        // the steps tools\setup_game.py goes through, with about how much of the time each takes
        Steps = new ObservableCollection<SetupStep>
        {
            new("Reading your disc", 0.06, "about 1 min"),
            new("Getting the build tools", 0.04, "only if they're missing"),
            new("Building the recompiler", 0.12, "about 1 min"),
            new("Turning the game's code into C++", 0.10, "under a minute"),
            new("Building the game", 0.66, "about 10 min"),
            new("Finishing", 0.02, ""),
        };
    }

    public ObservableCollection<SetupStep> Steps { get; }

    // --- step 1: the disc ---
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsDiscPage), nameof(IsBuildPage), nameof(BuildingNow))]
    private int _page = 1;

    public bool IsDiscPage => Page == 1;
    public bool IsBuildPage => Page == 2;
    public bool BuildingNow => IsBuildPage && Building;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasDisc), nameof(CanBuild), nameof(DiscAlreadyRead))]
    private string? _discPath;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanBuild), nameof(ShowDiscOk), nameof(ShowDiscBad))]
    private bool _discOk;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanBuild))]
    private bool _checking;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ShowDiscBad))]
    private string _discProblem = "";

    [ObservableProperty] private bool _makeShortcuts = true;

    public bool HasDisc => DiscPath != null;
    public bool ShowDiscOk => DiscOk && !Checking;
    public bool ShowDiscBad => DiscProblem.Length > 0 && !Checking;
    // a disc read in an earlier attempt (game\ is there) needs no new one
    public bool CanBuild => !Checking && !FolderTooLong && (DiscOk || (DiscPath == null && _folder.HasGame));

    // The build's deepest files lie ~140 characters below this folder, and the compiler can't write past Windows'
    // 260-character path limit ("cannot open program database"): a folder deep inside others has to move first.
    public const int MaxFolderLength = 100;
    public bool FolderTooLong => _folder.Root.Length > MaxFolderLength;
    public bool DiscAlreadyRead => DiscPath == null && _folder.HasGame;
    public string FolderPath => _folder.Root;
    public string SpaceText
    {
        get
        {
            var free = _folder.FreeSpaceGb;
            return "About 8 GB needed" + (free >= 0 ? $" · {free.ToString("0", CultureInfo.InvariantCulture)} GB free" : "");
        }
    }

    public string SpaceLine
    {
        get
        {
            var free = _folder.FreeSpaceGb;
            var freeText = free >= 0 ? $" ({free.ToString("0", CultureInfo.InvariantCulture)} GB free)" : "";
            return $"Needs about 8 GB of free space{freeText}. Want it somewhere else? Close this, move the folder, open it again.";
        }
    }

    partial void OnCheckingChanged(bool value)
    {
        OnPropertyChanged(nameof(ShowDiscOk));
        OnPropertyChanged(nameof(ShowDiscBad));
    }

    public async Task SetDiscAsync(string path)
    {
        DiscPath = path;
        DiscOk = false;
        DiscProblem = "";
        Checking = true;
        var problem = await _folder.CheckDiscAsync(path);
        Checking = false;
        DiscOk = problem == null;
        DiscProblem = problem ?? "";
    }

    // --- step 2: building ---
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(PercentText), nameof(BarWidth))]
    private double _percent;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Building), nameof(Title), nameof(BuildingNow))]
    private bool _done;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Building), nameof(HasError), nameof(BuildingNow), nameof(Title))]
    private string _error = "";

    [ObservableProperty] private string _timeLine = "";
    [ObservableProperty] private string _lastLine = "";

    public bool Building => !Done && Error.Length == 0;
    public bool HasError => Error.Length > 0;
    public string PercentText => $"{Math.Round(Percent * 100)}%";
    public double BarWidth => Percent; // 0-1, the view scales it
    public string Title => Done ? "The game is built" : HasError ? "The setup stopped" : "Building the game from your disc";
    public string LogPath => Path.Combine(_folder.LogsPath, "setup.log");

    [RelayCommand]
    private void Build()
    {
        if (!CanBuild)
            return;
        Page = 2;
        Start();
    }

    private void Start()
    {
        foreach (var s in Steps)
            s.State = "next";
        Percent = 0;
        Done = false;
        Error = "";
        TimeLine = "Starting…";
        LastLine = "";
        _cancelled = false;
        Directory.CreateDirectory(_folder.LogsPath);
        _log = new StreamWriter(LogPath, append: false) { AutoFlush = true };
        _clock.Restart();
        try
        {
            _process = Process.Start(_folder.SetupStart(DiscOk ? DiscPath : null));
        }
        catch (Exception ex)
        {
            Fail($"Couldn't start the setup: {ex.Message}");
            return;
        }
        if (_process == null)
        {
            Fail("Couldn't start the setup.");
            return;
        }
        var process = _process;
        _ = Task.Run(async () =>
        {
            string? line;
            while ((line = await process.StandardOutput.ReadLineAsync()) != null)
            {
                var text = line;
                Dispatcher.UIThread.Post(() => OnLine(text));
            }
        });
        _ = Task.Run(async () =>
        {
            string? line;
            while ((line = await process.StandardError.ReadLineAsync()) != null)
            {
                var text = line;
                Dispatcher.UIThread.Post(() => OnLine(text));
            }
        });
        _ = Task.Run(async () =>
        {
            await process.WaitForExitAsync();
            await Task.Delay(300); // the last lines first
            Dispatcher.UIThread.Post(() => OnExit(process.ExitCode));
        });
    }

    private int _step = -1;
    private double _stepProgress;

    private void OnLine(string line)
    {
        _log?.WriteLine(line);
        if (line.StartsWith("[step] "))
        {
            var parts = line.Split(' ', 4);
            if (int.TryParse(parts[1], out var n))
            {
                for (int i = 0; i < Steps.Count; i++)
                    Steps[i].State = i < n - 1 ? "done" : (i == n - 1 ? "now" : "next");
                _step = n - 1;
                _stepProgress = 0;
                UpdatePercent();
            }
            return;
        }
        if (line.StartsWith("[progress] "))
        {
            if (double.TryParse(line[11..], NumberStyles.Float, CultureInfo.InvariantCulture, out var p))
            {
                _stepProgress = p;
                UpdatePercent();
            }
            return;
        }
        if (line.StartsWith("[error] "))
        {
            Fail(line[8..]);
            return;
        }
        if (line == "[done]")
            return;
        if (line.Trim().Length > 0)
            LastLine = line.Length > 160 ? line[..160] + "…" : line;
    }

    private void UpdatePercent()
    {
        double before = 0;
        for (int i = 0; i < _step && i < Steps.Count; i++)
            before += Steps[i].Weight;
        var total = before + (_step >= 0 && _step < Steps.Count ? Steps[_step].Weight * _stepProgress : 0);
        Percent = Math.Clamp(total, 0, 1);
        var elapsed = _clock.Elapsed;
        if (Percent > 0.08 && Percent < 1)
        {
            var left = TimeSpan.FromSeconds(elapsed.TotalSeconds * (1 - Percent) / Percent);
            var minutes = Math.Max(1, (int)Math.Round(left.TotalMinutes));
            TimeLine = $"About {minutes} minute{(minutes == 1 ? "" : "s")} left";
        }
        else if (Percent < 1)
            TimeLine = $"{(int)elapsed.TotalMinutes} min so far";
    }

    private void OnExit(int code)
    {
        _log?.Dispose();
        _log = null;
        _process = null;
        if (_cancelled)
            return;
        if (code != 0 || Error.Length > 0)
        {
            if (Error.Length == 0)
                Fail($"The setup stopped (exit code {code}).");
            return;
        }
        foreach (var s in Steps)
            s.State = "done";
        Percent = 1;
        if (MakeShortcuts && _folder.CreateShortcuts() is { } problem)
            LastLine = problem;
        else if (MakeShortcuts)
            LastLine = "Shortcuts to the game are on your desktop and in this folder.";
        TimeLine = $"Done in {Math.Max(1, (int)Math.Round(_clock.Elapsed.TotalMinutes))} minutes";
        Done = true;
    }

    private void Fail(string why)
    {
        Error = why;
        TimeLine = "";
        foreach (var s in Steps.Where(s => s.State == "now"))
            s.State = "next";
    }

    [RelayCommand]
    private void Cancel()
    {
        _cancelled = true;
        try { _process?.Kill(entireProcessTree: true); } catch { }
        _process = null;
        _log?.Dispose();
        _log = null;
        Error = "";
        Page = 1;
    }

    [RelayCommand]
    private void TryAgain() => Start();

    [RelayCommand]
    private void Back()
    {
        Error = "";
        Page = 1;
    }

    /// <summary>Step 3: on to the settings.</summary>
    [RelayCommand]
    private void Continue() => _finished();

    [RelayCommand]
    private void OpenLog() => GameFolder.Open(LogPath);

    /// <summary>For pictures (--shot setup2): the build page part-way.</summary>
    public void ShowSample(double percent, bool done)
    {
        DiscPath = @"C:\Games\Discs\Def Jam Vendetta (USA).chd";
        DiscOk = true;
        Page = 2;
        _step = done ? Steps.Count : 4;
        for (int i = 0; i < Steps.Count; i++)
            Steps[i].State = done || i < 4 ? "done" : (i == 4 ? "now" : "next");
        Percent = percent;
        Done = done;
        TimeLine = done ? "Done in 17 minutes" : "About 7 minutes left";
        LastLine = done ? "Shortcuts to the game are on your desktop and in this folder." : "[512/799] Building CXX object ps2xRuntime/…";
    }
}
