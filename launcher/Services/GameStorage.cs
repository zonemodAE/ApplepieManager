using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Applepie.Launcher;

public static class GameStorage
{
    private static string Normalize(string root) => Path.TrimEndingDirectorySeparator(Path.GetFullPath(root));
    public static string DirectoryFor(string game)
    {
        var key = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(Normalize(game).ToUpperInvariant()))).ToLowerInvariant();
        return Files.Under(AppContext.BaseDirectory, ".applepie/games/" + key);
    }
    public static string File(string game, string relative) => Deployment.Target(game, relative, DirectoryFor(game));
    public static string Prepare(string game)
    {
        game = Normalize(game);
        GameDirectory.RequireStopped(game);
        var metadata = DirectoryFor(game);
        var identity = Files.Under(metadata, "game.json");
        if (System.IO.File.Exists(identity))
        {
            var saved = JsonSerializer.Deserialize<Dictionary<string, string>>(Files.ReadUtf8(identity), Product.Json);
            if (saved is null || !saved.TryGetValue("directory", out var prior) || !Normalize(prior).Equals(game, StringComparison.OrdinalIgnoreCase))
                throw new IOException("游戏安装记录与所选目录不一致。");
        }
        else Files.WriteAtomic(identity, JsonSerializer.SerializeToUtf8Bytes(new { directory = game }, Product.Json));
        Deployment.Recover(game, () => GameDirectory.RequireStopped(game), metadata);
        return metadata;
    }
}
