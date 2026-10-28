param([string]$DllPath)
$ErrorActionPreference = 'Stop'
if (-not $DllPath) {
    $projectRoot = Split-Path $PSScriptRoot -Parent
    $DllPath = Join-Path $projectRoot 'build/windows-vs2026/AmbiBINOrthobilateral_artefacts/Release/VST/AmbiBIN Orthobilateral.dll'
}
$DllPath = (Resolve-Path -LiteralPath $DllPath).Path
# Minimal loader check, not a substitute for host audio/session tests.
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class AmbiBinVstLoadCheck {
    [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
    [DllImport("kernel32")] static extern IntPtr GetProcAddress(IntPtr handle, string name);
    [DllImport("kernel32", CharSet=CharSet.Unicode)] static extern IntPtr GetModuleHandle(string name);
    [DllImport("kernel32", CharSet=CharSet.Unicode)] static extern uint GetModuleFileName(IntPtr handle, StringBuilder path, int length);
    [DllImport("kernel32")] static extern bool FreeLibrary(IntPtr handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    delegate IntPtr Host(IntPtr effect, int opcode, int index, IntPtr value, IntPtr ptr, float opt);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr Entry(Host host);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    delegate IntPtr Dispatch(IntPtr effect, int opcode, int index, IntPtr value, IntPtr ptr, float opt);
    [StructLayout(LayoutKind.Sequential)] struct Effect {
        public int magic;
        public IntPtr dispatcher, process, setParameter, getParameter;
        public int programs, parameters, inputs, outputs, flags;
        public IntPtr reserved1, reserved2;
        public int delay, realQualities, offQualities;
        public float ratio;
        public IntPtr obj, user;
        public int uniqueID, version;
    }
    public static string Run(string path) {
        IntPtr library = LoadLibraryEx(path, IntPtr.Zero, 8); // search beside DLL
        if (library == IntPtr.Zero) throw new Exception("Load failed: Win32 " + Marshal.GetLastWin32Error());
        Host host = (e, op, ix, v, p, opt) => new IntPtr(op == 1 ? 2400 : op == 16 ? 48000 : op == 17 ? 128 : 0);
        IntPtr effect = IntPtr.Zero;
        Dispatch dispatch = null;
        try {
            IntPtr address = GetProcAddress(library, "VSTPluginMain");
            if (address == IntPtr.Zero) throw new Exception("Missing VSTPluginMain");
            var entry = (Entry)Marshal.GetDelegateForFunctionPointer(address, typeof(Entry));
            effect = entry(host);
            if (effect == IntPtr.Zero) throw new Exception("Null VST instance");
            var info = (Effect)Marshal.PtrToStructure(effect, typeof(Effect));
            dispatch = (Dispatch)Marshal.GetDelegateForFunctionPointer(info.dispatcher, typeof(Dispatch));
            dispatch(effect, 0, 0, IntPtr.Zero, IntPtr.Zero, 0); // effOpen
            IntPtr nameBuffer = Marshal.AllocHGlobal(256);
            string name;
            try {
                Marshal.WriteByte(nameBuffer, 0);
                dispatch(effect, 45, 0, IntPtr.Zero, nameBuffer, 0); // effGetEffectName
                name = Marshal.PtrToStringAnsi(nameBuffer);
            }
            finally { Marshal.FreeHGlobal(nameBuffer); }
            if (name != "AmbiBIN Orthobilateral")
                throw new Exception("Unexpected plug-in name: " + name);
            // JUCE advertises maximum wrapper channel counts before host negotiation.
            if (info.magic != 0x56737450 || info.uniqueID != 0x41424249 ||
                info.inputs < 2 || info.outputs < 2 || info.parameters != 15)
                throw new Exception("Unexpected VST identity, parameter count, or channel capacity");
            var runtimePath = new StringBuilder(32768);
            GetModuleFileName(GetModuleHandle("saf_mkl_custom_lp64.dll"), runtimePath, runtimePath.Capacity);
            return String.Format("PASS VST load/open/close: {0}, ID={1:X8}, advertised I/O={2}/{3}, parameters={4}\nMKL runtime: {5}",
                name, info.uniqueID, info.inputs, info.outputs, info.parameters, runtimePath);
        }
        finally {
            if (effect != IntPtr.Zero && dispatch != null)
                dispatch(effect, 1, 0, IntPtr.Zero, IntPtr.Zero, 0); // effClose
            GC.KeepAlive(host);
            FreeLibrary(library);
        }
    }
}
'@
[AmbiBinVstLoadCheck]::Run($DllPath)
