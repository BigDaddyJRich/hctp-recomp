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
    private SetupViewModel? _watched;

    // when the setup stops, the error (at the bottom of the page) is scrolled into view
    protected override void OnDataContextChanged(EventArgs e)
    {
        base.OnDataContextChanged(e);
        if (_watched != null)
            _watched.PropertyChanged -= OnModelChanged;
        _watched = Model;
        if (_watched != null)
            _watched.PropertyChanged += OnModelChanged;
    }

    private void OnModelChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(SetupViewModel.HasError) && Model?.HasError == true)
            Avalonia.Threading.Dispatcher.UIThread.Post(() => BuildScroll.ScrollToEnd(), Avalonia.Threading.DispatcherPriority.Background);
    }

    private async void OnBrowse(object? sender, RoutedEventArgs e)
    {
        if (TopLevel.GetTopLevel(this)?.StorageProvider is not { } storage || Model is not { } model)
            return;
        var files = await storage.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Your SmackDown! Here Comes the Pain disc (USA, SLUS-20787)",
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
