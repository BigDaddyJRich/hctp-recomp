using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using HctpSettings.Services;

namespace HctpSettings.ViewModels;

/// <summary>A controller button choice: its name in settings.toml (PlayStation names) and what the list shows.</summary>
public sealed record PadOption(string Name, string Label)
{
    public override string ToString() => Label;
}

/// <summary>One row of the Controls page's button table: a PS2 button (or stick direction), the controller button that
/// presses it and the keyboard keys for it (settings.toml [controls] pad_* / key_*).</summary>
public partial class ButtonBinding : ObservableObject
{
    private static readonly string[] Labels =
    {
        "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2",
        "R2", "L3", "R3", "START", "SELECT", "Left stick up", "Left stick down", "Left stick left", "Left stick right",
        "Right stick up", "Right stick down", "Right stick left", "Right stick right",
    };
    private static readonly string[] Glyphs =
    {
        "↑", "↓", "←", "→", "✕", "○", "□", "△", "L1", "R1", "L2", "R2", "L3", "R3", "START", "SELECT",
        "L↑", "L↓", "L←", "L→", "R↑", "R↓", "R←", "R→",
    };

    /// <summary>A controller's buttons, PlayStation name first and the Xbox one after, then "None".</summary>
    public static readonly IReadOnlyList<PadOption> PadOptions = new List<PadOption>
    {
        new("Up", "D-pad up"), new("Down", "D-pad down"), new("Left", "D-pad left"), new("Right", "D-pad right"),
        new("Cross", "Cross (Xbox A)"), new("Circle", "Circle (Xbox B)"), new("Square", "Square (Xbox X)"),
        new("Triangle", "Triangle (Xbox Y)"), new("L1", "L1 (LB)"), new("R1", "R1 (RB)"), new("L2", "L2 (LT)"),
        new("R2", "R2 (RT)"), new("L3", "L3 (left stick click)"), new("R3", "R3 (right stick click)"),
        new("Start", "Options / Start (Menu)"), new("Select", "Share / Select (View)"), new("None", "Nothing"),
    };

    public ButtonBinding(int index)
    {
        Index = index;
        _keys = GameSettings.DefaultKeys[index];
        _pad = HasPad ? PadOptions[index] : null;
    }

    public int Index { get; }
    public string Label => Labels[Index];
    public string Glyph => Glyphs[Index];
    public bool WideGlyph => Glyph.Length > 1;
    public bool HasPad => Index < GameSettings.PadButtonBindings;
    public bool NoPad => !HasPad;
    public IReadOnlyList<PadOption> Options => PadOptions;

    [ObservableProperty] private string _keys;
    [ObservableProperty] private PadOption? _pad;
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(AddText))]
    private bool _isListening;

    public string AddText => IsListening ? "Press a key…" : "Add key";

    public void AddKey(string name)
    {
        var list = Keys.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).ToList();
        list.RemoveAll(k => k.Equals("None", StringComparison.OrdinalIgnoreCase));
        if (!list.Any(k => k.Equals(name, StringComparison.OrdinalIgnoreCase)))
            list.Add(name);
        Keys = string.Join(", ", list);
    }

    [RelayCommand]
    private void ClearKeys() => Keys = "None";

    public void Reset()
    {
        Keys = GameSettings.DefaultKeys[Index];
        if (HasPad)
            Pad = PadOptions[Index];
    }

    /// <summary>An Avalonia key as settings.toml names it (the game's ps2_pad.cpp keyCode), or null.</summary>
    public static string? KeyName(Avalonia.Input.Key key)
    {
        if (key is >= Avalonia.Input.Key.A and <= Avalonia.Input.Key.Z)
            return ((char)('A' + (key - Avalonia.Input.Key.A))).ToString();
        if (key is >= Avalonia.Input.Key.D0 and <= Avalonia.Input.Key.D9)
            return ((char)('0' + (key - Avalonia.Input.Key.D0))).ToString();
        if (key is >= Avalonia.Input.Key.NumPad0 and <= Avalonia.Input.Key.NumPad9)
            return "Numpad" + (key - Avalonia.Input.Key.NumPad0);
        if (key is >= Avalonia.Input.Key.F1 and <= Avalonia.Input.Key.F12)
            return "F" + (key - Avalonia.Input.Key.F1 + 1);
        return key switch
        {
            Avalonia.Input.Key.Up => "Up",
            Avalonia.Input.Key.Down => "Down",
            Avalonia.Input.Key.Left => "Left",
            Avalonia.Input.Key.Right => "Right",
            Avalonia.Input.Key.Space => "Space",
            Avalonia.Input.Key.Enter => "Enter",
            Avalonia.Input.Key.Tab => "Tab",
            Avalonia.Input.Key.Back => "Backspace",
            Avalonia.Input.Key.LeftShift => "LShift",
            Avalonia.Input.Key.RightShift => "RShift",
            Avalonia.Input.Key.LeftCtrl => "LCtrl",
            Avalonia.Input.Key.RightCtrl => "RCtrl",
            Avalonia.Input.Key.LeftAlt => "LAlt",
            Avalonia.Input.Key.RightAlt => "RAlt",
            Avalonia.Input.Key.Insert => "Insert",
            Avalonia.Input.Key.Delete => "Delete",
            Avalonia.Input.Key.Home => "Home",
            Avalonia.Input.Key.End => "End",
            Avalonia.Input.Key.PageUp => "PageUp",
            Avalonia.Input.Key.PageDown => "PageDown",
            Avalonia.Input.Key.OemComma => "Comma",
            Avalonia.Input.Key.OemPeriod => "Period",
            Avalonia.Input.Key.OemMinus => "Minus",
            Avalonia.Input.Key.OemPlus => "Equals",
            Avalonia.Input.Key.OemQuestion => "Slash",
            Avalonia.Input.Key.OemSemicolon => "Semicolon",
            Avalonia.Input.Key.OemQuotes => "Apostrophe",
            Avalonia.Input.Key.OemOpenBrackets => "LBracket",
            Avalonia.Input.Key.OemCloseBrackets => "RBracket",
            Avalonia.Input.Key.OemPipe or Avalonia.Input.Key.OemBackslash => "Backslash",
            Avalonia.Input.Key.OemTilde => "Grave",
            _ => null,
        };
    }
}
