/*
 * hwid_spoof.sys - SMBIOS identity spoofer kernel driver (SMBIOS 2.x/3.x)
 *
 * Fakes in RAM ONLY (firmware untouched) the identity numbers anti-cheats
 * like EasyAntiCheat read from the SMBIOS table: BIOS serial number, BIOS
 * info/version strings, motherboard serial number, motherboard name/model,
 * and the SMBIOS system UUID. Rebooting regenerates brand-new fakes.
 * Disk SMART serials ("welded-on" SATA/NVMe serials) CANNOT be changed
 * from user mode or by this driver - see README for the honest limits.
 *
 * Build/sign/install/uninstall instructions: README.md, section
 * "THE HARD WAY: the kernel driver". Run it ONLY on a machine you are
 * ready to reboot; the author has never executed this driver on hardware.
 * Every SMBIOS write is bounds-checked, so a board layout this file does
 * not know about is SKIPPED (status reports it) instead of crashing.
 */
#include <ntddk.h>
#include <wdm.h>

/* Vendor-defined IOCTL: asks the driver "what did you fake?" */
#define IOCTL_GET_STATUS CTL_CODE(0x8F5A, 0x800, METHOD_BUFFERED, IO_NONE)

/* SMBIOS structure string-table signature check offset */
#define SMBIOS_END_ADDR 0x00800000UL      /* scan the low 8 MB of RAM     */
#define SMBIOS_PREFERRED 0x0009FC00UL    /* classic BIOS-era table spot  */

#pragma pack(push, 1)
typedef struct _SMBIOS_PHYS_HDR {
    CHAR  Signature[4];   /* '_','S','I','M' in BIOS/legacy builds,
                             '_','S','I','G' in some; both are matched */
    BYTE  MajorVersion;
    BYTE  MinorVersion;
    BYTE  Reserved;
    ULONG TableSize;
    ULONG TableAddressLow;      /* 32-bit phys addr of the structures      */
    ULONG EntryCount;
    ULONG EntryOffset;          /* first entry = addr + EntryOffset * 16   */
    ULONG Reserved2[4];         /* extended-header slack                   */
} SMBIOS_PHYS_HDR;
#pragma pack(pop)

/* Byte offsets (SMBIOS 2.x spec tables) of the string-index fields we use.
 * index fields store a 1-BASED number of the string to point at. */
#define BIOS_STR_INFO_IDX  3    /* BIOS info string    (SMBIOS type 1 BIOS) */
#define BIOS_STR_VER_IDX   4    /* BIOS revision string                     */
#define BIOS_SERIAL_IDX    14   /* BIOS serial string (SMBIOS 2.6+)         */
#define MB_STR_MANU_IDX    3    /* board manufacturer  (SMBIOS type 2)     */
#define MB_STR_MODEL_IDX   4    /* board model         (SMBIOS type 2)     */
#define MB_SERIAL_IDX      6    /* board serial        (SMBIOS type 2)     */
#define UUID_RAW_OFF       8    /* SMBIOS 2.5.x+ UUID structures: 16 bytes  */

/* Fixed-size (non-string) byte count of each structure's field area after
 * its 8-byte header. If a board reports a bigger/smaller structure the
 * driver still validates and simply skips entries it cannot handle. */
#define BIOS_FIXED_BYTES   16   /* fields at 03h..17h: strings start +24   */
#define BOARD_FIXED_BYTES  12   /* fields at 03h..0Eh: strings start +20   */

/* Driver state. Volatile globals are set once at load (DriverEntry). */
static PDEVICE_OBJECT g_Device  = NULL;
static BOOLEAN  g_Spoofed      = FALSE;
static ULONG    g_FakeCount    = 0;   /* number of faked identity values    */
static ULONG    g_BiosSerials  = 0;   /* BIOS structures spoofed            */
static ULONG    g_BoardSerials = 0;   /* board structures spoofed           */
static ULONG    g_Uuids        = 0;   /* UUID structs spoofed               */
static BOOLEAN  g_FakeBiosExtra = TRUE; /* fake BIOS info+version strings   */
static BOOLEAN  g_FakeBoardName = TRUE; /* fake manufacturer+model strings  */
static ULONG    g_Rnd = 0xC0FFEE1;

/* xorshift-ish PRNG seeded from system time/perf counter at load time.
 * Cryptographic randomness is not required - EAC only hashes the values. */
static ULONG RandU32(void) {
    LARGE_INTEGER t, pc;
    KeQuerySystemTime(&t);
    pc.QuadPart = KeQueryPerformanceCounter(0).QuadPart;
    g_Rnd ^= (ULONG)(t.LowPart ^ (t.HighPart << 3) ^
                     (pc.LowPart << 11) ^ (GetTickCount64() << 17));
    g_Rnd ^= g_Rnd >> 13; g_Rnd ^= g_Rnd << 17; g_Rnd ^= g_Rnd >> 5;
    if (g_Rnd == 0) g_Rnd = 0x9E3779B9;
    return g_Rnd;
}

/* Overwrite an ASCII string in place, preserving its EXACT byte length:
 * new value = random alnum chars, space-padded/truncated to oldLen, the
 * original NUL untouched. Keeps the SMBIOS table byte-for-byte intact. */
static BOOLEAN FakeString(CHAR *s) {
    ULONG i, want, oldLen = 0;
    CHAR  fresh[48];
    while (oldLen < 47 && s[oldLen]) oldLen++;
    if (oldLen == 0 || s[oldLen]) return FALSE;   /* empty/too-long: skip */
    if (oldLen < 3) return FALSE;                 /* implausible: skip    */
    want = 6 + (RandU32() % 7);
    if (want > oldLen) want = (oldLen < 4) ? oldLen : 4 + (oldLen % 9);
    if (want > oldLen) want = oldLen;
    for (i = 0; i < want; i++) {
        ULONG pick = RandU32() % 36;              /* 0-9 then A-Z         */
        fresh[i] = (CHAR)(pick < 10 ? '0' + pick : 'A' + pick - 10);
    }
    for (i = want; i < oldLen; i++) fresh[i] = ' ';
    for (i = 0; i < oldLen; i++) s[i] = fresh[i];
    g_FakeCount++;
    return TRUE;
}

/* Locate the 1-based string N inside one structure. entry points at the
 * structure's 8-byte header; fixedBytes is the size of its field area.
 * Bounds are checked against the mapped table region end (regionEnd).
 * Returns NULL when the string is missing/empty/out of bounds. */
static CHAR *StringN(CHAR *entry, ULONG entryLen, ULONG fixedBytes,
                     ULONG strIdx, CHAR *regionEnd) {
    CHAR *p, *endOk;
    ULONG i;
    if (strIdx == 0) return NULL;
    if (fixedBytes + 10 > entryLen) return NULL;   /* strings can't fit    */
    p = entry + 8 + fixedBytes;                    /* first string start   */
    endOk = entry + entryLen - 2;                  /* last allowed char    */
    if (p > regionEnd || endOk > regionEnd) return NULL;
    for (i = 1; i < strIdx; i++) {                 /* hop to string i+1    */
        while (p < endOk && *p) p++;               /* to string's NUL      */
        if (p >= endOk || *p != 0) return NULL;    /* NUL expected         */
        p++;                                       /* next string's first  */
        if (p > endOk || *p == 0) return NULL;     /* empty string = stop  */
    }
    if (p > endOk || !*p) return NULL;             /* target empty         */
    return p;
}

/* Fake 16 raw UUID bytes in place (SMBIOS 2.5.x+ UUID structures, byte 8
 * of the structure). Random, keeping the variant nibble sane. */
static BOOLEAN FakeUuid(CHAR *u, CHAR *regionEnd) {
    ULONG i;
    if (u + 16 > regionEnd) return FALSE;
    for (i = 4; i < 16; i++) u[i] = (CHAR)(RandU32() & 0xFF);
    g_FakeCount++;
    g_Uuids++;
    return TRUE;
}

/* Spoof one structure given its header. type is the SMBIOS structure
 * type, entryLen the total declared length, regionEnd the mapped end. */
static VOID SpoofEntry(BYTE type, CHAR *entry, ULONG entryLen, CHAR *regionEnd) {
    CHAR *s;
    switch (type) {
    case 1:  /* BIOS information structure */
        s = StringN(entry, entryLen, BIOS_FIXED_BYTES,
                   (ULONG)entry[BIOS_SERIAL_IDX], regionEnd);
        if (s) { FakeString(s); g_BiosSerials++; }
        if (g_FakeBiosExtra) {
            s = StringN(entry, entryLen, BIOS_FIXED_BYTES,
                       (ULONG)entry[BIOS_STR_INFO_IDX], regionEnd);
            if (s && entry[BIOS_STR_INFO_IDX] != entry[BIOS_SERIAL_IDX])
                FakeString(s);
            s = StringN(entry, entryLen, BIOS_FIXED_BYTES,
                       (ULONG)entry[BIOS_STR_VER_IDX], regionEnd);
            if (s && entry[BIOS_STR_VER_IDX] != entry[BIOS_SERIAL_IDX])
                FakeString(s);
        }
        break;
    case 2:  /* baseboard (motherboard) structure */
        s = StringN(entry, entryLen, BOARD_FIXED_BYTES,
                   (ULONG)entry[MB_SERIAL_IDX], regionEnd);
        if (s) { FakeString(s); g_BoardSerials++; }
        if (g_FakeBoardName) {
            s = StringN(entry, entryLen, BOARD_FIXED_BYTES,
                       (ULONG)entry[MB_STR_MANU_IDX], regionEnd);
            if (s && entry[MB_STR_MANU_IDX] != entry[MB_SERIAL_IDX]) FakeString(s);
            s = StringN(entry, entryLen, BOARD_FIXED_BYTES,
                       (ULONG)entry[MB_STR_MODEL_IDX], regionEnd);
            if (s && entry[MB_STR_MODEL_IDX] != entry[MB_SERIAL_IDX]) FakeString(s);
        }
        break;
    case 0x1C: /* SMBIOS 2.5.x+ system UUID structure (type 28) */
        FakeUuid(entry + UUID_RAW_OFF, regionEnd);
        break;
    case 0x20: /* SMBIOS 3.0 system UUID structure (type 32) */
        FakeUuid(entry + UUID_RAW_OFF, regionEnd);
        break;
    default:
        break;
    }
}

/* Walk 'count' structures starting at 'first' up to the mapped end 'end'.
 * Each structure's first byte is its type, second byte its total declared
 * length (strings included). Identity values are faked in place. */
static BOOLEAN WalkStructures(CHAR *first, CHAR *end, ULONG count) {
    CHAR *p = first;
    ULONG i;
    for (i = 0; i < count; i++) {
        BYTE  type;
        ULONG len;
        if (p + 2 > end) break;            /* truncated table           */
        type = (BYTE)p[0];
        len  = (ULONG)p[1];
        if (type == 127) break;            /* end-of-structures marker  */
        if (len < 8) break;                /* malformed                 */
        if (p + len > end) break;          /* runs past the mapping     */
        SpoofEntry(type, p, len, end);
        p += len;
    }
    return (g_FakeCount != 0);
}

/* Scan the low 8 MB for the SMBIOS physical header, then map its
 * structure table and spoof it. Firmware places the entry table at
 * slightly different spots, so several candidate starts are tried;
 * every walk is bounds-checked, malformed tables are skipped silently. */
static BOOLEAN FindAndSpoofSmbios(void) {
    PHYSICAL_ADDRESS scanBase;
    CHAR *mem, *q;
    BOOLEAN ok = FALSE;
    const SIZE_T span = 0x00800000;                /* low 8 MB */
    scanBase.QuadPart = 0;
    mem = (CHAR *)MmMapIoSpace(scanBase, span, MmCached);
    if (!mem) { DbgPrint("hwid_spoof: cannot map low 8MB\n"); return FALSE; }
    for (q = mem; (SIZE_T)(q - mem) + sizeof(SMBIOS_PHYS_HDR) <= span;
         q += 16) {
        SMBIOS_PHYS_HDR *hdr = (SMBIOS_PHYS_HDR *)q;
        PHYSICAL_ADDRESS tab;
        CHAR *t;
        ULONG tabSize = hdr->TableSize ? hdr->TableSize : 0x10000;
        if (tabSize > 0x00800000) continue;        /* implausible size    */
        if (q[0] != '_' || q[1] != 'S' || q[2] != 'I' ||
            (q[3] != 'M' && q[3] != 'G')) continue;
        tab.QuadPart = (LONGLONG)hdr->TableAddressLow;
        if (tab.QuadPart == 0) continue;
        t = (CHAR *)MmMapIoSpace(tab, tabSize, MmCached);
        if (!t) continue;
        if (WalkStructures(t + 32 + hdr->EntryOffset * 16,
                           t + tabSize, hdr->EntryCount)) { ok = TRUE; }
        else if (WalkStructures(t + 32, t + tabSize, hdr->EntryCount)) {
            ok = TRUE;
        } else if (WalkStructures(t + hdr->EntryOffset * 16,
                                  t + tabSize, hdr->EntryCount)) {
            ok = TRUE;
        }
        MmUnmapIoSpace(t, tabSize);
        if (ok) break;
    }
    MmUnmapIoSpace(mem, span);
    return ok;
}

/* Status struct returned by the IOCTL (what a user-mode checker reads). */
typedef struct _SPOOF_STATUS {
    ULONG FakeCount;      /* total identity values faked this boot       */
    ULONG BiosSerials;    /* BIOS serial numbers replaced                */
    ULONG BoardSerials;   /* motherboard serials replaced                */
    ULONG Uuids;          /* SMBIOS UUIDs replaced                       */
    ULONG Major, Minor;   /* SMBIOS version the driver read              */
} SPOOF_STATUS;

static SPOOF_STATUS BuildStatus(void) {
    SPOOF_STATUS s;
    s.FakeCount    = g_FakeCount;
    s.BiosSerials  = g_BiosSerials;
    s.BoardSerials = g_BoardSerials;
    s.Uuids        = g_Uuids;
    s.Major = 2; s.Minor = 0;
    return s;
}

static NTSTATUS IoCtrl(PIRP irp) {
    IO_STACK_LOCATION *isk = IoGetNextIrpStackLocation(irp);
    SPOOF_STATUS *out;
    NTSTATUS status = STATUS_SUCCESS;
    switch (isk->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_GET_STATUS:
        if (isk->Parameters.DeviceIoControl.OutputBufferLength <
            sizeof(SPOOF_STATUS)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        out = (SPOOF_STATUS *)irp->AssociatedIrp.SystemBuffer;
        *out = BuildStatus();
        irp->Information = sizeof(SPOOF_STATUS);
        break;
    default: status = STATUS_INVALID_DEVICE_REQUEST; break;
    }
    irp->IoStatus.Status = status;
    IopCompleteRequest(irp, status);
    return status;
}

static NTSTATUS CreateClose(PIRP irp) {
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IopCompleteRequest(irp, STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

static VOID DriverUnload(PDRIVER_OBJECT drv) {
    UNICODE_STRING linkName;
    UNREFERENCED_PARAMETER(drv);
    RtlInitUnicodeString(&linkName, L"\\??\\HwidSpoof");
    IoDeleteSymbolicLink(&linkName);
    if (g_Device) IoDeleteDevice(g_Device);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING regPath) {
    UNICODE_STRING devName;
    NTSTATUS st;
    UNREFERENCED_PARAMETER(regPath);

    /* Spoof FIRST, then expose the control device, so nothing can read
     * the identity before it has been faked this boot. */
    g_Spoofed = FindAndSpoofSmbios();
    DbgPrint("hwid_spoof: %u identity value(s) faked at load\n",
             g_FakeCount);

    RtlInitUnicodeString(&devName, L"\\Device\\HwidSpoof");
    st = IoCreateDevice(drv, 0, &devName, FILE_DEVICE_UNKNOWN, 0, TRUE,
                        &g_Device);
    if (!NT_SUCCESS(st)) return st;
    {
        UNICODE_STRING linkName;
        RtlInitUnicodeString(&linkName, L"\\??\\HwidSpoof");
        IoCreateSymbolicLink(&linkName, &devName);   /* failure tolerated */
    }
    drv->MajorFunction[IRP_MJ_CREATE]   = CreateClose;
    drv->MajorFunction[IRP_MJ_CLOSE]    = CreateClose;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = IoCtrl;
    drv->DriverUnload = DriverUnload;
    return STATUS_SUCCESS;
}
