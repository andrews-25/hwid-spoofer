# check.ps1 - asks the running driver what it faked (no admin needed).
# Requires the driver installed AND a reboot since installing it.
Add-Type -Language CSharp @"
using System;
using System.Runtime.InteropServices;

public class HwidCheck {
    [StructLayout(LayoutKind.Sequential)]
    public struct Status {
        public uint FakeCount;
        public uint BiosSerials;
        public uint BoardSerials;
        public uint Uuids;
        public uint Major;
        public uint Minor;
    }

    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern IntPtr CreateFileW(string name, uint access, uint share,
        IntPtr sec, uint disp, uint flags, IntPtr attr);

    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, IntPtr inBuf,
        uint inSize, out Status st, uint outSize, IntPtr written, IntPtr ov);

    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool CloseHandle(IntPtr h);

    public static Status Query() {
        IntPtr h = CreateFileW("\\\\.\\HwidSpoof", 0xC0000000, 3,
            IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (h == (IntPtr)(-1))
            throw new Exception("driver not running - install + reboot first");
        Status st;
        // IOCTL_GET_STATUS = CTL_CODE(0x8F5A, 0x800, METHOD_BUFFERED, IO_NONE)
        bool ok = DeviceIoControl(h, (uint)((0x8F5Au << 16) | (0x800u << 2)),
            IntPtr.Zero, 0, out st,
            (uint)Marshal.SizeOf(typeof(Status)), IntPtr.Zero, IntPtr.Zero);
        CloseHandle(h);
        if (!ok)
            throw new Exception("IOCTL failed: " + Marshal.GetLastWin32Error());
        return st;
    }
}
"@

$s = [HwidCheck]::Query()
Write-Host ('Faked identity values : ' + $s.FakeCount)
Write-Host ('  BIOS serials        : ' + $s.BiosSerials)
Write-Host ('  Board serials       : ' + $s.BoardSerials)
Write-Host ('  SMBIOS UUIDs        : ' + $s.Uuids)
if ($s.FakeCount -eq 0) {
    Write-Host 'WARNING: the driver loaded but faked nothing - this board'
    Write-Host 'layout (SMBIOS 3.x-only firmware) is not supported yet.'
    Write-Host 'Ask the maintainer for a SMBIOS-3.x build (see README).'
}
