// FSR 4 for MHGU Recomp, as an engine plugin (plugin_api.h). The port has no FSR 4 code and no hooks for it: this plugin
// applies its changes to the running program itself, in memory, once it is loaded:
//
//   1. vkCreateInstance / vkCreateDevice (the port's import table entries): the instance asks for Vulkan 1.3 (FSR 4's shaders
//      use SPIR-V 1.6), and the device gets the extensions and features AMD's models use (cooperative matrix, float8, integer
//      dot products, compute shader derivatives, ...) where the GPU has them.
//   2. ffxVkPortableUpscaleContextRecordDispatch (the port's FSR 3.1 call, which the exe exports): the plugin takes the call
//      with exactly the inputs FSR 3.1 would get and records AMD's FSR 4 into the same command buffer.
//   When anything is missing or fails the plugin puts the original code back for good and FSR 3.1 carries on, from a fresh
//   history. With [Upscaler] FSR = Off nothing is touched at all.
//
// AMD's FSR SDK 2.4.0 loader and upscaler DLLs (amd_fidelityfx_loader_vk.dll, amd_fidelityfx_upscaler_vk.dll) sit in this
// plugin's folder; `make` downloads them (SHA-256 checked) from AMD's tagged release. Windows x64 only.
//
//   MHGU_FSR_DEBUG=1           FFX debug checking of the inputs (messages in the log)
//   MHGU_FSR4_LOADER=<path>    another loader DLL (its upscaler DLL is looked up beside it)
//   MHGU_FSR4_LINEAR=1         (A/B) the colour is not flagged as gamma-encoded
//   MHGU_FSR4_FAILTEST=<n>     report the n-th upscale as failed, to see the FSR 3.1 fallback
#define VK_NO_PROTOTYPES
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin_api.h"

#ifndef _WIN64
#include <vulkan/vulkan.h>
// Not Windows x64: nothing to patch with, the plugin declines.
MHGU_PLUGIN_EXPORT bool mhgu_plugin_entry(const MhguHost *host, MhguPlugin *out) {
    (void)out;
    host->log("[fsr4] this plugin is Windows x64 only: not loaded\n");
    return false;
}
#else

#include <windows.h>

#include <vulkan/vulkan.h>

#include <ffx_api.h>
#include <ffx_upscale.h>
#include <vk/ffx_api_vk.h>

#include "third_party/ffx_vk_portable.h"

static const MhguHost *g_host;
#define LOG(...) g_host->log(__VA_ARGS__)

#define NO_FSR3 "FSR 3.1 carries on"

// ================================================================== state
static PFN_vkGetInstanceProcAddr p_gipa;
static PFN_vkGetDeviceProcAddr p_gdpa;
static PFN_vkCreateInstance real_create_instance;
static PFN_vkCreateDevice real_create_device;
static PFN_vkEnumerateDeviceExtensionProperties p_enum_ext;
static PFN_vkGetPhysicalDeviceFeatures2 p_features2;

static VkInstance g_instance;
static VkPhysicalDevice g_phys;
static VkDevice g_dev;
static bool device_ok;   // the device was created with what FSR 4 needs
static bool linear_in;

typedef FfxVkPortableResult (*RecordFn)(FfxVkPortableUpscaleContext *, const FfxVkPortableUpscaleDispatchInfo *);
static RecordFn g_target;           // the port's FSR 3.1 dispatch (in the exe)
static uint8_t g_saved[14];         // its first bytes, put back when the plugin steps aside
static bool g_patched;

// ================================================================== patching (no disassembly: the original bytes are put back whole)
static bool write_code(void *where, const void *bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect(where, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(where, bytes, n);
    VirtualProtect(where, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), where, n);
    return true;
}

static void unpatch(void) {
    if (g_patched && write_code((void *)g_target, g_saved, sizeof g_saved)) g_patched = false;
}

// jmp [rip+0] ; .quad target
static bool patch(void *from, void *to) {
    uint8_t code[14] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(code + 6, &to, 8);
    memcpy(g_saved, from, sizeof g_saved);
    if (!write_code(from, code, sizeof code)) return false;
    g_patched = true;
    return true;
}

// The entry of `name` imported from `dll` in `mod`'s import table: replaced by `nw`, the old value into *orig.
static bool iat_hook(HMODULE mod, const char *dll, const char *name, void *nw, void **orig) {
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress) return false;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd->VirtualAddress); d->Name; d++) {
        if (_stricmp((const char *)(base + d->Name), dll)) continue;
        IMAGE_THUNK_DATA *oft = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA *ft = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            if (strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)(base + oft->u1.AddressOfData))->Name, name)) continue;
            DWORD old;
            if (!VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) return false;
            *orig = (void *)ft->u1.Function;
            ft->u1.Function = (ULONG_PTR)nw;
            VirtualProtect(&ft->u1.Function, sizeof(void *), old, &old);
            return true;
        }
    }
    return false;
}

// ================================================================== the device
// What the GPU must have for any FSR 4 model: the rest of the list is optional and switched on when the GPU offers it. The
// models differ per GPU (RDNA 4: float8 + cooperative matrix; RDNA 2 / 3: the INT8 models, which want cooperative matrix
// (RDNA 3) or integer dot products), and AMD's loader / upscaler DLL decides whether this GPU gets FSR 4 at all. The device is
// created before the loader is asked, so everything the models may use is enabled here; unused ones are harmless.
static bool have_cm, have_f8, have_dp, have_cd;  // (cd: the INT8 / prepass shaders use ComputeDerivativeGroupLinear)

static VkPhysicalDeviceCooperativeMatrixFeaturesKHR en_cm = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
static VkPhysicalDeviceShaderFloat8FeaturesEXT en_f8 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
static VkPhysicalDeviceShaderIntegerDotProductFeaturesKHR en_dp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES_KHR};
static VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR en_cd = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
static VkPhysicalDeviceSynchronization2FeaturesKHR en_s2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
static VkPhysicalDeviceSubgroupSizeControlFeaturesEXT en_sg = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};

#define MAX_EXT 40

static bool has_name(const char *const *list, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(list[i], name)) return true;
    return false;
}

// Works out what to add to this device creation. false: this GPU is not for FSR 4 (logged; nothing is changed).
// exts: the extension names to add; the features are set in the host's own structures (f11, f12, f) and chained after f12.
static bool plan_device(VkPhysicalDevice phys, const char **add, uint32_t *n_add, VkPhysicalDeviceVulkan11Features *f11,
                        VkPhysicalDeviceVulkan12Features *f12, VkPhysicalDeviceFeatures *f) {
    uint32_t ne = 0;
    p_enum_ext(phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
    if (!ep) return false;
    p_enum_ext(phys, NULL, &ne, ep);
    static const char *const need[] = {VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME};
    bool has[2] = {false, false};
    have_cm = have_f8 = have_dp = have_cd = false;
    for (uint32_t j = 0; j < ne; j++) {
        for (int i = 0; i < 2; i++) has[i] |= !strcmp(ep[j].extensionName, need[i]);
        have_cm |= !strcmp(ep[j].extensionName, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
        have_f8 |= !strcmp(ep[j].extensionName, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
        have_dp |= !strcmp(ep[j].extensionName, VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME);
        have_cd |= !strcmp(ep[j].extensionName, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
    }
    free(ep);
    for (int i = 0; i < 2; i++) {
        if (has[i]) continue;
        LOG("[fsr4] the GPU lacks %s: FSR 4 cannot run here, " NO_FSR3 "\n", need[i]);
        return false;
    }
    // what the GPU reports, for the optional structures only those whose extension exists
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm = {.sType = en_cm.sType};
    VkPhysicalDeviceShaderFloat8FeaturesEXT f8 = {.sType = en_f8.sType};
    VkPhysicalDeviceShaderIntegerDotProductFeaturesKHR dp = {.sType = en_dp.sType};
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR cd = {.sType = en_cd.sType};
    VkPhysicalDeviceSynchronization2FeaturesKHR s2 = {.sType = en_s2.sType};
    VkPhysicalDeviceSubgroupSizeControlFeaturesEXT sg = {.sType = en_sg.sType};
    void *chain = NULL;
    sg.pNext = chain, chain = &sg;
    s2.pNext = chain, chain = &s2;
    if (have_cm) cm.pNext = chain, chain = &cm;
    if (have_f8) f8.pNext = chain, chain = &f8;
    if (have_dp) dp.pNext = chain, chain = &dp;
    if (have_cd) cd.pNext = chain, chain = &cd;
    VkPhysicalDeviceVulkan12Features h12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = chain};
    VkPhysicalDeviceVulkan11Features h11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &h12};
    VkPhysicalDeviceFeatures2 h = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &h11};
    p_features2(phys, &h);
    bool use_cm = have_cm && cm.cooperativeMatrix, use_f8 = have_f8 && f8.shaderFloat8, use_dp = have_dp && dp.shaderIntegerDotProduct;
    LOG("[fsr4] GPU features: cooperative matrix %d, float8 %d/%d, integer dot product %d, compute derivatives %d\n", use_cm,
        f8.shaderFloat8, f8.shaderFloat8CooperativeMatrix, use_dp, have_cd && cd.computeDerivativeGroupLinear);
    if (!s2.synchronization2 || !sg.subgroupSizeControl || !sg.computeFullSubgroups || !h12.shaderFloat16 || !h12.shaderInt8 ||
        !h12.bufferDeviceAddress || !h12.vulkanMemoryModel || !h.features.shaderInt16 || !h11.storageBuffer16BitAccess) {
        LOG("[fsr4] the GPU lacks a feature FSR 4 needs (synchronization2 %d, subgroup size control %d/%d, fp16 %d, int8 %d, "
            "buffer device address %d, memory model %d, int16 %d, 16-bit storage %d): " NO_FSR3 "\n", s2.synchronization2,
            sg.subgroupSizeControl, sg.computeFullSubgroups, h12.shaderFloat16, h12.shaderInt8, h12.bufferDeviceAddress,
            h12.vulkanMemoryModel, h.features.shaderInt16, h11.storageBuffer16BitAccess);
        return false;
    }
    if (!use_cm && !use_f8 && !use_dp) {
        LOG("[fsr4] the GPU offers none of cooperative matrix, float8 or integer dot products: " NO_FSR3 "\n");
        return false;
    }
    uint32_t n = 0;
    add[n++] = need[0];
    add[n++] = need[1];
    if (have_cm) add[n++] = VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME;
    if (have_f8) add[n++] = VK_EXT_SHADER_FLOAT8_EXTENSION_NAME;
    if (have_dp) add[n++] = VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME;
    if (have_cd) add[n++] = VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME;
    *n_add = n;
    if (use_cm) en_cm.cooperativeMatrix = en_cm.cooperativeMatrixRobustBufferAccess = VK_TRUE;
    if (use_f8) en_f8.shaderFloat8 = VK_TRUE, en_f8.shaderFloat8CooperativeMatrix = f8.shaderFloat8CooperativeMatrix;
    if (use_dp) en_dp.shaderIntegerDotProduct = VK_TRUE;
    if (have_cd) en_cd.computeDerivativeGroupLinear = cd.computeDerivativeGroupLinear, en_cd.computeDerivativeGroupQuads = cd.computeDerivativeGroupQuads;
    en_s2.synchronization2 = VK_TRUE;
    en_sg.subgroupSizeControl = en_sg.computeFullSubgroups = VK_TRUE;
    f12->shaderFloat16 = f12->shaderInt8 = f12->bufferDeviceAddress = f12->vulkanMemoryModel = VK_TRUE;
    f12->vulkanMemoryModelDeviceScope = h12.vulkanMemoryModelDeviceScope;
    f12->shaderSubgroupExtendedTypes = h12.shaderSubgroupExtendedTypes;
    f12->uniformAndStorageBuffer8BitAccess = f12->storageBuffer8BitAccess = VK_TRUE;
    f12->descriptorIndexing = h12.descriptorIndexing;
    f12->runtimeDescriptorArray = h12.runtimeDescriptorArray;
    f12->descriptorBindingPartiallyBound = h12.descriptorBindingPartiallyBound;
    f12->shaderSampledImageArrayNonUniformIndexing = h12.shaderSampledImageArrayNonUniformIndexing;
    f12->shaderStorageImageArrayNonUniformIndexing = h12.shaderStorageImageArrayNonUniformIndexing;
    f11->storageBuffer16BitAccess = f11->uniformAndStorageBuffer16BitAccess = VK_TRUE;
    f11->variablePointers = h11.variablePointers;
    f11->variablePointersStorageBuffer = h11.variablePointersStorageBuffer;
    f->shaderInt16 = VK_TRUE;
    f->shaderStorageImageReadWithoutFormat = f->shaderStorageImageWriteWithoutFormat = VK_TRUE;
    // splice the extension structures in after the 1.2 features (whatever f12 pointed to stays behind them)
    void *rest = f12->pNext;
    en_sg.pNext = rest, rest = &en_sg;
    en_s2.pNext = rest, rest = &en_s2;
    if (have_cm) en_cm.pNext = rest, rest = &en_cm;
    if (have_f8) en_f8.pNext = rest, rest = &en_f8;
    if (have_dp) en_dp.pNext = rest, rest = &en_dp;
    if (have_cd) en_cd.pNext = rest, rest = &en_cd;
    f12->pNext = rest;
    return true;
}

// ================================================================== the Vulkan hooks
static VkResult VKAPI_CALL hook_create_instance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkInstance *out) {
    // Vulkan 1.3 at least (FSR 4's shaders use SPIR-V 1.6). The port asks for 1.2; if a driver refuses, it gets what it asked.
    VkApplicationInfo app = ci->pApplicationInfo ? *ci->pApplicationInfo : (VkApplicationInfo){.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO};
    if (app.apiVersion < VK_API_VERSION_1_3) {
        VkInstanceCreateInfo nc = *ci;
        app.apiVersion = VK_API_VERSION_1_3;
        nc.pApplicationInfo = &app;
        VkResult r = real_create_instance(&nc, alloc, out);
        if (r == VK_SUCCESS) { g_instance = *out; return r; }
        LOG("[fsr4] vkCreateInstance with Vulkan 1.3 failed (%d): asking for what the port asked\n", r);
    }
    VkResult r = real_create_instance(ci, alloc, out);
    if (r == VK_SUCCESS) g_instance = *out;
    return r;
}

static VkResult VKAPI_CALL hook_create_device(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc,
                                              VkDevice *out) {
    VkResult r;
    VkPhysicalDeviceFeatures2 *f2 = NULL;
    VkPhysicalDeviceVulkan11Features *f11 = NULL;
    VkPhysicalDeviceVulkan12Features *f12 = NULL;
    for (VkBaseOutStructure *s = (VkBaseOutStructure *)ci->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) f2 = (VkPhysicalDeviceFeatures2 *)s;
        else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) f11 = (VkPhysicalDeviceVulkan11Features *)s;
        else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) f12 = (VkPhysicalDeviceVulkan12Features *)s;
    }
    const char *add[MAX_EXT];
    uint32_t n_add = 0;
    VkPhysicalDeviceVulkan11Features b11;
    VkPhysicalDeviceVulkan12Features b12;
    VkPhysicalDeviceFeatures bf;
    VkBaseOutStructure *b12_next = NULL;
    bool changed = false;
    if (g_instance && f2 && f11 && f12 && p_enum_ext && p_features2) {
        b11 = *f11, b12 = *f12, bf = f2->features, b12_next = (VkBaseOutStructure *)f12->pNext;  // (to put back if it fails)
        changed = plan_device(phys, add, &n_add, f11, f12, &f2->features);
    } else {
        LOG("[fsr4] the port's device creation has no feature chain to extend: " NO_FSR3 "\n");
    }
    if (changed) {
        const char **names = calloc(ci->enabledExtensionCount + n_add, sizeof *names);
        uint32_t n = 0;
        for (uint32_t i = 0; names && i < ci->enabledExtensionCount; i++) names[n++] = ci->ppEnabledExtensionNames[i];
        for (uint32_t i = 0; names && i < n_add; i++)
            if (!has_name(names, n, add[i])) names[n++] = add[i];
        if (names) {
            VkDeviceCreateInfo nc = *ci;
            nc.enabledExtensionCount = n;
            nc.ppEnabledExtensionNames = names;
            r = real_create_device(phys, &nc, alloc, out);
            free(names);
            if (r == VK_SUCCESS) { g_phys = phys; g_dev = *out; device_ok = true; return r; }
            LOG("[fsr4] the device with FSR 4's features was refused (%d): the port's own is created\n", r);
        }
        *f11 = b11, *f12 = b12, f2->features = bf;  // (put back what the port had set up)
        f12->pNext = b12_next;
    }
    r = real_create_device(phys, ci, alloc, out);
    if (r == VK_SUCCESS) g_phys = phys, g_dev = *out;
    return r;
}

// ================================================================== the loader (AMD's, beside this plugin)
static char prov_name[64];
static ffxReturnCode_t (*f_create)(ffxContext *, ffxCreateContextDescHeader *, const ffxAllocationCallbacks *);
static ffxReturnCode_t (*f_destroy)(ffxContext *, const ffxAllocationCallbacks *);
static ffxReturnCode_t (*f_dispatch)(ffxContext *, const ffxDispatchDescHeader *);
static uint64_t loader_ver;
static ffxContext ctx;
static uint32_t ctx_rw, ctx_rh, ctx_ow, ctx_oh;
static bool loader_tried, loaded, failed;
static uint64_t frames;
static const char *g_dir;

static void file_version(const wchar_t *path, char *out, size_t n) {
    snprintf(out, n, "?");
    DWORD z, sz = GetFileVersionInfoSizeW(path, &z);
    if (!sz) return;
    void *buf = malloc(sz);
    VS_FIXEDFILEINFO *fi;
    UINT l;
    if (buf && GetFileVersionInfoW(path, 0, sz, buf) && VerQueryValueW(buf, L"\\", (void **)&fi, &l) && fi)
        snprintf(out, n, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS),
                 LOWORD(fi->dwFileVersionLS));
    free(buf);
}

static bool init_loader(void) {
    wchar_t path[MAX_PATH] = {0};
    const char *env = getenv("MHGU_FSR4_LOADER");
    char file[MAX_PATH + 40];
    if (env) snprintf(file, sizeof file, "%s", env);
    else snprintf(file, sizeof file, "%s/amd_fidelityfx_loader_vk.dll", g_dir);
    MultiByteToWideChar(CP_UTF8, 0, file, -1, path, MAX_PATH);
    for (wchar_t *c = path; *c; c++)
        if (*c == L'/') *c = L'\\';
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        LOG("[fsr4] %ls not found (the plugin's `make` downloads it): " NO_FSR3 "\n", path);
        return false;
    }
    HMODULE ld = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!ld) { LOG("[fsr4] %ls could not be loaded (error %lu)\n", path, GetLastError()); return false; }
    char ver[32];
    file_version(path, ver, sizeof ver);
    LOG("[fsr4] loader %ls, file version %s\n", path, ver);
    PfnFfxQuery query = (PfnFfxQuery)GetProcAddress(ld, "ffxQuery");
    f_create = (void *)GetProcAddress(ld, "ffxCreateContext");
    f_destroy = (void *)GetProcAddress(ld, "ffxDestroyContext");
    f_dispatch = (void *)GetProcAddress(ld, "ffxDispatch");
    if (!query || !f_create || !f_destroy || !f_dispatch) { LOG("[fsr4] the loader lacks the ffx-api exports\n"); return false; }
    VkDeviceContext dc = {.vkDevice = g_dev, .vkPhysicalDevice = g_phys, .vkDeviceProcAddr = p_gdpa};
    uint64_t n = 16, ids[16] = {0};
    const char *names[16] = {0};
    struct ffxQueryDescGetVersions gv = {.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS, .createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
                                         .device = &dc, .outputCount = &n, .versionIds = ids, .versionNames = names};
    ffxReturnCode_t rc = query(NULL, &gv.header);
    if (rc != FFX_API_RETURN_OK) {
        LOG("[fsr4] the loader's version query failed (ffx return code %d; is amd_fidelityfx_upscaler_vk.dll beside it?)\n", (int)rc);
        return false;
    }
    int best = -1;
    for (uint64_t i = 0; i < n; i++) {
        LOG("[fsr4] loader offers upscaler %s (id %#llx)\n", names[i] ? names[i] : "?", (unsigned long long)ids[i]);
        if (names[i] && names[i][0] == '4' && names[i][1] == '.' && (best < 0 || strcmp(names[i], names[best]) > 0)) best = (int)i;
    }
    if (best < 0) { LOG("[fsr4] this loader offers no FSR 4 (4.x) upscaler on this GPU\n"); return false; }
    loader_ver = ids[best];
    snprintf(prov_name, sizeof prov_name, "FSR %s", names[best]);
    return true;
}

// ================================================================== context and dispatch
static void ffx_message(uint32_t type, const wchar_t *msg) {
    LOG("[fsr4] runtime %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "ERROR" : "warning", msg);
}

static uint32_t ffx_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case VK_FORMAT_R32G32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32G32_FLOAT;
    case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_R8_UNORM: return FFX_API_SURFACE_FORMAT_R8_UNORM;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

// every image lives in GENERAL in the port, which is what "unordered access" means to the backend: no layout changes
static struct FfxApiResource resource(const FfxVkPortableImage *i, bool uav) {
    struct FfxApiResource r = {0};
    if (!i->image) return r;
    r.resource = (void *)i->image;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffx_format(i->format);
    r.description.width = i->extent.width, r.description.height = i->extent.height, r.description.depth = 1, r.description.mipCount = 1;
    r.description.usage = uav ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    r.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    return r;
}

static bool fail(const char *why, int rc) {
    LOG("[fsr4] %s (ffx return code %d): FSR 4 is off from here on, " NO_FSR3 "\n", why, rc);
    failed = true;
    return false;
}

static bool make_context(const FfxVkPortableUpscaleDispatchInfo *d) {
    if (ctx && ctx_rw == d->renderSize.width && ctx_rh == d->renderSize.height && ctx_ow == d->outputSize.width && ctx_oh == d->outputSize.height)
        return true;
    if (ctx) { f_destroy(&ctx, NULL); ctx = NULL; }
    // LDR, gamma-encoded colour (the post-processed picture), standard depth, render-size motion vectors without jitter
    struct ffxCreateContextDescUpscale cu = {.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
                                      .flags = (linear_in ? 0 : FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE) | (getenv("MHGU_FSR_DEBUG") ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING : 0),
                                      .maxRenderSize = {d->renderSize.width, d->renderSize.height},
                                      .maxUpscaleSize = {d->outputSize.width, d->outputSize.height}, .fpMessage = ffx_message};
    struct ffxCreateContextDescUpscaleVersion uv = {.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION, .version = FFX_UPSCALER_VERSION};
    struct ffxCreateBackendVKDesc bd = {.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK, .deviceContext = {g_dev, g_phys, p_gdpa}};
    struct ffxOverrideVersion ov = {.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION, .versionId = loader_ver};
    bd.header.pNext = &uv.header;
    uv.header.pNext = &ov.header;
    cu.header.pNext = &bd.header;
    ULONGLONG t0 = GetTickCount64();
    ffxReturnCode_t rc = f_create(&ctx, &cu.header, NULL);
    if (rc != FFX_API_RETURN_OK || !ctx) { ctx = NULL; return fail("the FSR 4 context could not be created", (int)rc); }
    ctx_rw = d->renderSize.width, ctx_rh = d->renderSize.height, ctx_ow = d->outputSize.width, ctx_oh = d->outputSize.height;
    LOG("[fsr4] %s context %ux%u -> %ux%u created in %llu ms (non-linear colour; colour VkFormat %d, depth %d, motion vectors %d, "
        "reactive %d, output %d)\n", prov_name, ctx_rw, ctx_rh, ctx_ow, ctx_oh, GetTickCount64() - t0, d->color.format,
        d->depth.format, d->motionVectors.format, d->reactiveMask.image ? d->reactiveMask.format : 0, d->output.format);
    return true;
}

static bool upscale(const FfxVkPortableUpscaleDispatchInfo *d) {
    if (!loader_tried) {
        loader_tried = true;
        if (!init_loader()) { failed = true; return false; }
        linear_in = getenv("MHGU_FSR4_LINEAR") != NULL;
        loaded = true;
        LOG("[fsr4] route: AMD's FSR SDK loader (ffx-api), running as %s\n", prov_name);
    }
    if (!loaded || failed || !make_context(d)) return false;
    struct ffxDispatchDescUpscale du = {.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE, .commandList = d->commandBuffer};
    du.color = resource(&d->color, false);
    du.depth = resource(&d->depth, false);
    du.motionVectors = resource(&d->motionVectors, false);
    du.reactive = resource(&d->reactiveMask, false);
    du.output = resource(&d->output, true);
    du.jitterOffset = (struct FfxApiFloatCoords2D){d->jitterOffset.x, d->jitterOffset.y};
    du.motionVectorScale = (struct FfxApiFloatCoords2D){1.0f, 1.0f};
    du.renderSize = (struct FfxApiDimensions2D){d->renderSize.width, d->renderSize.height};
    du.upscaleSize = (struct FfxApiDimensions2D){d->outputSize.width, d->outputSize.height};
    du.enableSharpening = d->enableSharpening;
    du.sharpness = d->sharpness;
    du.frameTimeDelta = d->frameTimeMilliseconds;
    du.preExposure = 1.0f;
    du.reset = d->reset;
    du.cameraNear = d->cameraNear;
    du.cameraFar = d->cameraFar;
    du.cameraFovAngleVertical = d->cameraVerticalFovRadians;
    du.viewSpaceToMetersFactor = d->viewSpaceToMeters;
    du.flags = linear_in ? 0 : FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    static int fail_at = -2;
    if (fail_at == -2) fail_at = getenv("MHGU_FSR4_FAILTEST") ? atoi(getenv("MHGU_FSR4_FAILTEST")) : -1;
    ffxReturnCode_t rc = fail_at >= 0 && frames == (uint64_t)fail_at ? FFX_API_RETURN_ERROR : f_dispatch(&ctx, &du.header);
    if (rc != FFX_API_RETURN_OK) return fail("the FSR 4 dispatch failed", (int)rc);
    if (++frames == 1) LOG("[fsr4] first %s dispatch recorded\n", prov_name);
    return true;
}

// ================================================================== the port's FSR 3.1 call
static FfxVkPortableResult hook_record_dispatch(FfxVkPortableUpscaleContext *c, const FfxVkPortableUpscaleDispatchInfo *d) {
    if (d && d->structSize >= sizeof *d && device_ok && upscale(d)) return FFX_VK_PORTABLE_OK;
    // Not FSR 4 (anything failed, logged): the original code goes back for good, and FSR 3.1 runs this frame from a fresh history.
    unpatch();
    if (!d || d->structSize < sizeof *d) {
        LOG("[fsr4] the port's FSR 3.1 dispatch info is %u bytes, this plugin knows %zu: " NO_FSR3 "\n", d ? d->structSize : 0, sizeof *d);
        return g_target(c, d);
    }
    FfxVkPortableUpscaleDispatchInfo fresh = *d;
    fresh.reset = VK_TRUE;
    return g_target(c, &fresh);
}

// ================================================================== entry
static bool setting_off(const char *key) {
    char v[32];
    if (!g_host->setting(key, v, sizeof v)) return true;
    return !_stricmp(v, "off") || !_stricmp(v, "0") || !_stricmp(v, "false") || !_stricmp(v, "no");
}

MHGU_PLUGIN_EXPORT bool mhgu_plugin_entry(const MhguHost *host, MhguPlugin *out) {
    g_host = host;
    g_dir = host->plugin_dir;
    out->api = MHGU_PLUGIN_API;
    out->name = "FSR 4 (AMD SDK)";
    if (setting_off("FSR")) {
        LOG("[fsr4] [Upscaler] FSR is Off: nothing to do\n");
        return true;  // (loaded, idle: changing FSR to On takes effect at the next start)
    }
    HMODULE exe = GetModuleHandleW(NULL), vk = LoadLibraryA("vulkan-1.dll");
    g_target = (RecordFn)(void *)GetProcAddress(exe, "ffxVkPortableUpscaleContextRecordDispatch");
    p_gipa = vk ? (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(vk, "vkGetInstanceProcAddr") : NULL;
    p_gdpa = vk ? (PFN_vkGetDeviceProcAddr)(void *)GetProcAddress(vk, "vkGetDeviceProcAddr") : NULL;
    if (!g_target || !p_gipa || !p_gdpa) {
        LOG("[fsr4] this build of the port does not have what the plugin hooks (FSR 3.1's dispatch %p, Vulkan %p): not loaded\n",
            (void *)g_target, (void *)vk);
        return false;
    }
    void *o1 = NULL, *o2 = NULL;
    if (!iat_hook(exe, "vulkan-1.dll", "vkCreateInstance", (void *)hook_create_instance, &o1) ||
        !iat_hook(exe, "vulkan-1.dll", "vkCreateDevice", (void *)hook_create_device, &o2)) {
        LOG("[fsr4] the port does not import vkCreateInstance / vkCreateDevice from vulkan-1.dll: not loaded\n");
        return false;
    }
    real_create_instance = (PFN_vkCreateInstance)o1;
    real_create_device = (PFN_vkCreateDevice)o2;
    p_enum_ext = (PFN_vkEnumerateDeviceExtensionProperties)(void *)GetProcAddress(vk, "vkEnumerateDeviceExtensionProperties");
    p_features2 = (PFN_vkGetPhysicalDeviceFeatures2)(void *)GetProcAddress(vk, "vkGetPhysicalDeviceFeatures2");
    if (!p_features2) p_features2 = (PFN_vkGetPhysicalDeviceFeatures2)(void *)GetProcAddress(vk, "vkGetPhysicalDeviceFeatures2KHR");
    if (!patch((void *)g_target, (void *)hook_record_dispatch)) {
        LOG("[fsr4] the port's code could not be patched: not loaded\n");
        return false;
    }
    LOG("[fsr4] hooks in place (vkCreateInstance, vkCreateDevice, FSR 3.1 dispatch at %p)\n", (void *)g_target);
    return true;
}

#endif  // _WIN64
