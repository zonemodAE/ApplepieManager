using System.Reflection;
using System.Text.Json;

namespace Applepie.Launcher;

public sealed record FileSnapshot(string? Sha256);
public sealed record FileChange(string RelativePath, string? Source, FileSnapshot? Expected = null);

public static class Deployment
{
    public sealed class Journal
    {
        public string Root { get; set; } = "";
        public string State { get; set; } = "Prepared";
        public string Purpose { get; set; } = "";
        public bool ExternalMetadata { get; set; }
        public List<Entry> Entries { get; set; } = [];
    }
    public sealed class Entry
    {
        public string Path { get; set; } = "";
        public string? Before { get; set; }
        public string? After { get; set; }
        public bool Started { get; set; }
    }
    // Game operations map the logical .applepie/ paths to the desktop's game
    // records directory. Payloads and records still share one rollback journal.
    internal static string Target(string root, string relative, string? metadataDirectory) =>
        metadataDirectory is not null && relative.Replace('\\', '/').StartsWith(".applepie/", StringComparison.OrdinalIgnoreCase)
            ? Files.Under(metadataDirectory, relative[10..]) : Files.Under(root, relative);
    public static DeploymentResult Apply(string root, IReadOnlyList<FileChange> changes, IProgress<ProgressInfo>? progress = null, Action? guard = null, Action? verify = null, string purpose = "", string? metadataDirectory = null)
    {
        root = Path.GetFullPath(root);
        Files.RejectLinks(root); guard?.Invoke();
        Recover(root, guard, metadataDirectory);
        var distinct = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var journal = new Journal { Root = root, Purpose = purpose, ExternalMetadata = metadataDirectory is not null };
        var sources = new Dictionary<string, string?>(StringComparer.OrdinalIgnoreCase);
        foreach (var change in changes)
        {
            var target = Target(root, change.RelativePath, metadataDirectory);
            if (!distinct.Add(target)) throw new InvalidDataException($"安装计划有重复目标：{change.RelativePath}");
            if (Directory.Exists(target)) throw new IOException($"目标文件位置已被文件夹占用：{target}");
            var before = File.Exists(target) ? Files.Hash(target) : null;
            if (change.Expected is { } expected && before != expected.Sha256) throw new IOException($"目标文件已被其他程序修改：{change.RelativePath}");
            var after = change.Source is not null ? Files.Hash(change.Source) : null;
            if (before == after) continue;
            journal.Entries.Add(new() { Path = change.RelativePath, Before = before, After = after });
            sources[change.RelativePath] = change.Source;
        }
        if (journal.Entries.Count == 0) { verify?.Invoke(); return new("", 0); }
        var backup = Target(root, $".applepie/backups/{DateTime.UtcNow:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}", metadataDirectory);
        Directory.CreateDirectory(backup);
        var journalPath = Path.Combine(backup, "transaction.json");
        void Save() => Files.WriteAtomic(journalPath, JsonSerializer.SerializeToUtf8Bytes(journal, Product.Json));
        // Complete every backup before replacing the first destination.
        foreach (var entry in journal.Entries)
        {
            if (entry.Before is null) continue;
            var original = Target(root, entry.Path, metadataDirectory);
            var copy = Files.Under(backup, "original/" + entry.Path);
            Directory.CreateDirectory(Path.GetDirectoryName(copy)!); File.Copy(original, copy, false);
            if (Files.Hash(copy) != entry.Before) throw new IOException($"备份时文件发生变化：{entry.Path}");
        }
        journal.State = "Applying"; Save();
        try
        {
            for (var i = 0; i < journal.Entries.Count; ++i)
            {
                guard?.Invoke();
                var entry = journal.Entries[i]; var target = Target(root, entry.Path, metadataDirectory);
                var current = File.Exists(target) ? Files.Hash(target) : null;
                if (current != entry.Before) throw new IOException($"目标文件已被其他程序修改：{entry.Path}");
                entry.Started = true; Save();
                progress?.Report(new($"正在写入 {entry.Path}", 100.0 * i / journal.Entries.Count));
                if (sources[entry.Path] is { } source)
                {
                    if (Files.Hash(source) != entry.After) throw new IOException("安装源文件在校验后发生变化。");
                    Files.CopyAtomic(source, target);
                }
                else File.Delete(target);
            }
            verify?.Invoke();
            journal.State = "Committed"; Save();
            return new(backup, journal.Entries.Count);
        }
        catch (Exception failure)
        {
            try { guard?.Invoke(); Rollback(journal, backup, metadataDirectory); }
            catch (Exception rollback) { journal.State = "RollbackFailed"; Save(); throw new IOException($"操作未完成，恢复也暂时受阻。备份保存在 {backup}。\n{rollback.Message}", failure); }
            journal.State = "RolledBack"; Save();
            throw new IOException($"操作未完成，已恢复原文件。{failure.Message}", failure);
        }
    }
    public static void Recover(string root, Action? guard = null, string? metadataDirectory = null)
    {
        var directory = Target(root, ".applepie/backups", metadataDirectory);
        if (!Directory.Exists(directory)) return;
        foreach (var backup in Directory.EnumerateDirectories(directory))
        {
            Files.RejectLinks(backup);
            var path = Path.Combine(backup, "transaction.json");
            if (!File.Exists(path)) continue;
            var journal = JsonSerializer.Deserialize<Journal>(File.ReadAllText(path), Product.Json) ?? throw new InvalidDataException("无法读取安装恢复记录。");
            if (journal.State is not ("Applying" or "RollbackFailed")) continue;
            if (journal.ExternalMetadata != (metadataDirectory is not null)) throw new IOException("安装恢复记录的存储位置不一致，已保留备份。");
            if (!string.Equals(Path.TrimEndingDirectorySeparator(Path.GetFullPath(journal.Root)), Path.TrimEndingDirectorySeparator(Path.GetFullPath(root)), StringComparison.OrdinalIgnoreCase)) throw new IOException("安装恢复记录与目录不一致。");
            guard?.Invoke(); Rollback(journal, backup, metadataDirectory); journal.State = "RolledBack";
            Files.WriteAtomic(path, JsonSerializer.SerializeToUtf8Bytes(journal, Product.Json));
        }
    }
    private static void Rollback(Journal journal, string backup, string? metadataDirectory)
    {
        foreach (var entry in journal.Entries.AsEnumerable().Reverse().Where(e => e.Started))
        {
            var target = Target(journal.Root, entry.Path, metadataDirectory);
            var current = File.Exists(target) ? Files.Hash(target) : null;
            if (current == entry.Before) continue;
            if (current != entry.After) throw new IOException($"{entry.Path} 后来被修改，已保留该文件和备份，请手动核对。");
            if (entry.Before is null) File.Delete(target);
            else
            {
                var source = Files.Under(backup, "original/" + entry.Path);
                if (Files.Hash(source) != entry.Before) throw new IOException($"备份校验失败：{entry.Path}");
                Files.CopyAtomic(source, target);
            }
        }
    }
}

public static class ManagerInstaller
{
    public static readonly string[] PayloadNames = ["applepie_manager.dll", "d3dcompiler_47.dll", "vulkan-1.dll"];
    public static string? InstalledVersion(string root)
    {
        var path = Files.Under(root, "plugin/applepie_manager.dll");
        return File.Exists(path) ? System.Diagnostics.FileVersionInfo.GetVersionInfo(path).ProductVersion ?? "未知版本" : null;
    }
    public static DeploymentResult Install(string root, IProgress<ProgressInfo>? progress = null)
    {
        GameDirectory.RequireStopped(root);
        var work = Product.NewWorkDirectory();
        try
        {
            var changes = new List<FileChange>();
            foreach (var name in PayloadNames)
            {
                using var resource = Assembly.GetExecutingAssembly().GetManifestResourceStream("Payload." + name) ?? throw new IOException($"安装包缺少 {name}");
                var source = Path.Combine(work, name);
                using (var output = File.Create(source)) resource.CopyTo(output);
                changes.Add(new(name == "applepie_manager.dll" ? "plugin/" + name : name, source));
            }
            return ProxyBackups.Install(root, changes, progress);
        }
        finally { Files.RemoveWork(work); }
    }
    public static DeploymentResult Install(string root, CorePackage package, IProgress<ProgressInfo>? progress = null)
    {
        if (package.Directory is null) return Install(root, progress);
        GameDirectory.RequireStopped(root);
        var validated = CorePackages.Validate(package.Directory);
        if (validated.Version != package.Version) throw new InvalidDataException("核心缓存版本发生变化，请重新检查更新。");
        var changes = PayloadNames.Select(name => new FileChange(name == "applepie_manager.dll" ? "plugin/" + name : name, Files.Under(package.Directory, name))).ToArray();
        return ProxyBackups.Install(root, changes, progress);
    }
}
