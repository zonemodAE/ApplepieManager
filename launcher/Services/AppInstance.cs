namespace Applepie.Launcher;

// Launcher, setup, and the update helper share one gate. No background polling;
// the helper acquires it only after the parent process has actually exited.
public sealed class AppInstance : IDisposable
{
    private readonly Mutex mutex;
    private AppInstance(Mutex value) => mutex = value;
    public static AppInstance Acquire()
    {
        var mutex = new Mutex(false, @"Local\ApplepieManager.Desktop." + Environment.UserName);
        bool owned;
        try { owned = mutex.WaitOne(0); }
        catch (AbandonedMutexException) { owned = true; }
        if (!owned) { mutex.Dispose(); throw new IOException("Applepie Manager 正在运行或处理安装。请先关闭另一窗口再继续。"); }
        return new(mutex);
    }
    public void Dispose() { mutex.ReleaseMutex(); mutex.Dispose(); }
}
