#include "AlphaFix.h"
#include "AlphaFixDecoder.h"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

#ifdef _WIN32
    #include <pathcch.h>
    #include <delayimp.h>
    #pragma comment(lib, "pathcch.lib")
    #pragma comment(lib, "delayimp.lib")
#endif

// ── Global plugin ID — registered once in GlobalSetup, reused everywhere ────
static AEGP_PluginID g_PluginID = 0;


// ═══════════════════════════════════════════════════════════════════════════════
// Handle Suite Helpers
// ═══════════════════════════════════════════════════════════════════════════════

static PF_Handle AllocHandle(PF_InData* in_data, size_t size)
{
    AEFX_SuiteScoper<PF_HandleSuite1> suite(
        in_data, kPFHandleSuite, kPFHandleSuiteVersion1, nullptr);
    return suite->host_new_handle(static_cast<A_HandleSize>(size));
}

static void* LockHandle(PF_InData* in_data, PF_Handle h)
{
    AEFX_SuiteScoper<PF_HandleSuite1> suite(
        in_data, kPFHandleSuite, kPFHandleSuiteVersion1, nullptr);
    return suite->host_lock_handle(h);
}

static void UnlockHandle(PF_InData* in_data, PF_Handle h)
{
    AEFX_SuiteScoper<PF_HandleSuite1> suite(
        in_data, kPFHandleSuite, kPFHandleSuiteVersion1, nullptr);
    suite->host_unlock_handle(h);
}

static void DisposeHandle(PF_InData* in_data, PF_Handle h)
{
    AEFX_SuiteScoper<PF_HandleSuite1> suite(
        in_data, kPFHandleSuite, kPFHandleSuiteVersion1, nullptr);
    suite->host_dispose_handle(h);
}


// ═══════════════════════════════════════════════════════════════════════════════
// FFmpeg loader
//
// Why this is not just SetDllDirectory:
//
//   1. SetDllDirectory is process-wide and last-writer-wins. Any other AE
//      plugin can overwrite it after our GlobalSetup ran.
//   2. If ANY module in the process calls SetDefaultDllDirectories, the
//      directory set by SetDllDirectory stops participating in the search
//      entirely.
//   3. Delay-loaded imports resolve lazily, on a render worker thread, long
//      after GlobalSetup. Whatever the search path looks like at that moment
//      is what we get, and a miss raises 0xC06D007E which takes AE down.
//
// So instead we do two independent things:
//
//   a. Explicitly LoadLibraryExW every DLL in the sidecar folder by ABSOLUTE
//      path with LOAD_WITH_ALTERED_SEARCH_PATH, in dependency order. Once a
//      module with that base name is loaded, the delay-load helper finds it
//      regardless of search path state.
//   b. Install delay-load notify and failure hooks that load by absolute path
//      from our folder. This covers any DLL the enumeration missed and any
//      later load attempt.
// ═══════════════════════════════════════════════════════════════════════════════

#ifdef _WIN32

// Delay-load exception codes raised by __delayLoadHelper2.
static const unsigned int kDelayLoadModNotFound  = 0xC06D007Eu;
static const unsigned int kDelayLoadProcNotFound = 0xC06D007Fu;

// ── Loader log ───────────────────────────────────────────────────────────────
// Always active (a handful of lines per session) and separate from the
// per-frame debug log, because loader failures happen before the user has any
// chance to tick the Debug Log checkbox.
static void LoaderLog(const char* fmt, ...)
{
    char path[512];
    const char* tmp = getenv("TEMP");
    if (!tmp) tmp = "C:\\Temp";
    snprintf(path, sizeof(path), "%s\\AlphaFix_loader.log", tmp);

    FILE* f = fopen(path, "a");
    if (!f) return;

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fclose(f);
}

// ── Sidecar directory: <folder containing AlphaFix.aex>\alphafix.ffmpeg ──────
static bool ComputeFFmpegDir(std::wstring& out)
{
    wchar_t modulePath[MAX_PATH] = {};
    HMODULE hSelf = nullptr;

    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ComputeFFmpegDir),
            &hSelf) || !hSelf) {
        return false;
    }

    if (GetModuleFileNameW(hSelf, modulePath, MAX_PATH) == 0)
        return false;

    if (FAILED(PathCchRemoveFileSpec(modulePath, MAX_PATH)))
        return false;

    out  = modulePath;
    out += L"\\alphafix.ffmpeg";
    return true;
}

static const std::wstring& FFmpegDir()
{
    // Magic static: thread-safe one-time init under MSVC 2015 and later.
    static std::wstring dir = []() -> std::wstring {
        std::wstring d;
        if (!ComputeFFmpegDir(d)) d.clear();
        return d;
    }();
    return dir;
}

// ── Load one DLL by absolute path out of the sidecar folder ─────────────────
// LOAD_WITH_ALTERED_SEARCH_PATH makes that DLL's own dependencies resolve from
// its directory first, which is how sibling FFmpeg DLLs find each other.
static HMODULE LoadFFmpegDll(const wchar_t* fileName)
{
    if (!fileName || !*fileName) return nullptr;

    const std::wstring& dir = FFmpegDir();
    if (dir.empty()) return nullptr;

    std::wstring full = dir;
    full += L"\\";
    full += fileName;

    return LoadLibraryExW(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

// ── Delay-load hook ──────────────────────────────────────────────────────────
// Used for BOTH the notify hook (fires before the default search, so the
// process search path never even gets consulted) and the failure hook (last
// resort). Returning a non-null HMODULE cast to FARPROC satisfies the helper.
//
// delayimp.lib is statically linked into this .aex, so these globals are
// private to our module and cannot disturb other plugins.
static FARPROC WINAPI AlphaFixDliHook(unsigned dliNotify, PDelayLoadInfo pdli)
{
    if ((dliNotify == dliNotePreLoadLibrary || dliNotify == dliFailLoadLib) &&
        pdli && pdli->szDll)
    {
        wchar_t wide[MAX_PATH] = {};
        if (MultiByteToWideChar(CP_ACP, 0, pdli->szDll, -1, wide, MAX_PATH) > 0) {
            HMODULE h = LoadFFmpegDll(wide);
            if (h) return reinterpret_cast<FARPROC>(h);

            if (dliNotify == dliFailLoadLib) {
                LoaderLog("[delayload] FAILED to resolve %s from sidecar (err %lu)",
                          pdli->szDll, (unsigned long)GetLastError());
            }
        }
    }
    return nullptr;
}

// ── Hook installation ────────────────────────────────────────────────────────
// delayimp.h declares these as `const PfnDliHook`, so they cannot be assigned
// at runtime (C3892). They must be DEFINED here instead. delayimp.lib carries
// default null definitions in a separate object file, which the linker only
// pulls in if the symbol is still unresolved, so ours wins with no duplicate
// symbol error.
//
// These are statically initialized with a function address, so there is no
// static-init-order hazard: the hooks are armed before any code runs.
extern "C" const PfnDliHook __pfnDliNotifyHook2  = AlphaFixDliHook;
extern "C" const PfnDliHook __pfnDliFailureHook2 = AlphaFixDliHook;

// ── Load order priority: dependencies first ─────────────────────────────────
static int DllLoadPriority(const std::wstring& name)
{
    struct Entry { const wchar_t* prefix; int prio; };
    static const Entry table[] = {
        { L"avutil",     0 },
        { L"swresample", 1 },
        { L"swscale",    2 },
        { L"postproc",   3 },
        { L"avcodec",    4 },
        { L"avformat",   5 },
        { L"avfilter",   6 },
        { L"avdevice",   7 },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        size_t len = wcslen(table[i].prefix);
        if (_wcsnicmp(name.c_str(), table[i].prefix, len) == 0)
            return table[i].prio;
    }
    return 9;   // Unknown (dependency DLLs etc.) go last.
}

static bool StartsWithNoCase(const std::wstring& s, const wchar_t* prefix)
{
    return _wcsnicmp(s.c_str(), prefix, wcslen(prefix)) == 0;
}

// ── Enumerate and load everything in the sidecar folder ─────────────────────
static bool LoadAllFFmpegDlls()
{
    const std::wstring& dir = FFmpegDir();
    if (dir.empty()) {
        LoaderLog("[loader] Could not determine plugin directory.");
        return false;
    }

    // Log the resolved directory as UTF-8 so the log is readable.
    {
        char dirUtf8[1024] = {};
        WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, dirUtf8, sizeof(dirUtf8), nullptr, nullptr);
        LoaderLog("[loader] FFmpeg directory: %s", dirUtf8);
    }

    std::wstring pattern = dir + L"\\*.dll";

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        LoaderLog("[loader] No DLLs found in sidecar folder (err %lu). "
                  "Expected the FFmpeg DLLs in an 'alphafix.ffmpeg' folder "
                  "next to AlphaFix.aex.", (unsigned long)GetLastError());
        return false;
    }

    std::vector<std::wstring> files;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            files.push_back(fd.cFileName);
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    std::sort(files.begin(), files.end(),
        [](const std::wstring& a, const std::wstring& b) {
            int pa = DllLoadPriority(a);
            int pb = DllLoadPriority(b);
            if (pa != pb) return pa < pb;
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        });

    // Multi-pass: a DLL that fails because a sibling was not loaded yet gets
    // another chance on the next pass.
    std::vector<std::wstring> pending = files;
    std::vector<std::wstring> loaded;

    for (int pass = 0; pass < 3 && !pending.empty(); ++pass) {
        std::vector<std::wstring> retry;
        for (size_t i = 0; i < pending.size(); ++i) {
            if (LoadFFmpegDll(pending[i].c_str()))
                loaded.push_back(pending[i]);
            else
                retry.push_back(pending[i]);
        }
        if (retry.size() == pending.size()) {   // No progress, stop early.
            pending.swap(retry);
            break;
        }
        pending.swap(retry);
    }

    bool haveFormat = false;
    bool haveCodec  = false;
    bool haveUtil   = false;
    for (size_t i = 0; i < loaded.size(); ++i) {
        if (StartsWithNoCase(loaded[i], L"avformat")) haveFormat = true;
        if (StartsWithNoCase(loaded[i], L"avcodec"))  haveCodec  = true;
        if (StartsWithNoCase(loaded[i], L"avutil"))   haveUtil   = true;
    }

    LoaderLog("[loader] Loaded %d of %d DLLs (avformat=%d avcodec=%d avutil=%d)",
              (int)loaded.size(), (int)files.size(),
              haveFormat ? 1 : 0, haveCodec ? 1 : 0, haveUtil ? 1 : 0);

    for (size_t i = 0; i < pending.size(); ++i) {
        char nameUtf8[512] = {};
        WideCharToMultiByte(CP_UTF8, 0, pending[i].c_str(), -1,
                            nameUtf8, sizeof(nameUtf8), nullptr, nullptr);
        LoaderLog("[loader]   FAILED: %s", nameUtf8);
    }

    return haveFormat && haveCodec && haveUtil;
}

#endif  // _WIN32


bool AlphaFix_EnsureFFmpegLoaded()
{
#ifdef _WIN32
    // Magic static gives us thread-safe run-exactly-once semantics, which
    // matters because SUPPORTS_THREADED_RENDERING means EffectMain can be
    // entered from several render threads at the same instant.
    static const bool ok = []() -> bool {
        // The delay-load hooks are already armed via the const definitions
        // above (statically initialized), so nothing to install here.
        bool r = LoadAllFFmpegDlls();
        LoaderLog("[loader] FFmpeg %s", r ? "READY" : "UNAVAILABLE (effect will pass through)");
        return r;
    }();
    return ok;
#else
    return true;
#endif
}

bool AlphaFix_LoadFFmpegDLLs()
{
    return AlphaFix_EnsureFFmpegLoaded();
}


// ═══════════════════════════════════════════════════════════════════════════════
// SEH-guarded decoder wrappers
//
// A delay-load miss raises a structured exception (0xC06D007E). Under /EHsc the
// catch(...) in EffectMain does NOT catch that, so it walks straight out of the
// plugin and kills AE. These wrappers convert it into a clean nullptr/error so
// the effect degrades to pass-through.
//
// These functions deliberately contain no objects with destructors, which is
// what __try/__except requires (C2712).
// ═══════════════════════════════════════════════════════════════════════════════

#ifdef _WIN32
static int DelayLoadFilter(unsigned int code)
{
    if (code == kDelayLoadModNotFound || code == kDelayLoadProcNotFound) {
        LoaderLog("[seh] Caught delay-load exception 0x%08X while calling FFmpeg. "
                  "Check that the alphafix.ffmpeg folder sits next to AlphaFix.aex "
                  "and contains matching x64 DLLs.", code);
        return EXCEPTION_EXECUTE_HANDLER;
    }
    // Anything else (a real access violation, say) is NOT swallowed. Masking
    // those would hide genuine bugs.
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static AlphaFixDecoder::Context* SafeCreateDecoder(const char* path)
{
#ifdef _WIN32
    __try {
        return AlphaFixDecoder::Create(path);
    }
    __except (DelayLoadFilter(GetExceptionCode())) {
        return nullptr;
    }
#else
    return AlphaFixDecoder::Create(path);
#endif
}

static PF_Err SafeDecodeAlpha(
    AlphaFixDecoder::Context* ctx,
    A_long      frameNum,
    uint8_t**   outAlpha,
    A_long*     outWidth,
    A_long*     outHeight)
{
#ifdef _WIN32
    __try {
        return AlphaFixDecoder::DecodeAlpha(ctx, frameNum, outAlpha, outWidth, outHeight);
    }
    __except (DelayLoadFilter(GetExceptionCode())) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
#else
    return AlphaFixDecoder::DecodeAlpha(ctx, frameNum, outAlpha, outWidth, outHeight);
#endif
}

static void SafeDestroyDecoder(AlphaFixDecoder::Context* ctx)
{
#ifdef _WIN32
    __try {
        AlphaFixDecoder::Destroy(ctx);
    }
    __except (DelayLoadFilter(GetExceptionCode())) {
        // Leaked, but the alternative is taking AE down.
    }
#else
    AlphaFixDecoder::Destroy(ctx);
#endif
}


// ═══════════════════════════════════════════════════════════════════════════════
// Plugin Data Entry
// ═══════════════════════════════════════════════════════════════════════════════

extern "C" ALPHAFIX_API PF_Err PluginDataEntryFunction(
    PF_PluginDataPtr    inPtr,
    PF_PluginDataCB     inPluginDataCallBackPtr,
    SPBasicSuite*       inSPBasicSuitePtr,
    const char*         inHostName,
    const char*         inHostVersion)
{
    PF_Err result = PF_Err_NONE;
    result = PF_REGISTER_EFFECT(
        inPtr, inPluginDataCallBackPtr,
        ALPHAFIX_NAME, ALPHAFIX_MATCH_NAME,
        "Channel", AE_RESERVED_INFO);
    return result;
}


// ═══════════════════════════════════════════════════════════════════════════════
// Global Setup
// ═══════════════════════════════════════════════════════════════════════════════

static PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data)
{    
    out_data->my_version = PF_VERSION(
        ALPHAFIX_MAJOR_VERSION, ALPHAFIX_MINOR_VERSION,
        ALPHAFIX_BUG_VERSION, PF_Stage_DEVELOP, ALPHAFIX_BUILD_VERSION);

    out_data->out_flags  = PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_NON_PARAM_VARY;
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_SMART_RENDER |
                           PF_OutFlag2_FLOAT_COLOR_AWARE |
                           PF_OutFlag2_SUPPORTS_THREADED_RENDERING;

    // Register once here — g_PluginID is reused by GetLayerSourceFilePath
    // on every render call
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    suites.UtilitySuite6()->AEGP_RegisterWithAEGP(
        nullptr, ALPHAFIX_NAME, &g_PluginID);

    return PF_Err_NONE;
}


// ═══════════════════════════════════════════════════════════════════════════════
// Params Setup
// ═══════════════════════════════════════════════════════════════════════════════

static PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data)
{
    PF_Err      err = PF_Err_NONE;
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Enable", "Fix Alpha", TRUE, 0, ALPHAFIX_ENABLE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Alpha Only", "Show Alpha", FALSE, 0, ALPHAFIX_ALPHA_ONLY);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Alpha Mode", BLEND_NUM_MODES, BLEND_REPLACE + 1,
        "Replace|Multiply|Maximum", ALPHAFIX_BLEND_MODE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Remove Color Matting", "", TRUE, 0, ALPHAFIX_REMOVE_COLOR_MATTING);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Frame Offset", -100, 100, -10, 10, 0, ALPHAFIX_FRAME_OFFSET);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Debug Log", "Write to %TEMP%", FALSE, 0, ALPHAFIX_DEBUG_LOG);

    out_data->num_params = ALPHAFIX_NUM_PARAMS;
    return err;
}


// ═══════════════════════════════════════════════════════════════════════════════
// Helper: Get source file path via AEGP
// ═══════════════════════════════════════════════════════════════════════════════

static PF_Err GetLayerSourceFilePath(PF_InData* in_data, char* outPath, A_long maxPathLen)
{
    PF_Err err = PF_Err_NONE;
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    AEGP_PluginID pluginId = g_PluginID;
    if (!pluginId) return PF_Err_INTERNAL_STRUCT_DAMAGED;

    AEGP_EffectRefH effectRef = nullptr;
    ERR(suites.PFInterfaceSuite1()->AEGP_GetNewEffectForEffect(
        pluginId, in_data->effect_ref, &effectRef));
    if (err || !effectRef) return err;

    AEGP_LayerH layerH = nullptr;
    ERR(suites.PFInterfaceSuite1()->AEGP_GetEffectLayer(
        in_data->effect_ref, &layerH));

    if (!err && layerH) {
        AEGP_ItemH itemH = nullptr;
        ERR(suites.LayerSuite8()->AEGP_GetLayerSourceItem(layerH, &itemH));

        if (!err && itemH) {
            AEGP_ItemType itemType;
            ERR(suites.ItemSuite9()->AEGP_GetItemType(itemH, &itemType));

            if (!err && itemType == AEGP_ItemType_FOOTAGE) {
                AEGP_FootageH footageH = nullptr;
                ERR(suites.FootageSuite5()->AEGP_GetMainFootageFromItem(itemH, &footageH));

                if (!err && footageH) {
                    AEGP_MemHandle pathMemH = nullptr;
                    ERR(suites.FootageSuite5()->AEGP_GetFootagePath(
                        footageH, 0, AEGP_FOOTAGE_MAIN_FILE_INDEX, &pathMemH));

                    if (!err && pathMemH) {
                        A_UTF16Char* utf16Path = nullptr;
                        ERR(suites.MemorySuite1()->AEGP_LockMemHandle(
                            pathMemH, reinterpret_cast<void**>(&utf16Path)));
                        if (!err && utf16Path) {
#ifdef _WIN32
                            WideCharToMultiByte(CP_UTF8, 0,
                                reinterpret_cast<LPCWSTR>(utf16Path), -1,
                                outPath, maxPathLen, nullptr, nullptr);
#else
                            // AE supplies UTF-16 on both platforms; FFmpeg expects UTF-8.
                            CFIndex length = 0;
                            while (utf16Path[length]) ++length;
                            CFStringRef path = CFStringCreateWithCharacters(
                                kCFAllocatorDefault,
                                reinterpret_cast<const UniChar*>(utf16Path), length);
                            if (path) {
                                if (!CFStringGetCString(path, outPath, maxPathLen,
                                                        kCFStringEncodingUTF8)) {
                                    outPath[0] = '\0';
                                    err = PF_Err_BAD_CALLBACK_PARAM;
                                }
                                CFRelease(path);
                            } else {
                                outPath[0] = '\0';
                                err = PF_Err_OUT_OF_MEMORY;
                            }
#endif
                            suites.MemorySuite1()->AEGP_UnlockMemHandle(pathMemH);
                        }
                        suites.MemorySuite1()->AEGP_FreeMemHandle(pathMemH);
                    }
                }
            }
        }
    }

    suites.EffectSuite4()->AEGP_DisposeEffect(effectRef);
    return err;
}


// ═══════════════════════════════════════════════════════════════════════════════
// Helper: Get source footage time in seconds
//
// For plain layers and time-stretched layers, current_time is already correct —
// AE folds stretch into it before calling the effect.
// For time-remapped layers, current_time is still comp time and we must
// evaluate the TIME_REMAP stream ourselves to get the actual source time.
// ═══════════════════════════════════════════════════════════════════════════════

static double GetLayerSourceTimeSeconds(PF_InData* in_data)
{
    double fallback = static_cast<double>(in_data->current_time) /
                      static_cast<double>(in_data->time_scale);

    if (!g_PluginID) return fallback;

    AEGP_SuiteHandler suites(in_data->pica_basicP);

    AEGP_LayerH layerH = nullptr;
    if (suites.PFInterfaceSuite1()->AEGP_GetEffectLayer(
            in_data->effect_ref, &layerH) != A_Err_NONE || !layerH)
        return fallback;

    // Check if the user explicitly enabled Time Remapping via the Layer menu.
    // AEGP_GetLayerFlags returns a bitmask — AEGP_LayerFlag_TIME_REMAPPING
    // is only set when the user turned it on, unlike the stream which exists
    // for time-stretched layers too.
    AEGP_LayerFlags layerFlags = AEGP_LayerFlag_NONE;
    if (suites.LayerSuite8()->AEGP_GetLayerFlags(
            layerH, &layerFlags) != A_Err_NONE)
        return fallback;

    if (!(layerFlags & AEGP_LayerFlag_TIME_REMAPPING))
        return fallback;   // Time stretch or plain — current_time is correct

    // User-enabled Time Remapping — evaluate the stream at comp time
    AEGP_StreamRefH remapStream = nullptr;
    if (suites.StreamSuite5()->AEGP_GetNewLayerStream(
            g_PluginID, layerH,
            AEGP_LayerStream_TIME_REMAP,
            &remapStream) != A_Err_NONE || !remapStream)
        return fallback;

    A_Time compTime = { in_data->current_time, in_data->time_scale };
    AEGP_StreamValue2 val;
    AEFX_CLR_STRUCT(val);

    double result = fallback;
    if (suites.StreamSuite5()->AEGP_GetNewStreamValue(
            g_PluginID,
            remapStream,
            AEGP_LTimeMode_CompTime,
            &compTime,
            FALSE,
            &val) == A_Err_NONE)
    {
        result = val.val.one_d;
        suites.StreamSuite5()->AEGP_DisposeStreamValue(&val);
    }

    suites.StreamSuite5()->AEGP_DisposeStream(remapStream);
    return result;
}


// ═══════════════════════════════════════════════════════════════════════════════
// Bit-depth detection
//
// PF_EffectWorld pixel sizes:
//   8bpc  — PF_Pixel      = 4 bytes  → rowbytes = width * 4
//   16bpc — PF_Pixel16    = 8 bytes  → rowbytes = width * 8   (PF_WORLD_IS_DEEP)
//   32bpc — PF_PixelFloat = 16 bytes → rowbytes = width * 16
//
// We check bytes-per-pixel derived from rowbytes first (most reliable),
// then fall back to PF_WORLD_IS_DEEP for the 16-bit case.
// ═══════════════════════════════════════════════════════════════════════════════

static int GetBitDepth(PF_EffectWorld* world)
{
    if (world->width <= 0) return 8;
    A_long bytesPerPixel = world->rowbytes / world->width;

    // 32bpc float: 4 channels * 4 bytes = 16 bytes per pixel
    if (bytesPerPixel >= 16)
        return 32;

    // 16bpc: 4 channels * 2 bytes = 8 bytes per pixel
    if (bytesPerPixel >= 8 || PF_WORLD_IS_DEEP(world))
        return 16;

    // 8bpc: 4 channels * 1 byte = 4 bytes per pixel
    return 8;
}

static void CopyPixels(PF_EffectWorld* src, PF_EffectWorld* dst)
{
    A_long h = (dst->height < src->height) ? dst->height : src->height;
    for (A_long y = 0; y < h; y++) {
        char* inRow  = reinterpret_cast<char*>(src->data) + y * src->rowbytes;
        char* outRow = reinterpret_cast<char*>(dst->data) + y * dst->rowbytes;
        A_long copyBytes = (src->rowbytes < dst->rowbytes) ? src->rowbytes : dst->rowbytes;
        memcpy(outRow, inRow, copyBytes);
    }
}


// ═══════════════════════════════════════════════════════════════════════════════
// Render debug log helper
// Writes to the same log file as the decoder when debugLog is enabled.
// ═══════════════════════════════════════════════════════════════════════════════

static void RenderLog(bool enabled, const char* fmt, ...)
{
    if (!enabled) return;
    static FILE* f = nullptr;
    if (!f) {
#ifdef _WIN32
        char path[512];
        const char* tmp = getenv("TEMP");
        if (!tmp) tmp = "C:\\Temp";
        snprintf(path, sizeof(path), "%s\\AlphaFix_debug.log", tmp);
        f = fopen(path, "a");   // append so decoder entries are preserved
#else
        f = fopen("/tmp/AlphaFix_debug.log", "a");
#endif
    }
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fflush(f);
}


// ═══════════════════════════════════════════════════════════════════════════════
// SmartFX: PreRender
// ═══════════════════════════════════════════════════════════════════════════════

static PF_Err PreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra)
{
    PF_Err err = PF_Err_NONE;

    PF_RenderRequest req = extra->input->output_request;
    PF_CheckoutResult checkout;
    req.preserve_rgb_of_zero_alpha = TRUE;

    ERR(extra->cb->checkout_layer(
        in_data->effect_ref, ALPHAFIX_INPUT, ALPHAFIX_INPUT,
        &req, in_data->current_time, in_data->time_step, in_data->time_scale, &checkout));

    if (!err) {
        extra->output->result_rect     = checkout.result_rect;
        extra->output->max_result_rect = checkout.max_result_rect;
    }

    PF_Handle preDataH = AllocHandle(in_data, sizeof(AlphaFixPreRenderData));
    if (preDataH) {
        AlphaFixPreRenderData* preData =
            reinterpret_cast<AlphaFixPreRenderData*>(LockHandle(in_data, preDataH));

        if (preData) {
            PF_ParamDef p;

            AEFX_CLR_STRUCT(p);
            ERR(PF_CHECKOUT_PARAM(in_data, ALPHAFIX_ENABLE,
                in_data->current_time, in_data->time_step, in_data->time_scale, &p));
            preData->enabled = p.u.bd.value;
            PF_CHECKIN_PARAM(in_data, &p);

            AEFX_CLR_STRUCT(p);
            ERR(PF_CHECKOUT_PARAM(in_data, ALPHAFIX_ALPHA_ONLY,
                in_data->current_time, in_data->time_step, in_data->time_scale, &p));
            preData->alphaOnly = p.u.bd.value;
            PF_CHECKIN_PARAM(in_data, &p);

            AEFX_CLR_STRUCT(p);
            ERR(PF_CHECKOUT_PARAM(in_data, ALPHAFIX_BLEND_MODE,
                in_data->current_time, in_data->time_step, in_data->time_scale, &p));
            preData->blendMode = p.u.pd.value - 1;
            PF_CHECKIN_PARAM(in_data, &p);

            AEFX_CLR_STRUCT(p);
            ERR(PF_CHECKOUT_PARAM(in_data, ALPHAFIX_REMOVE_COLOR_MATTING,
                in_data->current_time, in_data->time_step, in_data->time_scale, &p));
            preData->removeColorMatting = p.u.bd.value;
            PF_CHECKIN_PARAM(in_data, &p);

            AEFX_CLR_STRUCT(p);
            ERR(PF_CHECKOUT_PARAM(in_data, ALPHAFIX_FRAME_OFFSET,
                in_data->current_time, in_data->time_step, in_data->time_scale, &p));
            preData->frameOffset = p.u.sd.value;
            PF_CHECKIN_PARAM(in_data, &p);

            AEFX_CLR_STRUCT(p);
            ERR(PF_CHECKOUT_PARAM(in_data, ALPHAFIX_DEBUG_LOG,
                in_data->current_time, in_data->time_step, in_data->time_scale, &p));
            preData->debugLog = p.u.bd.value;
            PF_CHECKIN_PARAM(in_data, &p);

            // Stash the checkout result_rect so SmartRender can use it as an
            // offset into the full-frame alpha buffer. PF_SmartRenderExtra has
            // no result_rect member, so this is the only way to pass it through.
            preData->result_rect = checkout.result_rect;

            UnlockHandle(in_data, preDataH);
        }
        extra->output->pre_render_data = preDataH;
    }

    return err;
}


// ═══════════════════════════════════════════════════════════════════════════════
// SmartFX: SmartRender — 8/16/32 bpc
//
// Alpha buffer coordinate mapping
// ─────────────────────────────────
// The alpha buffer from FFmpeg is ALWAYS the full source frame (alphaWidth x
// alphaHeight). The outputWorld AE hands us is at the CURRENT downsample
// factor, and its top-left corner in downsampled layer space is
// result_rect.left/top:
//
//   Full     -> 4096 x 2304   (downsample 1/1)
//   Half     -> 2048 x 1152   (downsample 1/2)
//   Quarter  -> 1024 x  576   (downsample 1/4)
//
// So the mapping is:
//   alphaX = (originX + x) * scaleX,  scaleX = alphaWidth  / in_data->width
//   alphaY = (originY + y) * scaleY,  scaleY = alphaHeight / in_data->height
//
// The origin is added BEFORE scaling, because result_rect is itself in
// downsampled coordinates.
//
// At reduced resolution one output pixel covers a scaleX x scaleY block of
// source alpha, so we box-average that block. Point sampling would give
// aliased alpha edges against AE's filtered RGB and make Remove Color Matting
// fringe.
// ═══════════════════════════════════════════════════════════════════════════════

// ── Box-average a rectangle of the full-res alpha buffer ─────────────────────
// [x0,x1) x [y0,y1) must already be clamped and non-empty.
static inline A_u_char BoxAvgAlpha(
    const uint8_t*  buf,
    A_long          alphaWidth,
    A_long          x0, A_long x1,
    A_long          y0, A_long y1)
{
    // Fast path: single sample (Full resolution).
    if ((x1 - x0) == 1 && (y1 - y0) == 1)
        return buf[static_cast<size_t>(y0) * static_cast<size_t>(alphaWidth) + x0];

    uint32_t sum   = 0;
    uint32_t count = static_cast<uint32_t>(x1 - x0) * static_cast<uint32_t>(y1 - y0);

    for (A_long yy = y0; yy < y1; ++yy) {
        const uint8_t* row = buf + static_cast<size_t>(yy) * static_cast<size_t>(alphaWidth);
        for (A_long xx = x0; xx < x1; ++xx)
            sum += row[xx];
    }
    return static_cast<A_u_char>((sum + count / 2) / count);
}


// ── Build the [start,end) alpha span covered by one output pixel index ───────
static inline void SpanForIndex(
    A_long      dsIndex,        // origin + x  (or origin + y), downsampled space
    double      scale,          // alpha pixels per downsampled pixel
    A_long      alphaExtent,    // alphaWidth or alphaHeight
    A_long*     outStart,
    A_long*     outEnd)
{
    double f0 = static_cast<double>(dsIndex)     * scale;
    double f1 = static_cast<double>(dsIndex + 1) * scale;

    A_long a = static_cast<A_long>(floor(f0));
    A_long b = static_cast<A_long>(ceil(f1));

    if (a < 0)                 a = 0;
    if (a > alphaExtent - 1)   a = alphaExtent - 1;
    if (b > alphaExtent)       b = alphaExtent;
    if (b <= a)                b = a + 1;

    *outStart = a;
    *outEnd   = b;
}


static PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra)
{
    PF_Err err = PF_Err_NONE;

    PF_Handle preDataH = reinterpret_cast<PF_Handle>(extra->input->pre_render_data);
    if (!preDataH) return PF_Err_INTERNAL_STRUCT_DAMAGED;

    AlphaFixPreRenderData* preData =
        reinterpret_cast<AlphaFixPreRenderData*>(LockHandle(in_data, preDataH));
    if (!preData) return PF_Err_INTERNAL_STRUCT_DAMAGED;

    PF_EffectWorld* inputWorld = nullptr;
    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, ALPHAFIX_INPUT, &inputWorld));

    PF_EffectWorld* outputWorld = nullptr;
    ERR(extra->cb->checkout_output(in_data->effect_ref, &outputWorld));

    if (err || !inputWorld || !outputWorld) {
        UnlockHandle(in_data, preDataH);
        return err ? err : PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    if (!preData->enabled) {
        CopyPixels(inputWorld, outputWorld);
        UnlockHandle(in_data, preDataH);
        return PF_Err_NONE;
    }

    // FFmpeg missing or unloadable: behave like a disabled effect rather than
    // raising a delay-load exception out of the plugin.
    if (!AlphaFix_EnsureFFmpegLoaded()) {
        RenderLog(preData->debugLog != 0,
            "SMART_RENDER: FFmpeg unavailable, passing through. "
            "See %%TEMP%%\\AlphaFix_loader.log");
        CopyPixels(inputWorld, outputWorld);
        UnlockHandle(in_data, preDataH);
        return PF_Err_NONE;
    }

    char filePath[2048] = {0};
    PF_Err pathErr = GetLayerSourceFilePath(in_data, filePath, sizeof(filePath));

    if (pathErr || filePath[0] == '\0') {
        CopyPixels(inputWorld, outputWorld);
        UnlockHandle(in_data, preDataH);
        return PF_Err_NONE;
    }

    AlphaFixDecoder::Context* decoder = SafeCreateDecoder(filePath);
    if (!decoder) {
        CopyPixels(inputWorld, outputWorld);
        UnlockHandle(in_data, preDataH);
        return PF_Err_NONE;
    }
    decoder->debugLog = preData->debugLog ? true : false;

    // Compute frame number from AE time values using integer math + rounding
    // to avoid float truncation (e.g. 122.9999 → 122 instead of 123).
    double sourceTimeSecs = GetLayerSourceTimeSeconds(in_data);
    A_long frameNum = static_cast<A_long>(sourceTimeSecs * decoder->fps + 0.5);
    frameNum += preData->frameOffset;
    if (frameNum < 0) frameNum = 0;

    uint8_t* alphaBuffer = nullptr;
    A_long   alphaWidth  = 0;
    A_long   alphaHeight = 0;

    PF_Err decodeErr = SafeDecodeAlpha(
        decoder, frameNum, &alphaBuffer, &alphaWidth, &alphaHeight);

    if (decodeErr || !alphaBuffer || alphaWidth <= 0 || alphaHeight <= 0) {
        if (alphaBuffer) AlphaFixDecoder::FreeAlphaBuffer(alphaBuffer);
        SafeDestroyDecoder(decoder);
        CopyPixels(inputWorld, outputWorld);
        UnlockHandle(in_data, preDataH);
        return PF_Err_NONE;
    }

    // ── Determine bit depth ──────────────────────────────────────────────────
    int bitDepth = GetBitDepth(outputWorld);

    // ── Render region origin, in downsampled layer coordinates ───────────────
    A_long originX = preData->result_rect.left;
    A_long originY = preData->result_rect.top;

    A_long outW = outputWorld->width;
    A_long outH = outputWorld->height;

    // Never read past the end of the input world. AE can hand us worlds of
    // differing size when other effects are in the stack.
    A_long procW = (outW < inputWorld->width)  ? outW : inputWorld->width;
    A_long procH = (outH < inputWorld->height) ? outH : inputWorld->height;
    if (procW < 0) procW = 0;
    if (procH < 0) procH = 0;

    // ── Downsample-aware scale ───────────────────────────────────────────────
    // in_data->downsample_x is a PF_RationalScale: num/den is 1/1 at Full,
    // 1/2 at Half, 1/4 at Quarter. den/num therefore gives full-res pixels per
    // rendered pixel. This is the authoritative scale.
    double dsX = 1.0, dsY = 1.0;
    if (in_data->downsample_x.num > 0)
        dsX = static_cast<double>(in_data->downsample_x.den) /
              static_cast<double>(in_data->downsample_x.num);
    if (in_data->downsample_y.num > 0)
        dsY = static_cast<double>(in_data->downsample_y.den) /
              static_cast<double>(in_data->downsample_y.num);

    // The only remaining unknown is whether the SOURCE VIDEO is the same size
    // as the LAYER. Normally it is (the layer IS the footage) and the scale is
    // just the downsample factor. When it isn't, we need the ratio
    // alphaWidth / fullResLayerWidth as a correction.
    //
    // Deriving fullResLayerWidth is the tricky part, because in_data->width is
    // documented inconsistently: on some paths it is the layer size at FULL
    // resolution, on others at the CURRENT resolution. Rather than guess, test
    // both candidates and keep whichever lands closer to the alpha buffer,
    // which we know is the true full-res source size.
    //
    //   candidate A: in_data->width is already full-res
    //   candidate B: in_data->width is downsampled, so full-res = width * ds
    //
    // When layer and source match (the normal case) both candidates agree on
    // the answer and the correction cancels to 1.0, leaving scale == ds.
    double fullLayerW = static_cast<double>(alphaWidth);
    double fullLayerH = static_cast<double>(alphaHeight);

    if (in_data->width > 0) {
        double candA = static_cast<double>(in_data->width);
        double candB = static_cast<double>(in_data->width) * dsX;
        fullLayerW = (fabs(candA - alphaWidth) <= fabs(candB - alphaWidth)) ? candA : candB;
    }
    if (in_data->height > 0) {
        double candA = static_cast<double>(in_data->height);
        double candB = static_cast<double>(in_data->height) * dsY;
        fullLayerH = (fabs(candA - alphaHeight) <= fabs(candB - alphaHeight)) ? candA : candB;
    }

    double scaleX = dsX;
    double scaleY = dsY;
    if (fullLayerW > 0.0) scaleX = dsX * (static_cast<double>(alphaWidth)  / fullLayerW);
    if (fullLayerH > 0.0) scaleY = dsY * (static_cast<double>(alphaHeight) / fullLayerH);
    if (!(scaleX > 0.0)) scaleX = dsX;   // Also catches NaN.
    if (!(scaleY > 0.0)) scaleY = dsY;

    // ── Debug: log render geometry once per render call ──────────────────────
    bool logOn = (preData->debugLog != 0);
    RenderLog(logOn, "======== SMART_RENDER frame=%ld ========", (long)frameNum);
    RenderLog(logOn, "  bitDepth     = %d  (rowbytes=%ld  width=%ld  bytes/px=%ld)",
        bitDepth, (long)outputWorld->rowbytes, (long)outputWorld->width,
        outputWorld->width > 0 ? (long)(outputWorld->rowbytes / outputWorld->width) : -1);
    RenderLog(logOn, "  outputWorld  = %ld x %ld", (long)outW, (long)outH);
    RenderLog(logOn, "  inputWorld   = %ld x %ld",
        (long)inputWorld->width, (long)inputWorld->height);
    RenderLog(logOn, "  in_data w/h  = %ld x %ld",
        (long)in_data->width, (long)in_data->height);
    RenderLog(logOn, "  alphaBuffer  = %ld x %ld", (long)alphaWidth, (long)alphaHeight);
    RenderLog(logOn, "  downsample   = %ld/%ld , %ld/%ld  (dsX=%.4f dsY=%.4f)",
        (long)in_data->downsample_x.num, (long)in_data->downsample_x.den,
        (long)in_data->downsample_y.num, (long)in_data->downsample_y.den, dsX, dsY);
    RenderLog(logOn, "  fullLayer    = %.1f x %.1f  (derived full-res layer size)",
        fullLayerW, fullLayerH);
    RenderLog(logOn, "  scale        = %.6f x %.6f  (alpha px per output px)", scaleX, scaleY);
    RenderLog(logOn, "  result_rect  = {L=%ld T=%ld R=%ld B=%ld}",
        (long)preData->result_rect.left, (long)preData->result_rect.top,
        (long)preData->result_rect.right, (long)preData->result_rect.bottom);
    RenderLog(logOn, "  origin       = (%ld, %ld)", (long)originX, (long)originY);

    // ── Precompute column spans once, reuse for every row ────────────────────
    std::vector<A_long> colStart(static_cast<size_t>(procW > 0 ? procW : 0));
    std::vector<A_long> colEnd  (static_cast<size_t>(procW > 0 ? procW : 0));
    for (A_long x = 0; x < procW; ++x) {
        SpanForIndex(originX + x, scaleX, alphaWidth,
                     &colStart[static_cast<size_t>(x)],
                     &colEnd  [static_cast<size_t>(x)]);
    }

    // ── Per-pixel loop ───────────────────────────────────────────────────────
    for (A_long y = 0; y < procH; y++) {

        A_long rowStart = 0, rowEnd = 1;
        SpanForIndex(originY + y, scaleY, alphaHeight, &rowStart, &rowEnd);

        if (bitDepth == 32) {
            // ── 32 bpc float ─────────────────────────────────────────────────
            PF_PixelFloat* inRow = reinterpret_cast<PF_PixelFloat*>(
                reinterpret_cast<char*>(inputWorld->data) + y * inputWorld->rowbytes);
            PF_PixelFloat* outRow = reinterpret_cast<PF_PixelFloat*>(
                reinterpret_cast<char*>(outputWorld->data) + y * outputWorld->rowbytes);

            for (A_long x = 0; x < procW; x++) {
                A_u_char sampled = BoxAvgAlpha(alphaBuffer, alphaWidth,
                    colStart[static_cast<size_t>(x)], colEnd[static_cast<size_t>(x)],
                    rowStart, rowEnd);

                PF_FpShort correctAlphaF = sampled / 255.0f;
                PF_FpShort r = inRow[x].red, g = inRow[x].green, b = inRow[x].blue;
                PF_FpShort oldA = inRow[x].alpha;

                if (preData->alphaOnly) {
                    outRow[x].red = outRow[x].green = outRow[x].blue = correctAlphaF;
                    outRow[x].alpha = 1.0f;
                } else {
                    PF_FpShort newA = correctAlphaF;
                    switch (preData->blendMode) {
                        case BLEND_REPLACE:  newA = correctAlphaF; break;
                        case BLEND_MULTIPLY: newA = oldA * correctAlphaF; break;
                        case BLEND_MAX:      newA = (correctAlphaF > oldA) ? correctAlphaF : oldA; break;
                        default:             newA = correctAlphaF; break;
                    }
                    outRow[x].alpha = newA;

                    if (preData->removeColorMatting && newA > 0.0f && newA < 1.0f) {
                        r /= newA; g /= newA; b /= newA;
                        if (r > 1.0f) r = 1.0f;
                        if (g > 1.0f) g = 1.0f;
                        if (b > 1.0f) b = 1.0f;
                    }
                    outRow[x].red = r; outRow[x].green = g; outRow[x].blue = b;
                }
            }

        } else if (bitDepth == 16) {
            // ── 16 bpc (AE uses 0-32768) ─────────────────────────────────────
            PF_Pixel16* inRow = reinterpret_cast<PF_Pixel16*>(
                reinterpret_cast<char*>(inputWorld->data) + y * inputWorld->rowbytes);
            PF_Pixel16* outRow = reinterpret_cast<PF_Pixel16*>(
                reinterpret_cast<char*>(outputWorld->data) + y * outputWorld->rowbytes);

            for (A_long x = 0; x < procW; x++) {
                A_u_char sampled = BoxAvgAlpha(alphaBuffer, alphaWidth,
                    colStart[static_cast<size_t>(x)], colEnd[static_cast<size_t>(x)],
                    rowStart, rowEnd);

                A_u_short correctA16 = static_cast<A_u_short>(
                    (static_cast<A_long>(sampled) * 32768) / 255);
                A_u_short r = inRow[x].red, g = inRow[x].green, b = inRow[x].blue;
                A_u_short oldA16 = inRow[x].alpha;

                if (preData->alphaOnly) {
                    outRow[x].red = outRow[x].green = outRow[x].blue = correctA16;
                    outRow[x].alpha = 32768;
                } else {
                    A_u_short newA16 = correctA16;
                    switch (preData->blendMode) {
                        case BLEND_REPLACE:  newA16 = correctA16; break;
                        case BLEND_MULTIPLY: newA16 = (A_u_short)((A_long)oldA16 * correctA16 / 32768); break;
                        case BLEND_MAX:      newA16 = (correctA16 > oldA16) ? correctA16 : oldA16; break;
                        default:             newA16 = correctA16; break;
                    }
                    outRow[x].alpha = newA16;

                    if (preData->removeColorMatting && newA16 > 0 && newA16 < 32768) {
                        A_long sR = (A_long)r * 32768 / newA16;
                        A_long sG = (A_long)g * 32768 / newA16;
                        A_long sB = (A_long)b * 32768 / newA16;
                        r = (A_u_short)(sR > 32768 ? 32768 : sR);
                        g = (A_u_short)(sG > 32768 ? 32768 : sG);
                        b = (A_u_short)(sB > 32768 ? 32768 : sB);
                    }
                    outRow[x].red = r; outRow[x].green = g; outRow[x].blue = b;
                }
            }

        } else {
            // ── 8 bpc ────────────────────────────────────────────────────────
            PF_Pixel* inRow = reinterpret_cast<PF_Pixel*>(
                reinterpret_cast<char*>(inputWorld->data) + y * inputWorld->rowbytes);
            PF_Pixel* outRow = reinterpret_cast<PF_Pixel*>(
                reinterpret_cast<char*>(outputWorld->data) + y * outputWorld->rowbytes);

            for (A_long x = 0; x < procW; x++) {
                A_u_char correctAlpha = BoxAvgAlpha(alphaBuffer, alphaWidth,
                    colStart[static_cast<size_t>(x)], colEnd[static_cast<size_t>(x)],
                    rowStart, rowEnd);

                A_u_char r = inRow[x].red, g = inRow[x].green, b = inRow[x].blue;
                A_u_char oldAlpha = inRow[x].alpha;

                if (preData->alphaOnly) {
                    outRow[x].red = outRow[x].green = outRow[x].blue = correctAlpha;
                    outRow[x].alpha = 255;
                } else {
                    A_u_char newAlpha = correctAlpha;
                    switch (preData->blendMode) {
                        case BLEND_REPLACE:  newAlpha = correctAlpha; break;
                        case BLEND_MULTIPLY: newAlpha = (A_u_char)((A_long)oldAlpha * correctAlpha / 255); break;
                        case BLEND_MAX:      newAlpha = (correctAlpha > oldAlpha) ? correctAlpha : oldAlpha; break;
                        default:             newAlpha = correctAlpha; break;
                    }
                    outRow[x].alpha = newAlpha;

                    if (preData->removeColorMatting && newAlpha > 0 && newAlpha < 255) {
                        A_long sR = (A_long)r * 255 / newAlpha;
                        A_long sG = (A_long)g * 255 / newAlpha;
                        A_long sB = (A_long)b * 255 / newAlpha;
                        r = (A_u_char)(sR > 255 ? 255 : sR);
                        g = (A_u_char)(sG > 255 ? 255 : sG);
                        b = (A_u_char)(sB > 255 ? 255 : sB);
                    }
                    outRow[x].red = r; outRow[x].green = g; outRow[x].blue = b;
                }
            }
        }
    }

    AlphaFixDecoder::FreeAlphaBuffer(alphaBuffer);
    SafeDestroyDecoder(decoder);
    UnlockHandle(in_data, preDataH);

    return err;
}


// ═══════════════════════════════════════════════════════════════════════════════
// Legacy Render
// ═══════════════════════════════════════════════════════════════════════════════

static PF_Err Render(PF_InData* in_data, PF_OutData* out_data,
    PF_ParamDef* params[], PF_LayerDef* output)
{
    if (params[ALPHAFIX_INPUT]) {
        PF_LayerDef* input = &params[ALPHAFIX_INPUT]->u.ld;
        A_long h = (output->height < input->height) ? output->height : input->height;
        A_long copyBytes = (input->rowbytes < output->rowbytes)
                         ? input->rowbytes : output->rowbytes;
        for (A_long y = 0; y < h; y++) {
            char* inRow  = reinterpret_cast<char*>(input->data)  + y * input->rowbytes;
            char* outRow = reinterpret_cast<char*>(output->data) + y * output->rowbytes;
            memcpy(outRow, inRow, copyBytes);
        }
    }
    return PF_Err_NONE;
}


// ═══════════════════════════════════════════════════════════════════════════════
// Main Dispatcher
// ═══════════════════════════════════════════════════════════════════════════════

extern "C" ALPHAFIX_API PF_Err EffectMain(
    PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data,
    PF_ParamDef* params[], PF_LayerDef* output, void* extra)
{
    PF_Err err = PF_Err_NONE;

    // Runs its work exactly once per process, thread-safe. Doing it here rather
    // than only in GLOBAL_SETUP means the DLLs are resolved and pinned before
    // any code path can trigger a lazy delay-load on a render worker thread.
    AlphaFix_EnsureFFmpegLoaded();

    try {
        switch (cmd) {
            case PF_Cmd_ABOUT:
                snprintf(out_data->return_msg, sizeof(out_data->return_msg),
                    "%s v%d.%d\nFixes 8-bit ProRes 4444 alpha artifacts.\nby Guy Micciche",
                    ALPHAFIX_NAME, ALPHAFIX_MAJOR_VERSION, ALPHAFIX_MINOR_VERSION);
                break;
            case PF_Cmd_GLOBAL_SETUP:
                // Note: a failed FFmpeg load is NOT fatal here. Returning an
                // error would make the effect unregisterable. Instead the
                // effect passes pixels through and logs to
                // %TEMP%\AlphaFix_loader.log.
                err = GlobalSetup(in_data, out_data);
                break;
            case PF_Cmd_GLOBAL_SETDOWN:
                return PF_Err_NONE;
            case PF_Cmd_PARAMS_SETUP:
                err = ParamsSetup(in_data, out_data);
                break;
            case PF_Cmd_SEQUENCE_SETUP:
            case PF_Cmd_SEQUENCE_RESETUP:
            case PF_Cmd_SEQUENCE_SETDOWN:
            case PF_Cmd_SEQUENCE_FLATTEN:
                break;
            case PF_Cmd_SMART_PRE_RENDER:
                err = PreRender(in_data, out_data,
                    reinterpret_cast<PF_PreRenderExtra*>(extra));
                break;
            case PF_Cmd_SMART_RENDER:
                err = SmartRender(in_data, out_data,
                    reinterpret_cast<PF_SmartRenderExtra*>(extra));
                break;
            case PF_Cmd_RENDER:
                err = Render(in_data, out_data, params, output);
                break;
            default:
                break;
        }
    }
    catch (PF_Err& thrown_err) { err = thrown_err; }
    catch (...) { err = PF_Err_INTERNAL_STRUCT_DAMAGED; }

    return err;
}