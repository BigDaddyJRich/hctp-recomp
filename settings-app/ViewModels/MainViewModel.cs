using System.ComponentModel;
using System.Reflection;
using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using HctpSettings.Services;

namespace HctpSettings.ViewModels;

public partial class MainViewModel : ObservableObject
{
    public const string XUrl = "https://x.com/jcsix";
    public const string GitHubUrl = "https://github.com/jcsix694";

    public GameFolder Folder { get; }
    public SettingsViewModel Settings { get; }
    public SetupViewModel Setup { get; }
    public UpdateViewModel Updates { get; }

    private readonly bool _readOnly;

    public MainViewModel(GameFolder folder, bool readOnly)
    {
        Folder = folder;
        _readOnly = readOnly;
        Settings = new SettingsViewModel(folder, readOnly);
        Setup = new SetupViewModel(folder, FinishSetup);
        Updates = new UpdateViewModel(folder, () => Settings.CheckUpdates, () => Exit());
        InSetup = folder.NeedsSetup;
        Updates.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(UpdateViewModel.Mode) or nameof(UpdateViewModel.ShowNotice))
            {
                OnPropertyChanged(nameof(InUpdate));
                OnPropertyChanged(nameof(NormalPages));
                OnPropertyChanged(nameof(CanPlay));
                OnStatusChanged();
                if (e.PropertyName == nameof(UpdateViewModel.Mode) && !Updates.Active)
                    OnPropertyChanged(string.Empty); // after an update: the version, the game and the save read again
            }
        };
        Settings.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(SettingsViewModel.TabIndex))
                OnPageChanged();
            if (e.PropertyName == nameof(SettingsViewModel.Status))
                OnStatusChanged();
        };
        Setup.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(SetupViewModel.Page) or nameof(SetupViewModel.Done) or
                nameof(SetupViewModel.Error) or nameof(SetupViewModel.Building))
            {
                OnStatusChanged();
                OnPropertyChanged(nameof(CanPlay));
                OnPropertyChanged(nameof(SetupStep1Now));
                OnPropertyChanged(nameof(SetupStep2Now));
                OnPropertyChanged(nameof(SetupStep2Done));
                OnPropertyChanged(nameof(SetupStep2Next));
            }
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

    [ObservableProperty] private int _pageIndex;

    partial void OnPageIndexChanged(int value) => OnPageChanged();

    private void OnPageChanged()
    {
        foreach (var name in new[]
                 {
                     nameof(IsPlay), nameof(IsSettings), nameof(IsAbout), nameof(IsDisplay), nameof(IsGraphics),
                     nameof(IsSound), nameof(IsControls), nameof(IsGameplay), nameof(ShowOpenFolder),
                 })
            OnPropertyChanged(name);
        OnStatusChanged();
    }

    /// <summary>First run (no game built yet): the setup in the page list's place and in the page.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(NotInSetup))]
    private bool _inSetup;

    partial void OnInSetupChanged(bool value)
    {
        OnStatusChanged();
        OnPropertyChanged(nameof(CanPlay));
    }

    public bool NotInSetup => !InSetup;
    /// <summary>An update's pages (what's new, updating, updated, couldn't finish) in the window's place.</summary>
    public bool InUpdate => Updates.Active;
    public bool NormalPages => !InSetup && !InUpdate;

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
    public bool IsDisplay => IsSettings && Settings.TabIndex == 0;
    public bool IsGraphics => IsSettings && Settings.TabIndex == 1;
    public bool IsSound => IsSettings && Settings.TabIndex == 2;
    public bool IsControls => IsSettings && Settings.TabIndex == 3;
    public bool IsGameplay => IsSettings && Settings.TabIndex == 4;
    /// <summary>The header's grey button: Open game folder, or on the settings pages Reset to defaults.</summary>
    public bool ShowOpenFolder => !IsSettings;

    // the setup's steps in the page list
    public bool SetupStep1Now => Setup.IsDiscPage;
    public bool SetupStep2Now => Setup.IsBuildPage && !Setup.Done;
    public bool SetupStep2Done => Setup.Done;
    public bool SetupStep2Next => Setup.IsDiscPage;

    [ObservableProperty] private string _message = "";

    partial void OnMessageChanged(string value) => OnStatusChanged();

    /// <summary>The game is open: PLAY is off (two copies would share the memory card).</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanPlay))]
    private bool _gameRunning;

    partial void OnGameRunningChanged(bool value)
    {
        if (!value && Message == GameFolder.AlreadyRunning)
            Message = "";
        OnStatusChanged();
    }

    // --- the status at the right of the header: a dot (good / warn / bad) and a few words ---
    public string Status
    {
        get
        {
            if (InSetup)
            {
                if (Setup.HasError) return "The setup stopped";
                if (Setup.Done) return "Ready to play";
                return Setup.IsBuildPage ? "Building…" : "First launch: set up from your disc";
            }
            if (InUpdate) return Updates.Status;
            if (Message.Length > 0) return Message;
            if (GameRunning) return "The game is running";
            if (Updates.ShowNotice && IsPlay) return "Update available";
            if (IsSettings) return Settings.Status.Length > 0 ? Settings.Status : "Saved as you change them";
            return CanPlay ? "Ready to play" : "The game isn't built yet";
        }
    }

    public bool StatusGood => !StatusWarn && !StatusBad;
    public bool StatusWarn =>
        InSetup ? !Setup.Done && !Setup.HasError
        : InUpdate ? Updates.IsWorking
        : Message.Length > 0 || GameRunning || (Updates.ShowNotice && IsPlay);
    public bool StatusBad =>
        InSetup ? Setup.HasError
        : InUpdate ? Updates.IsFailed
        : !CanPlay && !GameRunning && Message.Length == 0 && !IsSettings;

    private void OnStatusChanged()
    {
        OnPropertyChanged(nameof(Status));
        OnPropertyChanged(nameof(StatusGood));
        OnPropertyChanged(nameof(StatusWarn));
        OnPropertyChanged(nameof(StatusBad));
    }

    public string Version => "v" + Updater.CurrentText;

    public bool HasGame => Folder.HasGame;
    public string DiscTitle => Folder.HasGame ? "SmackDown! Here Comes the Pain · PS2 · USA" : "Not set up yet";
    public string SaveLine => Folder.HasSave ? "Saved · game\\mc0" : "No save yet";
    // during the setup PLAY waits for the build to finish
    public bool CanPlay => Folder.HasGame && Folder.GameExe != null && !GameRunning && (!InSetup || Setup.Done) && !Updates.IsWorking;
    public string GameExeName => Folder.GameExe is { } exe ? Path.GetFileName(exe) : "not built yet";
    public string FolderPath => Folder.Root;

    [RelayCommand] private void ShowPlay() => PageIndex = 0;
    [RelayCommand] private void ShowDisplay() => ShowSettingsTab(0);
    [RelayCommand] private void ShowGraphics() => ShowSettingsTab(1);
    [RelayCommand] private void ShowSound() => ShowSettingsTab(2);
    [RelayCommand] private void ShowControls() => ShowSettingsTab(3);
    [RelayCommand] private void ShowGameplay() => ShowSettingsTab(4);
    [RelayCommand] private void ShowAbout() => PageIndex = 2;

    private void ShowSettingsTab(int tab)
    {
        Settings.TabIndex = tab;
        PageIndex = 1;
        OnPageChanged();
    }

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
    [RelayCommand] private void OpenGitHub() => GameFolder.Open(GitHubUrl);
    [RelayCommand] private void OpenLicence() => GameFolder.OpenText(Path.Combine(Folder.SourceRoot, "LICENSE"));
    [RelayCommand] private void OpenNotices() => GameFolder.OpenText(Path.Combine(Folder.SourceRoot, "THIRD-PARTY-NOTICES.txt"));

    [RelayCommand]
    private void Exit() =>
        (Application.Current?.ApplicationLifetime as IClassicDesktopStyleApplicationLifetime)?.Shutdown();

    /// <summary>When the window has opened: an update left halfway, or a check for a new one.</summary>
    public Task OnOpenedAsync() => _readOnly ? Task.CompletedTask : Updates.StartAsync(InSetup); // not for pictures

    /// <summary>For pictures of the pages (--shot): play, display, graphics, sound, controls, gameplay, about, setup1, setup2,
    /// setupdone.</summary>
    public void ShowPage(string page)
    {
        switch (page)
        {
            case "setup1": InSetup = true; return;
            case "setup2": InSetup = true; Setup.ShowSample(0.62, false); return;
            case "setupdone": InSetup = true; Setup.ShowSample(1, true); return;
            case "update-notice": PageIndex = 0; Updates.ShowSample("notice"); return;
            case "update-notes": Updates.ShowSample("notes"); return;
            case "update-working": Updates.ShowSample("working"); return;
            case "update-done": Updates.ShowSample("done"); return;
            case "update-failed": Updates.ShowSample("failed"); return;
            case "update-buildfailed": Updates.ShowSample("buildfailed"); return;
            case "setupfailed": InSetup = true; Setup.ShowSampleError(); return;
            case "about": PageIndex = 2; break;
            case "display": ShowSettingsTab(0); break;
            case "graphics": ShowSettingsTab(1); break;
            case "sound": ShowSettingsTab(2); break;
            case "controls": ShowSettingsTab(3); break;
            case "gameplay": ShowSettingsTab(4); break;
            default: PageIndex = 0; break;
        }
    }
}
