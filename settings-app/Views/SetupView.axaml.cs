using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;
using VendettaSettings.ViewModels;

namespace VendettaSettings.Views;

public partial class SetupView : UserControl
{
    public SetupView()
    {
        InitializeComponent();
        DropZone.AddHandler(DragDrop.DropEvent, OnDrop);
        DropZone.AddHandler(DragDrop.DragOverEvent, OnDragOver);
    }

    private SetupViewModel? Model => DataContext as SetupViewModel;

    private async void OnBrowse(object? sender, RoutedEventArgs e)
    {
        if (TopLevel.GetTopLevel(this)?.StorageProvider is not { } storage || Model is not { } model)
            return;
        var files = await storage.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Your Def Jam Vendetta disc (USA, SLUS-20639)",
            AllowMultiple = false,
            FileTypeFilter = new[]
            {
                new FilePickerFileType("PS2 disc image (.chd, .iso)") { Patterns = new[] { "*.chd", "*.iso" } },
                FilePickerFileTypes.All,
            },
        });
        if (files.Count > 0 && files[0].TryGetLocalPath() is { } path)
            await model.SetDiscAsync(path);
    }

    private void OnDragOver(object? sender, DragEventArgs e)
    {
        e.DragEffects = e.DataTransfer.Contains(DataFormat.File) ? DragDropEffects.Copy : DragDropEffects.None;
    }

    private async void OnDrop(object? sender, DragEventArgs e)
    {
        if (Model is not { } model)
            return;
        var file = e.DataTransfer.TryGetFiles()?.FirstOrDefault();
        if (file?.TryGetLocalPath() is { } path)
            await model.SetDiscAsync(path);
    }
}
