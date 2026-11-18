using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using HctpSettings.ViewModels;

namespace HctpSettings.Views;

public partial class SettingsView : UserControl
{
    public SettingsView()
    {
        InitializeComponent();
        // the Controls page's "Add key": the next key pressed anywhere in the window goes to that row (Escape cancels)
        AddHandler(KeyDownEvent, OnKeyDownTunnel, RoutingStrategies.Tunnel, handledEventsToo: true);
    }

    private void OnKeyDownTunnel(object? sender, KeyEventArgs e)
    {
        if (DataContext is not SettingsViewModel vm || vm.Listening == null)
            return;
        var name = e.Key == Key.Escape ? null : ButtonBinding.KeyName(e.Key == Key.None ? e.PhysicalKey switch
        {
            PhysicalKey.ShiftLeft => Key.LeftShift,
            PhysicalKey.ShiftRight => Key.RightShift,
            _ => Key.None,
        } : e.Key);
        if (e.Key != Key.Escape && name == null)
            return; // a key the game can't use: keep listening
        e.Handled = vm.KeyPressed(name);
    }
}
