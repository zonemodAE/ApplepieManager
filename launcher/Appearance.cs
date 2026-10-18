using System.Text.Json;
using System.Windows;

namespace Applepie.Launcher;

public enum AppearanceMode { Dark, Light }

public static class Appearance
{
    private static ResourceDictionary? palette;
    public static AppearanceMode Current { get; private set; } = AppearanceMode.Light;
    public static string Preference => Current == AppearanceMode.Light ? "light" : "dark";

    public static void Apply(AppearanceMode mode)
    {
        if (mode is not AppearanceMode.Dark and not AppearanceMode.Light) throw new ArgumentOutOfRangeException(nameof(mode));
        var resources = Application.Current.Resources;
        var assembly = typeof(Appearance).Assembly.GetName().Name;
        var next = new ResourceDictionary { Source = new Uri($"/{assembly};component/Themes/{mode}.xaml", UriKind.Relative) };
        var index = palette is null ? -1 : resources.MergedDictionaries.IndexOf(palette);
        if (index < 0) resources.MergedDictionaries.Add(next);
        else resources.MergedDictionaries[index] = next;
        palette = next; Current = mode;
    }

    public static void RestorePreference(string? settingsPath = null)
    {
        var mode = AppearanceMode.Light;
        try
        {
            if (string.Equals(LauncherSettings.Load(settingsPath).Theme, "dark", StringComparison.OrdinalIgnoreCase)) mode = AppearanceMode.Dark;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException)
        { /* Use the default light palette; the main window reports unreadable settings. */ }
        Apply(mode);
    }

    public static void SavePreference(AppearanceMode mode, string? settingsPath = null)
    {
        if (mode is not AppearanceMode.Dark and not AppearanceMode.Light) throw new ArgumentOutOfRangeException(nameof(mode));
        var settings = LauncherSettings.Load(settingsPath);
        settings.Theme = mode == AppearanceMode.Light ? "light" : "dark";
        settings.Save(settingsPath);
        Apply(mode);
    }

    public static void Toggle(Window owner)
    {
        try { SavePreference(Current == AppearanceMode.Dark ? AppearanceMode.Light : AppearanceMode.Dark); }
        catch (Exception error) { Dialogs.Message(owner, "外观设置未保存", error.Message); }
    }
}
