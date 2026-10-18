using System.Diagnostics;
using System.Text;

namespace Applepie.Launcher;

public static class GameLauncher
{
    public static ProcessStartInfo CreateStartInfo(string root, string arguments = "")
    {
        GameDirectory.Validate(root);
        GameLaunchArguments.Validate(arguments);
        return new(Files.Under(root, "Endfield.exe")) { WorkingDirectory = Path.GetFullPath(root), UseShellExecute = true, Arguments = arguments };
    }

    public static bool Start(string root, GameLaunchPreference preference)
    {
        // Re-read on launch; the installation may have changed since selection.
        if (GameDirectory.IsRunning(root)) return false;
        var arguments = GameLaunchArguments.ForLaunch(preference);
        var plan = GameLaunchDiscovery.Resolve(root, preference);
        if (!plan.CanStart) throw new IOException(plan.Detail);
        if (plan.Channel == GameLaunchChannel.Steam)
        {
            using var game = Process.Start(CreateStartInfo(root, arguments)) ?? throw new IOException("无法启动 Endfield.exe。");
            return true;
        }
        using var verified = OfficialLauncherTrust.OpenVerified(plan.OfficialLauncher!);
        _ = OfficialGameProcess.Start(Files.Under(root, "Endfield.exe"), plan.OfficialLauncher!, arguments);
        return true;
    }
}

public static class GameLaunchArguments
{
    public const string DirectX11 = "-force-d3d11";
    public static string ForLaunch(GameLaunchPreference preference)
    {
        var arguments = preference.Arguments ?? "";
        Validate(arguments);
        return preference.ForceDirectX11 ? WithDirectX11(arguments, true) : arguments;
    }
    public static void Validate(string arguments)
    {
        if (arguments.Contains('\0')) throw new IOException("启动参数不能包含空字符。");
    }
    public static bool HasDirectX11(string arguments) => Tokens(arguments).Any(token => token.Value.Equals(DirectX11, StringComparison.OrdinalIgnoreCase));
    public static string WithDirectX11(string arguments, bool enabled)
    {
        var matches = Tokens(arguments).Where(token => token.Value.Equals(DirectX11, StringComparison.OrdinalIgnoreCase)).ToList();
        if (enabled)
        {
            if (matches.Count > 0) return arguments;
            var updated = arguments.TrimEnd() + (string.IsNullOrWhiteSpace(arguments) ? "" : " ") + DirectX11;
            if (!HasDirectX11(updated)) throw new IOException("启动参数的双引号未闭合，请补全后再启动游戏。");
            return updated;
        }
        foreach (var match in matches.AsEnumerable().Reverse()) arguments = arguments.Remove(match.Start, match.Length);
        return arguments.Trim();
    }

    // Track original spans so legacy DX11 migration preserves quoted paths,
    // backslashes, and values that merely contain the switch text.
    private static IEnumerable<(int Start, int Length, string Value)> Tokens(string arguments)
    {
        var index = 0;
        while (index < arguments.Length)
        {
            while (index < arguments.Length && arguments[index] is ' ' or '\t') ++index;
            if (index == arguments.Length) yield break;
            var start = index; var quoted = false; var value = new StringBuilder();
            while (index < arguments.Length && (quoted || arguments[index] is not (' ' or '\t')))
            {
                var slashes = 0;
                while (index < arguments.Length && arguments[index] == '\\') { ++slashes; ++index; }
                if (index < arguments.Length && arguments[index] == '"')
                {
                    value.Append('\\', slashes / 2);
                    if (slashes % 2 != 0) value.Append('"');
                    else if (quoted && index + 1 < arguments.Length && arguments[index + 1] == '"') { value.Append('"'); ++index; }
                    else quoted = !quoted;
                    ++index;
                }
                else
                {
                    value.Append('\\', slashes);
                    if (index < arguments.Length && (quoted || arguments[index] is not (' ' or '\t'))) value.Append(arguments[index++]);
                }
            }
            yield return (start, index - start, value.ToString());
        }
    }
}
