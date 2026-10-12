using System.ComponentModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using VendettaSettings.Services;

namespace VendettaSettings.ViewModels;

/// <summary>One entry of a drop-down list.</summary>
public sealed record Choice(string Label, int Value, int Value2 = 0);

/// <summary>The Settings page: every change is written to settings.toml at once (the game reads it when it starts).</summary>
public partial class SettingsViewModel : ObservableObject
{
    private static readonly int[] Scales = { 1, 2, 4, 8, 16 };
    private static readonly string[] Modes = { "window", "borderless", "fullscreen" };
    private static readonly string[] Filters = { "fsr", "smooth", "blocky" };
    private static readonly string[] Cameras = { "vendetta", "nomercy", "wk2" };

    // the properties that are settings (a change is saved); the others are worked out from them
    private static readonly HashSet<string> Saved = new()
    {
        nameof(ModeIndex), nameof(Screen), nameof(Maximised), nameof(Resolution), nameof(Refresh), nameof(VsyncIndex),
        nameof(ShapeIndex), nameof(RendererIndex), nameof(ScaleIndex), nameof(FilterIndex), nameof(Sharpening),
        nameof(Volume), nameof(Reverb), nameof(Multitap), nameof(Vibration1), nameof(Vibration2), nameof(Vibration3),
        nameof(Vibration4), nameof(HighPoly), nameof(Sharp2D), nameof(KeyboardIndex), nameof(CheckUpdates), nameof(CameraIndex),
        nameof(UnlockAll),
    };

    private readonly GameFolder _folder;
    private readonly bool _readOnly;
    private readonly IReadOnlyList<Displays.Screen> _screens;
    private bool _loading;

    public SettingsViewModel(GameFolder folder, bool readOnly)
    {
        _folder = folder;
        _readOnly = readOnly;
        _screens = Displays.Find();
        Load(GameSettings.Load(folder.SettingsPath));
        UpdatePads();
        if (!readOnly)
        {
            // which controllers are plugged in, for the Controls page (asking XInput about an empty slot is slow, so
            // every 2 s)
            var timer = new Avalonia.Threading.DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
            timer.Tick += (_, _) => UpdatePads();
            timer.Start();
        }
    }

    // the Controls page: what each player has (the game gives the Xbox-style controllers out in order)
    [ObservableProperty] private string _pad1 = "";
    [ObservableProperty] private string _pad2 = "";
    [ObservableProperty] private string _pad3 = "";
    [ObservableProperty] private string _pad4 = "";
    [ObservableProperty] private bool _pad1In;
    [ObservableProperty] private bool _pad2In;
    [ObservableProperty] private bool _pad3In;
    [ObservableProperty] private bool _pad4In;

    private void UpdatePads()
    {
        var count = Controllers.XInputCount();
        bool Keyboard(int player) => KeyboardIndex == player;
        string Pad(int player) =>
            player >= 2 && !Multitap ? "Off (2 players)"
            : player < count ? (Keyboard(player) ? "Xbox-style controller + the keyboard" : "Xbox-style controller")
            : Keyboard(player) ? "The keyboard" : "Not plugged in";
        bool In(int player) => (player < 2 || Multitap) && (player < count || Keyboard(player));
        Pad1 = Pad(0);
        Pad2 = Pad(1);
        Pad3 = Pad(2);
        Pad4 = Pad(3);
        Pad1In = In(0);
        Pad2In = In(1);
        Pad3In = In(2);
        Pad4In = In(3);
    }

    partial void OnMultitapChanged(bool value) => UpdatePads();
    partial void OnKeyboardIndexChanged(int value) => UpdatePads();

    // the Play page's "how it will open"
    public string SummaryWindow => ModeIndex switch
    {
        1 => "Borderless",
        2 => $"Fullscreen · {ChosenRate} Hz",
        _ => Maximised ? "Window · maximised" : "Window",
    };

    public string SummaryPicture =>
        (ShapeIndex switch { 1 => "4:3", 2 => "Stretched", _ => "Widescreen" }) + " · " +
        (RendererIndex == 0 ? $"graphics card · {Scales[Math.Clamp(ScaleIndex, 0, 4)]}×" : "CPU");

    public string SummaryPlayers =>
        (Multitap ? "Up to 4" : "2") + " · vibration " +
        (Vibration1 || Vibration2 || Vibration3 || Vibration4 ? "on" : "off");

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsDisplayTab), nameof(IsGraphicsTab), nameof(IsSoundTab), nameof(IsControlsTab),
        nameof(IsGameplayTab))]
    private int _tabIndex;

    public bool IsDisplayTab => TabIndex == 0;
    public bool IsGraphicsTab => TabIndex == 1;
    public bool IsSoundTab => TabIndex == 2;
    public bool IsControlsTab => TabIndex == 3;
    public bool IsGameplayTab => TabIndex == 4;

    // Display
    [ObservableProperty] private int _modeIndex;
    [ObservableProperty] private List<Choice> _screenChoices = new();
    [ObservableProperty] private Choice? _screen;
    [ObservableProperty] private bool _maximised;
    [ObservableProperty] private List<Choice> _resolutionChoices = new();
    [ObservableProperty] private Choice? _resolution;
    [ObservableProperty] private List<Choice> _refreshChoices = new();
    [ObservableProperty] private Choice? _refresh;
    [ObservableProperty] private int _vsyncIndex;
    [ObservableProperty] private int _shapeIndex;
    // Graphics
    [ObservableProperty] private int _rendererIndex;
    [ObservableProperty] private int _scaleIndex;
    [ObservableProperty] private int _filterIndex;
    [ObservableProperty] private double _sharpening;
    [ObservableProperty] private bool _highPoly;
    [ObservableProperty] private bool _sharp2D;
    /// <summary>The fight camera: 0 Def Jam Vendetta (the game's own), 1 WWF No Mercy, 2 Wrestle Kingdom 2.</summary>
    [ObservableProperty] private int _cameraIndex;
    [ObservableProperty] private bool _checkUpdates = true;
    // Sound
    [ObservableProperty] private double _volume;
    [ObservableProperty] private bool _reverb;
    // Controls
    // Gameplay
    [ObservableProperty] private bool _unlockAll;
    [ObservableProperty] private bool _multitap;
    [ObservableProperty] private int _keyboardIndex;   // 0-3 = players 1-4, 4 = nobody
    [ObservableProperty] private bool _vibration1;
    [ObservableProperty] private bool _vibration2;
    [ObservableProperty] private bool _vibration3;
    [ObservableProperty] private bool _vibration4;

    [ObservableProperty] private string _status = "";

    public bool IsWindowMode => ModeIndex == 0;
    public bool IsFullscreen => ModeIndex == 2;
    public bool IsNotFullscreen => ModeIndex != 2;

    public string ModeHint => ModeIndex switch
    {
        1 => "A window with no frame that covers the whole screen; switching apps is instant",
        2 => "The game takes over the screen and can set its own resolution and refresh rate",
        _ => "A normal window you can move and resize",
    };

    public string MaximisedHint => IsWindowMode
        ? "Opens filling the screen, title bar kept"
        : "Only for Window: Borderless and Fullscreen already fill the screen";

    public string FullscreenHint => IsFullscreen ? "The screen switches to this while the game runs" : "Only for Fullscreen";

    private Displays.Screen? CurrentScreen =>
        Screen is { } chosen ? _screens.FirstOrDefault(s => s.Number == chosen.Value) : _screens.FirstOrDefault();

    public string WindowsRate => CurrentScreen is { } s ? $"Windows' setting · {s.Current.Rate} Hz" : "Windows' setting";

    /// <summary>The rate the game will ask for in fullscreen (smoothest / the screen's own worked out).</summary>
    private int ChosenRate
    {
        get
        {
            if (CurrentScreen is not { } s)
                return Refresh?.Value > 0 ? Refresh.Value : 60;
            var (w, h) = FullscreenSize(s);
            return Refresh?.Value switch
            {
                GameSettings.RefreshSmoothest or null => s.SmoothestAt(w, h),
                GameSettings.RefreshScreens => s.Current.Rate,
                var r => r.Value,
            };
        }
    }

    public string RefreshHint
    {
        get
        {
            if (!IsFullscreen)
                return "In a window or borderless, the screen keeps the rate set in Windows. Pick Fullscreen to choose it here.";
            var rate = ChosenRate;
            return rate % 60 == 0 || (rate + 1) % 60 == 0
                ? $"The game makes 60 frames a second: at {rate} Hz every frame stays on screen for the same time."
                : $"The game makes 60 frames a second: at {rate} Hz some stay up longer than others, a slight judder. A multiple of 60 Hz is smoother.";
        }
    }

    public string VsyncHint => VsyncIndex == 1
        ? "No tearing: each frame waits for the screen. With G-Sync or FreeSync, smooth at any rate."
        : "The lowest delay; fast moves can tear in fullscreen.";

    public string ShapeHint => ShapeIndex switch
    {
        1 => "As on the PS2: black bars at the sides of a wide screen.",
        2 => "Fills a wide screen, but everyone looks wider.",
        _ => "Matches at 16:9 with more of the arena; menus and movies stay 4:3. New: tested in the warehouse arena so far.",
    };

    public string RendererHint => RendererIndex == 0 ? "Default: sharp and fast" : "The old way, for checking: 1× only";

    public string ScaleHint => RendererIndex == 1 ? "The CPU draws at 1×" : ScaleIndex switch
    {
        0 => "As sharp as the PS2",
        1 => "A little sharper",
        2 => "Sharp",
        3 => "Very sharp, lighter on the graphics card",
        _ => "Default: sharpest, four times the PS2's detail",
    };

    public string FilterHint => FilterIndex switch
    {
        1 => "Soft, like a TV",
        2 => "Hard pixels, like an emulator with no filter",
        _ => "Default: smooth and crisp at any window size",
    };

    public string SharpeningText => $"{(int)Math.Round(Sharpening)}%";
    public string VolumeText => $"{(int)Math.Round(Volume)}%";
    public bool SharpeningEnabled => FilterIndex == 0;

    /// <summary>A short line for the Play page: how the game will open.</summary>
    public string Summary
    {
        get
        {
            var mode = ModeIndex switch
            {
                1 => "Borderless",
                2 => $"Fullscreen · {ChosenRate} Hz",
                _ => Maximised ? "Maximised" : "Window",
            };
            var renderer = RendererIndex == 0 ? $"graphics card · {Scales[Math.Clamp(ScaleIndex, 0, 4)]}×" : "CPU";
            return $"{mode} · {renderer}";
        }
    }

    protected override void OnPropertyChanged(PropertyChangedEventArgs e)
    {
        base.OnPropertyChanged(e);
        if (e.PropertyName is nameof(Screen) or nameof(Resolution) && !_loading)
            RebuildChoices();
        if (e.PropertyName is { } name && Saved.Contains(name))
        {
            foreach (var derived in new[]
                     {
                         nameof(IsWindowMode), nameof(IsFullscreen), nameof(IsNotFullscreen), nameof(ModeHint),
                         nameof(MaximisedHint), nameof(FullscreenHint), nameof(WindowsRate), nameof(RefreshHint),
                         nameof(VsyncHint), nameof(ShapeHint), nameof(RendererHint), nameof(ScaleHint), nameof(FilterHint),
                         nameof(SharpeningText), nameof(VolumeText), nameof(SharpeningEnabled), nameof(Summary),
                         nameof(SummaryWindow), nameof(SummaryPicture), nameof(SummaryPlayers),
                     })
                base.OnPropertyChanged(new PropertyChangedEventArgs(derived));
            Save();
        }
    }

    [RelayCommand]
    private void ResetToDefaults()
    {
        Load(new GameSettings());
        Save();
        Status = "Back to the defaults.";
    }

    private void Load(GameSettings s)
    {
        _loading = true;
        try
        {
            ModeIndex = Math.Max(0, Array.IndexOf(Modes, s.Mode));
            ScreenChoices = _screens.Select(sc => new Choice(
                    $"Screen {sc.Number} · {sc.Current.Width} × {sc.Current.Height} · {sc.Current.Rate} Hz{(sc.Primary ? " (main)" : "")}",
                    sc.Number))
                .ToList();
            if (ScreenChoices.All(c => c.Value != s.Screen))
                ScreenChoices = ScreenChoices.Append(new Choice($"Screen {s.Screen} · not connected (the main one is used)", s.Screen)).ToList();
            Screen = ScreenChoices.First(c => c.Value == s.Screen);
            Maximised = s.Maximised;
            RebuildChoices(s.FullscreenWidth, s.FullscreenHeight, s.RefreshRate);
            VsyncIndex = s.Vsync ? 1 : 0;
            ShapeIndex = s.Shape == "4:3" ? 1 : s.Shape == "stretch" ? 2 : 0; // 0 widescreen (the default), 1 4:3, 2 stretched
            RendererIndex = s.Renderer == "cpu" ? 1 : 0;
            ScaleIndex = Math.Max(0, Array.IndexOf(Scales, s.Scale));
            FilterIndex = Math.Max(0, Array.IndexOf(Filters, s.Filter));
            Sharpening = s.Sharpening;
            HighPoly = s.HighPoly;
            Sharp2D = s.Sharp2D;
            CameraIndex = Math.Max(0, Array.IndexOf(Cameras, s.Camera));
            Volume = s.Volume;
            Reverb = s.Reverb;
            Multitap = s.Multitap;
            Vibration1 = s.Vibration[0];
            Vibration2 = s.Vibration[1];
            Vibration3 = s.Vibration[2];
            Vibration4 = s.Vibration[3];
            KeyboardIndex = s.Keyboard is >= 1 and <= 4 ? s.Keyboard - 1 : 4;
            UnlockAll = s.UnlockAll;
            CheckUpdates = s.CheckUpdates;
        }
        finally
        {
            _loading = false;
        }
        foreach (var name in new[] { nameof(ModeHint), nameof(Summary), nameof(RefreshHint), nameof(WindowsRate) })
            base.OnPropertyChanged(new PropertyChangedEventArgs(name));
    }

    private (int W, int H) FullscreenSize(Displays.Screen screen) =>
        Resolution is { Value: > 0 } r ? (r.Value, r.Value2) : (screen.Current.Width, screen.Current.Height);

    /// <summary>The resolutions and refresh rates of the chosen screen (keeping what is chosen).</summary>
    private void RebuildChoices() => RebuildChoices(Resolution?.Value ?? 0, Resolution?.Value2 ?? 0, Refresh?.Value ?? GameSettings.RefreshSmoothest);

    private void RebuildChoices(int width, int height, int rate)
    {
        var wasLoading = _loading;
        _loading = true;
        try
        {
            var screen = CurrentScreen;
            var sizes = new List<Choice>
            {
                new(screen is { } s0 ? $"Same as the screen · {s0.Current.Width} × {s0.Current.Height}" : "Same as the screen", 0),
            };
            if (screen != null)
                sizes.AddRange(screen.Modes
                    .Where(m => m.Width >= 1024 && !(m.Width == screen.Current.Width && m.Height == screen.Current.Height))
                    .Select(m => (m.Width, m.Height)).Distinct()
                    .OrderByDescending(m => m.Width * m.Height)
                    .Select(m => new Choice($"{m.Width} × {m.Height}", m.Width, m.Height)));
            if (width > 0 && sizes.All(c => c.Value != width || c.Value2 != height))
                sizes.Add(new Choice($"{width} × {height} · not on this screen", width, height));
            ResolutionChoices = sizes;
            Resolution = sizes.FirstOrDefault(c => c.Value == width && c.Value2 == height) ?? sizes[0];

            var rates = new List<Choice>();
            if (screen != null)
            {
                var (w, h) = FullscreenSize(screen);
                rates.Add(new Choice($"Smoothest · {screen.SmoothestAt(w, h)} Hz", GameSettings.RefreshSmoothest));
                rates.AddRange(screen.RatesAt(w, h).Select(r => new Choice($"{r} Hz", r)));
                rates.Add(new Choice($"The screen's own · {screen.Current.Rate} Hz", GameSettings.RefreshScreens));
            }
            else
            {
                rates.Add(new Choice("Smoothest", GameSettings.RefreshSmoothest));
                rates.Add(new Choice("The screen's own", GameSettings.RefreshScreens));
            }
            if (rates.All(c => c.Value != rate))
                rates.Add(new Choice($"{rate} Hz · not on this screen (the closest is used)", rate));
            RefreshChoices = rates;
            Refresh = rates.First(c => c.Value == rate);
        }
        finally
        {
            _loading = wasLoading;
        }
        base.OnPropertyChanged(new PropertyChangedEventArgs(nameof(RefreshHint)));
        base.OnPropertyChanged(new PropertyChangedEventArgs(nameof(Summary)));
    }

    private GameSettings Current() => new()
    {
        Mode = Modes[Math.Clamp(ModeIndex, 0, 2)],
        Screen = Screen?.Value ?? 1,
        Maximised = Maximised,
        FullscreenWidth = Resolution?.Value ?? 0,
        FullscreenHeight = Resolution?.Value2 ?? 0,
        RefreshRate = Refresh?.Value ?? GameSettings.RefreshSmoothest,
        Vsync = VsyncIndex == 1,
        Shape = ShapeIndex switch { 1 => "4:3", 2 => "stretch", _ => "widescreen" },
        Renderer = RendererIndex == 1 ? "cpu" : "gpu",
        Scale = Scales[Math.Clamp(ScaleIndex, 0, 4)],
        Filter = Filters[Math.Clamp(FilterIndex, 0, 2)],
        Sharpening = (int)Math.Round(Sharpening),
        HighPoly = HighPoly,
        Sharp2D = Sharp2D,
        Camera = Cameras[Math.Clamp(CameraIndex, 0, 2)],
        Volume = (int)Math.Round(Volume),
        Reverb = Reverb,
        Multitap = Multitap,
        Vibration = new[] { Vibration1, Vibration2, Vibration3, Vibration4 },
        Keyboard = KeyboardIndex is >= 0 and <= 3 ? KeyboardIndex + 1 : 0,
        UnlockAll = UnlockAll,
        CheckUpdates = CheckUpdates,
    };

    private void Save()
    {
        if (_loading || _readOnly)
            return;
        try
        {
            Current().Save(_folder.SettingsPath);
            Status = "Saved. Used the next time the game starts.";
        }
        catch (Exception ex)
        {
            Status = $"Couldn't save {_folder.SettingsPath}: {ex.Message}";
        }
    }
}
