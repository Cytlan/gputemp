/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gputemp.c - C99 reconstruction of the prebuilt `gputemp` binary from
 * https://github.com/r0ver2/gputemp
 *
 * Reconstructed by hand from the x86-64 disassembly of the shipped ELF
 * (sha256 2b9dc9162f20c5a18b1631b9c605f025a97bff7340bb481be96c913c9a3b3554,
 * built with GCC 11.4.0 / Ubuntu 22.04, not stripped).  The original was
 * three translation units - main.c, nvml_shim.c, rusd_mem_temp.c - which are
 * merged here.  Function names, globals, strings, constants and control flow
 * follow the binary; comments marked "NOTE" describe quirks preserved as-is.
 *
 * Build:  cc -std=c99 -O2 -Wall -Wextra -o gputemp gputemp.c -ldl
 *
 * No NVIDIA headers are needed: libnvidia-ml is loaded at runtime with
 * dlopen(), exactly like the original, and the few NVML / RM ABI structs
 * used are declared below.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* ======================================================================
 * Minimal NVML ABI (matches nvml.h for the entry points used)
 * ====================================================================== */
typedef int nvmlReturn_t;               /* 0 == NVML_SUCCESS */
typedef struct nvmlDevice_st *nvmlDevice_t;

#define NVML_SUCCESS                 0
#define NVML_ERROR_NOT_SUPPORTED     3
#define NVML_TEMPERATURE_GPU         0
#define NVML_FI_DEV_MEMORY_TEMP      82
#define NVML_DEVICE_ARCH_AMPERE      7
#define NVML_DEVICE_ARCH_ADA         8
#define NVML_DEVICE_ARCH_BLACKWELL   10
#define NVML_DEVICE_ARCH_UNKNOWN     0xffffffffu

typedef struct {                        /* nvmlPciInfo_t (v3), 68 bytes */
    char         busIdLegacy[16];
    unsigned int domain;
    unsigned int bus;
    unsigned int device;
    unsigned int pciDeviceId;
    unsigned int pciSubSystemId;
    char         busId[32];
} nvmlPciInfo_t;

typedef union {
    double             dVal;
    unsigned int       uiVal;
    unsigned long      ulVal;
    unsigned long long ullVal;
    long long          sllVal;
    int                siVal;
} nvmlValue_t;

typedef struct {                        /* nvmlFieldValue_t, 40 bytes */
    unsigned int fieldId;
    unsigned int scopeId;
    long long    timestamp;
    long long    latencyUsec;
    int          valueType;             /* 0 = double, 1..5 = integer types */
    nvmlReturn_t nvmlReturn;
    nvmlValue_t  value;
} nvmlFieldValue_t;

/* ======================================================================
 * nvml_shim.c - dlopen()-based NVML loader
 * ====================================================================== */
static void *g_handle;

static nvmlReturn_t (*p_nvmlInit_v2)(void);
static nvmlReturn_t (*p_nvmlShutdown)(void);
static nvmlReturn_t (*p_nvmlDeviceGetCount_v2)(unsigned int *);
static nvmlReturn_t (*p_nvmlDeviceGetHandleByIndex_v2)(unsigned int, nvmlDevice_t *);
static nvmlReturn_t (*p_nvmlDeviceGetName)(nvmlDevice_t, char *, unsigned int);
static nvmlReturn_t (*p_nvmlDeviceGetPciInfo_v3)(nvmlDevice_t, nvmlPciInfo_t *);
static nvmlReturn_t (*p_nvmlDeviceGetArchitecture)(nvmlDevice_t, unsigned int *);
static nvmlReturn_t (*p_nvmlDeviceGetTemperature)(nvmlDevice_t, int, unsigned int *);
static const char  *(*p_nvmlErrorString)(nvmlReturn_t);
static nvmlReturn_t (*p_nvmlDeviceGetFanSpeed)(nvmlDevice_t, unsigned int *);
static nvmlReturn_t (*p_nvmlDeviceGetNumFans)(nvmlDevice_t, unsigned int *);
static nvmlReturn_t (*p_nvmlDeviceSetFanSpeed)(nvmlDevice_t, unsigned int, unsigned int);
static nvmlReturn_t (*p_nvmlDeviceSetDefaultFanSpeed)(nvmlDevice_t, unsigned int);
static nvmlReturn_t (*p_nvmlDeviceGetPowerUsage)(nvmlDevice_t, unsigned int *);
static nvmlReturn_t (*p_nvmlDeviceGetPowerManagementLimit)(nvmlDevice_t, unsigned int *);
static nvmlReturn_t (*p_nvmlDeviceGetFieldValues)(nvmlDevice_t, int, nvmlFieldValue_t *);

/* dlsym() returns void*; converting that to a function pointer is the
 * POSIX-sanctioned idiom (memcpy keeps strict C99 compilers quiet). */
#define LOAD_SYM(var, name) do { void *s_ = dlsym(g_handle, (name)); \
                                 memcpy(&(var), &s_, sizeof s_); } while (0)
#define LOAD_REQUIRED(var, name) do { LOAD_SYM(var, name);                 \
        if (!(var)) {                                                       \
            fprintf(stderr, "nvml_shim: missing symbol %s (%s)\n",          \
                    (name), dlerror());                                     \
            ok = 0;                                                         \
        } } while (0)

int nvml_shim_load(void)
{
    int ok = 1;

    g_handle = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!g_handle)
        g_handle = dlopen("libnvidia-ml.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_handle) {
        fprintf(stderr, "nvml_shim: could not load libnvidia-ml.so.1 (%s)\n"
                        "           Is the NVIDIA driver installed?\n", dlerror());
        return -1;
    }

    LOAD_REQUIRED(p_nvmlInit_v2,                   "nvmlInit_v2");
    LOAD_REQUIRED(p_nvmlShutdown,                  "nvmlShutdown");
    LOAD_REQUIRED(p_nvmlDeviceGetCount_v2,         "nvmlDeviceGetCount_v2");
    LOAD_REQUIRED(p_nvmlDeviceGetHandleByIndex_v2, "nvmlDeviceGetHandleByIndex_v2");
    LOAD_REQUIRED(p_nvmlDeviceGetName,             "nvmlDeviceGetName");
    LOAD_REQUIRED(p_nvmlDeviceGetPciInfo_v3,       "nvmlDeviceGetPciInfo_v3");
    LOAD_SYM    (p_nvmlDeviceGetArchitecture,      "nvmlDeviceGetArchitecture");
    LOAD_REQUIRED(p_nvmlDeviceGetTemperature,      "nvmlDeviceGetTemperature");
    LOAD_REQUIRED(p_nvmlErrorString,               "nvmlErrorString");
    /* optional */
    LOAD_SYM(p_nvmlDeviceGetFanSpeed,             "nvmlDeviceGetFanSpeed");
    LOAD_SYM(p_nvmlDeviceGetNumFans,              "nvmlDeviceGetNumFans");
    LOAD_SYM(p_nvmlDeviceSetFanSpeed,             "nvmlDeviceSetFanSpeed_v2");
    LOAD_SYM(p_nvmlDeviceSetDefaultFanSpeed,      "nvmlDeviceSetDefaultFanSpeed_v2");
    LOAD_SYM(p_nvmlDeviceGetPowerUsage,           "nvmlDeviceGetPowerUsage");
    LOAD_SYM(p_nvmlDeviceGetPowerManagementLimit, "nvmlDeviceGetPowerManagementLimit");
    LOAD_SYM(p_nvmlDeviceGetFieldValues,          "nvmlDeviceGetFieldValues");

    if (!ok) {
        dlclose(g_handle);
        g_handle = NULL;
        return -1;
    }
    return 0;
}

void nvml_shim_unload(void)
{
    if (g_handle) {
        dlclose(g_handle);
        g_handle = NULL;
    }
}

nvmlReturn_t nvml_Init(void)        { return p_nvmlInit_v2(); }
nvmlReturn_t nvml_Shutdown(void)    { return p_nvmlShutdown(); }
nvmlReturn_t nvml_DeviceGetCount(unsigned int *n) { return p_nvmlDeviceGetCount_v2(n); }
nvmlReturn_t nvml_DeviceGetHandleByIndex(unsigned int i, nvmlDevice_t *d)
                                    { return p_nvmlDeviceGetHandleByIndex_v2(i, d); }
nvmlReturn_t nvml_DeviceGetName(nvmlDevice_t d, char *b, unsigned int n)
                                    { return p_nvmlDeviceGetName(d, b, n); }
nvmlReturn_t nvml_DeviceGetPciInfo(nvmlDevice_t d, nvmlPciInfo_t *p)
                                    { return p_nvmlDeviceGetPciInfo_v3(d, p); }
nvmlReturn_t nvml_DeviceGetArchitecture(nvmlDevice_t d, unsigned int *a)
{ return p_nvmlDeviceGetArchitecture ? p_nvmlDeviceGetArchitecture(d, a) : NVML_ERROR_NOT_SUPPORTED; }
nvmlReturn_t nvml_DeviceGetTemperature(nvmlDevice_t d, int s, unsigned int *t)
                                    { return p_nvmlDeviceGetTemperature(d, s, t); }
nvmlReturn_t nvml_DeviceGetFanSpeed(nvmlDevice_t d, unsigned int *s)
{ return p_nvmlDeviceGetFanSpeed ? p_nvmlDeviceGetFanSpeed(d, s) : NVML_ERROR_NOT_SUPPORTED; }
nvmlReturn_t nvml_DeviceGetNumFans(nvmlDevice_t d, unsigned int *n)
{ return p_nvmlDeviceGetNumFans ? p_nvmlDeviceGetNumFans(d, n) : NVML_ERROR_NOT_SUPPORTED; }
nvmlReturn_t nvml_DeviceSetFanSpeed(nvmlDevice_t d, unsigned int f, unsigned int s)
{ return p_nvmlDeviceSetFanSpeed ? p_nvmlDeviceSetFanSpeed(d, f, s) : NVML_ERROR_NOT_SUPPORTED; }
nvmlReturn_t nvml_DeviceSetDefaultFanSpeed(nvmlDevice_t d, unsigned int f)
{ return p_nvmlDeviceSetDefaultFanSpeed ? p_nvmlDeviceSetDefaultFanSpeed(d, f) : NVML_ERROR_NOT_SUPPORTED; }
nvmlReturn_t nvml_DeviceGetPowerUsage(nvmlDevice_t d, unsigned int *mw)
{ return p_nvmlDeviceGetPowerUsage ? p_nvmlDeviceGetPowerUsage(d, mw) : NVML_ERROR_NOT_SUPPORTED; }
nvmlReturn_t nvml_DeviceGetPowerManagementLimit(nvmlDevice_t d, unsigned int *mw)
{ return p_nvmlDeviceGetPowerManagementLimit ? p_nvmlDeviceGetPowerManagementLimit(d, mw) : NVML_ERROR_NOT_SUPPORTED; }
nvmlReturn_t nvml_DeviceGetFieldValues(nvmlDevice_t d, int n, nvmlFieldValue_t *v)
{ return p_nvmlDeviceGetFieldValues ? p_nvmlDeviceGetFieldValues(d, n, v) : NVML_ERROR_NOT_SUPPORTED; }
int nvml_has_field_values_support(void) { return p_nvmlDeviceGetFieldValues != NULL; }
const char *nvml_ErrorString(nvmlReturn_t r) { return p_nvmlErrorString(r); }

/* ======================================================================
 * rusd_mem_temp.c - Blackwell memory temperature via the NVIDIA Resource
 * Manager "RM_USER_SHARED_DATA" (RUSD) object.
 *
 * Allocates an RM client -> device -> subdevice -> RUSD object using the
 * standard /dev/nvidiactl ioctls (the same ones libcuda/NVML use), maps the
 * RUSD page READ-ONLY, and heuristically scans it for a {core temp, ...,
 * memory temp} record.  Values are fixed-point 1/256 degC.  Nothing is
 * written to the GPU.
 * ====================================================================== */
#define NV_IOCTL_MAGIC          'F'
#define NV_ESC_RM_FREE          0x29
#define NV_ESC_RM_ALLOC         0x2B
#define NV_ESC_RM_MAP_MEMORY    0x4E

#define NV01_ROOT_CLIENT        0x0041
#define NV01_DEVICE_0           0x0080
#define NV20_SUBDEVICE_0        0x2080
#define RM_USER_SHARED_DATA     0x00DE

typedef struct {                        /* NVOS64_PARAMETERS, 48 bytes */
    uint32_t hRoot;
    uint32_t hObjectParent;
    uint32_t hObjectNew;
    uint32_t hClass;
    uint64_t pAllocParms;
    uint64_t pRightsRequested;
    uint32_t paramsSize;
    uint32_t flags;
    uint32_t status;
    uint32_t _pad;
} nv_rm_alloc_t;

typedef struct {                        /* NVOS33 + fd, 56 bytes */
    uint32_t hClient;
    uint32_t hDevice;
    uint32_t hMemory;
    uint32_t _pad0;
    uint64_t offset;
    uint64_t length;
    uint64_t pLinearAddress;
    uint32_t status;
    uint32_t flags;
    int32_t  fd;
    uint32_t _pad1;
} nv_rm_map_t;

typedef struct { uint32_t deviceId; uint8_t rest[0x34]; } nv0080_alloc_t; /* 0x38 */

#define NV_IOCTL_RM_ALLOC  _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_ALLOC, nv_rm_alloc_t)  /* 0xC030462B */
#define NV_IOCTL_RM_FREE   _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_FREE,  nv_rm_alloc_t)  /* 0xC0304629 */
#define NV_IOCTL_RM_MAP    _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_MAP_MEMORY, nv_rm_map_t) /* 0xC038464E */

static int g_cached_length;

/* Map the RUSD object; returns the fd backing the mapping, or -1. */
static int try_map(int ctl_fd, uint32_t hClient, uint32_t hSubdev,
                   uint32_t hRusd, int length, void **out)
{
    nv_rm_map_t p;
    void *ptr;
    int map_fd = open("/dev/nvidiactl", O_RDWR);
    if (map_fd < 0)
        return -1;

    memset(&p, 0, sizeof p);
    p.hClient = hClient;
    p.hDevice = hSubdev;
    p.hMemory = hRusd;
    p.length  = (uint32_t)length;
    p.fd      = map_fd;
    if (ioctl(ctl_fd, NV_IOCTL_RM_MAP, &p) >= 0 && p.status == 0) {
        ptr = mmap(NULL, (size_t)length, PROT_READ, MAP_SHARED, map_fd, 0);
        if (ptr != MAP_FAILED) {
            *out = ptr;
            return map_fd;
        }
    }
    close(map_fd);
    return -1;
}

/* Returns 0 and sets *out_c on success; -1 on failure, -2 if /dev/nvidiaN
 * can't be opened. core_c (< 0 if unknown) is used to pick the record. */
int rusd_read_memory_temp_c(unsigned int gpu_index, int core_c, int *out_c)
{
    nv_rm_alloc_t a;
    nv0080_alloc_t devp;
    uint32_t subdevp = 0;
    uint64_t rusdp;
    uint32_t hClient, hDevice, hSubdev, hRusd;
    char path[32];
    void *map = NULL;
    int ctl_fd, dev_fd, map_fd, len, ret = -1;

    ctl_fd = open("/dev/nvidiactl", O_RDWR);
    if (ctl_fd < 0)
        return -1;
    snprintf(path, sizeof path, "/dev/nvidia%u", gpu_index);
    dev_fd = open(path, O_RDWR);
    if (dev_fd < 0) {
        close(ctl_fd);
        return -2;
    }

    /* 1. root client */
    memset(&a, 0, sizeof a);
    a.hClass = NV01_ROOT_CLIENT;
    if (ioctl(ctl_fd, NV_IOCTL_RM_ALLOC, &a) < 0 || a.status != 0) {
        close(dev_fd);
        close(ctl_fd);
        return -1;
    }
    hClient = a.hObjectNew;

    /* 2. device (NV0080), deviceId = NVML index */
    memset(&devp, 0, sizeof devp);
    devp.deviceId = gpu_index;
    memset(&a, 0, sizeof a);
    a.hRoot = hClient; a.hObjectParent = hClient;
    a.hClass = NV01_DEVICE_0;
    a.pAllocParms = (uint64_t)(uintptr_t)&devp;
    a.paramsSize = sizeof devp;
    if (ioctl(ctl_fd, NV_IOCTL_RM_ALLOC, &a) < 0 || a.status != 0)
        goto out_free;
    hDevice = a.hObjectNew;

    /* 3. subdevice 0 (NV2080) */
    memset(&a, 0, sizeof a);
    a.hRoot = hClient; a.hObjectParent = hDevice;
    a.hClass = NV20_SUBDEVICE_0;
    a.pAllocParms = (uint64_t)(uintptr_t)&subdevp;
    a.paramsSize = sizeof subdevp;
    if (ioctl(ctl_fd, NV_IOCTL_RM_ALLOC, &a) < 0 || a.status != 0)
        goto out_free;
    hSubdev = a.hObjectNew;

    /* 4. RUSD object (NV00DE), polledDataMask = 0x10 */
    rusdp = 0x10;
    memset(&a, 0, sizeof a);
    a.hRoot = hClient; a.hObjectParent = hSubdev;
    a.hClass = RM_USER_SHARED_DATA;
    a.pAllocParms = (uint64_t)(uintptr_t)&rusdp;
    a.paramsSize = sizeof rusdp;
    if (ioctl(ctl_fd, NV_IOCTL_RM_ALLOC, &a) < 0 || a.status != 0)
        goto out_free;
    hRusd = a.hObjectNew;

    /* 5. map it read-only; the struct size is driver-version dependent, so
     *    probe lengths 4064, 4032, ... 2048 and remember what worked. */
    map_fd = -1;
    len = 0;
    if (g_cached_length != 0) {
        map_fd = try_map(ctl_fd, hClient, hSubdev, hRusd, g_cached_length, &map);
        if (map_fd >= 0) {
            len = g_cached_length;
            if ((long)len <= 0x4f) {        /* too small to scan */
                ret = -1;
                goto out_unmap;
            }
        }
    }
    if (map_fd < 0) {
        for (len = 0xfe0; len != 0x7e0; len -= 0x20) {
            map_fd = try_map(ctl_fd, hClient, hSubdev, hRusd, len, &map);
            if (map_fd >= 0)
                break;
        }
        if (map_fd < 0)
            goto out_free;
    }

    /* 6. heuristic scan */
    {
        const unsigned char *base = (const unsigned char *)map;
        int result = -1;
        unsigned int best = 0xffffffffu;
        long off = 8;
        do {
            int32_t  raw_a, raw_b;
            uint64_t ts;
            double   ta, tb;
            memcpy(&raw_a, base + off,        4);
            memcpy(&ts,    base + off + 8,    8);
            memcpy(&raw_b, base + off + 0x10, 4);
            ta = (double)raw_a * 0.00390625;   /* 1/256 degC */
            tb = (double)raw_b * 0.00390625;
            if (!(10.0 > ta) && !(ta > 110.0) &&
                !(10.0 > tb) && !(tb > 130.0) && ts != 0) {
                if (core_c >= 0) {
                    int d = (int)(ta - (double)core_c);
                    int ad = d < 0 ? -d : d;
                    if (ad <= 5 && (unsigned int)ad < best) {
                        best = (unsigned int)ad;
                        result = (int)(tb + 0.5);
                    }
                } else if (result < 0) {
                    result = (int)(tb + 0.5);
                }
            }
            off += 8;
        } while ((unsigned long)(off + 0x48) <= (unsigned long)len);

        if (result < 0) {
            ret = -1;
        } else {
            g_cached_length = len;
            *out_c = result;
            ret = 0;
        }
    }

out_unmap:
    munmap(map, (size_t)len);
    close(map_fd);
out_free:
    /* Free the whole client tree. NOTE: the binary passes a 48-byte zeroed
     * buffer {hRoot=client, hObjectParent=0, hObjectOld=client}. */
    memset(&a, 0, sizeof a);
    a.hRoot = hClient;
    a.hObjectNew = hClient;            /* offset 8 == NVOS00.hObjectOld */
    ioctl(ctl_fd, NV_IOCTL_RM_FREE, &a);
    close(dev_fd);
    close(ctl_fd);
    return ret;
}

/* ======================================================================
 * main.c
 * ====================================================================== */
#define MAX_GPUS 32

enum { ARCH_OTHER = 0, ARCH_AMPERE_ADA = 1, ARCH_BLACKWELL = 2 };

typedef struct {                        /* 0xCC bytes in the original */
    unsigned int index;                 /* 0x00 */
    char name[96];                      /* 0x04 */
    char pci_bus_id[32];                /* 0x64  full NVML busId (unused for output) */
    char pci_short[16];                 /* 0x84  "bb:dd.f" */
    char sysfs_bdf[32];                 /* 0x94  "dddd:bb:dd.f" */
    int  core_temp_c;                   /* 0xB4 */
    int  mem_temp_c;                    /* 0xB8 */
    int  fan_percent;                   /* 0xBC */
    int  power_w;                       /* 0xC0 */
    int  power_limit_w;                 /* 0xC4 */
    int  arch_category;                 /* 0xC8 */
} gpu_info_t;

typedef struct {
    int    enabled;                     /* --target given */
    double target_c;
    int    min_set;                     /* --min given */
    int    min_percent;
} fan_ctl_t;

static volatile sig_atomic_t g_stop;
static int g_fan_warned[MAX_GPUS];
static int g_fan_baseline_percent[MAX_GPUS];
static int g_fan_baseline_set[MAX_GPUS];

static const char *const needles_blackwell[] = {   /* needles.3 */
    "B100", "B200", "B300", "GB200", "GB300", "RTX 50", "RTX50",
    "5090", "5080", "5070", "5060", "Blackwell", NULL
};
static const char *const needles_ampere_ada[] = {  /* needles.2 */
    "A100", "A30", "A40", "A10", "A16", "RTX 30", "RTX30", "3090", "3080",
    "3070", "3060", "3050", "Ampere", "L40", "L20", "L4", "RTX 40", "RTX40",
    "4090", "4080", "4070", "4060", "4050", "Ada", NULL
};

/* Undocumented Blackwell junction-temperature registers (BAR0 offsets). */
static const uint32_t registers_blackwell[5] = {    /* registers.1 */
    0xad0a90, 0xad0a94, 0xad0a98, 0xad0a9c, 0xad0aa0
};

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void print_usage(const char *argv0)
{
    printf("Usage: %s [--json] [--watch] [--interval SECONDS]\n", argv0);
    puts("                [--target TEMP] [--min PERCENT]");
    putchar('\n');
    puts("  --json               machine-readable output");
    puts("  --watch              refresh continuously until Ctrl-C");
    puts("  --interval SECONDS   refresh interval for --watch (default 2)");
    puts("  --target TEMP        target core temperature, e.g. 65 or 65c.");
    puts("                       Enables active fan control (needs root) -");
    puts("                       implies --watch. Fan is held at its");
    puts("                       pre-control speed (or --min, if given) at");
    printf("                       or below target, ramping linearly to 100%%\n");
    printf("                       by target+10C. Forces 100%% above 95C or if\n");
    puts("                       temperature can't be read, and restores");
    puts("                       automatic fan control on exit (Ctrl-C or");
    puts("                       SIGTERM - NOT on SIGKILL or a crash).");
    puts("  --min PERCENT        minimum fan percent when --target is used");
    puts("                       (0-100). Without it, the fan is never");
    puts("                       forced lower than wherever it already was.");
    puts("  --find ID            identify a physical GPU: sets GPU ID's fan to");
    printf("                       100%% and every other GPU's fan to 0%% for 60\n");
    puts("                       seconds, then restores automatic control.");
    puts("                       Requires root and an interactive 'yes'");
    puts("                       confirmation (only safe on idle GPUs - a");
    printf("                       fan at 0%% under load can cause overheating).\n");
    puts("                       Implies not --watch; cannot combine with");
    puts("                       --target.");
    putchar('\n');
    puts("Junction temperature for Blackwell-class GPUs requires root and");
    puts("uses undocumented register reads. It is best-effort - always");
    puts("cross-check it against Core temperature.");
}

/* Restore the driver's automatic fan curve on every GPU we touched. */
static void restore_all_fans_to_auto(void)
{
    unsigned int count = 0, i;

    if (nvml_Init() != NVML_SUCCESS)
        return;
    if (nvml_DeviceGetCount(&count) == NVML_SUCCESS) {
        if (count > MAX_GPUS)
            count = MAX_GPUS;
        for (i = 0; i < count; i++) {
            nvmlDevice_t dev;
            unsigned int nfans = 0, f;
            if (!g_fan_baseline_set[i])
                continue;
            if (nvml_DeviceGetHandleByIndex(i, &dev) != NVML_SUCCESS)
                continue;
            if (nvml_DeviceGetNumFans(dev, &nfans) != NVML_SUCCESS || nfans == 0)
                continue;
            for (f = 0; f < nfans; f++)
                nvml_DeviceSetDefaultFanSpeed(dev, f);
        }
    }
    nvml_Shutdown();
}

/* Read one 32-bit register from the GPU's BAR0 via sysfs (READ-ONLY map).
 * Returns 0, or -1 bad args, -2 open failed, -3 mmap failed. */
static int read_reg32(const char *sysfs_bdf, uint32_t offset, uint32_t *out)
{
    char path[512];
    void *bar;
    int fd;

    if (offset > 0xffffff)
        return -1;
    snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", sysfs_bdf);
    fd = open(path, O_RDONLY | O_SYNC);
    if (fd < 0)
        return -2;
    bar = mmap(NULL, 0x1000000, PROT_READ, MAP_SHARED, fd, 0);
    if (bar == MAP_FAILED) {
        close(fd);
        return -3;
    }
    *out = *(volatile uint32_t *)((volatile unsigned char *)bar + offset);
    munmap(bar, 0x1000000);
    close(fd);
    return 0;
}

static int name_matches(const char *name, const char *const *needles)
{
    for (; *needles; needles++)
        if (strstr(name, *needles))
            return 1;
    return 0;
}

/* Apply --target fan control to one GPU (only called as root). */
static void apply_fan_control(gpu_info_t *g, nvmlDevice_t dev, unsigned int idx,
                              const fan_ctl_t *ctl)
{
    unsigned int nfans = 0, f;
    int core = g->core_temp_c, base, speed;
    nvmlReturn_t r;

    r = nvml_DeviceGetNumFans(dev, &nfans);
    if (r != NVML_SUCCESS || nfans == 0) {
        if (!g_fan_warned[idx]) {
            fprintf(stderr, "note: GPU %u has no controllable fan (or the driver "
                    "doesn't expose one) - fan control skipped for this GPU\n", idx);
            g_fan_warned[idx] = 1;
        }
        return;
    }

    if (!g_fan_baseline_set[idx]) {
        unsigned int cur = 0;
        int b = (nvml_DeviceGetFanSpeed(dev, &cur) == NVML_SUCCESS) ? (int)cur : 50;
        if (ctl->min_set && ctl->min_percent > b)
            b = ctl->min_percent;
        g_fan_baseline_percent[idx] = b;
        g_fan_baseline_set[idx] = 1;
    }
    base = g_fan_baseline_percent[idx];

    /* NOTE: unsigned compare - an unread temperature (-1) also forces 100%. */
    if ((unsigned int)core > 94) {
        speed = 100;
    } else if (ctl->target_c >= (double)core) {
        int v = ctl->min_set ? ctl->min_percent : base;
        speed = v < 0 ? 0 : (v > 100 ? 100 : v);
    } else {
        double frac = ((double)core - ctl->target_c) / 10.0;
        double floor_pct;
        int v;
        if (frac > 1.0)
            frac = 1.0;
        floor_pct = (ctl->min_set && ctl->min_percent > base)
                        ? (double)ctl->min_percent : (double)base;
        v = (int)(frac * (100.0 - floor_pct) + floor_pct + 0.5);
        speed = v < 0 ? 0 : (v > 100 ? 100 : v);
    }

    for (f = 0; f < nfans; f++) {
        r = nvml_DeviceSetFanSpeed(dev, f, (unsigned int)speed);
        if (r != NVML_SUCCESS && !g_fan_warned[idx]) {
            fprintf(stderr, "note: GPU %u: nvmlDeviceSetFanSpeed_v2 failed (%s) - "
                    "manual fan control may not be supported for this GPU/driver "
                    "combination\n", idx, nvml_ErrorString(r));
            g_fan_warned[idx] = 1;
        }
    }
}

/* Fill gpus[]; returns GPU count (<= 32) or -1. */
static int query_nvml(gpu_info_t *gpus, int is_root, const fan_ctl_t *ctl)
{
    unsigned int count = 0, i;
    nvmlReturn_t r;

    r = nvml_Init();
    if (r != NVML_SUCCESS) {
        fprintf(stderr, "error: nvmlInit failed: %s\n", nvml_ErrorString(r));
        return -1;
    }
    r = nvml_DeviceGetCount(&count);
    if (r != NVML_SUCCESS) {
        fprintf(stderr, "error: nvmlDeviceGetCount failed: %s\n", nvml_ErrorString(r));
        nvml_Shutdown();
        return -1;
    }
    if ((int)count > MAX_GPUS)
        count = MAX_GPUS;

    for (i = 0; i < count; i++) {
        gpu_info_t *g = &gpus[i];
        nvmlDevice_t dev;
        nvmlPciInfo_t pci;
        unsigned int arch, temp, fan, mw;

        memset(g, 0, sizeof *g);
        g->index = i;
        g->core_temp_c = g->mem_temp_c = g->fan_percent = -1;
        g->power_w = g->power_limit_w = -1;

        r = nvml_DeviceGetHandleByIndex(i, &dev);
        if (r != NVML_SUCCESS) {
            snprintf(g->name, sizeof g->name, "(unavailable: %s)", nvml_ErrorString(r));
            continue;
        }
        if (nvml_DeviceGetName(dev, g->name, sizeof g->name) != NVML_SUCCESS)
            strcpy(g->name, "(unknown)");

        memset(&pci, 0, sizeof pci);
        if (nvml_DeviceGetPciInfo(dev, &pci) == NVML_SUCCESS) {
            unsigned int dom = 0, bus = 0, d = 0, fn = 0;
            snprintf(g->pci_bus_id, sizeof g->pci_bus_id, "%s", pci.busId);
            if (sscanf(pci.busId, "%x:%x:%x.%x", &dom, &bus, &d, &fn) == 4)
                snprintf(g->sysfs_bdf, sizeof g->sysfs_bdf, "%04x:%02x:%02x.%01x",
                         (unsigned int)(uint16_t)dom, bus, d, fn);
            dom = bus = d = fn = 0;
            if (sscanf(pci.busId, "%x:%x:%x.%x", &dom, &bus, &d, &fn) == 4)
                snprintf(g->pci_short, sizeof g->pci_short, "%02x:%02x.%01x", bus, d, fn);
        }

        /* architecture: NVML first, then fall back to name matching */
        arch = NVML_DEVICE_ARCH_UNKNOWN;
        r = nvml_DeviceGetArchitecture(dev, &arch);
        if (r == NVML_SUCCESS && arch == NVML_DEVICE_ARCH_BLACKWELL)
            g->arch_category = ARCH_BLACKWELL;
        else if (r == NVML_SUCCESS && (arch == NVML_DEVICE_ARCH_AMPERE ||
                                       arch == NVML_DEVICE_ARCH_ADA))
            g->arch_category = ARCH_AMPERE_ADA;
        else if (r == NVML_SUCCESS && arch != NVML_DEVICE_ARCH_UNKNOWN)
            g->arch_category = ARCH_OTHER;
        else if (name_matches(g->name, needles_blackwell))
            g->arch_category = ARCH_BLACKWELL;
        else if (name_matches(g->name, needles_ampere_ada))
            g->arch_category = ARCH_AMPERE_ADA;
        else
            g->arch_category = ARCH_OTHER;

        temp = 0;
        if (nvml_DeviceGetTemperature(dev, NVML_TEMPERATURE_GPU, &temp) == NVML_SUCCESS)
            g->core_temp_c = (int)temp;

        if (is_root && ctl->enabled && i < MAX_GPUS)
            apply_fan_control(g, dev, i, ctl);

        /* memory temperature */
        {
            int have = 0;
            if (nvml_has_field_values_support()) {
                nvmlFieldValue_t fv;
                memset(&fv, 0, sizeof fv);
                fv.fieldId = NVML_FI_DEV_MEMORY_TEMP;
                if ((nvml_DeviceGetFieldValues(dev, 1, &fv) | fv.nvmlReturn) == 0 &&
                    (unsigned int)fv.valueType <= 5) {
                    g->mem_temp_c = (fv.valueType == 0) ? (int)fv.value.dVal
                                                        : (int)fv.value.uiVal;
                    have = 1;
                }
            }
            if (!have && is_root) {
                if (g->sysfs_bdf[0] && g->arch_category == ARCH_AMPERE_ADA) {
                    /* GDDR6X memory temp register at BAR0+0xE2A8 */
                    uint32_t v = 0;
                    if (read_reg32(g->sysfs_bdf, 0xe2a8, &v) == 0) {
                        v &= 0xfff;
                        if (v <= 0xfdf)
                            g->mem_temp_c = (int)(v >> 5);
                    }
                } else if (g->arch_category == ARCH_BLACKWELL) {
                    int m = -1;
                    if (rusd_read_memory_temp_c(g->index, g->core_temp_c, &m) == 0)
                        g->mem_temp_c = m;
                }
            }
        }

        fan = 0;
        if (nvml_DeviceGetFanSpeed(dev, &fan) == NVML_SUCCESS)
            g->fan_percent = (int)fan;
        mw = 0;
        if (nvml_DeviceGetPowerUsage(dev, &mw) == NVML_SUCCESS)
            g->power_w = (int)(mw / 1000);
        if (nvml_DeviceGetPowerManagementLimit(dev, &mw) == NVML_SUCCESS)
            g->power_limit_w = (int)(mw / 1000);
    }

    nvml_Shutdown();
    return (int)count;
}

static const char *reg_err_str(int rc)
{
    switch (rc) {
    case -1: return "bad args";
    case -2: return "open failed (need root?)";
    case -3: return "mmap failed";
    case -4: return "no plausible reading";
    default: return "unknown error";
    }
}

/* Junction temperature (root only, BAR0 reads). Returns >0 on success,
 * 0 and fills note[] otherwise. */
static int read_junction(const gpu_info_t *g, int is_root, char *note, size_t n)
{
    int junc = 0, any_ok = 0, rc = -4;
    uint32_t v;

    if (g->arch_category != ARCH_AMPERE_ADA && g->arch_category != ARCH_BLACKWELL) {
        snprintf(note, n, "junction read not attempted (unsupported arch)");
        return 0;
    }
    if (!g->sysfs_bdf[0]) {
        snprintf(note, n, "no PCI bus id from NVML");
        return 0;
    }
    if (!is_root) {
        snprintf(note, n, "junction read failed: %s", "open failed (need root?)");
        return 0;
    }

    if (g->arch_category == ARCH_BLACKWELL) {
        size_t k;
        for (k = 0; k < 5; k++) {
            int r;
            v = 0;
            r = read_reg32(g->sysfs_bdf, registers_blackwell[k], &v);
            if (r == 0) {
                unsigned int b = (v >> 8) & 0xff;
                if (b > (unsigned int)junc && b <= 115)
                    junc = (int)b;
                any_ok = 1;
            } else {
                rc = r;
            }
        }
    } else {
        /* Ampere/Ada: two hotspot registers, take the larger plausible one. */
        int r1, r2;
        unsigned int b;
        v = 0;
        r1 = read_reg32(g->sysfs_bdf, 0x2046c, &v);
        if (r1 == 0) {
            b = (v >> 8) & 0xff;
            junc = (b >= 1 && b <= 115) ? (int)b : 0;
        }
        v = 0;
        r2 = read_reg32(g->sysfs_bdf, 0x20470, &v);
        if (r2 == 0) {
            b = (v >> 8) & 0xff;
            if (b > (unsigned int)junc && b <= 115)
                junc = (int)b;
        }
        any_ok = (r1 == 0 || r2 == 0);
        rc = r2;                        /* error reported is the second read's */
    }

    if (!any_ok) {
        snprintf(note, n, "junction read failed: %s", reg_err_str(rc));
        return 0;
    }
    if (junc == 0) {
        snprintf(note, n, "junction read failed: %s", "no plausible reading");
        return 0;
    }
    note[0] = '\0';
    if (g->core_temp_c > junc && g->core_temp_c >= 0)
        snprintf(note, n, "warn: junction < core, treat as unreliable");
    return junc;
}

static void print_table(const gpu_info_t *gpus, int count, int is_root, int json)
{
    int i;

    if (json) {
        puts("[");
    } else {
        printf("%-3s %-28s %-10s %6s %6s %6s %5s %14s  %s\n",
               "ID", "NAME", "PCI ID", "TEMP", "JUNC", "MEM", "FAN", "POWER", "NOTE");
        puts("-------------------------------------------------------------------------------------------");
    }

    for (i = 0; i < count; i++) {
        const gpu_info_t *g = &gpus[i];
        char note[120];
        char b0[16], b1[16], b2[16], b3[16], b4[24];
        int junc;

        memset(note, 0, sizeof note);
        junc = read_junction(g, is_root, note, sizeof note);

        if (json) {
            const char *arch = g->arch_category == ARCH_BLACKWELL ? "blackwell" :
                               g->arch_category == ARCH_AMPERE_ADA ? "ampere_ada" : "other";
            strcpy(b0, "null"); strcpy(b1, "null"); strcpy(b2, "null");
            strcpy(b3, "null"); strcpy(b4, "null");
            if (junc)                  snprintf(b0, sizeof b0, "%d", junc);
            if (g->mem_temp_c >= 0)    snprintf(b1, sizeof b1, "%d", g->mem_temp_c);
            if (g->fan_percent >= 0)   snprintf(b2, sizeof b2, "%d", g->fan_percent);
            if (g->power_w >= 0)       snprintf(b3, sizeof b3, "%d", g->power_w);
            if (g->power_limit_w >= 0) snprintf(b4, 16, "%d", g->power_limit_w);
            /* NOTE: name/note are not JSON-escaped, as in the original. */
            printf("  {\"index\": %u, \"name\": \"%s\", \"pci_bus_id\": \"%s\", "
                   "\"core_temp_c\": %d, \"arch_category\": \"%s\", "
                   "\"junction_temp_c\": %s, \"mem_temp_c\": %s, \"fan_percent\": %s, "
                   "\"power_watts\": %s, \"power_limit_watts\": %s, \"note\": \"%s\"}%s\n",
                   g->index, g->name, g->pci_short, g->core_temp_c, arch,
                   b0, b1, b2, b3, b4, note, (i == count - 1) ? "" : ",");
        } else {
            strcpy(b0, "N/A"); strcpy(b1, "N/A"); strcpy(b2, "N/A");
            strcpy(b3, "N/A"); strcpy(b4, "N/A");
            if (g->core_temp_c >= 0) snprintf(b0, sizeof b0, "%dC", g->core_temp_c);
            if (junc)                snprintf(b1, sizeof b1, "%dC", junc);
            if (g->mem_temp_c >= 0)  snprintf(b2, sizeof b2, "%dC", g->mem_temp_c);
            if (g->fan_percent >= 0) snprintf(b3, sizeof b3, "%d%%", g->fan_percent);
            if (g->power_w >= 0) {
                if (g->power_limit_w >= 0)
                    snprintf(b4, sizeof b4, "%dW/%dW", g->power_w, g->power_limit_w);
                else
                    snprintf(b4, sizeof b4, "%dW/N/A", g->power_w);
            }
            printf("%-3u %-28.28s %-10s %6s %6s %6s %5s %14s  %s\n",
                   g->index, g->name, g->pci_short, b0, b1, b2, b3, b4, note);
        }
    }

    if (json)
        puts("]");
}

/* --find: spin one GPU's fan to 100%, all others to 0%, for 60 s. */
static int run_find(int find_id)
{
    unsigned int count = 0, i, nfans;
    nvmlDevice_t dev;
    char answer[16];
    int secs;

    if (nvml_Init() != NVML_SUCCESS) {
        fputs("error: nvmlInit failed\n", stderr);
        return 1;
    }
    if (nvml_DeviceGetCount(&count) != NVML_SUCCESS || count == 0) {
        fputs("error: could not enumerate GPUs\n", stderr);
        nvml_Shutdown();
        return 1;
    }
    if (count > MAX_GPUS)
        count = MAX_GPUS;
    if (find_id < 0 || (unsigned int)find_id >= count) {
        fprintf(stderr, "error: --find %d is not a valid GPU ID - %u GPU(s) detected "
                "(valid range: 0-%u)\n", find_id, count, count - 1);
        nvml_Shutdown();
        return 1;
    }

    printf("WARNING: this will set GPU %d's fan to 100%% and every OTHER detected\n", find_id);
    printf("GPU's fan to 0%% for 60 seconds, then restore automatic control.\n\n");
    puts("Only proceed if none of these GPUs are under load right now - running a");
    printf("fan at 0%% on a GPU that's actively working can cause it to overheat.\n\n");
    printf("Type 'yes' to continue, anything else to cancel: ");
    fflush(stdout);
    if (!fgets(answer, sizeof answer, stdin) || strncasecmp(answer, "yes", 3) != 0) {
        puts("Cancelled - no fan speeds were changed.");
        nvml_Shutdown();
        return 0;
    }
    putchar('\n');

    for (i = 0; i < count; i++) {
        unsigned int speed, f;
        if (nvml_DeviceGetHandleByIndex(i, &dev) != NVML_SUCCESS) {
            fprintf(stderr, "note: GPU %u unavailable - skipped\n", i);
            continue;
        }
        nfans = 0;
        if (nvml_DeviceGetNumFans(dev, &nfans) != NVML_SUCCESS || nfans == 0) {
            fprintf(stderr, "note: GPU %u has no controllable fan - skipped\n", i);
            continue;
        }
        speed = (i == (unsigned int)find_id) ? 100 : 0;
        for (f = 0; f < nfans; f++) {
            nvmlReturn_t r = nvml_DeviceSetFanSpeed(dev, f, speed);
            if (r != NVML_SUCCESS)
                fprintf(stderr, "note: GPU %u: failed to set fan speed (%s)\n",
                        i, nvml_ErrorString(r));
        }
        g_fan_baseline_set[i] = 1;
        printf("GPU %u: fan set to %u%%\n", i, speed);
    }

    puts("\nWaiting 60 seconds (Ctrl-C to stop early and restore immediately)...\n");
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    for (secs = 60; secs > 0 && !g_stop; secs--) {
        struct timespec one = { 1, 0 };
        printf("\r%2d seconds remaining  |  ", secs);
        for (i = 0; i < count; i++) {
            unsigned int fan = 0;
            if (nvml_DeviceGetHandleByIndex(i, &dev) == NVML_SUCCESS &&
                nvml_DeviceGetFanSpeed(dev, &fan) == NVML_SUCCESS)
                printf("GPU%u=%3u%%  ", i, fan);
            else
                printf("GPU%u=??%%  ", i);
        }
        printf("   ");
        fflush(stdout);
        nanosleep(&one, NULL);
    }
    puts("\n\nrestoring automatic fan control...");
    nvml_Shutdown();
    restore_all_fans_to_auto();
    puts("done.");
    return 0;
}

int main(int argc, char **argv)
{
    static gpu_info_t gpus[MAX_GPUS];
    fan_ctl_t ctl;
    int json = 0, watch = 0, find = 0, find_id = -1, is_root, n, i;
    double interval = 2.0;

    memset(&ctl, 0, sizeof ctl);

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--json") == 0) {
            json = 1;
        } else if (strcmp(a, "--watch") == 0) {
            watch = 1;
        } else if (strcmp(a, "--interval") == 0 && i + 1 < argc) {
            interval = strtod(argv[++i], NULL);
            if (!(interval > 0.0))
                interval = 2.0;
        } else if (strcmp(a, "--target") == 0 && i + 1 < argc) {
            const char *s = argv[i + 1];
            char *end = NULL;
            double t = strtod(s, &end);
            if (end == s)
                goto bad_target;
            while ((*end & ~0x20) == 'C' || *end == ' ')
                end++;
            if (*end != '\0' || t < 0.0 || t > 100.0)
                goto bad_target;
            ctl.enabled = 1;
            ctl.target_c = t;
            i++;
        } else if (strcmp(a, "--min") == 0 && i + 1 < argc) {
            long v = strtol(argv[++i], NULL, 10);
            if ((unsigned int)v > 100) {
                fputs("error: --min expects a percentage from 0 to 100\n", stderr);
                return 1;
            }
            ctl.min_set = 1;
            ctl.min_percent = (int)v;
        } else if (strcmp(a, "--find") == 0 && i + 1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 0) {
                fputs("error: --find expects a non-negative GPU ID, e.g. --find 0\n", stderr);
                return 1;
            }
            find_id = (int)v;
            find = 1;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", a);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (ctl.min_set && !ctl.enabled) {
        fputs("error: --min only applies together with --target\n", stderr);
        return 1;
    }
    if (find) {
        watch = 0;
        if (ctl.enabled) {
            fputs("error: --find cannot be combined with --target\n", stderr);
            return 1;
        }
    } else if (ctl.enabled) {
        watch = 1;                       /* --target implies --watch */
    }

    if (nvml_shim_load() != 0) {
        fputs("error: could not initialize NVML. Is the NVIDIA driver installed?\n", stderr);
        return 1;
    }

    is_root = (geteuid() == 0);
    if (!json && !is_root)
        fputs("note: not running as root - junction temps for Blackwell GPUs will be "
              "unavailable. Re-run with sudo for full data.\n\n", stderr);

    if (find) {
        int rc;
        if (!is_root) {
            fputs("error: --find requires root (fan control needs elevated NVML access)\n", stderr);
            nvml_shim_unload();
            return 1;
        }
        rc = run_find(find_id);
        nvml_shim_unload();
        return rc;
    }

    if (ctl.enabled) {
        if (!is_root) {
            fputs("error: --target requires root (fan control needs elevated NVML access)\n", stderr);
            nvml_shim_unload();
            return 1;
        }
        fprintf(stderr, "WARNING: active fan control enabled (target=%.1fC", ctl.target_c);
        if (ctl.min_set)
            fprintf(stderr, ", min=%d%%", ctl.min_percent);
        fputs(").\n", stderr);
        fputs("This disengages the GPU's automatic fan curve. Misconfiguration can\n", stderr);
        fputs("lead to inadequate cooling. Automatic control is restored on exit via\n", stderr);
        fputs("Ctrl-C or SIGTERM, but NOT on SIGKILL (kill -9) or a crash.\n\n", stderr);
    }

    if (!watch) {
        n = query_nvml(gpus, is_root, &ctl);
        if (n == 0)
            fputs("no NVIDIA GPUs found\n", stderr);
        if (n <= 0) {
            nvml_shim_unload();
            return 1;
        }
        print_table(gpus, n, is_root, json);
        nvml_shim_unload();
        return 0;
    }

    /* --watch loop */
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    {
        struct timespec ts;
        ts.tv_sec  = (time_t)interval;
        ts.tv_nsec = (long)((interval - (double)ts.tv_sec) * 1e9);

        while (!g_stop) {
            n = query_nvml(gpus, is_root, &ctl);
            if (!json)
                printf("\033[2J\033[H");
            if (n < 0) {
                fputs("error: NVML query failed\n", stderr);
            } else if (n == 0) {
                fputs("no NVIDIA GPUs found\n", stderr);
            } else if (json) {
                print_table(gpus, n, is_root, 1);
                fflush(stdout);
            } else {
                char tbuf[64];
                time_t now = time(NULL);
                strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", localtime(&now));
                printf("gputemp --watch  (interval=%.1fs, Ctrl-C to stop)  %s\n", interval, tbuf);
                if (ctl.enabled) {
                    printf("fan control: target=%.1fC", ctl.target_c);
                    if (ctl.min_set)
                        printf(" (--min %d%%)", ctl.min_percent);
                    puts("\n");
                } else {
                    putchar('\n');
                }
                print_table(gpus, n, is_root, 0);
            }
            nanosleep(&ts, NULL);
        }
    }

    if (ctl.enabled) {
        if (!json)
            fputs("\nrestoring automatic fan control...\n", stderr);
        restore_all_fans_to_auto();
    }
    if (!json)
        puts("\nstopped.");
    nvml_shim_unload();
    return 0;

bad_target:
    fputs("error: --target expects a temperature like 65 or 65c\n", stderr);
    return 1;
}
