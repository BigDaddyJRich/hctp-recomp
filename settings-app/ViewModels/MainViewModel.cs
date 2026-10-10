using System.ComponentModel;
using System.Reflection;
using CommunityToolkit.Mvvm.ComponentModel;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.Input;
using VendettaSettings.Services;

namespace VendettaSettings.ViewModels;

public partial class MainViewModel : ObservableObject
{
    public const string XUrl = "https://x.com/jcsix694";

    public GameFolder Folder { get; }
    public SettingsViewModel Settings { get; }
    public SetupViewModel Setup { get; }

    public MainViewModel(GameFolder folder, bool readOnly)
    {
        Folder = folder;
        Settings = new SettingsViewModel(folder, readOnly);
        Setup = new SetupViewModel(folder, FinishSetup);
        InSetup = folder.NeedsSetup;
        Settings.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(SettingsViewModel.Summary))
                OnPropertyChanged(nameof(PlaySummary));
        };
        if (!readOnly)
        {
            // whether the game is running, however it was started (here, run.bat, …): checked each second
            GameRunning = GameFolder.IsGameRunning();
            var timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
            timer.Tick += (_, _) => GameRunning = GameFolder.IsGameRunning();
            timer.Start();
        }
    }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsPlay), nameof(IsSettings), nameof(IsAbout))]
    private int _pageIndex;

    /// <summary>First run (no game built yet): the setup screens instead of the pages.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(NotInSetup))]
    private bool _inSetup;

    public bool NotInSetup => !InSetup;

    // step 3: the settings, with everything the pages show read again (the game, the disc, the save)
    private void FinishSetup()
    {
        InSetup = false;
        OnPropertyChanged(string.Empty);
        ShowPage("display");
    }

    public bool IsPlay => PageIndex == 0;
    public bool IsSettings => PageIndex == 1;
    public bool IsAbout => PageIndex == 2;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ShowRunning))]
    private string _message = "";

    /// <summary>The game is open: PLAY is off (two copies would share the memory card).</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanPlay), nameof(ShowRunning))]
    private bool _gameRunning;

    public bool ShowRunning => GameRunning && Message.Length == 0;

    partial void OnGameRunningChanged(bool value)
    {
        if (!value && Message == GameFolder.AlreadyRunning)
            Message = "";
    }

    public string Version =>
        "v" + (Assembly.GetExecutingAssembly().GetName().Version is { } v ? $"{v.Major}.{v.Minor}.{v.Build}" : "0.1.0");

    public bool HasGame => Folder.HasGame;
    public bool NoGame => !Folder.HasGame;
    public string DiscLine => Folder.HasGame ? $"✓ {GameFolder.Serial}" : "No game files yet";
    public string DiscTitle => Folder.HasGame ? "Def Jam Vendetta · PS2 · USA" : "Not set up yet";
    public string SaveTitle => Folder.HasSave ? "Saved" : "No save yet";
    public string SaveLine => "Kept in this folder, in game\\mc0\\";
    public bool CanPlay => Folder.HasGame && Folder.GameExe != null && !GameRunning;
    public string PlaySummary => Settings.Summary;
    public string GameExeName => Folder.GameExe is { } exe ? Path.GetFileName(exe) : "not built yet";
    public string FolderPath => Folder.Root;

    [RelayCommand] private void ShowPlay() => PageIndex = 0;
    [RelayCommand] private void ShowSettings() => PageIndex = 1;
    [RelayCommand] private void ShowAbout() => PageIndex = 2;

    [RelayCommand]
    private async Task Play()
    {
        if (Folder.Play(out var game) is { } error)
        {
            Message = error;
            return;
        }
        // the note stays while the game's window opens (a few seconds), then goes; PLAY is off meanwhile
        Message = "Starting the game…";
        GameRunning = true;
        var closed = game!.WaitForExitAsync();
        await Task.WhenAny(closed, Task.Delay(TimeSpan.FromSeconds(6)));
        Message = closed.IsCompleted && game.ExitCode != 0 ? "The game closed while starting: see logs\\game.log.err" : "";
    }

    [RelayCommand] private void OpenFolder() => GameFolder.Open(Folder.Root);
    [RelayCommand] private void OpenSaves() => GameFolder.Open(Folder.SavesPath);
    [RelayCommand] private void OpenX() => GameFolder.Open(XUrl);

    /// <summary>For pictures of the pages (--shot): play, display, graphics, sound, controls or about.</summary>
    public void ShowPage(string page)
    {
        switch (page)
        {
            case "setup1": InSetup = true; return;
            case "setup2": InSetup = true; Setup.ShowSample(0.62, false); return;
            case "setupdone": InSetup = true; Setup.ShowSample(1, true); return;
            case "about": PageIndex = 2; break;
            case "display": PageIndex = 1; Settings.TabIndex = 0; break;
            case "graphics": PageIndex = 1; Settings.TabIndex = 1; break;
            case "sound": PageIndex = 1; Settings.TabIndex = 2; break;
            case "controls": PageIndex = 1; Settings.TabIndex = 3; break;
            default: PageIndex = 0; break;
        }
    }
}
