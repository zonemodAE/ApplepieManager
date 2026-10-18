using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.Json;
using Microsoft.Win32;

namespace Applepie.Launcher;

public sealed class AppPackage
{
    public const string ReceiptPath = ".applepie/desktop-package.json";
    public string Product { get; set; } = "";
    public string Version { get; set; } = "";
    public string? CoreVersion { get; set; }
    public List<OwnedFile> Files { get; set; } = [];
    public static AppPackage Validate(string folder)
    {
        global::Applepie.Launcher.Files.RejectLinks(folder);
        var legacy = global::Applepie.Launcher.Files.Under(folder, "package.json");
        var receipt = global::Applepie.Launcher.Files.Under(folder, ReceiptPath);
        var manifestPath = File.Exists(legacy) ? legacy : File.Exists(receipt) ? receipt : null;
        AppPackage manifest;
        if (manifestPath is not null)
            manifest = JsonSerializer.Deserialize<AppPackage>(File.ReadAllText(manifestPath), Launcher.Product.Json)
                ?? throw new InvalidDataException("无法读取桌面管理器安装记录。");
        else
        {
            var executable = global::Applepie.Launcher.Files.Under(folder, "ApplepieManager.exe");
            var info = FileVersionInfo.GetVersionInfo(executable);
            if (info.ProductName != "Applepie Manager" || info.CompanyName != "ApplepieManager" || info.OriginalFilename != "ApplepieManager.dll")
                throw new InvalidDataException("不是 Applepie Manager 桌面程序。");
            var version = info.ProductVersion?.Split('+')[0] ?? throw new InvalidDataException("桌面程序缺少版本号。");
            manifest = new() { Product = "ApplepieManager", Version = version, CoreVersion = version,
                Files = [new("ApplepieManager.exe", global::Applepie.Launcher.Files.Hash(executable))] };
        }
        Verify(folder, manifest);
        return manifest;
    }
    public static void Verify(string folder, AppPackage manifest)
    {
        if (manifest.Product != "ApplepieManager") throw new InvalidDataException("不是 Applepie Manager 安装包。");
        ReleaseVersion.Parse(manifest.Version);
        var unique = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var file in manifest.Files)
        {
            // Application packages own a flat, explicit set of files. Game files
            // and per-user settings are never targets of a launcher self-update.
            if (Path.GetFileName(file.Path) != file.Path || file.Path.Equals("package.json", StringComparison.OrdinalIgnoreCase) || file.Path.Equals("launcher-install.json", StringComparison.OrdinalIgnoreCase) || !unique.Add(file.Path))
                throw new InvalidDataException("桌面管理器包包含无效或重复文件。");
            if (!global::Applepie.Launcher.Files.Hash(global::Applepie.Launcher.Files.Under(folder, file.Path)).Equals(file.Sha256, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException($"安装包文件校验失败：{file.Path}");
        }
        if (!unique.Contains("ApplepieManager.exe")) throw new InvalidDataException("安装包缺少 ApplepieManager.exe。");
        var version = FileVersionInfo.GetVersionInfo(Path.Combine(folder, "ApplepieManager.exe")).ProductVersion?.Split('+')[0];
        if (version is null || ReleaseVersion.Parse(version).CompareTo(ReleaseVersion.Parse(manifest.Version)) != 0) throw new InvalidDataException("安装包清单与程序版本不一致。");
    }
}

public sealed class UpdatePlan
{
    public int ParentProcess { get; set; }
    public long ParentStartUtc { get; set; }
    public string Destination { get; set; } = "";
    public string Source { get; set; } = "";
    public string Version { get; set; } = "";
    public bool Uninstall { get; set; }
}

public static class SelfUpdate
{
    public static async Task<string> Prepare(OnlineService online, DesktopUpdate update, IProgress<ProgressInfo>? progress, CancellationToken cancellation)
    {
        new ReleaseManifest { SchemaVersion = 1, Product = "ApplepieManager", Desktop = update.Component }.Validate(update.Release);
        var work = Product.NewWorkDirectory();
        try
        {
            var published = update.Component.Portable;
            var asset = published.Resolve(update.Release);
            var source = Path.Combine(work, "package");
            if (published.Asset.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
            {
                Directory.CreateDirectory(source);
                await online.Download(asset.Url, Path.Combine(source, "ApplepieManager.exe"), published.Sha256, progress, cancellation, published.Size);
            }
            else
            {
                // Older releases used ZIP archives; keep them readable.
                var zip = Path.Combine(work, "portable.zip");
                await online.Download(asset.Url, zip, published.Sha256, progress, cancellation, published.Size);
                await Task.Run(() => Files.ExtractZip(zip, source, cancellation), cancellation);
            }
            var manifest = await Task.Run(() => AppPackage.Validate(source), cancellation);
            if (ReleaseVersion.Parse(manifest.Version).CompareTo(ReleaseVersion.Parse(update.Component.Version)) != 0 || !ReleaseVersion.Newer(manifest.Version, Product.Version))
                throw new InvalidDataException("更新包版本与 GitHub Release 不一致，或没有高于当前版本。");
            return source;
        }
        catch { Files.RemoveWork(work); throw; }
    }
    public static void Start(string source, bool uninstall = false)
    {
        var work = Product.NewWorkDirectory();
        try
        {
            var helper = Path.Combine(work, "ApplepieManager.Update.exe");
            var current = Environment.ProcessPath ?? throw new IOException("无法定位管理器程序。");
            if (!Path.GetFileName(current).Equals("ApplepieManager.exe", StringComparison.OrdinalIgnoreCase)) throw new IOException("请从发布版 ApplepieManager.exe 执行更新或卸载。");
            File.Copy(current, helper);
            using var process = Process.GetCurrentProcess();
            var plan = new UpdatePlan { ParentProcess = process.Id, ParentStartUtc = process.StartTime.ToUniversalTime().Ticks,
                Destination = Path.GetDirectoryName(current)!, Source = source, Version = uninstall ? Product.Version : AppPackage.Validate(source).Version, Uninstall = uninstall };
            var planPath = Path.Combine(work, "update.json"); Files.WriteAtomic(planPath, JsonSerializer.SerializeToUtf8Bytes(plan, Product.Json));
            var start = new ProcessStartInfo(helper) { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = work };
            start.ArgumentList.Add("--apply-update"); start.ArgumentList.Add(planPath);
            if (Process.Start(start) is null) throw new IOException("无法启动更新程序。");
        }
        catch { Files.RemoveWork(work); throw; }
    }
    public static async Task Apply(string planPath)
    {
        var plan = JsonSerializer.Deserialize<UpdatePlan>(File.ReadAllText(planPath), Product.Json) ?? throw new InvalidDataException("更新计划无效。");
        Files.RejectLinks(plan.Destination);
        var executable = Files.Under(plan.Destination, "ApplepieManager.exe");
        if (!File.Exists(executable)) throw new IOException("原管理器程序不存在，已取消更新。");
        try
        {
            using var parent = Process.GetProcessById(plan.ParentProcess);
            if (parent.StartTime.ToUniversalTime().Ticks == plan.ParentStartUtc)
            {
                using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(60));
                await parent.WaitForExitAsync(timeout.Token);
            }
        }
        catch (ArgumentException) { } // Parent has already exited.
        using (AppInstance.Acquire())
        {
            if (plan.Uninstall) { DesktopInstaller.Remove(plan.Destination); return; }
            var package = AppPackage.Validate(plan.Source);
            if (package.Version != plan.Version) throw new InvalidDataException("更新计划与安装包版本不一致。");
            DesktopInstaller.InstallFiles(plan.Source, plan.Destination, cleanup: false);
            DesktopInstaller.RefreshRegistration(plan.Destination, package);
        }
        var start = new ProcessStartInfo(executable) { UseShellExecute = true, WorkingDirectory = plan.Destination };
        start.ArgumentList.Add("--updated");
        using var restarted = Process.Start(start) ?? throw new IOException("文件更新完成，但无法重新打开管理器；回滚备份已保留。");
        DesktopInstaller.CleanupBackups(plan.Destination);
        Files.RemoveWork(Path.GetDirectoryName(plan.Source)!);
    }
}

public sealed record DesktopInstallation(string Directory, string Version, bool DesktopShortcut);

internal sealed class DesktopInstallRecord
{
    public string Version { get; set; } = "";
    public bool DesktopShortcut { get; set; } = true;
}

public static class DesktopInstaller
{
    private const string UninstallKey = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\ApplepieManager";
    private const string BackupPurpose = "desktop";
    private static readonly HashSet<string> DesktopFiles = new(StringComparer.OrdinalIgnoreCase)
        { "ApplepieManager.exe", "LICENSE", "THIRD_PARTY_NOTICES", "package.json", "launcher-install.json", AppPackage.ReceiptPath };
    public static string DefaultDirectory => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs", "ApplepieManager");
    public static DesktopInstallation? FindInstallation()
    {
        string? registered = null;
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(UninstallKey);
            registered = key?.GetValue("InstallLocation") as string;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.Security.SecurityException) { }
        return FindInstallation([registered, DefaultDirectory]);
    }
    public static DesktopInstallation? FindInstallation(IEnumerable<string?> candidates)
    {
        foreach (var candidate in candidates)
        {
            if (string.IsNullOrWhiteSpace(candidate) || !Path.IsPathFullyQualified(candidate)) continue;
            try
            {
                var directory = Path.GetFullPath(candidate);
                Files.RejectLinks(directory);
                if (!File.Exists(Path.Combine(directory, "ApplepieManager.exe")) || File.Exists(Path.Combine(directory, "Endfield.exe"))) continue;
                var record = JsonSerializer.Deserialize<DesktopInstallRecord>(File.ReadAllText(Path.Combine(directory, "launcher-install.json")), Product.Json);
                if (record is null) continue;
                var package = AppPackage.Validate(directory);
                return new(directory, package.Version, record.DesktopShortcut);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or ArgumentException or NotSupportedException) { }
        }
        return null;
    }
    public static string ExtractBundle()
    {
        var work = Product.NewWorkDirectory();
        try
        {
            using var source = Assembly.GetExecutingAssembly().GetManifestResourceStream("Payload.ApplepieManager.exe") ?? throw new IOException("安装器未包含桌面管理器程序。");
            var folder = Path.Combine(work, "package"); Directory.CreateDirectory(folder);
            using (var file = File.Create(Path.Combine(folder, "ApplepieManager.exe"))) source.CopyTo(file);
            AppPackage.Validate(folder); return folder;
        }
        catch { Files.RemoveWork(work); throw; }
    }
    public static DeploymentResult InstallFiles(string source, string destination, IProgress<ProgressInfo>? progress = null, bool cleanup = true)
    {
        var package = AppPackage.Validate(source); Files.RejectLinks(destination);
        if (File.Exists(Path.Combine(destination, "Endfield.exe"))) throw new IOException("这里选择的是桌面管理器的安装位置，请勿直接安装到游戏根目录。");
        Deployment.Recover(destination);
        var changes = package.Files.Select(f => new FileChange(f.Path, Files.Under(source, f.Path))).ToList();
        var priorPath = Files.Under(destination, "package.json");
        if (File.Exists(priorPath) || File.Exists(Files.Under(destination, AppPackage.ReceiptPath)))
        {
            var prior = AppPackage.Validate(destination);
            foreach (var file in prior.Files.Where(f => !package.Files.Any(n => n.Path.Equals(f.Path, StringComparison.OrdinalIgnoreCase))))
                changes.Add(new(file.Path, null));
        }
        if (File.Exists(priorPath)) changes.Add(new("package.json", null));
        var work = Product.NewWorkDirectory();
        try
        {
            var receipt = Path.Combine(work, "desktop-package.json");
            Files.WriteAtomic(receipt, JsonSerializer.SerializeToUtf8Bytes(package, Product.Json));
            changes.Add(new(AppPackage.ReceiptPath, receipt));
            var result = Deployment.Apply(destination, changes, progress,
                verify: () => { AppPackage.Verify(destination, package); _ = AppPackage.Validate(destination); }, purpose: BackupPurpose);
            if (cleanup) CleanupBackups(destination, progress);
            return result;
        }
        finally { Files.RemoveWork(work); }
    }
    public static DeploymentResult Install(string source, string destination, bool desktop, IProgress<ProgressInfo>? progress = null)
    {
        using var instance = AppInstance.Acquire();
        var package = AppPackage.Validate(source);
        var result = InstallFiles(source, destination, progress, cleanup: false);
        Files.WriteAtomic(Files.Under(destination, "launcher-install.json"), JsonSerializer.SerializeToUtf8Bytes(new DesktopInstallRecord { Version = package.Version, DesktopShortcut = desktop }, Product.Json));
        Register(destination, package.Version);
        Shortcut(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Programs), "Applepie Manager.lnk"), destination);
        if (desktop) Shortcut(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "Applepie Manager.lnk"), destination);
        CleanupBackups(destination, progress);
        return result;
    }
    public static int CleanupBackups(string destination, IProgress<ProgressInfo>? progress = null)
    {
        destination = Path.GetFullPath(destination).TrimEnd(Path.DirectorySeparatorChar);
        Files.RejectLinks(destination);
        if (File.Exists(Path.Combine(destination, "Endfield.exe"))) throw new IOException("桌面备份清理不能用于游戏目录。");
        var backups = Files.Under(destination, ".applepie/backups");
        if (!Directory.Exists(backups)) return 0;
        var removed = 0;
        foreach (var folder in Directory.EnumerateDirectories(backups))
        {
            try
            {
                var backup = Files.Under(backups, Path.GetFileName(folder));
                var journalPath = Files.Under(backup, "transaction.json");
                if (!File.Exists(journalPath)) continue;
                var journal = JsonSerializer.Deserialize<Deployment.Journal>(File.ReadAllText(journalPath), Product.Json);
                if (journal is null || journal.State != "Committed" || journal.Entries.Count == 0 ||
                    !Path.GetFullPath(journal.Root).TrimEnd(Path.DirectorySeparatorChar).Equals(destination, StringComparison.OrdinalIgnoreCase) ||
                    journal.Entries.Any(e => !DesktopFiles.Contains(e.Path.Replace('\\', '/'))) ||
                    !(journal.Purpose == BackupPurpose || (journal.Purpose == "" && journal.Entries.Any(e => e.Path.Equals("ApplepieManager.exe", StringComparison.OrdinalIgnoreCase))))) continue;
                // Delete only files owned by this successful desktop transaction.
                // Failed transactions, unknown content and every game backup stay.
                var expected = journal.Entries.Where(e => e.Before is not null).Select(e => "original/" + e.Path.Replace('\\', '/'))
                    .Append("transaction.json").ToHashSet(StringComparer.OrdinalIgnoreCase);
                var pending = new Stack<string>(); pending.Push(backup);
                var known = true;
                while (known && pending.TryPop(out var directory))
                    foreach (var entry in Directory.EnumerateFileSystemEntries(directory))
                    {
                        var relative = Path.GetRelativePath(backup, entry).Replace('\\', '/');
                        var checkedPath = Files.Under(backup, relative);
                        if (Directory.Exists(checkedPath))
                        {
                            if (!expected.Any(e => e.StartsWith(relative + "/", StringComparison.OrdinalIgnoreCase))) { known = false; break; }
                            pending.Push(checkedPath);
                        }
                        else if (!expected.Contains(relative)) { known = false; break; }
                    }
                if (!known) continue;
                Directory.Delete(backup, true); ++removed;
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or ArgumentException)
            {
                progress?.Report(new("桌面文件已更新，旧备份暂时无法清理：" + error.Message));
            }
        }
        return removed;
    }
    public static void RefreshRegistration(string destination, AppPackage package)
    {
        var marker = Path.Combine(destination, "launcher-install.json");
        if (!File.Exists(marker)) return;
        var record = JsonSerializer.Deserialize<DesktopInstallRecord>(File.ReadAllText(marker), Product.Json) ?? throw new InvalidDataException("安装记录无效。");
        record.Version = package.Version;
        Files.WriteAtomic(marker, JsonSerializer.SerializeToUtf8Bytes(record, Product.Json));
        Register(destination, package.Version);
    }
    private static void Register(string folder, string version)
    {
        using var key = Registry.CurrentUser.CreateSubKey(UninstallKey);
        var exe = Path.Combine(folder, "ApplepieManager.exe");
        key.SetValue("DisplayName", "Applepie Manager"); key.SetValue("Publisher", "Sasye / ApplepieManager");
        key.SetValue("DisplayVersion", version); key.SetValue("InstallLocation", folder); key.SetValue("DisplayIcon", exe);
        key.SetValue("UninstallString", $"\"{exe}\" --uninstall"); key.SetValue("NoModify", 1); key.SetValue("NoRepair", 1);
    }
    private static void Shortcut(string path, string folder)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var type = Type.GetTypeFromProgID("WScript.Shell") ?? throw new IOException("无法创建 Windows 快捷方式。");
        dynamic shell = Activator.CreateInstance(type)!;
        try
        {
            dynamic shortcut = shell.CreateShortcut(path);
            try { shortcut.TargetPath = Path.Combine(folder, "ApplepieManager.exe"); shortcut.WorkingDirectory = folder; shortcut.Description = "Applepie Manager"; shortcut.Save(); }
            finally { Marshal.FinalReleaseComObject(shortcut); }
        }
        finally { Marshal.FinalReleaseComObject(shell); }
    }
    public static void Remove(string directory)
    {
        var package = AppPackage.Validate(directory);
        if (!File.Exists(Path.Combine(directory, "launcher-install.json"))) throw new IOException("这是免安装版本，直接删除程序文件即可。游戏内已安装的 Manager 和插件不会被移除。");
        var files = package.Files.Select(f => new FileChange(f.Path, null)).ToList();
        files.Add(new("package.json", null)); files.Add(new(AppPackage.ReceiptPath, null)); files.Add(new("launcher-install.json", null));
        Deployment.Apply(directory, files, verify: () =>
        {
            if (files.Any(f => File.Exists(Files.Under(directory, f.RelativePath)))) throw new IOException("桌面管理器卸载校验失败。");
        }, purpose: BackupPurpose);
        using (var key = Registry.CurrentUser.OpenSubKey(UninstallKey))
            if (!string.Equals(key?.GetValue("InstallLocation") as string, directory, StringComparison.OrdinalIgnoreCase)) return;
        Registry.CurrentUser.DeleteSubKeyTree(UninstallKey, false);
        foreach (var folder in new[] { Environment.GetFolderPath(Environment.SpecialFolder.Programs), Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory) })
        {
            var path = Path.Combine(folder, "Applepie Manager.lnk");
            if (File.Exists(path)) File.Delete(path);
        }
        CleanupBackups(directory);
    }
}
