using System.Buffers.Binary;
using System.Reflection.PortableExecutable;
using System.Text;

namespace Applepie.Launcher;

public sealed record PluginMetadata(string Id, string Name, string Version, bool Compatible, string Detail);

// Inspect PE data only. Never LoadLibrary a plugin in the launcher process.
public static class PluginReader
{
    public static PluginMetadata Read(string path)
    {
        try
        {
            using var stream = File.OpenRead(path); using var pe = new PEReader(stream);
            var header = pe.PEHeaders;
            if (header.CoffHeader.Machine != Machine.Amd64 || header.PEHeader is null || (header.CoffHeader.Characteristics & Characteristics.Dll) == 0)
                return Invalid("需要 Windows x64 DLL");
            var export = header.PEHeader.ExportTableDirectory;
            if (export.RelativeVirtualAddress == 0) return Invalid("未导出 Applepie 插件清单");
            byte[] Data(int rva, int count, bool readOnly = false)
            {
                var section = header.SectionHeaders.FirstOrDefault(s => rva >= s.VirtualAddress && (long)rva + count <= (long)s.VirtualAddress + s.SizeOfRawData);
                if (section.Name is null || count < 0 || (readOnly && (section.SectionCharacteristics & (SectionCharacteristics.MemExecute | SectionCharacteristics.MemWrite)) != 0))
                    throw new InvalidDataException("清单不是有效的只读 PE 数据");
                return pe.GetSectionData(rva).GetContent(0, count).ToArray();
            }
            var directory = Data(export.RelativeVirtualAddress, 40);
            uint U32(byte[] b, int n) => BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(n, 4));
            var functionCount = U32(directory, 20); var nameCount = U32(directory, 24);
            var functions = Data(checked((int)U32(directory, 28)), checked((int)functionCount * 4));
            var names = Data(checked((int)U32(directory, 32)), checked((int)nameCount * 4));
            var ordinals = Data(checked((int)U32(directory, 36)), checked((int)nameCount * 2));
            byte[]? Find(string wanted, int bytes, bool readOnly = true)
            {
                for (var i = 0; i < nameCount; ++i)
                {
                    var rva = checked((int)U32(names, i * 4));
                    var section = header.SectionHeaders.FirstOrDefault(s => rva >= s.VirtualAddress && (long)rva < (long)s.VirtualAddress + s.SizeOfRawData);
                    if (section.Name is null) throw new InvalidDataException("导出名称地址无效");
                    if ((long)rva + wanted.Length + 1 > (long)section.VirtualAddress + section.SizeOfRawData) continue;
                    var name = Data(rva, wanted.Length + 1);
                    if (name[^1] != 0 || Encoding.ASCII.GetString(name.AsSpan(0, wanted.Length)) != wanted) continue;
                    var ordinal = BinaryPrimitives.ReadUInt16LittleEndian(ordinals.AsSpan(i * 2, 2));
                    if (ordinal >= functionCount) throw new InvalidDataException("导出表无效");
                    var address = checked((int)U32(functions, ordinal * 4));
                    if (address >= export.RelativeVirtualAddress && (long)address < (long)export.RelativeVirtualAddress + export.Size) throw new InvalidDataException("不接受转发清单");
                    return Data(address, bytes, readOnly);
                }
                return null;
            }
            var manifest = Find("APH_PluginManifest", 160);
            if (manifest is null) return Invalid("依赖库或尚未适配 Applepie SDK");
            string Text(int offset)
            {
                var length = Array.IndexOf(manifest, (byte)0, offset, 64) - offset;
                if (length < 0) throw new InvalidDataException("清单字符串未终止");
                return new UTF8Encoding(false, true).GetString(manifest, offset, length);
            }
            var id = Files.SafeId(Text(32)); var name = Text(96);
            var major = BinaryPrimitives.ReadUInt16LittleEndian(manifest.AsSpan(4)); var minor = BinaryPrimitives.ReadUInt16LittleEndian(manifest.AsSpan(6));
            var flags = U32(manifest, 12);
            var compatible = U32(manifest, 0) == 160 && major == 1 && minor <= 1 && U32(manifest, 8) == 0x8664 &&
                (flags & ~0x700u) == 0 && ((flags & ~0x100u) == 0 || (flags & 0x100u) != 0);
            var version = System.Diagnostics.FileVersionInfo.GetVersionInfo(path).ProductVersion?.Split('+')[0] ?? "未知版本";
            if (Find("APH_PluginInfo", 728) is { } info)
            {
                if (U32(info, 0) != 728 || U32(info, 4) != 1 || U32(info, 16) > 8 || U32(info, 20) != 0 || minor < 1) compatible = false;
                var value = BinaryPrimitives.ReadUInt64LittleEndian(info.AsSpan(8));
                if ((value >> 48) != 0) compatible = false;
                version = $"{(value >> 32) & 65535}.{(value >> 16) & 65535}.{value & 65535}";
                // ABI 1.1 stores eight dependency records in its fixed DATA
                // export; this validates that layout, not an installer limit.
                var dependencies = new HashSet<string>(StringComparer.Ordinal);
                for (var i = 0; i < Math.Min(U32(info, 16), 8); ++i)
                {
                    var offset = 24 + i * 88;
                    var length = Array.IndexOf(info, (byte)0, offset, 64) - offset;
                    if (length < 1) { compatible = false; continue; }
                    var dependency = Files.SafeId(new UTF8Encoding(false, true).GetString(info, offset, length));
                    var minimum = BinaryPrimitives.ReadUInt64LittleEndian(info.AsSpan(offset + 64));
                    var maximum = BinaryPrimitives.ReadUInt64LittleEndian(info.AsSpan(offset + 72));
                    if (!dependencies.Add(dependency) || minimum >> 48 != 0 || maximum >> 48 != 0 ||
                        (maximum != 0 && maximum <= minimum) || (U32(info, offset + 80) & ~1u) != 0 || U32(info, offset + 84) != 0) compatible = false;
                }
            }
            if (Find("APH_Attach", 1, false) is null || Find("APH_Enable", 1, false) is null) compatible = false;
            return new(id, string.IsNullOrEmpty(name) ? id : name, version, compatible, compatible ? id : "插件 ABI 或入口不兼容当前 Manager");
        }
        catch (Exception e) when (e is IOException or BadImageFormatException or ArgumentException or OverflowException or UnauthorizedAccessException)
        { return Invalid(e.Message); }
        PluginMetadata Invalid(string detail) => new("", Path.GetFileNameWithoutExtension(path), "未知版本", false, detail);
    }
}
