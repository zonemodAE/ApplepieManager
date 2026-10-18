using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Applepie.Launcher;

public static class PluginConfiguration
{
    public const string RelativePath = "plugin/config/applepie.manager.ini";
    public static string Key(string filename) => "plugin." + new string(filename.Select(c => c is >= 'A' and <= 'Z' ? (char)(c + 32) : c).ToArray());
    public static Dictionary<string, string> Read(string game)
    {
        var path = Files.Under(game, RelativePath);
        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        if (File.Exists(path))
        {
            foreach (var raw in Files.ReadUtf8(path).Split('\n'))
            {
                var line = raw.TrimEnd('\r');
                if (line.Length == 0 || line[0] is '#' or ';') continue;
                var eq = line.IndexOf('=');
                if (eq < 1 || line[..eq].Trim() != line[..eq] || line.Any(char.IsControl) || !values.TryAdd(line[..eq], line[(eq + 1)..]))
                    throw new InvalidDataException("Manager 配置格式有误，已保留原文件，请先修复重复键或无效行。");
            }
            return values;
        }
        values = new(StringComparer.Ordinal) { ["language"] = "zh", ["hide_uid"] = "true", ["hide_ping"] = "false", ["hide_menu_uid"] = "true", ["hide_menu_name"] = "true", ["hide_card_uid"] = "true", ["hide_card_name"] = "true" };
        var legacy = Files.Under(game, "plugin/applepie_manager_config.txt");
        if (!File.Exists(legacy)) return values;
        var section = "";
        foreach (var raw in Files.ReadUtf8(legacy).Split('\n'))
        {
            var line = raw.Trim();
            if (line.Length == 0 || line[0] is '#' or ';') continue;
            if (line.StartsWith('[') && line.EndsWith(']')) { section = line[1..^1].ToLowerInvariant(); continue; }
            var eq = line.IndexOf('='); if (eq < 1) continue;
            var key = line[..eq].Trim(); var value = line[(eq + 1)..].Trim();
            if (section == "general" && key == "language" && value is "zh" or "en") values[key] = value;
            else if (section == "plugins") values[Key(key)] = value == "1" || value.Equals("true", StringComparison.OrdinalIgnoreCase) ? "true" : "false";
            else if (section == "uid" && key != "language" && values.ContainsKey(key)) values[key] = value == "1" || value.Equals("true", StringComparison.OrdinalIgnoreCase) ? "true" : "false";
        }
        return values;
    }
    public static bool Enabled(Dictionary<string, string> values, string name) => !values.TryGetValue(Key(name), out var value) || value is not ("false" or "0");
    public static void SetEnabled(string root, string name, bool enabled)
    {
        GameDirectory.RequireStopped(root);
        Files.Under(root, "plugin/" + name);
        var values = Read(root); values[Key(name)] = enabled ? "true" : "false";
        var content = "# Applepie basic configuration v1 (UTF-8)\n" + string.Join("\n", values.OrderBy(p => p.Key, StringComparer.Ordinal).Select(p => p.Key + "=" + p.Value)) + "\n";
        Files.WriteAtomic(Files.Under(root, RelativePath), Encoding.UTF8.GetBytes(content));
    }
}

public static class PluginManager
{
    private static bool Reserved(string path) => Path.GetFileName(path).Equals("applepie_manager.dll", StringComparison.OrdinalIgnoreCase) ||
        Path.GetFileName(path).Equals("d3dcompiler_47.dll", StringComparison.OrdinalIgnoreCase) || Path.GetFileName(path).Equals("vulkan-1.dll", StringComparison.OrdinalIgnoreCase);
    private static string ReceiptPath(string id) => ".applepie/plugins/" + Files.SafeId(id) + ".json";
    private static bool PackageDocument(string relative)
    {
        // Release documentation beside the entry DLL is not a runtime asset.
        // Leave data subdirectories and other file types to the plugin.
        if (relative.Contains('/')) return false;
        var extension = Path.GetExtension(relative).ToLowerInvariant();
        if (extension is not ("" or ".txt" or ".md" or ".rst" or ".html")) return false;
        return Regex.IsMatch(Path.GetFileNameWithoutExtension(relative), @"(^|[-_.])(licen[cs]e|copying|notices?|readme|changelog)([-_.]|$)", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
    }
    private static bool Configuration(string path)
    {
        var relative = path.Replace('\\', '/'); var name = Path.GetFileName(relative);
        return relative.StartsWith("config/", StringComparison.OrdinalIgnoreCase) ||
            name.Contains("config", StringComparison.OrdinalIgnoreCase) || name.Contains("settings", StringComparison.OrdinalIgnoreCase) ||
            new[] { ".ini", ".cfg", ".toml" }.Contains(Path.GetExtension(name), StringComparer.OrdinalIgnoreCase);
    }
    public static List<PluginItem> List(string game)
    {
        GameDirectory.Validate(game);
        var folder = Files.Under(game, "plugin"); var values = PluginConfiguration.Read(game);
        if (!Directory.Exists(folder)) return [];
        var receipts = Receipts(game);
        return Directory.EnumerateFiles(folder, "*.dll").Where(p => !Path.GetFileName(p).Equals("applepie_manager.dll", StringComparison.OrdinalIgnoreCase)).Select(path =>
        {
            var metadata = PluginReader.Read(path); var name = Path.GetFileName(path);
            var version = metadata.Version;
            if (version == "未知版本")
            {
                var receipt = receipts.FirstOrDefault(r => r.Id == metadata.Id && r.EntryDll.Equals(name, StringComparison.OrdinalIgnoreCase));
                if (receipt?.Files.Any(f => f.Path.Equals("plugin/" + name, StringComparison.OrdinalIgnoreCase) && f.Sha256 == Files.Hash(path)) == true) version = receipt.Version;
            }
            return new PluginItem { FileName = name, Name = metadata.Name, Id = metadata.Id, Version = version, Detail = metadata.Detail,
                Enabled = PluginConfiguration.Enabled(values, name), Manageable = metadata.Compatible };
        }).OrderBy(p => !p.Manageable).ThenBy(p => p.Name).ToList();
    }
    private static List<PluginReceipt> Receipts(string game)
    {
        var folder = GameStorage.File(game, ".applepie/plugins");
        if (!Directory.Exists(folder)) return [];
        return Directory.EnumerateFiles(folder, "*.json").Select(p => JsonSerializer.Deserialize<PluginReceipt>(File.ReadAllText(p), Product.Json)
            ?? throw new InvalidDataException($"插件安装记录无法读取：{p}")).ToList();
    }
    public static DeploymentResult Install(string game, string package, CatalogPlugin? online = null, IProgress<ProgressInfo>? progress = null, CancellationToken cancellation = default)
    {
        var metadata = GameStorage.Prepare(game);
        if (online is not null && !online.ReleaseAvailable) throw new InvalidDataException("尚未取得插件的最新 Release，请刷新在线目录。");
        var work = Product.NewWorkDirectory();
        try
        {
            var staging = Path.Combine(work, "files"); Directory.CreateDirectory(staging);
            if (Path.GetExtension(package).Equals(".dll", StringComparison.OrdinalIgnoreCase)) File.Copy(package, Files.Under(staging, Path.GetFileName(package)));
            else if (Path.GetExtension(package).Equals(".zip", StringComparison.OrdinalIgnoreCase)) Files.ExtractZip(package, staging, cancellation);
            else throw new InvalidDataException("请选择 DLL 或 ZIP 插件包。");
            var all = Directory.EnumerateFiles(staging, "*", SearchOption.AllDirectories).ToList();
            var candidates = all.Where(p => Path.GetExtension(p).Equals(".dll", StringComparison.OrdinalIgnoreCase) && !Reserved(p))
                .Select(p => (Path: p, Metadata: PluginReader.Read(p))).Where(p => p.Metadata.Compatible).ToList();
            if (online is not null) candidates = candidates.Where(p => p.Metadata.Id == online.Id && Path.GetFileName(p.Path).Equals(online.EntryDll, StringComparison.OrdinalIgnoreCase)).ToList();
            if (candidates.Count != 1) throw new InvalidDataException(online is null ? "插件包应包含一个已适配 Applepie 的主插件 DLL；其他 DLL 可以作为依赖。" : "下载包中的插件 ID 或入口 DLL 与在线目录不一致。");
            var main = candidates[0]; var sourceRoot = Path.GetDirectoryName(main.Path)!;
            var existingPlugin = List(game).FirstOrDefault(p => p.Id == main.Metadata.Id && !p.FileName.Equals(Path.GetFileName(main.Path), StringComparison.OrdinalIgnoreCase));
            if (existingPlugin is not null && !Receipts(game).Any(r => r.Id == main.Metadata.Id && r.EntryDll == existingPlugin.FileName))
                throw new IOException($"同一插件已通过 {existingPlugin.FileName} 手动安装，请先移除旧文件以避免重复 ID。");
            if (online is not null && main.Metadata.Version != "未知版本" && ReleaseVersion.Parse(main.Metadata.Version).CompareTo(ReleaseVersion.Parse(online.Version)) != 0)
                throw new InvalidDataException("下载包的插件版本与 Release 标签不一致。");
            // Support a flat package, plugin/, or a single wrapper directory.
            // Only deploy the main DLL's directory, never proxy/game executables.
            var files = all.Where(p => p.StartsWith(sourceRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)).ToList();
            var receipts = Receipts(game); var prior = receipts.FirstOrDefault(p => p.Id == main.Metadata.Id);
            var receipt = new PluginReceipt { Id = main.Metadata.Id, EntryDll = Path.GetFileName(main.Path), Version = online?.Version ?? main.Metadata.Version };
            var changes = new List<FileChange>();
            foreach (var file in files)
            {
                cancellation.ThrowIfCancellationRequested();
                var relative = Path.GetRelativePath(sourceRoot, file).Replace('\\', '/');
                if (Reserved(relative) || relative.StartsWith(".applepie/", StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException($"插件包包含 Manager 保留文件：{relative}");
                if (PackageDocument(relative)) continue;
                var destination = "plugin/" + relative;
                var existing = Files.Under(game, destination);
                if (!relative.EndsWith(".dll", StringComparison.OrdinalIgnoreCase) && Configuration(relative) && File.Exists(existing)) continue;
                var hash = Files.Hash(file);
                if (receipts.Any(p => p.Id != receipt.Id && p.Files.Any(f => f.Path.Equals(destination, StringComparison.OrdinalIgnoreCase) && f.Sha256 != hash)))
                    throw new IOException($"其他插件正在使用不同版本的 {relative}，已取消安装。");
                if (File.Exists(existing) && Path.GetExtension(relative).Equals(".dll", StringComparison.OrdinalIgnoreCase))
                {
                    var existingMetadata = PluginReader.Read(existing);
                    if (existingMetadata.Compatible && existingMetadata.Id != main.Metadata.Id) throw new IOException($"{relative} 属于另一个插件，不能覆盖。");
                }
                changes.Add(new(destination, file)); receipt.Files.Add(new(destination, hash));
            }
            if (prior is not null)
                foreach (var old in prior.Files.Where(f => !receipt.Files.Any(n => n.Path.Equals(f.Path, StringComparison.OrdinalIgnoreCase))))
                {
                    var target = Files.Under(game, old.Path);
                    if (!old.Path.StartsWith("plugin/", StringComparison.OrdinalIgnoreCase) || Reserved(old.Path) || Configuration(old.Path[7..])) continue;
                    if (!receipts.Any(r => r.Id != prior.Id && r.Files.Any(f => f.Path.Equals(old.Path, StringComparison.OrdinalIgnoreCase))) && File.Exists(target) && Files.Hash(target) == old.Sha256)
                        changes.Add(new(old.Path, null));
                }
            var receiptFile = Path.Combine(work, "receipt.json"); File.WriteAllText(receiptFile, JsonSerializer.Serialize(receipt, Product.Json));
            changes.Add(new(ReceiptPath(receipt.Id), receiptFile));
            cancellation.ThrowIfCancellationRequested();
            return Deployment.Apply(game, changes, progress, () => GameDirectory.RequireStopped(game), metadataDirectory: metadata);
        }
        finally { Files.RemoveWork(work); }
    }
    public static DeploymentResult Delete(string game, PluginItem plugin)
    {
        var metadata = GameStorage.Prepare(game);
        if (Reserved(plugin.FileName) || Path.GetFileName(plugin.FileName) != plugin.FileName) throw new InvalidDataException("无法删除此文件。");
        var receipts = Receipts(game); var receipt = receipts.FirstOrDefault(r => r.EntryDll.Equals(plugin.FileName, StringComparison.OrdinalIgnoreCase));
        var main = "plugin/" + plugin.FileName;
        if (receipts.Any(r => r.Id != receipt?.Id && r.Files.Any(f => f.Path.Equals(main, StringComparison.OrdinalIgnoreCase))))
            throw new IOException("这个 DLL 是其他已安装插件的依赖，请先移除使用它的插件。");
        var changes = new List<FileChange> { new(main, null) };
        if (receipt is not null)
        {
            foreach (var file in receipt.Files.Where(f => !f.Path.Equals(main, StringComparison.OrdinalIgnoreCase)))
            {
                if (!file.Path.StartsWith("plugin/", StringComparison.OrdinalIgnoreCase) || Configuration(file.Path[7..]) || Reserved(file.Path)) continue;
                var target = Files.Under(game, file.Path);
                if (!receipts.Any(r => r.Id != receipt.Id && r.Files.Any(f => f.Path.Equals(file.Path, StringComparison.OrdinalIgnoreCase))) && File.Exists(target) && Files.Hash(target) == file.Sha256)
                    changes.Add(new(file.Path, null));
            }
            changes.Add(new(ReceiptPath(receipt.Id), null));
        }
        return Deployment.Apply(game, changes, guard: () => GameDirectory.RequireStopped(game), metadataDirectory: metadata);
    }
}
