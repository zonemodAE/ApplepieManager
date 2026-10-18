using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;

namespace Applepie.Launcher;

public sealed class ReleaseAsset
{
    public string Name { get; set; } = "";
    [JsonPropertyName("browser_download_url")] public string Url { get; set; } = "";
    public string? Digest { get; set; }
    public long Size { get; set; }
}
public sealed class GitHubRelease
{
    [JsonPropertyName("tag_name")] public string Tag { get; set; } = "";
    [JsonPropertyName("html_url")] public string Page { get; set; } = "";
    public bool Draft { get; set; }
    public bool Prerelease { get; set; }
    public string? Body { get; set; }
    public List<ReleaseAsset> Assets { get; set; } = [];
}

public sealed class OnlineService : IDisposable
{
    private readonly HttpClient client;
    public OnlineService(HttpMessageHandler? handler = null)
    {
        client = handler is null ? new HttpClient() : new HttpClient(handler);
        client.DefaultRequestHeaders.UserAgent.ParseAdd("ApplepieManager/" + Product.Version);
        client.Timeout = Timeout.InfiniteTimeSpan;
    }
    public void Dispose() => client.Dispose();
    public static Uri Https(string value)
    {
        if (!Uri.TryCreate(value, UriKind.Absolute, out var uri) || uri.Scheme != Uri.UriSchemeHttps || !string.IsNullOrEmpty(uri.UserInfo)) throw new InvalidDataException("下载和目录地址必须为 HTTPS。");
        return uri;
    }
    private async Task<HttpResponseMessage> Get(string url, CancellationToken cancellation)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, Https(url));
        if (request.RequestUri!.Host == "api.github.com")
        {
            request.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
            request.Headers.Add("X-GitHub-Api-Version", "2022-11-28");
        }
        var response = await client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancellation);
        if (response.RequestMessage?.RequestUri is { Scheme: not "https" }) { response.Dispose(); throw new IOException("下载被重定向到非 HTTPS 地址。"); }
        return response;
    }
    public async Task<AvailableUpdates> Latest(CancellationToken cancellation = default)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation); timeout.CancelAfter(TimeSpan.FromSeconds(25));
        var endpoint = $"https://api.github.com/repos/{Product.Repository}/releases";
        string? url = endpoint + "?per_page=100";
        var visited = new HashSet<string>(StringComparer.Ordinal);
        var latest = new AvailableUpdates();
        while (url is not null)
        {
            var page = Https(url);
            if (page.GetLeftPart(UriPartial.Path) != endpoint || !visited.Add(url)) throw new InvalidDataException("GitHub Release 分页地址无效。");
            using var response = await Get(url, timeout.Token);
            if (response.StatusCode == HttpStatusCode.NotFound) return latest;
            response.EnsureSuccessStatusCode();
            var releases = await response.Content.ReadFromJsonAsync<List<GitHubRelease>>(Product.Json, timeout.Token) ?? throw new InvalidDataException("GitHub Release 响应为空。");
            foreach (var release in releases)
            {
                if (release is null || release.Draft || release.Prerelease) continue;
                var manifests = (release.Assets ?? []).Where(a => a is not null && a.Name == Product.ReleaseManifestName).ToArray();
                if (manifests.Length == 0) continue; // Releases predating the manifest protocol.
                try
                {
                    if (manifests.Length != 1) throw new InvalidDataException("发布清单重复。");
                    using var metadata = await Get(manifests[0].Url, timeout.Token);
                    metadata.EnsureSuccessStatusCode();
                    var manifest = await metadata.Content.ReadFromJsonAsync<ReleaseManifest>(Product.Json, timeout.Token) ?? throw new InvalidDataException("发布清单为空。");
                    latest.Include(release, manifest);
                }
                catch (Exception error) when (error is IOException or InvalidDataException or JsonException or HttpRequestException or InvalidOperationException)
                { latest.Warnings.Add($"{release.Tag}：{error.Message}"); }
            }
            url = null;
            if (response.Headers.TryGetValues("Link", out var links))
                foreach (var link in links.SelectMany(value => value.Split(',')))
                {
                    var match = Regex.Match(link, "<([^>]+)>;\\s*rel=\"next\"");
                    if (match.Success) { url = match.Groups[1].Value; break; }
                }
        }
        return latest;
    }
    public async Task<Catalog> GetCatalog(CancellationToken cancellation = default)
    {
        Catalog catalog;
        using (var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation))
        {
            timeout.CancelAfter(TimeSpan.FromSeconds(25));
            using var response = await Get(Product.CatalogUrl, timeout.Token);
            if (response.StatusCode == HttpStatusCode.NotFound) throw new IOException("在线插件目录尚未发布。将 catalog/plugins.json 推送到 GitHub 后即可使用；现在仍可安装本地插件。");
            response.EnsureSuccessStatusCode();
            catalog = await response.Content.ReadFromJsonAsync<Catalog>(Product.Json, timeout.Token) ?? throw new InvalidDataException("在线目录为空。");
        }
        Validate(catalog);
        // One request per repository per refresh, including repositories that
        // publish more than one plugin. A failed release only disables its rows.
        await Task.WhenAll(catalog.Plugins.GroupBy(p => p.Repository, StringComparer.OrdinalIgnoreCase).Select(async group =>
        {
            GitHubRelease release;
            try { release = await LatestPluginRelease(group.Key, cancellation); }
            catch (Exception error) when (CatalogFailure(error, cancellation))
            { foreach (var item in group) item.ReleaseError = error.Message; return; }
            foreach (var item in group)
                try { ResolvePluginRelease(item, release); }
                catch (Exception error) when (CatalogFailure(error, cancellation)) { item.ReleaseError = error.Message; }
        }));
        return catalog;
    }
    public static void Validate(Catalog catalog)
    {
        if (catalog.Plugins is null) throw new InvalidDataException("在线目录缺少插件列表。");
        var ids = new HashSet<string>(StringComparer.Ordinal);
        foreach (var item in catalog.Plugins)
        {
            if (item is null || string.IsNullOrEmpty(item.Id) || !ids.Add(Files.SafeId(item.Id))) throw new InvalidDataException("在线目录包含重复或无效插件。");
            if (string.IsNullOrEmpty(item.Repository) || !Regex.IsMatch(item.Repository, "^[A-Za-z0-9][A-Za-z0-9-]*/[A-Za-z0-9_.-]+$") || item.Repository.Split('/')[1] is "." or "..")
                throw new InvalidDataException($"{item.Id} 的 repository 必须为 GitHub 的 owner/repo。");
            if (string.IsNullOrEmpty(item.EntryDll) || Path.GetFileName(item.EntryDll) != item.EntryDll || !item.EntryDll.EndsWith(".dll", StringComparison.OrdinalIgnoreCase) || item.EntryDll.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
                throw new InvalidDataException("在线插件入口必须为 DLL 文件名。");
            if (item.AssetPattern is { } pattern && (string.IsNullOrWhiteSpace(pattern) || pattern.IndexOfAny(['/', '\\', ':']) >= 0 || pattern.Any(char.IsControl)))
                throw new InvalidDataException($"{item.Id} 的 assetPattern 必须匹配附件文件名，可使用 * 和 ?。");
        }
    }
    private static bool CatalogFailure(Exception error, CancellationToken cancellation) => !cancellation.IsCancellationRequested &&
        error is IOException or InvalidDataException or JsonException or HttpRequestException or InvalidOperationException;
    private async Task<GitHubRelease> LatestPluginRelease(string repository, CancellationToken cancellation)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation); timeout.CancelAfter(TimeSpan.FromSeconds(25));
        try
        {
            using var response = await Get($"https://api.github.com/repos/{repository}/releases/latest", timeout.Token);
            if (response.StatusCode == HttpStatusCode.NotFound) throw new IOException("仓库不存在或尚未发布正式 Release。");
            if (response.StatusCode == HttpStatusCode.TooManyRequests || (response.StatusCode == HttpStatusCode.Forbidden && response.Headers.TryGetValues("X-RateLimit-Remaining", out var remaining) && remaining.Contains("0")))
                throw new IOException("GitHub 请求额度已用尽，请稍后刷新。");
            response.EnsureSuccessStatusCode();
            var release = await response.Content.ReadFromJsonAsync<GitHubRelease>(Product.Json, timeout.Token) ?? throw new InvalidDataException("最新 Release 响应为空。");
            if (release.Draft || release.Prerelease) throw new InvalidDataException("最新发布尚不是正式 Release。");
            return release;
        }
        catch (OperationCanceledException) when (!cancellation.IsCancellationRequested) { throw new IOException("查询最新 Release 超时，请刷新重试。"); }
    }
    public async Task RefreshPlugin(CatalogPlugin item, CancellationToken cancellation = default)
    {
        Validate(new() { Plugins = [item] });
        item.Version = item.DownloadUrl = item.AssetName = item.ReleaseError = ""; item.Sha256 = null; item.DownloadSize = null;
        try { ResolvePluginRelease(item, await LatestPluginRelease(item.Repository, cancellation)); }
        catch (Exception error) when (CatalogFailure(error, cancellation)) { item.ReleaseError = error.Message; throw; }
    }
    private static void ResolvePluginRelease(CatalogPlugin item, GitHubRelease release)
    {
        if (string.IsNullOrWhiteSpace(release.Tag)) throw new InvalidDataException("最新 Release 缺少版本标签。");
        ReleaseVersion.Parse(release.Tag);
        var candidates = (release.Assets ?? []).Where(a => a is not null && !string.IsNullOrEmpty(a.Name) &&
            (a.Name.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) || a.Name.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))).ToList();
        if (item.AssetPattern is { } pattern)
            candidates = candidates.Where(a => System.IO.Enumeration.FileSystemName.MatchesSimpleExpression(pattern, a.Name, ignoreCase: true)).ToList();
        else
        {
            // Prefer the complete package when the release also offers a bare DLL.
            var archives = candidates.Where(a => a.Name.EndsWith(".zip", StringComparison.OrdinalIgnoreCase)).ToList();
            candidates = archives.Count > 0 ? archives : candidates.Where(a => a.Name.Equals(item.EntryDll, StringComparison.OrdinalIgnoreCase)).ToList();
        }
        if (candidates.Count == 0) throw new InvalidDataException("最新 Release 没有匹配的 DLL 或 ZIP 附件。");
        if (candidates.Count > 1) throw new InvalidDataException("最新 Release 有多个可用附件，请在目录中用 assetPattern 指定插件包。");
        var asset = candidates[0]; Https(asset.Url);
        string? sha256 = null;
        if (!string.IsNullOrEmpty(asset.Digest))
        {
            if (!Regex.IsMatch(asset.Digest, "^sha256:[0-9a-fA-F]{64}$", RegexOptions.IgnoreCase)) throw new InvalidDataException("GitHub 附件的 SHA-256 校验值无效。");
            sha256 = asset.Digest[7..];
        }
        item.Version = release.Tag.Trim().TrimStart('v', 'V'); item.DownloadUrl = asset.Url; item.AssetName = asset.Name;
        item.Sha256 = sha256; item.DownloadSize = asset.Size > 0 ? asset.Size : null;
    }
    public async Task Download(string url, string destination, string? expectedHash, IProgress<ProgressInfo>? progress, CancellationToken cancellation = default, long? size = null)
    {
        if (expectedHash is not null && !Regex.IsMatch(expectedHash, "^[0-9a-fA-F]{64}$")) throw new InvalidDataException("下载文件校验值无效。");
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation); timeout.CancelAfter(TimeSpan.FromMinutes(30));
        var created = false;
        try
        {
            using var response = await Get(url, timeout.Token); response.EnsureSuccessStatusCode();
            var total = response.Content.Headers.ContentLength ?? size;
            await using (var input = await response.Content.ReadAsStreamAsync(timeout.Token))
            await using (var output = new FileStream(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None, 81920, true))
            {
                created = true;
                var buffer = new byte[81920]; long received = 0; var next = Environment.TickCount64;
                int count;
                while ((count = await input.ReadAsync(buffer, timeout.Token)) != 0)
                {
                    await output.WriteAsync(buffer.AsMemory(0, count), timeout.Token); received += count;
                    // Throttle UI notifications only when bytes arrive; no polling.
                    if (Environment.TickCount64 >= next)
                    {
                        next = Environment.TickCount64 + 100;
                        progress?.Report(new($"正在下载 · {received / 1048576.0:F1} MB" + (total > 0 ? $" / {total / 1048576.0:F1} MB" : ""), total > 0 ? 100.0 * received / total.Value : -1));
                    }
                }
                if ((size is > 0 && received != size) || (total is > 0 && received != total)) throw new IOException("下载文件不完整。");
            }
            if (expectedHash is not null)
            {
                progress?.Report(new("正在校验下载文件…"));
                if (!Files.Hash(destination).Equals(expectedHash, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("下载文件 SHA-256 校验失败，未安装。");
            }
        }
        catch { if (created && File.Exists(destination)) File.Delete(destination); throw; }
    }
}
