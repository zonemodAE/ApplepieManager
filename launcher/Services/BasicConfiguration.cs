using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Applepie.Launcher;

public sealed class BasicField
{
    public string Key { get; set; } = "";
    public string Label { get; set; } = "";
    public string Description { get; set; } = "";
    public int Type { get; set; } // SDK: BOOL=1, INT=2, FLOAT=3, ENUM=4, TEXT=5, KEY=6; 0 is an undescribed key.
    public string DefaultValue { get; set; } = "";
    public double Minimum { get; set; }
    public double Maximum { get; set; }
    public int TextMaxBytes { get; set; } = 255;
    public List<string> Options { get; set; } = [];
    public string Normalize(string value)
    {
        if (value.Any(c => c < 32 || c == 127) || Encoding.UTF8.GetByteCount(value) > 255) throw new InvalidDataException($"{Label}：内容必须为一行，且不超过 255 个 UTF-8 字节。");
        var numberText = value.Trim();
        switch (Type)
        {
            case 0: return value;
            case 1:
                return value switch { "true" or "1" => "true", "false" or "0" => "false", _ => throw new InvalidDataException($"{Label}：请选择开或关。") };
            case 2:
            case 6:
                if (!int.TryParse(numberText, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var integer) ||
                    integer < (Type == 6 ? 0 : Minimum) || integer > (Type == 6 ? 255 : Maximum)) throw new InvalidDataException($"{Label}：数值超出允许范围。");
                return integer.ToString(CultureInfo.InvariantCulture);
            case 3:
                if (!double.TryParse(numberText, NumberStyles.Float, CultureInfo.InvariantCulture, out var number) || !double.IsFinite(number) || number < Minimum || number > Maximum)
                    throw new InvalidDataException($"{Label}：请输入 {Minimum} 到 {Maximum} 之间的数值。");
                return number.ToString("R", CultureInfo.InvariantCulture);
            case 4:
                if (!Options.Contains(value, StringComparer.Ordinal)) throw new InvalidDataException($"{Label}：请选择有效选项。");
                return value;
            case 5:
                if (Encoding.UTF8.GetByteCount(value) > TextMaxBytes) throw new InvalidDataException($"{Label}：最多 {TextMaxBytes} 个 UTF-8 字节。");
                return value;
            default: throw new InvalidDataException("不支持的配置字段类型。");
        }
    }
}

public sealed class BasicSchema
{
    public int SchemaVersion { get; set; }
    public string PluginId { get; set; } = "";
    public long ModuleSize { get; set; }
    public string ModuleWriteTime { get; set; } = "";
    public List<BasicField> Fields { get; set; } = [];
    public void Validate(string id, FileInfo dll)
    {
        if (SchemaVersion != 1 || PluginId != id || Fields is null || ModuleSize != dll.Length || ModuleWriteTime != dll.LastWriteTimeUtc.ToFileTimeUtc().ToString(CultureInfo.InvariantCulture))
            throw new InvalidDataException("设置描述与当前插件版本不匹配。");
        var keys = new HashSet<string>(StringComparer.Ordinal);
        foreach (var field in Fields)
        {
            if (field is null || field.Description is null || field.DefaultValue is null || field.Options is null) throw new InvalidDataException("设置字段不完整。");
            BasicConfiguration.ValidKey(field.Key);
            if (!keys.Add(field.Key) || string.IsNullOrEmpty(field.Label) || field.Type is < 1 or > 6) throw new InvalidDataException("设置描述包含无效或重复字段。");
            if (field.Type is 2 or 3 && (!double.IsFinite(field.Minimum) || !double.IsFinite(field.Maximum) || field.Minimum > field.Maximum)) throw new InvalidDataException("设置范围无效。");
            if (field.Type == 2 && (field.Minimum < int.MinValue || field.Maximum > int.MaxValue || Math.Truncate(field.Minimum) != field.Minimum || Math.Truncate(field.Maximum) != field.Maximum)) throw new InvalidDataException("整数设置范围无效。");
            if (field.Type == 5 && field.TextMaxBytes is < 1 or > 255) throw new InvalidDataException("文本设置长度无效。");
            if (field.Type == 4 && (field.Options.Count == 0 || field.Options.Any(string.IsNullOrEmpty) || field.Options.Distinct(StringComparer.Ordinal).Count() != field.Options.Count)) throw new InvalidDataException("设置选项无效。");
            field.Normalize(field.DefaultValue);
        }
    }
}

public sealed class BasicConfiguration
{
    private readonly string game, path, dllPath;
    private readonly long dllSize, dllStamp;
    private readonly SemaphoreSlim saves = new(1, 1);
    private string contents, expectedHash;
    private bool bom;
    private Dictionary<string, string> values;
    public IReadOnlyList<BasicField> Fields { get; }
    public string FilePath => path;
    public string Notice { get; }
    public static void ValidKey(string key)
    {
        if (string.IsNullOrEmpty(key) || key.Length >= 48 || !Regex.IsMatch(key, "^[a-zA-Z0-9_.-]+$")) throw new InvalidDataException("配置包含无效的键名。");
    }
    private static Dictionary<string, string> Parse(string text)
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var raw in text.Split('\n'))
        {
            var line = raw.EndsWith('\r') ? raw[..^1] : raw;
            if (line.Contains('\0')) throw new InvalidDataException("配置包含无效字符，原文件已保留。");
            if (line.Length == 0 || line[0] is '#' or ';') continue;
            var eq = line.IndexOf('=');
            if (eq < 1) throw new InvalidDataException("配置中存在无效行，原文件已保留。");
            var key = line[..eq]; ValidKey(key);
            var value = line[(eq + 1)..];
            if (value.Any(c => c < 32 || c == 127) || Encoding.UTF8.GetByteCount(value) > 255 || !result.TryAdd(key, value)) throw new InvalidDataException("配置有重复键或无效值，原文件已保留。");
        }
        return result;
    }
    private static (string Text, string Hash, bool Bom) Read(string path)
    {
        if (!File.Exists(path)) return ("", "", false);
        var bytes = File.ReadAllBytes(path);
        var text = new UTF8Encoding(false, true).GetString(bytes);
        var bom = text.StartsWith('\uFEFF');
        return (bom ? text[1..] : text, Convert.ToHexString(SHA256.HashData(bytes)), bom);
    }
    public BasicConfiguration(string game, PluginItem plugin)
    {
        this.game = game; GameDirectory.Validate(game);
        if (!plugin.Manageable || string.IsNullOrEmpty(plugin.Id)) throw new InvalidDataException("该文件未提供可识别的插件身份。");
        var id = Files.SafeId(plugin.Id);
        if (id == "manager") throw new InvalidDataException("保留的配置标识。");
        dllPath = Files.Under(game, "plugin/" + plugin.FileName);
        if (PluginReader.Read(dllPath) is not { Compatible: true } identity || identity.Id != id) throw new IOException("插件文件已改变，请刷新插件列表。");
        var dll = new FileInfo(dllPath); dllSize = dll.Length; dllStamp = dll.LastWriteTimeUtc.Ticks;
        path = Files.Under(game, $"plugin/config/applepie.{id}.ini");
        (contents, expectedHash, bom) = Read(path); values = Parse(contents);
        var description = Files.Under(game, $"plugin/config/applepie.{id}.schema.json");
        var fields = new List<BasicField>();
        Notice = "修改后自动保存，下次启动游戏生效。";
        if (File.Exists(description))
        {
            try
            {
                var schema = JsonSerializer.Deserialize<BasicSchema>(Files.ReadUtf8(description), Product.Json) ?? throw new InvalidDataException("设置描述为空。");
                schema.Validate(id, dll); fields.AddRange(schema.Fields);
            }
            catch (Exception error) when (error is IOException or InvalidDataException or JsonException or ArgumentException)
            { Notice = error.Message + " 现有配置仍可按键值编辑；启用插件一次后会刷新设置描述。"; }
        }
        else Notice = "启用插件一次后，可显示完整的设置名称和按键控件。现有配置可直接编辑，修改后自动保存。";
        foreach (var entry in values)
            if (!fields.Any(f => f.Key == entry.Key)) fields.Add(new() { Key = entry.Key, Label = entry.Key, DefaultValue = entry.Value });
        Fields = fields;
        if (fields.Count == 0) Notice = "该插件尚未生成配置。请在游戏中启用一次；未使用基础配置的插件通过插件自身界面设置。";
    }
    public string Value(BasicField field) => Volatile.Read(ref values).GetValueOrDefault(field.Key, field.DefaultValue);
    public async Task<string> Save(BasicField field, string requested)
    {
        if (!Fields.Contains(field)) throw new InvalidDataException("未知配置字段。");
        var normalized = field.Normalize(requested);
        await saves.WaitAsync();
        try
        {
            return await Task.Run(() =>
            {
                GameDirectory.RequireStopped(game);
                var dll = new FileInfo(dllPath);
                if (dll.Length != dllSize || dll.LastWriteTimeUtc.Ticks != dllStamp) throw new IOException("插件已经更新，请重新打开配置。");
                var current = Read(path);
                if (current.Hash != expectedHash) throw new IOException("配置文件已被其他程序修改，请关闭并重新打开配置，避免覆盖较新的内容。");
                if (values.TryGetValue(field.Key, out var old) && old == normalized) return normalized;
                var newline = contents.Contains("\r\n", StringComparison.Ordinal) ? "\r\n" : "\n";
                var next = contents.Length == 0 ? "# Applepie basic configuration v1 (UTF-8)" + newline : contents;
                if (values.ContainsKey(field.Key)) next = Regex.Replace(next, "(?m)^" + Regex.Escape(field.Key) + "=[^\r\n]*", _ => field.Key + "=" + normalized);
                else next += (next.EndsWith('\n') ? "" : newline) + field.Key + "=" + normalized + newline;
                var bytes = new UTF8Encoding(false, true).GetBytes((bom ? "\uFEFF" : "") + next);
                // Check once at the write boundary; no file or memory polling.
                GameDirectory.RequireStopped(game);
                if (Read(path).Hash != expectedHash) throw new IOException("配置文件刚被其他程序修改，本次未保存。请重新打开配置。");
                Files.WriteAtomic(path, bytes);
                contents = next; expectedHash = Convert.ToHexString(SHA256.HashData(bytes));
                Volatile.Write(ref values, Parse(next));
                return normalized;
            });
        }
        finally { saves.Release(); }
    }
}
