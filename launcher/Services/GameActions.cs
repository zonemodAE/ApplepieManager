using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;

namespace Applepie.Launcher;

// Keep the original .dll.bak files across upgrades. Intermediate Applepie
// versions belong only in desktop operation backups, never in these originals.
public static class ProxyBackups
{
    private const string ReceiptPath = ".applepie/proxies.json";
    private static readonly string[] Names = ["d3dcompiler_47.dll", "vulkan-1.dll"];
    private sealed class Receipt
    {
        public Receipt() { }
        public int SchemaVersion { get; set; } = 1;
        public List<Proxy> Proxies { get; set; } = [];
    }
    private sealed class Proxy
    {
        public Proxy() { }
        public string Name { get; set; } = "";
        public string InstalledHash { get; set; } = "";
        public string? OriginalHash { get; set; }
        public string? OriginalPath { get; set; }
    }
    private static string? Hash(string path) => File.Exists(path) ? Files.Hash(path) : null;
    private static void ValidateOriginal(string root, string metadata, Proxy proxy)
    {
        if (proxy.OriginalHash is null && proxy.OriginalPath is null) return;
        if (proxy.OriginalHash is null || proxy.OriginalPath is null ||
            proxy.OriginalPath != proxy.Name + ".bak" ||
            Hash(Deployment.Target(root, proxy.OriginalPath, metadata)) != proxy.OriginalHash)
            throw new IOException($"{proxy.Name} 的原始备份缺失或已改变，无法自动还原，请检查 {proxy.Name}.bak 及桌面管理器内的备份记录。");
    }
    private static Receipt Read(string root, string metadata)
    {
        var path = Deployment.Target(root, ReceiptPath, metadata);
        var result = new Receipt();
        if (File.Exists(path))
        {
            result = JsonSerializer.Deserialize<Receipt>(Files.ReadUtf8(path), Product.Json) ?? throw new InvalidDataException("代理安装记录为空。");
            if (result.SchemaVersion != 1 || result.Proxies is null || result.Proxies.Any(p => p is null || !Names.Contains(p.Name) || string.IsNullOrEmpty(p.InstalledHash)) || result.Proxies.Select(p => p.Name).Distinct().Count() != result.Proxies.Count)
                throw new InvalidDataException($"代理安装记录无效，请检查 {path}。");
        }

        // Recognize manually copied bundled proxies and any adjacent .bak.
        // Never treat our own proxy as an original, or identify foreign DLLs by name.
        foreach (var name in Names.Where(n => result.Proxies.All(p => p.Name != n)))
        {
            var current = Hash(Files.Under(root, name));
            if (current is null) continue;
            using var payload = Assembly.GetExecutingAssembly().GetManifestResourceStream("Payload." + name) ?? throw new IOException("安装包缺少代理。");
            if (current == Convert.ToHexString(SHA256.HashData(payload)).ToLowerInvariant())
            {
                var original = Hash(Files.Under(root, name + ".bak"));
                result.Proxies.Add(new() { Name = name, InstalledHash = current, OriginalHash = original, OriginalPath = original is null ? null : name + ".bak" });
            }
        }
        return result;
    }
    public static DeploymentResult Install(string root, IReadOnlyList<FileChange> payload, IProgress<ProgressInfo>? progress = null)
    {
        var metadata = GameStorage.Prepare(root);
        var receipt = Read(root, metadata);
        var receiptHash = Hash(Deployment.Target(root, ReceiptPath, metadata));
        var changes = new List<FileChange>();
        foreach (var change in payload)
        {
            if (!Names.Contains(change.RelativePath)) { changes.Add(change); continue; }
            var target = Files.Under(root, change.RelativePath);
            var current = Hash(target);
            var proxy = receipt.Proxies.SingleOrDefault(p => p.Name == change.RelativePath);
            if (proxy is null || current != proxy.InstalledHash)
            {
                if (proxy is not null) receipt.Proxies.Remove(proxy);
                proxy = new() { Name = change.RelativePath, OriginalHash = current };
                if (current is not null)
                {
                    proxy.OriginalPath = proxy.Name + ".bak";
                    var saved = Hash(Files.Under(root, proxy.OriginalPath));
                    if (saved is not null && saved != current) throw new IOException($"{proxy.OriginalPath} 已存在且与当前原始文件不同，已保留备份并取消覆盖。");
                    if (saved is null) changes.Add(new(proxy.OriginalPath, target, new(null)));
                }
                receipt.Proxies.Add(proxy);
            }
            else ValidateOriginal(root, metadata, proxy);
            proxy.InstalledHash = Files.Hash(change.Source!);
            changes.Add(change with { Expected = new(current) });
        }
        return Apply(root, metadata, receipt, receiptHash, changes, progress);
    }
    public static DeploymentResult Restore(string root, IProgress<ProgressInfo>? progress = null)
    {
        var metadata = GameStorage.Prepare(root);
        var receipt = Read(root, metadata);
        if (receipt.Proxies.Count == 0) return new("", 0);
        var receiptHash = Hash(Deployment.Target(root, ReceiptPath, metadata));
        var changes = new List<FileChange>();
        foreach (var proxy in receipt.Proxies)
        {
            var current = Hash(Files.Under(root, proxy.Name));
            if (current is not null && current != proxy.InstalledHash && current != proxy.OriginalHash)
                throw new IOException($"{proxy.Name} 已被其他程序修改，已保留现有文件。请先核对它与原始 .bak 备份。");
            ValidateOriginal(root, metadata, proxy);
            if (current != proxy.OriginalHash)
                changes.Add(new(proxy.Name, proxy.OriginalPath is null ? null : Deployment.Target(root, proxy.OriginalPath, metadata), new(current)));
            if (proxy.OriginalPath is not null) changes.Add(new(proxy.OriginalPath, null, new(proxy.OriginalHash)));
        }
        return Apply(root, metadata, new(), receiptHash, changes, progress);
    }
    private static DeploymentResult Apply(string root, string metadata, Receipt receipt, string? receiptHash, List<FileChange> changes, IProgress<ProgressInfo>? progress)
    {
        var work = Product.NewWorkDirectory();
        try
        {
            var source = Path.Combine(work, "proxies.json");
            File.WriteAllText(source, JsonSerializer.Serialize(receipt, Product.Json));
            changes.Add(new(ReceiptPath, source, new(receiptHash)));
            return Deployment.Apply(root, changes, progress, () => GameDirectory.RequireStopped(root), metadataDirectory: metadata);
        }
        finally { Files.RemoveWork(work); }
    }
}

public static class PluginDirectory
{
    public static bool Delete(string root)
    {
        var metadata = GameStorage.Prepare(root);
        var files = new List<string>(); var directories = new List<string>();
        // Validate the entire tree before deleting anything, including its receipts.
        Scan(Files.Under(root, "plugin")); Scan(Files.Under(metadata, "plugins"));
        if (directories.Count == 0) return false;
        GameDirectory.RequireStopped(root);
        try
        {
            foreach (var file in files) { Files.RejectLinks(file); File.Delete(file); }
            foreach (var directory in directories) { Files.RejectLinks(directory); Directory.Delete(directory, false); }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { throw new IOException("部分文件未能删除，请关闭占用这些文件的程序后重试。尚未删除的内容已保留。\n" + error.Message, error); }
        return true;

        void Scan(string folder)
        {
            Files.RejectLinks(folder);
            if (!Directory.Exists(folder)) return;
            foreach (var entry in Directory.EnumerateFileSystemEntries(folder))
            {
                var path = Files.Under(folder, Path.GetFileName(entry));
                if (Directory.Exists(path)) Scan(path);
                else files.Add(path);
            }
            directories.Add(folder);
        }
    }
}
