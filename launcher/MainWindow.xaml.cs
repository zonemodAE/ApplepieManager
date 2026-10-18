using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using Microsoft.Win32;

namespace Applepie.Launcher;

public partial class MainWindow : Window
{
    private LauncherSettings settings = new();
    private readonly OnlineService online = new();
    private AvailableUpdates updates = new();
    private readonly CorePackages corePackages = new();
    private CorePackage availableCore = CorePackage.Embedded;
    private string? installedCore;
    private List<PluginItem> plugins = [];
    private Catalog? catalog;
    private CancellationTokenSource? operation;
    private bool busy, checking, ready, catalogAttempted;
    private string? game;
    private bool changingLaunchControls;
    private GameLaunchPlan? launchPlan;
    private string displayedOfficialLauncher = "";
    private bool officialLauncherPathDirty;

    public MainWindow() : this(true) { }
    public MainWindow(bool initialize)
    {
        InitializeComponent(); VersionLabel.Text = "VERSION " + Product.Version; LauncherVersion.Text = "v" + Product.Version;
        BundledCoreVersion.Text = "随包核心 v" + Product.CoreVersion;
        ready = true;
        UpdateGameActions();
        Closing += OnClosing;
        Closed += (_, _) => { operation?.Cancel(); operation?.Dispose(); online.Dispose(); };
        if (initialize) Loaded += async (_, _) =>
        {
            try { availableCore = await Task.Run(corePackages.Current); UpdateCoreLabel(); }
            catch (Exception error) { Dialogs.Message(this, "核心缓存无法读取", error.Message + "\n本次可使用随包核心，或重新下载核心更新。"); }
            try { settings = LauncherSettings.Load(); }
            catch (Exception error) { Dialogs.Message(this, "配置读取失败", error.Message + "\n原文件已保留，本次仍可手动选择游戏目录。"); }
            if (settings.GameDirectory is { } saved) { GamePathInput.Text = saved; await Resolve(saved); }
            await CheckUpdate(true);
        };
    }
    private void OnClosing(object? sender, CancelEventArgs e)
    {
        if (!busy) return;
        e.Cancel = true;
        StatusText.Text = "正在处理文件或下载。请等待完成，或先取消下载。";
    }
    private void ToggleTheme(object sender, RoutedEventArgs e) => Appearance.Toggle(this);
    private async void Navigate(object sender, RoutedEventArgs e)
    {
        if (!ready || sender is not RadioButton { Tag: NavigationPage page }) return;
        DeploymentPage.Visibility = page == NavigationPage.Deployment ? Visibility.Visible : Visibility.Collapsed;
        PluginsPage.Visibility = page == NavigationPage.Plugins ? Visibility.Visible : Visibility.Collapsed;
        DownloadsPage.Visibility = page == NavigationPage.Downloads ? Visibility.Visible : Visibility.Collapsed;
        AboutPage.Visibility = page == NavigationPage.About ? Visibility.Visible : Visibility.Collapsed;
        if (page == NavigationPage.Plugins && !busy) await Run(() => RefreshPlugins());
        if (page == NavigationPage.Downloads && !catalogAttempted && !busy) await LoadCatalog();
    }
    private void Busy(bool value, bool cancellable = false)
    {
        busy = value;
        DeploymentPage.IsEnabled = PluginsPage.IsEnabled = DownloadsPage.IsEnabled = AboutPage.IsEnabled = !value;
        NavigationPanel.IsEnabled = !value;
        OperationProgress.Visibility = value ? Visibility.Visible : Visibility.Collapsed;
        OperationProgress.IsIndeterminate = value;
        CancelButton.Visibility = value && cancellable ? Visibility.Visible : Visibility.Collapsed;
        UpdateGameActions();
    }
    private void UpdateGameActions()
    {
        LaunchGameButton.Visibility = game is null ? Visibility.Collapsed : Visibility.Visible;
        LaunchOptionsPanel.Visibility = game is null ? Visibility.Collapsed : Visibility.Visible;
        OfficialLauncherControls.Visibility = game is not null && launchPlan?.Channel != GameLaunchChannel.Steam ? Visibility.Visible : Visibility.Collapsed;
        LaunchGameButton.ToolTip = game is null ? null : Path.Combine(game, "Endfield.exe");
        LaunchGameButton.IsEnabled = InstallButton.IsEnabled = RestoreProxiesButton.IsEnabled = DeletePluginDirectoryButton.IsEnabled = game is not null && !busy;
        if (launchPlan is { CanStart: false } || (officialLauncherPathDirty && launchPlan?.Channel != GameLaunchChannel.Steam)) LaunchGameButton.IsEnabled = false;
    }
    private IProgress<ProgressInfo> Progress() => new Progress<ProgressInfo>(p =>
    {
        StatusText.Text = p.Message; OperationProgress.IsIndeterminate = p.Percent < 0;
        if (p.Percent >= 0) OperationProgress.Value = p.Percent;
    });
    private async Task Run(Func<Task> action, bool cancellable = false)
    {
        if (busy) return;
        operation?.Dispose(); operation = new(); Busy(true, cancellable);
        try { await action(); }
        catch (OperationCanceledException) { StatusText.Text = "操作已取消，现有文件保持不变。"; }
        catch (Exception error) { StatusText.Text = error.Message; Dialogs.Message(this, "操作未完成", error.Message); }
        finally { Busy(false); }
    }
    private string RequireGame()
    {
        if (game is null) throw new IOException(GameDirectory.NotFound);
        GameDirectory.Validate(game); return game;
    }
    private async Task Resolve(string selected)
    {
        await Run(async () =>
        {
            StatusText.Text = "正在识别游戏目录…";
            var matches = await Task.Run(() => GameDirectory.Find(selected, operation!.Token));
            if (matches.Count == 0) { game = null; installedCore = null; InstalledVersion.Text = "未检测"; ResolvedPath.Text = GameDirectory.NotFound; BindUpdates(); throw new IOException(GameDirectory.NotFound); }
            var chosen = matches.Count == 1 ? matches[0] : Dialogs.Choose(this, matches);
            if (chosen is null) { StatusText.Text = "未更改游戏目录。"; return; }
            GameDirectory.Validate(chosen); game = chosen; GamePathInput.Text = chosen; settings.GameDirectory = chosen; settings.Theme = Appearance.Preference; settings.Save();
            ResolvedPath.Text = "已识别  /  " + Path.Combine(chosen, "Endfield.exe");
            await RefreshLaunchOptions();
            installedCore = ManagerInstaller.InstalledVersion(chosen);
            InstalledVersion.Text = installedCore is { } version ? "v" + version : "尚未安装";
            BindUpdates();
            await RefreshPlugins(); StatusText.Text = "游戏目录已就绪。";
        });
    }
    private async void BrowseGame(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog { Title = "选择终末地游戏文件夹", Multiselect = false };
        if (game is not null) dialog.InitialDirectory = game;
        if (dialog.ShowDialog(this) == true) await Resolve(dialog.FolderName);
    }
    private async void ResolvePath(object sender, RoutedEventArgs e) => await Resolve(GamePathInput.Text);
    private async void PathKeyDown(object sender, KeyEventArgs e) { if (e.Key == Key.Enter) { e.Handled = true; await Resolve(GamePathInput.Text); } }
    private void PathEdited(object sender, TextChangedEventArgs e)
    {
        if (!ready || game is null || string.Equals(game, GamePathInput.Text, StringComparison.OrdinalIgnoreCase)) return;
        game = null; installedCore = null; plugins = []; LocalItems.ItemsSource = plugins; BindUpdates();
        launchPlan = null;
        UpdateGameActions();
        InstalledVersion.Text = "未检测"; ResolvedPath.Text = "目录已更改，请点击识别。";
        if (catalog is not null) BindCatalog();
    }
    private async void InstallManager(object sender, RoutedEventArgs e)
    {
        await Run(async () =>
        {
            var root = RequireGame();
            var current = ManagerInstaller.InstalledVersion(root);
            if (current is not null && ReleaseVersion.Newer(current, availableCore.Version) && !Dialogs.Ask(this, "安装较早版本？", $"游戏内核心是 v{current}，当前可用核心为 v{availableCore.Version}。继续将备份后替换为较早版本。", "备份并替换")) return;
            var progress = Progress();
            var result = await Task.Run(() => ManagerInstaller.Install(root, availableCore, progress));
            installedCore = ManagerInstaller.InstalledVersion(root); InstalledVersion.Text = "v" + installedCore; BindUpdates();
            BackupLabel.Text = result.ChangedFiles > 0 ? "备份保存在  " + result.BackupDirectory : "代理与核心文件均与当前版本一致。";
            StatusText.Text = result.ChangedFiles > 0 ? "安装完成 · 两个代理与 Manager 核心已部署。" : "已是当前版本，无需替换。";
            await RefreshPlugins();
        });
    }
    private async void LaunchGame(object sender, RoutedEventArgs e)
    {
        await Run(async () =>
        {
            var root = RequireGame();
            StatusText.Text = "正在连接至塔卫二…";
            SaveLaunchOptions();
            var preference = settings.LaunchFor(root);
            var started = await Task.Run(() => GameLauncher.Start(root, preference));
            StatusText.Text = started ? "已启动终末地。" : "检测到终末地已经运行。";
        });
    }
    private async Task RefreshLaunchOptions()
    {
        if (game is null) return;
        var root = game;
        var preference = settings.LaunchFor(root);
        changingLaunchControls = true;
        try
        {
            AutoChannel.IsChecked = preference.Channel == GameLaunchChannel.Auto;
            OfficialChannel.IsChecked = preference.Channel == GameLaunchChannel.Official;
            SteamChannel.IsChecked = preference.Channel == GameLaunchChannel.Steam;
            LaunchArgumentsInput.Text = preference.Arguments ?? "";
            DirectX11Option.IsChecked = preference.ForceDirectX11;
        }
        finally { changingLaunchControls = false; }
        launchPlan = await Task.Run(() => GameLaunchDiscovery.Resolve(root, preference));
        LaunchChannelDetail.Text = launchPlan.Channel == GameLaunchChannel.Steam ? launchPlan.Detail
            : launchPlan.CanStart ? (preference.Channel == GameLaunchChannel.Official ? "手动选择：官方" : "已识别：官方")
            : "请在下方识别官方启动器，或手动选择 Steam 渠道。";
        changingLaunchControls = true;
        try
        {
            displayedOfficialLauncher = launchPlan.OfficialLauncher ?? preference.OfficialLauncher ?? "";
            OfficialLauncherPathInput.Text = displayedOfficialLauncher;
            officialLauncherPathDirty = false;
        }
        finally { changingLaunchControls = false; }
        OfficialLauncherResolvedPath.Text = launchPlan.Detail;
        UpdateGameActions();
    }
    private void SaveLaunchPreference(GameLaunchPreference preference)
    {
        var root = RequireGame();
        var previous = settings.LaunchFor(root);
        settings.SetLaunch(root, preference);
        settings.Theme = Appearance.Preference;
        try { settings.Save(); }
        catch { settings.SetLaunch(root, previous); throw; }
    }
    private bool SaveLaunchOptions()
    {
        var preference = settings.LaunchFor(RequireGame());
        var arguments = LaunchArgumentsInput.Text;
        GameLaunchArguments.Validate(arguments);
        var forceDirectX11 = DirectX11Option.IsChecked == true;
        if (preference.Arguments == arguments && preference.ForceDirectX11 == forceDirectX11) return false;
        SaveLaunchPreference(preference with { Arguments = arguments, ForceDirectX11 = forceDirectX11 });
        return true;
    }
    private void CommitLaunchArguments()
    {
        if (!ready || changingLaunchControls || busy || game is null) return;
        try { if (SaveLaunchOptions()) StatusText.Text = "启动项已自动保存。"; }
        catch (Exception error) { StatusText.Text = error.Message; Dialogs.Message(this, "启动项保存失败", error.Message); }
    }
    private void LaunchArgumentsLostFocus(object sender, RoutedEventArgs e) => CommitLaunchArguments();
    private void LaunchArgumentsKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter) { e.Handled = true; CommitLaunchArguments(); }
    }
    private void ChangeDirectX11(object sender, RoutedEventArgs e)
    {
        if (!ready || changingLaunchControls || busy || game is null) return;
        try
        {
            if (SaveLaunchOptions()) StatusText.Text = "启动设置已自动保存。";
        }
        catch (Exception error)
        {
            changingLaunchControls = true;
            try { DirectX11Option.IsChecked = settings.LaunchFor(game).ForceDirectX11; }
            finally { changingLaunchControls = false; }
            StatusText.Text = error.Message; Dialogs.Message(this, "启动项未更新", error.Message);
        }
    }
    private async void ChangeLaunchChannel(object sender, RoutedEventArgs e)
    {
        if (!ready || changingLaunchControls || busy || game is null || sender is not RadioButton { Tag: GameLaunchChannel channel }) return;
        await Run(async () =>
        {
            SaveLaunchOptions();
            SaveLaunchPreference(settings.LaunchFor(game) with { Channel = channel });
            await RefreshLaunchOptions();
            StatusText.Text = "启动渠道已保存。";
        });
    }
    private async void BrowseOfficialLauncher(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Title = "选择官方启动器", Filter = "官方启动程序 (Games.exe;Launcher.exe)|Games.exe;Launcher.exe", CheckFileExists = true };
        if (launchPlan?.OfficialLauncher is { } current && Directory.Exists(Path.GetDirectoryName(current))) dialog.InitialDirectory = Path.GetDirectoryName(current)!;
        if (dialog.ShowDialog(this) != true) return;
        await ResolveOfficialLauncher(dialog.FileName);
    }
    private async void ResolveOfficialLauncherPath(object sender, RoutedEventArgs e) => await ResolveOfficialLauncher(OfficialLauncherPathInput.Text);
    private async void OfficialLauncherPathKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter) { e.Handled = true; await ResolveOfficialLauncher(OfficialLauncherPathInput.Text); }
    }
    private void OfficialLauncherPathEdited(object sender, TextChangedEventArgs e)
    {
        if (!ready || changingLaunchControls || game is null) return;
        officialLauncherPathDirty = !string.Equals(OfficialLauncherPathInput.Text.Trim().Trim('"'), displayedOfficialLauncher, StringComparison.OrdinalIgnoreCase);
        OfficialLauncherResolvedPath.Text = officialLauncherPathDirty ? "位置已更改，请点击识别；留空可自动查找。" : launchPlan?.Detail ?? "尚未选择官方启动器";
        UpdateGameActions();
    }
    private async Task ResolveOfficialLauncher(string selected)
    {
        await Run(async () =>
        {
            SaveLaunchOptions();
            var preference = settings.LaunchFor(RequireGame());
            selected = selected.Trim().Trim('"');
            if (selected.Length == 0)
            {
                SaveLaunchPreference(preference with { OfficialLauncher = null });
                await RefreshLaunchOptions(); StatusText.Text = "已重新查找官方启动器。";
                return;
            }
            var path = await Task.Run(() => GameLaunchDiscovery.SelectOfficialLauncher(selected));
            await Task.Run(() => { using var verified = OfficialLauncherTrust.OpenVerified(path); });
            SaveLaunchPreference(preference with { Channel = GameLaunchChannel.Official, OfficialLauncher = path });
            await RefreshLaunchOptions(); StatusText.Text = "官方启动器已保存。";
        });
    }
    private async void RestoreProxies(object sender, RoutedEventArgs e)
    {
        await Run(async () =>
        {
            var root = RequireGame(); GameDirectory.RequireStopped(root);
            if (!Dialogs.Ask(this, "卸载并还原代理 DLL", $"游戏目录：\n{root}\n\n将还原 d3dcompiler_47.dll 和 vulkan-1.dll 的安装前备份；原先不存在或没有原始备份的已识别 Applepie 代理将删除。\n\nplugin 文件夹、插件配置和日志保留。还原后，Applepie 代理将不再加载插件。", "确认还原", destructive: true)) return;
            var progress = Progress();
            var result = await Task.Run(() => ProxyBackups.Restore(root, progress));
            BackupLabel.Text = result.ChangedFiles > 0 ? "本次操作备份保存在  " + result.BackupDirectory : "未找到需要还原的 Applepie 代理。";
            StatusText.Text = result.ChangedFiles > 0 ? "代理已还原，plugin 文件夹已保留。" : "无需还原代理。";
        });
    }
    private async void DeletePluginDirectory(object sender, RoutedEventArgs e)
    {
        await Run(async () =>
        {
            var root = RequireGame(); GameDirectory.RequireStopped(root);
            var folder = Files.Under(root, "plugin");
            if (!Dialogs.Ask(this, "删除整个 plugin 文件夹", $"将永久删除：\n{folder}\n\n其中所有插件（含 Manager 核心）、配置、日志及其他文件都会删除，无法撤销。游戏根目录中的代理 DLL、原始 .bak 备份及桌面管理器内的操作备份会保留。", "确认删除", destructive: true)) return;
            var removed = await Task.Run(() => PluginDirectory.Delete(root));
            installedCore = ManagerInstaller.InstalledVersion(root); InstalledVersion.Text = installedCore is null ? "尚未安装" : "v" + installedCore; BindUpdates();
            await RefreshPlugins();
            StatusText.Text = removed ? "plugin 文件夹已删除。需要完整卸载时，请同时还原代理 DLL。" : "plugin 文件夹不存在，无需删除。";
        });
    }
    private async Task RefreshPlugins()
    {
        plugins = game is null ? [] : await Task.Run(() => PluginManager.List(game));
        LocalItems.ItemsSource = plugins; PluginCount.Text = $"{plugins.Count} 个文件";
        LocalEmpty.Visibility = plugins.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        LocalEmpty.Text = game is null ? "选择游戏目录后，在这里管理你的插件。" : "尚未安装插件，可从本地文件或在线目录添加。";
        if (catalog is not null) BindCatalog();
    }
    private async void RefreshLocal(object sender, RoutedEventArgs e) => await Run(async () => { await RefreshPlugins(); StatusText.Text = "插件列表已刷新。"; });
    private async void ConfigurePlugin(object sender, RoutedEventArgs e)
    {
        if (((FrameworkElement)sender).DataContext is not PluginItem plugin) return;
        await Run(async () =>
        {
            var root = RequireGame();
            var configuration = await Task.Run(() => new BasicConfiguration(root, plugin));
            new PluginSettingsWindow(plugin, configuration) { Owner = this }.ShowDialog();
            StatusText.Text = "配置编辑已结束。";
        });
    }
    private async void TogglePlugin(object sender, RoutedEventArgs e)
    {
        if (((FrameworkElement)sender).DataContext is not PluginItem item) return;
        await Run(async () =>
        {
            var root = RequireGame(); await Task.Run(() => PluginConfiguration.SetEnabled(root, item.FileName, !item.Enabled));
            await RefreshPlugins(); StatusText.Text = $"{item.Name} 已{(item.Enabled ? "停用" : "启用")}，配置已自动保存。";
        });
    }
    private async void DeletePlugin(object sender, RoutedEventArgs e)
    {
        if (((FrameworkElement)sender).DataContext is not PluginItem item || !Dialogs.Ask(this, "删除插件", $"删除 {item.Name}（{item.FileName}）？\n插件配置会保留，被删除的文件会先备份。", "删除插件")) return;
        await Run(async () => { var root = RequireGame(); await Task.Run(() => PluginManager.Delete(root, item)); await RefreshPlugins(); StatusText.Text = "插件已删除，配置及备份已保留。"; });
    }
    private async void InstallLocal(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Title = "安装本地插件", Filter = "插件包 (*.dll;*.zip)|*.dll;*.zip", Multiselect = false };
        if (dialog.ShowDialog(this) != true) return;
        await Run(async () =>
        {
            var root = RequireGame(); var progress = Progress();
            await Task.Run(() => PluginManager.Install(root, dialog.FileName, progress: progress, cancellation: operation!.Token));
            await RefreshPlugins(); StatusText.Text = "本地插件已安装。";
        });
    }
    private async void RefreshCatalog(object sender, RoutedEventArgs e) => await LoadCatalog();
    private async Task LoadCatalog()
    {
        catalogAttempted = true;
        await Run(async () =>
        {
            CatalogStatus.Text = "正在获取目录与最新版本…";
            try { catalog = await online.GetCatalog(operation!.Token); BindCatalog(); StatusText.Text = "在线插件目录及最新 Release 已刷新。"; }
            catch (Exception error) { CatalogStatus.Text = "目录暂不可用"; OnlineEmpty.Text = error.Message; throw; }
        }, true);
    }
    private void BindCatalog()
    {
        foreach (var item in catalog!.Plugins) item.InstalledVersion = plugins.FirstOrDefault(p => p.Id == item.Id)?.Version ?? "";
        OnlineItems.ItemsSource = null; OnlineItems.ItemsSource = catalog.Plugins;
        var unavailable = catalog.Plugins.Count(p => !p.ReleaseAvailable);
        CatalogStatus.Text = $"{catalog.Plugins.Count} 个插件 · GitHub" + (unavailable == 0 ? "" : $" · {unavailable} 个暂不可用");
        OnlineEmpty.Visibility = catalog.Plugins.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        OnlineEmpty.Text = "在线目录已连接，暂时还没有发布插件。";
    }
    private async void InstallOnline(object sender, RoutedEventArgs e)
    {
        if (((FrameworkElement)sender).DataContext is not CatalogPlugin item) return;
        await Run(async () =>
        {
            var root = RequireGame(); GameDirectory.RequireStopped(root);
            if (ManagerInstaller.InstalledVersion(root) is null) throw new IOException("请先将 Applepie Manager 安装到游戏目录。");
            try { await online.RefreshPlugin(item, operation!.Token); }
            finally { BindCatalog(); }
            if (!item.CanInstall) { StatusText.Text = "当前插件已是最新版本。"; return; }
            var work = Product.NewWorkDirectory();
            try
            {
                var extension = Path.GetExtension(item.AssetName);
                var package = Path.Combine(work, extension.Equals(".dll", StringComparison.OrdinalIgnoreCase) ? item.EntryDll : "plugin.zip");
                var progress = Progress();
                await online.Download(item.DownloadUrl, package, item.Sha256, progress, operation!.Token, item.DownloadSize);
                await Task.Run(() => PluginManager.Install(root, package, item, progress, operation.Token));
                await RefreshPlugins(); StatusText.Text = $"{item.Name} v{item.Version} 已安装。";
            }
            finally { Files.RemoveWork(work); }
        }, true);
    }
    private async void CheckUpdates(object sender, RoutedEventArgs e) => await CheckUpdate(false);
    private bool HasDesktopUpdate => updates.Desktop is { } desktop && ReleaseVersion.Newer(desktop.Component.Version, Product.Version);
    private string CandidateCoreVersion => updates.Core is { } core && ReleaseVersion.Newer(core.Component.Version, availableCore.Version) ? core.Component.Version : availableCore.Version;
    private bool HasCoreUpdate => updates.Core is not null && ((game is not null && (installedCore is null or "未知版本")) || ReleaseVersion.Newer(CandidateCoreVersion, game is null ? availableCore.Version : installedCore!));
    private void UpdateCoreLabel() => BundledCoreVersion.Text = (availableCore.Directory is null ? "随包核心 v" : "可用核心 v") + availableCore.Version;
    private void BindUpdates()
    {
        UpdateTitle.Text = updates.Desktop is null ? "暂无桌面管理器发布版" : HasDesktopUpdate ? "桌面新版 v" + updates.Desktop.Component.Version : "桌面已是最新版本";
        UpdateDescription.Text = "当前 v" + Product.Version + (updates.Desktop is { } desktop ? "  /  最新 v" + desktop.Component.Version : "");
        CoreUpdateTitle.Text = updates.Core is null ? "暂无核心发布版" : HasCoreUpdate ? "核心更新 v" + CandidateCoreVersion : "核心已是最新版本";
        CoreUpdateDescription.Text = (game is null ? "尚未选择游戏目录" : "游戏内核心 " + (installedCore is null ? "尚未安装" : "v" + installedCore)) +
            "  /  可用核心 v" + availableCore.Version + (updates.Core is { } core ? "  /  最新 v" + core.Component.Version : "");
        UpdateButton.Visibility = HasDesktopUpdate ? Visibility.Visible : Visibility.Collapsed;
        CoreUpdateButton.Visibility = HasCoreUpdate ? Visibility.Visible : Visibility.Collapsed;
        CoreUpdateButton.Content = game is null ? "下载核心" : "更新游戏内核心";
        UpdateAllButton.Visibility = HasDesktopUpdate && HasCoreUpdate ? Visibility.Visible : Visibility.Collapsed;
        ReleaseWarnings.Text = string.Join("\n", updates.Warnings.Select(w => "无法使用的发布清单：" + w));
    }
    private async Task CheckUpdate(bool prompt)
    {
        if (checking) return; checking = true;
        try
        {
            UpdateTitle.Text = "正在检查更新…";
            updates = await online.Latest(); BindUpdates();
            if (prompt && !busy && (HasDesktopUpdate || HasCoreUpdate)) await ConfirmUpdate(HasDesktopUpdate, HasCoreUpdate);
        }
        catch (Exception error) { updates = new(); BindUpdates(); UpdateTitle.Text = CoreUpdateTitle.Text = "暂时无法检查更新"; UpdateDescription.Text = error.Message; }
        finally { checking = false; }
    }
    private async void UpdateApplication(object sender, RoutedEventArgs e) => await ConfirmUpdate(true, false);
    private async void UpdateCore(object sender, RoutedEventArgs e) => await ConfirmUpdate(false, true);
    private async void UpdateAll(object sender, RoutedEventArgs e) => await ConfirmUpdate(true, true);
    private async Task ConfirmUpdate(bool desktop, bool core)
    {
        desktop &= HasDesktopUpdate; core &= HasCoreUpdate;
        if (!desktop && !core) return;
        var message = desktop ? $"桌面管理器 v{updates.Desktop!.Component.Version}，更新完成后重新打开。\n" : "";
        if (core) message += $"核心 v{CandidateCoreVersion}，" + (game is null ? "下载后保存在本机，选择游戏目录后可部署。" : "将备份并更新到已选游戏目录，请先退出游戏。");
        if (Dialogs.Ask(this, "更新 Applepie Manager", message, "开始更新")) await PerformUpdate(desktop, core);
    }
    private async Task PerformUpdate(bool desktop, bool core)
    {
        await Run(async () =>
        {
            var root = core && game is not null ? RequireGame() : null;
            var desktopUpdate = updates.Desktop; var coreUpdate = updates.Core;
            if (root is not null) GameDirectory.RequireStopped(root);
            string? source = null;
            try
            {
                var progress = Progress();
                if (desktop) source = await SelfUpdate.Prepare(online, desktopUpdate!, progress, operation!.Token);
                if (core)
                {
                    availableCore = await corePackages.Download(online, coreUpdate!, progress, operation!.Token); UpdateCoreLabel();
                    if (root is not null)
                    {
                        operation.Token.ThrowIfCancellationRequested();
                        CancelButton.Visibility = Visibility.Collapsed;
                        var result = await Task.Run(() => ManagerInstaller.Install(root, availableCore, progress));
                        installedCore = ManagerInstaller.InstalledVersion(root); InstalledVersion.Text = "v" + installedCore;
                        if (result.ChangedFiles > 0) BackupLabel.Text = "备份保存在  " + result.BackupDirectory;
                    }
                    BindUpdates(); StatusText.Text = root is null ? "核心已下载，可在部署页安装到游戏。" : "游戏内核心已更新。";
                }
                if (desktop)
                {
                    SelfUpdate.Start(source!); source = null;
                    Busy(false); Application.Current.Shutdown();
                }
            }
            finally { if (source is not null) Files.RemoveWork(Path.GetDirectoryName(source)!); }
        }, true);
    }
    private void CancelOperation(object sender, RoutedEventArgs e) => operation?.Cancel();
}

public static class Dialogs
{
    private static (Window Window, StackPanel Content) Create(Window owner, string title)
    {
        var window = new Window { Owner = owner, Title = "Applepie Manager", Width = 560, SizeToContent = SizeToContent.Height, MaxHeight = 650, ResizeMode = ResizeMode.NoResize, WindowStartupLocation = WindowStartupLocation.CenterOwner, ShowInTaskbar = false };
        var content = new StackPanel { Margin = new Thickness(28) };
        content.Children.Add(new TextBlock { Text = title, FontSize = 22, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 0, 0, 18) }); window.Content = content;
        return (window, content);
    }
    public static bool Ask(Window owner, string title, string text, string accept, bool destructive = false)
    {
        var (window, content) = Create(owner, title);
        content.Children.Add(new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, LineHeight = 24 });
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 24, 0, 0) };
        var no = new Button { Content = "取消", IsCancel = true, IsDefault = destructive }; no.Click += (_, _) => window.DialogResult = false;
        var yes = new Button { Content = accept, Style = (Style)owner.FindResource("PrimaryButton"), Margin = new Thickness(10, 0, 0, 0), IsDefault = !destructive }; yes.Click += (_, _) => window.DialogResult = true;
        buttons.Children.Add(no); buttons.Children.Add(yes); content.Children.Add(buttons); return window.ShowDialog() == true;
    }
    public static void Message(Window owner, string title, string text)
    {
        var (window, content) = Create(owner, title);
        content.Children.Add(new ScrollViewer { MaxHeight = 420, Content = new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, LineHeight = 23 } });
        var button = new Button { Content = "知道了", Style = (Style)owner.FindResource("PrimaryButton"), IsDefault = true, IsCancel = true, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 22, 0, 0) };
        button.Click += (_, _) => window.Close(); content.Children.Add(button); window.ShowDialog();
    }
    public static string? Choose(Window owner, IReadOnlyList<string> paths)
    {
        var (window, content) = Create(owner, "发现多个游戏目录");
        content.Children.Add(new TextBlock { Text = "请选择要管理的那一份游戏：", Margin = new Thickness(0, 0, 0, 14) });
        var list = new ListBox { ItemsSource = paths, SelectedIndex = 0, MaxHeight = 300, Padding = new Thickness(10) };
        list.SetResourceReference(Control.BackgroundProperty, "Panel"); list.SetResourceReference(Control.ForegroundProperty, "Ink");
        content.Children.Add(list); var button = new Button { Content = "使用此目录", Style = (Style)owner.FindResource("PrimaryButton"), Margin = new Thickness(0, 18, 0, 0) };
        button.Click += (_, _) => window.DialogResult = true; content.Children.Add(button);
        return window.ShowDialog() == true ? list.SelectedItem as string : null;
    }
}
