using System.Diagnostics;
using System.Windows;
using Microsoft.Win32;

namespace Applepie.Launcher;
public partial class SetupWindow : Window
{
    private bool busy, installed;
    public SetupWindow()
    {
        InitializeComponent();
        var existing = DesktopInstaller.FindInstallation();
        Destination.Text = existing?.Directory ?? DesktopInstaller.DefaultDirectory;
        if (existing is not null)
        {
            DesktopShortcut.IsChecked = existing.DesktopShortcut;
            SetupStatus.Text = $"已找到桌面管理器 v{existing.Version}，将沿用此位置安装 v{Product.Version}。";
            Install.Content = "更新 Applepie Manager  ↗";
        }
        Closing += (_, e) => { if (busy) e.Cancel = true; };
    }
    private void ToggleTheme(object sender, RoutedEventArgs e) => Appearance.Toggle(this);
    private void Browse(object sender, RoutedEventArgs e)
    {
        if (busy || installed) return;
        var dialog = new OpenFolderDialog { Title = "选择桌面管理器的安装位置", InitialDirectory = Directory.Exists(Destination.Text) ? Destination.Text : "" };
        if (dialog.ShowDialog(this) == true)
        {
            var existing = DesktopInstaller.FindInstallation([dialog.FolderName]);
            Destination.Text = existing?.Directory ?? (Path.GetFileName(dialog.FolderName).Equals("ApplepieManager", StringComparison.OrdinalIgnoreCase)
                ? dialog.FolderName : Path.Combine(dialog.FolderName, "ApplepieManager"));
            if (existing is not null) DesktopShortcut.IsChecked = existing.DesktopShortcut;
        }
    }
    private async void InstallClick(object sender, RoutedEventArgs e)
    {
        if (installed) { Process.Start(new ProcessStartInfo(Path.Combine(Destination.Text, "ApplepieManager.exe")) { UseShellExecute = true }); Close(); return; }
        if (busy) return;
        busy = true; Install.IsEnabled = Destination.IsEnabled = DesktopShortcut.IsEnabled = false; SetupProgress.Visibility = Visibility.Visible; SetupProgress.IsIndeterminate = true;
        string? source = null;
        try
        {
            var destination = Path.GetFullPath(Destination.Text); var shortcut = DesktopShortcut.IsChecked == true;
            var progress = new Progress<ProgressInfo>(p => SetupStatus.Text = p.Message);
            SetupStatus.Text = "正在解包并校验…";
            source = await Task.Run(DesktopInstaller.ExtractBundle);
            await Task.Run(() => DesktopInstaller.Install(source, destination, shortcut, progress));
            Destination.Text = destination; installed = true; SetupStatus.Text = "安装完成。打开管理器，即可继续部署到游戏。"; Install.Content = "打开 Applepie Manager  ↗";
        }
        catch (Exception error) { SetupStatus.Text = error.Message; Dialogs.Message(this, "安装未完成", error.Message); }
        finally { if (source is not null) Files.RemoveWork(Path.GetDirectoryName(source)!); busy = false; Install.IsEnabled = true; Destination.IsEnabled = DesktopShortcut.IsEnabled = !installed; SetupProgress.Visibility = Visibility.Collapsed; }
    }
}
