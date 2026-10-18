using System.Diagnostics;
using System.Reflection.PortableExecutable;
using System.Text;
using System.Text.Json;

namespace Applepie.Launcher;

public sealed record CorePackage(string Version, string? Directory = null)
{
    public static CorePackage Embedded => new(Product.CoreVersion);
}

public sealed class CorePackages(string? cacheDirectory = null)
{
    private readonly string root = cacheDirectory ?? Path.Combine(Product.DataDirectory, "core-packages");
    public CorePackage Current()
    {
        var pointer = Files.Under(root, "current.txt");
        if (!File.Exists(pointer)) return CorePackage.Embedded;
        var id = File.ReadAllText(pointer).Trim();
        if (!Guid.TryParseExact(id, "N", out _)) throw new InvalidDataException("核心缓存记录无效，请重新下载核心。");
        var package = Validate(Files.Under(root, id));
        return ReleaseVersion.Newer(package.Version, Product.CoreVersion) ? package : CorePackage.Embedded;
    }
    public static CorePackage Validate(string directory)
    {
        var manifest = JsonSerializer.Deserialize<CoreRelease>(Files.ReadUtf8(Files.Under(directory, "core-package.json")), Product.Json)
            ?? throw new InvalidDataException("核心包缺少清单。");
        if (string.IsNullOrEmpty(manifest.Version)) throw new InvalidDataException("核心包缺少版本。");
        ReleaseVersion.Parse(manifest.Version);
        if (manifest.Files is null || manifest.Files.Any(f => f is null) || manifest.Files.Count != ManagerInstaller.PayloadNames.Length || !manifest.Files.Select(f => f.Asset).Order().SequenceEqual(ManagerInstaller.PayloadNames.Order()))
            throw new InvalidDataException("核心包文件不完整。");
        foreach (var file in manifest.Files)
        {
            var path = Files.Under(directory, file.Asset);
            if (new FileInfo(path).Length != file.Size || !Files.Hash(path).Equals(file.Sha256, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException($"核心文件校验失败：{file.Asset}");
            using var stream = File.OpenRead(path); using var pe = new PEReader(stream);
            if (pe.PEHeaders.CoffHeader.Machine != Machine.Amd64 || (pe.PEHeaders.CoffHeader.Characteristics & Characteristics.Dll) == 0)
                throw new InvalidDataException($"核心文件不是 Windows x64 DLL：{file.Asset}");
        }
        var nativeVersion = FileVersionInfo.GetVersionInfo(Files.Under(directory, "applepie_manager.dll")).ProductVersion;
        if (nativeVersion is null || ReleaseVersion.Parse(nativeVersion).CompareTo(ReleaseVersion.Parse(manifest.Version)) != 0)
            throw new InvalidDataException("核心 DLL 与发布版本不一致。");
        return new(manifest.Version, directory);
    }
    public async Task<CorePackage> Download(OnlineService online, CoreUpdate update, IProgress<ProgressInfo>? progress, CancellationToken cancellation)
    {
        var manifest = new ReleaseManifest { SchemaVersion = 1, Product = "ApplepieManager", Core = update.Component };
        manifest.Validate(update.Release);
        try
        {
            var current = Current();
            if (ReleaseVersion.Parse(current.Version).CompareTo(ReleaseVersion.Parse(update.Component.Version)) >= 0) return current;
        }
        catch (Exception error) when (error is IOException or InvalidDataException or JsonException or BadImageFormatException) { } // A verified new download repairs a damaged cache.
        var work = Product.NewWorkDirectory();
        try
        {
            foreach (var file in update.Component.Files)
                await online.Download(file.Resolve(update.Release).Url, Path.Combine(work, file.Asset), file.Sha256, progress, cancellation, file.Size);
            Files.WriteAtomic(Path.Combine(work, "core-package.json"), JsonSerializer.SerializeToUtf8Bytes(update.Component, Product.Json));
            await Task.Run(() => Validate(work), cancellation);
            cancellation.ThrowIfCancellationRequested();
            var id = Guid.NewGuid().ToString("N"); var destination = Files.Under(root, id);
            // Publish the pointer only after the entire independent core package is valid.
            foreach (var file in ManagerInstaller.PayloadNames.Append("core-package.json")) Files.CopyAtomic(Path.Combine(work, file), Files.Under(destination, file));
            var result = Validate(destination);
            Files.WriteAtomic(Files.Under(root, "current.txt"), Encoding.UTF8.GetBytes(id));
            return result;
        }
        finally { Files.RemoveWork(work); }
    }
}
