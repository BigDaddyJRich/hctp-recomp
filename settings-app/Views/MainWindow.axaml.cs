using Avalonia.Controls;
using HctpSettings.ViewModels;

namespace HctpSettings.Views;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        Opened += async (_, _) =>
        {
            if (DataContext is MainViewModel vm)
                await vm.OnOpenedAsync();
        };
    }
}
