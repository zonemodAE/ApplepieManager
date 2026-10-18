using System.Collections.Concurrent;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace Applepie.Launcher;

internal static class OfficialLauncherTrust
{
    // Keep the image locked through verification and process creation.
    internal static FileStream OpenVerified(string path)
    {
        if (!Path.GetFileName(path).Equals("Games.exe", StringComparison.OrdinalIgnoreCase))
            throw new IOException("请选择官方 Games.exe。");
        Files.RejectLinks(path);
        var image = File.Open(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        try
        {
            Verify(path, image.SafeFileHandle.DangerousGetHandle());
            return image;
        }
        catch { image.Dispose(); throw; }
    }

    private static void Verify(string path, IntPtr handle)
    {
        var file = new TrustFile { Size = (uint)Marshal.SizeOf<TrustFile>(), Path = path, Handle = handle };
        var pointer = Marshal.AllocHGlobal(Marshal.SizeOf<TrustFile>());
        Marshal.StructureToPtr(file, pointer, false);
        var data = new TrustData { Size = (uint)Marshal.SizeOf<TrustData>(), UiChoice = 2, UnionChoice = 1, File = pointer, StateAction = 1, ProviderFlags = 0x80 };
        var action = new Guid("00AAC56B-CD44-11d0-8CC2-00C04FC295EE");
        try
        {
            var status = WinVerifyTrust(new IntPtr(-1), ref action, ref data);
            if (status != 0) throw new IOException($"Games.exe 数字签名验证失败（0x{status:X8}），请更新官方启动器后重试。\n{path}");
#pragma warning disable SYSLIB0057
            using var signer = new X509Certificate2(X509Certificate.CreateFromSignedFile(path));
#pragma warning restore SYSLIB0057
            var publisher = signer.GetNameInfo(X509NameType.SimpleName, false);
            if (!publisher.Equals("GRYPH FRONTIER PTE. LTD.", StringComparison.OrdinalIgnoreCase) &&
                !publisher.Equals("Shanghai Hypergryph Network Technology Co., Ltd.", StringComparison.OrdinalIgnoreCase))
                throw new IOException("Games.exe 的签名发行者不是官方，请重新选择官方启动器。\n" + path);
        }
        catch (CryptographicException error) { throw new IOException("无法读取 Games.exe 的官方签名。", error); }
        finally
        {
            data.StateAction = 2; WinVerifyTrust(new IntPtr(-1), ref action, ref data);
            Marshal.DestroyStructure<TrustFile>(pointer); Marshal.FreeHGlobal(pointer);
        }
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct TrustFile { public uint Size; [MarshalAs(UnmanagedType.LPWStr)] public string Path; public IntPtr Handle, KnownSubject; }
    [StructLayout(LayoutKind.Sequential)]
    private struct TrustData
    {
        public uint Size; public IntPtr PolicyCallback, SipClient;
        public uint UiChoice, RevocationChecks, UnionChoice; public IntPtr File;
        public uint StateAction; public IntPtr StateData, Url;
        public uint ProviderFlags, UiContext; public IntPtr SignatureSettings;
    }
    [DllImport("wintrust.dll", ExactSpelling = true)]
    private static extern int WinVerifyTrust(IntPtr window, ref Guid action, ref TrustData data);
}

// Uses only handles returned by our CreateProcess calls. Never finds or stops a
// user's existing launcher. Completion is signalled by the OS, without polling.
internal static class OfficialGameProcess
{
    private static readonly ConcurrentDictionary<Session, byte> Sessions = new();
    static OfficialGameProcess() => AppDomain.CurrentDomain.ProcessExit += (_, _) => StopAll();
    internal static void StopAll() { foreach (var session in Sessions.Keys) session.Stop(); }

    internal static Task Start(string game, string parent, string arguments = "")
    {
        GameLaunchArguments.Validate(arguments);
        var root = Path.GetDirectoryName(game)!;
        var startup = new StartupInfoEx { Info = new StartupInfo { Size = Marshal.SizeOf<StartupInfo>() } };
        var parentInfo = Create(parent, Path.GetDirectoryName(parent)!, 0x00000004 | 0x08000000, ref startup);
        CloseHandle(parentInfo.Thread);
        var parentHandle = new SafeProcessHandle(parentInfo.Process, true);
        IntPtr attributes = IntPtr.Zero, value = IntPtr.Zero;
        bool initialized = false, adopted = false;
        try
        {
            nuint size = 0;
            InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref size);
            if (size == 0) throw new Win32Exception(Marshal.GetLastWin32Error());
            attributes = Marshal.AllocHGlobal(checked((int)size));
            if (!InitializeProcThreadAttributeList(attributes, 1, 0, ref size)) throw new Win32Exception(Marshal.GetLastWin32Error());
            initialized = true;
            value = Marshal.AllocHGlobal(IntPtr.Size);
            Marshal.WriteIntPtr(value, parentHandle.DangerousGetHandle());
            // PROC_THREAD_ATTRIBUTE_PARENT_PROCESS; see Windows STARTUPINFOEX.
            if (!UpdateProcThreadAttribute(attributes, 0, 0x00020000, value, (nuint)IntPtr.Size, IntPtr.Zero, IntPtr.Zero))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            startup = new() { Info = new StartupInfo { Size = Marshal.SizeOf<StartupInfoEx>() }, Attributes = attributes };
            var child = Create(game, root, 0x00080000, ref startup, arguments);
            CloseHandle(child.Thread);
            var session = new Session(parentHandle, child.Process);
            adopted = true;
            return session.Observe();
        }
        finally
        {
            if (initialized) DeleteProcThreadAttributeList(attributes);
            if (attributes != IntPtr.Zero) Marshal.FreeHGlobal(attributes);
            if (value != IntPtr.Zero) Marshal.FreeHGlobal(value);
            if (!adopted) { TerminateProcess(parentHandle, 0); parentHandle.Dispose(); }
        }
    }

    private static ProcessInformation Create(string executable, string directory, uint flags, ref StartupInfoEx startup, string arguments = "")
    {
        if (executable.IndexOfAny(['"', '\0']) >= 0) throw new IOException("启动路径含有无效字符。");
        var command = new StringBuilder('"' + executable + '"');
        if (arguments.Length > 0) command.Append(' ').Append(arguments);
        if (command.Length >= 32767) throw new IOException("启动命令超过 Windows 的长度限制。");
        if (!CreateProcessW(executable, command, IntPtr.Zero, IntPtr.Zero,
                false, flags, IntPtr.Zero, directory, ref startup, out var process))
        {
            var error = Marshal.GetLastWin32Error();
            throw new Win32Exception(error, $"无法启动 {Path.GetFileName(executable)}：{new Win32Exception(error).Message}");
        }
        return process;
    }

    private sealed class ProcessSignal : WaitHandle
    {
        internal ProcessSignal(IntPtr handle) => SafeWaitHandle = new SafeWaitHandle(handle, true);
    }
    private sealed class Session(SafeProcessHandle parent, IntPtr child)
    {
        private readonly object gate = new();
        private readonly ProcessSignal signal = new(child);
        private readonly TaskCompletionSource completion = new(TaskCreationOptions.RunContinuationsAsynchronously);
        private RegisteredWaitHandle? registration;
        private bool stopped;
        internal Task Observe()
        {
            try
            {
                lock (gate)
                {
                    Sessions.TryAdd(this, 0);
                    registration = ThreadPool.RegisterWaitForSingleObject(signal, (_, _) => Stop(), null, Timeout.Infinite, true);
                }
                return completion.Task;
            }
            catch { Stop(); throw; }
        }
        internal void Stop()
        {
            lock (gate)
            {
                if (stopped) return;
                stopped = true; Sessions.TryRemove(this, out _);
                registration?.Unregister(null);
                TerminateProcess(parent, 0); parent.Dispose(); signal.Dispose();
                completion.TrySetResult();
            }
        }
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct StartupInfo
    {
        public int Size; public IntPtr Reserved, Desktop, Title;
        public uint X, Y, XSize, YSize, XChars, YChars, FillAttribute, Flags;
        public ushort ShowWindow, ReservedSize; public IntPtr ReservedBytes, StdInput, StdOutput, StdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct StartupInfoEx { public StartupInfo Info; public IntPtr Attributes; }
    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessInformation { public IntPtr Process, Thread; public uint ProcessId, ThreadId; }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateProcessW(string application, StringBuilder command, IntPtr processAttributes, IntPtr threadAttributes,
        [MarshalAs(UnmanagedType.Bool)] bool inherit, uint flags, IntPtr environment, string directory, ref StartupInfoEx startup, out ProcessInformation information);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool InitializeProcThreadAttributeList(IntPtr list, int count, uint flags, ref nuint size);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UpdateProcThreadAttribute(IntPtr list, uint flags, nuint attribute, IntPtr value, nuint size, IntPtr previous, IntPtr returnedSize);
    [DllImport("kernel32.dll")]
    private static extern void DeleteProcThreadAttributeList(IntPtr list);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool TerminateProcess(SafeProcessHandle process, uint code);
    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);
}
