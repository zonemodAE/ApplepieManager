using System.Text.Json.Serialization;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace Applepie.Launcher;

[JsonConverter(typeof(JsonStringEnumConverter<GameLaunchChannel>))]
public enum GameLaunchChannel { Auto, Official, Steam }

public sealed record GameLaunchPreference
{
    public GameLaunchChannel Channel { get; init; } = GameLaunchChannel.Auto;
    public string? OfficialLauncher { get; init; }
    public string Arguments { get; init; } = "";
    public bool ForceDirectX11 { get; init; }
}

public sealed record GameLaunchPlan(string Root, GameLaunchChannel Channel, string? OfficialLauncher, string Detail)
{
    public bool CanStart => Channel == GameLaunchChannel.Steam || (Channel == GameLaunchChannel.Official && OfficialLauncher is not null);
}

public static class GameLaunchDiscovery
{
    public const string SteamAppId = "4732690";

    public static GameLaunchPlan Resolve(string root, GameLaunchPreference preference)
    {
        GameDirectory.Validate(root);
        root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(root));
        if (!Enum.IsDefined(preference.Channel)) throw new InvalidDataException("启动渠道配置无效，请重新选择。");
        if (preference.Channel == GameLaunchChannel.Steam)
            return new(root, GameLaunchChannel.Steam, null, "手动选择：Steam · 直接启动当前目录的游戏，请保持 Steam 登录。");
        if (preference.Channel == GameLaunchChannel.Auto && IsSteam(root))
            return new(root, GameLaunchChannel.Steam, null, "已识别：Steam · 直接启动当前目录的游戏，请保持 Steam 登录。");

        var manual = !string.IsNullOrWhiteSpace(preference.OfficialLauncher);
        var candidates = manual ? [Path.GetFullPath(preference.OfficialLauncher!)] : FindOfficialLaunchers(root);
        var prefix = preference.Channel == GameLaunchChannel.Official ? "手动选择：官方" : "已识别：官方";
        if (candidates.Count == 1 && File.Exists(candidates[0]))
            return new(root, GameLaunchChannel.Official, candidates[0], prefix + " · " + candidates[0]);
        if (manual)
            return new(root, GameLaunchChannel.Official, null, "保存的官方启动程序已不存在，请重新选择 Games.exe 或恢复自动查找。");
        if (candidates.Count > 1)
            return new(root, GameLaunchChannel.Official, null, "找到多个官方启动器，请选择当前游戏使用的 Games.exe。");
        return new(root, preference.Channel, null, preference.Channel == GameLaunchChannel.Official
            ? "未找到官方 Games.exe，请选择官方启动器中的 Games.exe 或 Launcher.exe。"
            : "未能自动识别渠道，请手动选择官方或 Steam；官方渠道需要定位 Games.exe。");
    }

    public static bool IsSteam(string root)
    {
        root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(root));
        // A folder name alone is not a channel marker. Require the matching
        // Steam manifest, or the channel's app ID together with its Steam API.
        var appId = Path.Combine(root, "steam_appid.txt");
        if (File.Exists(appId) && File.Exists(Path.Combine(root, "steam_api64.dll")) && File.ReadAllText(appId).Trim().TrimStart('\uFEFF') == SteamAppId)
            return true;
        var common = Directory.GetParent(root);
        var steamapps = common?.Parent;
        if (common?.Name.Equals("common", StringComparison.OrdinalIgnoreCase) != true ||
            steamapps?.Name.Equals("steamapps", StringComparison.OrdinalIgnoreCase) != true) return false;
        var manifest = Path.Combine(steamapps.FullName, "appmanifest_" + SteamAppId + ".acf");
        if (!File.Exists(manifest)) return false;
        var text = File.ReadAllText(manifest);
        string Field(string name) => Regex.Match(text, "\"" + name + "\"\\s+\"([^\"]*)\"", RegexOptions.IgnoreCase).Groups[1].Value;
        return Field("appid") == SteamAppId && string.Equals(Field("installdir"), Path.GetFileName(Path.TrimEndingDirectorySeparator(root)), StringComparison.OrdinalIgnoreCase);
    }

    public static IReadOnlyList<string> FindOfficialLaunchers(string gameRoot)
    {
        // Prefer the launcher enclosing this game over unrelated installations.
        for (var directory = new DirectoryInfo(gameRoot); directory is not null; directory = directory.Parent)
        {
            var path = InLauncherFolder(directory.FullName);
            if (path is not null) return [path];
        }
        var candidates = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var folder in RegisteredFolders())
        {
            var path = InLauncherFolder(folder);
            if (path is not null) candidates.Add(path);
        }
        return candidates.Order(StringComparer.OrdinalIgnoreCase).ToList();
    }

    public static string SelectOfficialLauncher(string selected)
    {
        selected = Path.GetFullPath(selected);
        if (File.Exists(selected) && Path.GetFileName(selected).Equals("Games.exe", StringComparison.OrdinalIgnoreCase)) return selected;
        var folder = Directory.Exists(selected) ? selected : Path.GetDirectoryName(selected)!;
        return InLauncherFolder(folder) ?? throw new FileNotFoundException("此位置没有找到 Games.exe，请选择官方启动器中的 Games.exe 或 Launcher.exe。");
    }

    public static string? InLauncherFolder(string folder)
    {
        try
        {
            if (!Directory.Exists(folder)) return null;
            var direct = Path.Combine(folder, "Games.exe");
            if (File.Exists(direct)) return direct;
            if (!File.Exists(Path.Combine(folder, "Launcher.exe"))) return null;
            return new DirectoryInfo(folder).EnumerateDirectories()
                .Where(d => (d.Attributes & FileAttributes.ReparsePoint) == 0 && Version.TryParse(d.Name, out _))
                .OrderByDescending(d => Version.Parse(d.Name))
                .Select(d => Path.Combine(d.FullName, "Games.exe")).FirstOrDefault(File.Exists);
        }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
    }

    private static List<string> RegisteredFolders()
    {
        var folders = new List<string>();
        foreach (var special in new[] { Environment.SpecialFolder.ProgramFiles, Environment.SpecialFolder.ProgramFilesX86 })
            foreach (var name in new[] { "GRYPHLINK", "Hypergryph Launcher" })
                folders.Add(Path.Combine(Environment.GetFolderPath(special), name));
        foreach (var hive in new[] { RegistryHive.CurrentUser, RegistryHive.LocalMachine })
        foreach (var view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
        {
            try
            {
                using var root = RegistryKey.OpenBaseKey(hive, view);
                using var known = root.OpenSubKey(@"Software\GRYPHLINK\Launcher\94005b4608518100ed8df1d8107356a8");
                if (known?.GetValue("install_path") is string installed && Path.IsPathFullyQualified(installed)) folders.Add(installed);
                using var uninstall = root.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Uninstall");
                if (uninstall is null) continue;
                foreach (var key in uninstall.GetSubKeyNames())
                {
                    using var app = uninstall.OpenSubKey(key);
                    if (app?.GetValue("DisplayName") is not string name ||
                        !(name.Contains("GRYPHLINK", StringComparison.OrdinalIgnoreCase) || name.Contains("Hypergryph Launcher", StringComparison.OrdinalIgnoreCase) || name.Contains("鹰角启动器"))) continue;
                    if (app.GetValue("InstallLocation") is string location && Path.IsPathFullyQualified(location)) folders.Add(location);
                }
            }
            catch (System.Security.SecurityException) { }
            catch (UnauthorizedAccessException) { }
            catch (IOException) { }
        }
        return folders;
    }
}
