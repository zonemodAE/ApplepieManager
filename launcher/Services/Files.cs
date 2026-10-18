using System.Diagnostics;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;

namespace Applepie.Launcher;

public static class Files
{
    public static string Hash(string path) { using var file = File.OpenRead(path); return Convert.ToHexString(SHA256.HashData(file)).ToLowerInvariant(); }
    public static string Under(string root, string relative)
    {
        if (string.IsNullOrWhiteSpace(relative) || Path.IsPathRooted(relative) || relative.Contains(':')) throw new InvalidDataException($"无效的包内路径：{relative}");
        var parts = relative.Replace('\\', '/').Split('/');
        if (parts.Any(p => p is "" or "." or ".." || p.EndsWith(' ') || p.EndsWith('.') || p.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0))
            throw new InvalidDataException($"无效的包内路径：{relative}");
        var fullRoot = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        var path = Path.GetFullPath(Path.Combine(fullRoot, Path.Combine(parts)));
        if (!path.StartsWith(fullRoot, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("包内路径越出安装目录。");
        RejectLinks(fullRoot);
        var parent = path;
        while (parent.Length >= fullRoot.Length) { RejectLink(parent); parent = Path.GetDirectoryName(parent)!; }
        return path;
    }
    private static void RejectLink(string path)
    {
        if ((File.Exists(path) || Directory.Exists(path)) && (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
            throw new IOException($"安装目标包含链接或重解析点，请选择实际目录：{path}");
    }
    public static void RejectLinks(string path)
    {
        for (var part = Path.GetFullPath(path); part is not null; part = Path.GetDirectoryName(part)) RejectLink(part);
    }
    public static void WriteAtomic(string path, byte[] bytes)
    {
        RejectLinks(Path.GetDirectoryName(path)!); RejectLink(path);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temporary = path + ".tmp-" + Guid.NewGuid().ToString("N");
        try
        {
            using (var file = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None)) { file.Write(bytes); file.Flush(true); }
            File.Move(temporary, path, true);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
    public static void CopyAtomic(string source, string destination)
    {
        RejectLinks(Path.GetDirectoryName(destination)!); RejectLink(destination);
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        var temporary = destination + ".tmp-" + Guid.NewGuid().ToString("N");
        try { File.Copy(source, temporary, false); File.Move(temporary, destination, true); }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
    public static void ExtractZip(string archivePath, string destination, CancellationToken cancellation = default)
    {
        using var archive = ZipFile.OpenRead(archivePath);
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var entry in archive.Entries)
        {
            cancellation.ThrowIfCancellationRequested();
            var name = entry.FullName.Replace('\\', '/').TrimEnd('/');
            if (name.Length == 0) continue;
            var path = Under(destination, name);
            if (!seen.Add(path)) throw new InvalidDataException($"压缩包中有重复路径：{name}");
            if (((entry.ExternalAttributes >> 16) & 0xF000) == 0xA000) throw new InvalidDataException("不接受包含符号链接的压缩包。");
            if (entry.Name.Length == 0) { Directory.CreateDirectory(path); continue; }
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            using var input = entry.Open(); using var output = new FileStream(path, FileMode.CreateNew);
            input.CopyTo(output);
        }
    }
    public static void RemoveWork(string directory)
    {
        var work = Path.GetFullPath(Product.WorkDirectory) + Path.DirectorySeparatorChar;
        var path = Path.GetFullPath(directory);
        if (!path.StartsWith(work, StringComparison.OrdinalIgnoreCase) || path.Length <= work.Length) return;
        try { RejectLinks(path); if (Directory.Exists(path)) Directory.Delete(path, true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
    }
    public static string SafeId(string id) => System.Text.RegularExpressions.Regex.IsMatch(id, "^[a-z0-9._-]+$") && id is not "." and not ".."
        ? id : throw new InvalidDataException($"无效插件 ID：{id}");
    public static string ReadUtf8(string path) => new UTF8Encoding(false, true).GetString(File.ReadAllBytes(path)).TrimStart('\uFEFF');
}

public static class GameDirectory
{
    public const string NotFound = "请选择Endfield.exe所在文件夹";
    public static IReadOnlyList<string> Find(string selected, CancellationToken cancellation = default)
    {
        if (string.IsNullOrWhiteSpace(selected)) return [];
        var chosen = Path.GetFullPath(selected.Trim().Trim('"'));
        if (File.Exists(chosen)) chosen = Path.GetDirectoryName(chosen)!;
        if (!Directory.Exists(chosen)) return [];
        // Prefer the selected directory, then an enclosing game. Only if absent
        // search nearby descendants; never choose arbitrarily between copies.
        if (File.Exists(Path.Combine(chosen, "Endfield.exe"))) return [chosen];
        var parent = Directory.GetParent(chosen);
        for (var ancestor = parent; ancestor is not null; ancestor = ancestor.Parent)
            if (File.Exists(Path.Combine(ancestor.FullName, "Endfield.exe"))) return [ancestor.FullName];
        var found = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        Search(chosen, 4);
        if (found.Count == 0 && parent?.Parent is not null) Search(parent.FullName, 3);
        return found.Order(StringComparer.OrdinalIgnoreCase).ToList();
        void Search(string root, int depth)
        {
            var queue = new Queue<(string Path, int Depth)>(); queue.Enqueue((root, 0));
            while (queue.TryDequeue(out var item))
            {
                cancellation.ThrowIfCancellationRequested();
                if (File.Exists(Path.Combine(item.Path, "Endfield.exe"))) { found.Add(item.Path); continue; }
                if (item.Depth >= depth) continue;
                try
                {
                    foreach (var dir in new DirectoryInfo(item.Path).EnumerateDirectories())
                    {
                        if ((dir.Attributes & (FileAttributes.ReparsePoint | FileAttributes.System)) != 0 || dir.Name.StartsWith('.')) continue;
                        queue.Enqueue((dir.FullName, item.Depth + 1));
                    }
                }
                catch (IOException) { } catch (UnauthorizedAccessException) { }
            }
        }
    }
    public static void Validate(string root)
    {
        if (!File.Exists(Path.Combine(root, "Endfield.exe"))) throw new DirectoryNotFoundException(NotFound);
        Files.RejectLinks(root);
    }
    public static void RequireStopped(string root)
    {
        if (IsRunning(root)) throw new IOException("请先退出游戏，再安装、更新、删除插件或修改启用偏好。游戏内的即时启停可在 Applepie 页签操作。");
    }
    public static bool IsRunning(string root)
    {
        Validate(root);
        foreach (var process in Process.GetProcessesByName("Endfield"))
        {
            using (process)
            {
                string? exe;
                try { exe = process.MainModule?.FileName; }
                catch (System.ComponentModel.Win32Exception) { return true; }
                catch (InvalidOperationException) { continue; }
                if (exe is null || string.Equals(Path.GetDirectoryName(exe), Path.GetFullPath(root).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
                    return true;
            }
        }
        return false;
    }
}
