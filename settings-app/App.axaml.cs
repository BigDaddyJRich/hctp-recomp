using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using HctpSettings.Services;
using HctpSettings.ViewModels;
using HctpSettings.Views;

namespace HctpSettings;

public partial class App : Application
{
    public override void Initialize() => AvaloniaXamlLoader.Load(this);

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
            desktop.MainWindow = new MainWindow { DataContext = new MainViewModel(GameFolder.Find(), readOnly: false) };
        base.OnFrameworkInitializationCompleted();
    }
}
