using System.ComponentModel;
using System.Numerics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;

namespace Applepie.Launcher;

public static class Product
{
    public const string Repository = "Sasye/ApplepieManager";
    public const string ReleaseManifestName = "applepie-release.json";
    public const string CatalogUrl = "https://raw.githubusercontent.com/Sasye/ApplepieManager/master/catalog/plugins.json";
    public static string Version => typeof(Product).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()!.InformationalVersion.Split('+')[0];
    public static string CoreVersion => Version;
    public static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web) { WriteIndented = true };
    public static string DataDirectory => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "ApplepieManager");
    public static string WorkDirectory => Path.Combine(Path.GetTempPath(), "ApplepieManager");
    public static string NewWorkDirectory()
    {
        var path = Path.Combine(WorkDirectory, Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(path);
        return path;
    }
}

public enum NavigationPage { Deployment, Plugins, Downloads, About }

public sealed class ReleaseVersion : IComparable<ReleaseVersion>
{
    private readonly BigInteger[] numbers;
    private readonly string[] prerelease;
    public string Text { get; }
    private ReleaseVersion(string text, BigInteger[] parts, string[] pre) { Text = text; numbers = parts; prerelease = pre; }
    public static ReleaseVersion Parse(string text)
    {
        var match = Regex.Match(text.Trim(), @"^[vV]?(\d+)\.(\d+)\.(\d+)(?:\.(\d+))?(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$");
        if (!match.Success) throw new InvalidDataException($"无法识别版本号：{text}");
        return new(text, [.. Enumerable.Range(1, 4).Select(i => BigInteger.Parse(match.Groups[i].Success ? match.Groups[i].Value : "0"))],
            match.Groups[5].Success ? match.Groups[5].Value.Split('.') : []);
    }
    public static bool Newer(string remote, string local)
    {
        try { return Parse(remote).CompareTo(Parse(local)) > 0; } catch (InvalidDataException) { return false; }
    }
    public int CompareTo(ReleaseVersion? other)
    {
        if (other is null) return 1;
        for (var i = 0; i < numbers.Length; ++i) { var c = numbers[i].CompareTo(other.numbers[i]); if (c != 0) return c; }
        if (prerelease.Length == 0 || other.prerelease.Length == 0) return other.prerelease.Length.CompareTo(prerelease.Length);
        for (var i = 0; i < Math.Min(prerelease.Length, other.prerelease.Length); ++i)
        {
            var a = BigInteger.TryParse(prerelease[i], out var av); var b = BigInteger.TryParse(other.prerelease[i], out var bv);
            var c = a && b ? av.CompareTo(bv) : a != b ? (a ? -1 : 1) : string.CompareOrdinal(prerelease[i], other.prerelease[i]);
            if (c != 0) return c;
        }
        return prerelease.Length.CompareTo(other.prerelease.Length);
    }
}

public sealed class LauncherSettings
{
    public string? GameDirectory { get; set; }
    public string Theme { get; set; } = "light";
    public Dictionary<string, GameLaunchPreference> GameLaunches { get; set; } = new();
    public GameLaunchPreference LaunchFor(string root) => GameLaunches.GetValueOrDefault(LaunchKey(root)) ?? new();
    public void SetLaunch(string root, GameLaunchPreference preference) => GameLaunches[LaunchKey(root)] = preference;
    private static string LaunchKey(string root) => Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar).ToUpperInvariant();
    public static string FilePath => Path.Combine(Product.DataDirectory, "launcher.json");
    public static LauncherSettings Load(string? path = null)
    {
        path ??= FilePath;
        if (!File.Exists(path)) return new();
        using var json = JsonDocument.Parse(File.ReadAllText(path));
        var settings = json.RootElement.Deserialize<LauncherSettings>(Product.Json) ?? new();
        if (json.RootElement.ValueKind != JsonValueKind.Object) return settings;
        var launches = json.RootElement.EnumerateObject().FirstOrDefault(p => p.Name.Equals("gameLaunches", StringComparison.OrdinalIgnoreCase)).Value;
        if (launches.ValueKind != JsonValueKind.Object) return settings;
        foreach (var entry in launches.EnumerateObject())
        {
            // Older versions stored the checkbox in Arguments. Migrate only
            // entries without the new flag; explicit false must stay false.
            if (entry.Value.ValueKind != JsonValueKind.Object ||
                entry.Value.EnumerateObject().Any(p => p.Name.Equals("forceDirectX11", StringComparison.OrdinalIgnoreCase)) ||
                !settings.GameLaunches.TryGetValue(entry.Name, out var preference) || preference is null) continue;
            var arguments = preference.Arguments ?? "";
            if (GameLaunchArguments.HasDirectX11(arguments))
                settings.GameLaunches[entry.Name] = preference with { ForceDirectX11 = true, Arguments = GameLaunchArguments.WithDirectX11(arguments, false) };
        }
        return settings;
    }
    public void Save(string? path = null) => Files.WriteAtomic(path ?? FilePath, JsonSerializer.SerializeToUtf8Bytes(this, Product.Json));
}

public abstract class Observable : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;
    protected void Changed([CallerMemberName] string? name = null) => PropertyChanged?.Invoke(this, new(name));
}

public sealed class PluginItem : Observable
{
    public required string FileName { get; init; }
    public required string Name { get; init; }
    public string Id { get; init; } = "";
    public string Version { get; init; } = "未知版本";
    public string Detail { get; init; } = "";
    public bool Manageable { get; init; }
    public bool Enabled { get; init; }
    public string StateText => !Manageable ? "依赖库 / 未适配" : Enabled ? "已启用" : "已停用";
    public string ActionText => Enabled ? "停用" : "启用";
}

public sealed class Catalog
{
    public List<CatalogPlugin> Plugins { get; set; } = [];
}
public sealed class CatalogPlugin
{
    public string Id { get; set; } = "";
    public string Repository { get; set; } = "";
    public string EntryDll { get; set; } = "";
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] public string? AssetPattern { get; set; }
    [JsonIgnore] public string Name => Repository.Split('/').Last();
    [JsonIgnore] public string ReleaseError { get; set; } = "";
    [JsonIgnore] public string Description => ReleaseError.Length == 0 ? Repository : ReleaseError;
    [JsonIgnore] public string Version { get; set; } = "";
    [JsonIgnore] public string DownloadUrl { get; set; } = "";
    [JsonIgnore] public string AssetName { get; set; } = "";
    [JsonIgnore] public string? Sha256 { get; set; }
    [JsonIgnore] public long? DownloadSize { get; set; }
    [JsonIgnore] public string InstalledVersion { get; set; } = "";
    [JsonIgnore] public bool ReleaseAvailable => ReleaseError.Length == 0 && Version.Length > 0 && DownloadUrl.Length > 0;
    [JsonIgnore] public bool CanInstall => ReleaseAvailable && (InstalledVersion.Length == 0 || InstalledVersion == "未知版本" || ReleaseVersion.Newer(Version, InstalledVersion));
    [JsonIgnore] public string ActionText => !ReleaseAvailable ? "暂不可用" : InstalledVersion.Length == 0 ? "安装" : CanInstall ? "更新" : "已是最新";
    [JsonIgnore] public string VersionText => !ReleaseAvailable ? (InstalledVersion.Length == 0 ? "尚未获取发布版本" : $"已安装 {InstalledVersion}")
        : InstalledVersion.Length == 0 ? $"最新 v{Version}" : $"已安装 {InstalledVersion}  /  最新 {Version}";
}
public sealed record ProgressInfo(string Message, double Percent = -1);
public sealed record DeploymentResult(string BackupDirectory, int ChangedFiles);
public sealed record OwnedFile(string Path, string Sha256);
public sealed class PluginReceipt
{
    public string Id { get; set; } = "";
    public string EntryDll { get; set; } = "";
    public string Version { get; set; } = "";
    public List<OwnedFile> Files { get; set; } = [];
}
