using System.Text.RegularExpressions;

namespace Applepie.Launcher;

public sealed class PublishedFile
{
    public string Asset { get; set; } = "";
    public string Sha256 { get; set; } = "";
    public long Size { get; set; }
    public ReleaseAsset Resolve(GitHubRelease release)
    {
        if (string.IsNullOrWhiteSpace(Asset) || Path.GetFileName(Asset) != Asset || Asset.Contains(':') ||
            string.IsNullOrEmpty(Sha256) || !Regex.IsMatch(Sha256, "^[0-9a-fA-F]{64}$") || Size <= 0) throw new InvalidDataException("发布清单的文件名、大小或校验值无效。");
        var matches = (release.Assets ?? []).Where(a => a is not null && a.Name == Asset).ToArray();
        if (matches.Length != 1) throw new InvalidDataException($"Release 缺少文件或文件重复：{Asset}");
        var asset = matches[0]; OnlineService.Https(asset.Url);
        if (asset.Size > 0 && asset.Size != Size) throw new InvalidDataException($"发布清单与附件大小不一致：{Asset}");
        if (asset.Digest is { } digest && digest.StartsWith("sha256:", StringComparison.Ordinal) && !digest[7..].Equals(Sha256, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException($"发布清单与附件校验值不一致：{Asset}");
        return asset;
    }
}

public sealed class DesktopRelease
{
    public string Version { get; set; } = "";
    public PublishedFile Portable { get; set; } = new();
}

public sealed class CoreRelease
{
    public string Version { get; set; } = "";
    public List<PublishedFile> Files { get; set; } = [];
}

public sealed class ReleaseManifest
{
    public int SchemaVersion { get; set; }
    public string Product { get; set; } = "";
    public string? Version { get; set; }
    public DesktopRelease? Desktop { get; set; }
    public CoreRelease? Core { get; set; }
    public void Validate(GitHubRelease release)
    {
        if (SchemaVersion != 1 || Product != "ApplepieManager" || (Desktop is null && Core is null))
            throw new InvalidDataException("发布清单格式或产品不匹配。");
        if (Version is not null)
        {
            StableVersion(Version);
            if (Desktop?.Version != Version || Core?.Version != Version)
                throw new InvalidDataException("发布清单中的桌面和核心必须使用同一个版本号。");
        }
        if (Desktop is { } desktop)
        {
            StableVersion(desktop.Version);
            if (desktop.Portable?.Asset is null ||
                !(desktop.Portable.Asset.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) || desktop.Portable.Asset.EndsWith(".zip", StringComparison.OrdinalIgnoreCase)))
                throw new InvalidDataException("桌面更新必须为 EXE 或旧版 ZIP 包。");
            desktop.Portable.Resolve(release);
        }
        if (Core is { } core)
        {
            StableVersion(core.Version);
            if (core.Files is null || core.Files.Any(f => f is null) || core.Files.Count != ManagerInstaller.PayloadNames.Length || !core.Files.Select(f => f.Asset).Order().SequenceEqual(ManagerInstaller.PayloadNames.Order()))
                throw new InvalidDataException("核心更新必须包含 Manager 和两个代理 DLL。");
            foreach (var file in core.Files) file.Resolve(release);
        }
    }
    private static void StableVersion(string version)
    {
        if (string.IsNullOrEmpty(version) || !Regex.IsMatch(version, @"^\d+\.\d+\.\d+$")) throw new InvalidDataException($"无效的组件正式版本：{version}");
        ReleaseVersion.Parse(version);
    }
}

public sealed record DesktopUpdate(GitHubRelease Release, DesktopRelease Component);
public sealed record CoreUpdate(GitHubRelease Release, CoreRelease Component);
public sealed class AvailableUpdates
{
    public DesktopUpdate? Desktop { get; private set; }
    public CoreUpdate? Core { get; private set; }
    public List<string> Warnings { get; } = [];
    public void Include(GitHubRelease release, ReleaseManifest manifest)
    {
        manifest.Validate(release);
        if (manifest.Desktop is { } desktop && (Desktop is null || ReleaseVersion.Newer(desktop.Version, Desktop.Component.Version))) Desktop = new(release, desktop);
        if (manifest.Core is { } core && (Core is null || ReleaseVersion.Newer(core.Version, Core.Component.Version))) Core = new(release, core);
    }
}
