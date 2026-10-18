using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;

namespace Applepie.Launcher;

public partial class App : Application
{
    private AppInstance? instance = null;
    public App()
    {
        // This settings UI does not need GPU acceleration. Keep its rendering
        // independent of adapter changes when moving between monitors.
        RenderOptions.ProcessRenderMode = RenderMode.SoftwareOnly;
    }
    protected override async void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        DispatcherUnhandledException += (_, error) => { MessageBox.Show(error.Exception.Message, "Applepie Manager", MessageBoxButton.OK, MessageBoxImage.Error); error.Handled = true; };
        try
        {
            if (e.Args.Length == 2 && e.Args[0] == "--apply-update") { await SelfUpdate.Apply(e.Args[1]); Shutdown(); return; }
            if (e.Args.Contains("--uninstall"))
            {
                if (MessageBox.Show("卸载桌面管理器？已安装到游戏目录的 Manager、插件和配置将保留。", "Applepie Manager", MessageBoxButton.OKCancel, MessageBoxImage.Question) == MessageBoxResult.OK)
                    SelfUpdate.Start("", true);
                Shutdown(); return;
            }
            Appearance.RestorePreference();
#if SETUP
            var window = new SetupWindow();
#else
            instance = AppInstance.Acquire();
            var window = new MainWindow();
#endif
            MainWindow = window; ShutdownMode = ShutdownMode.OnMainWindowClose; window.Show();
        }
        catch (Exception error)
        {
            if (e.Args.Length == 2 && e.Args[0] == "--apply-update")
            {
                try { Files.WriteAtomic(e.Args[1] + ".error.txt", System.Text.Encoding.UTF8.GetBytes(error.ToString())); }
                catch (Exception) { /* Keep the original failure visible if diagnostics cannot be written. */ }
            }
            MessageBox.Show(error.Message, "Applepie Manager", MessageBoxButton.OK, MessageBoxImage.Error); Shutdown(1);
        }
    }
    protected override void OnExit(ExitEventArgs e) { instance?.Dispose(); base.OnExit(e); }
}
