#pragma once

#include "AEConfig.h"
#include "entry.h"

#ifdef _WIN32
    #include <windows.h>
#endif

#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectCBSuites.h"
#include "AE_Macros.h"
#include "AEGP_SuiteHandler.h"
#include "Param_Utils.h"
#include "Smart_Utils.h"
#include "String_Utils.h"
#include "AEFX_SuiteHelper.h"

#ifdef _WIN32
    #define ALPHAFIX_API __declspec(dllexport)
#else
    #define ALPHAFIX_API __attribute__((visibility("default")))
#endif

#include "AlphaFixVersion.h"

enum {
    ALPHAFIX_INPUT = 0,
    ALPHAFIX_ENABLE,
    ALPHAFIX_ALPHA_ONLY,
    ALPHAFIX_BLEND_MODE,
    ALPHAFIX_REMOVE_COLOR_MATTING,
    ALPHAFIX_FRAME_OFFSET,
    ALPHAFIX_DEBUG_LOG,
    ALPHAFIX_NUM_PARAMS
};

enum AlphaBlendMode {
    BLEND_REPLACE = 0,
    BLEND_MULTIPLY,
    BLEND_MAX,
    BLEND_NUM_MODES
};

struct AlphaFixPreRenderData {
    PF_Boolean  enabled;
    PF_Boolean  alphaOnly;
    A_long      blendMode;
    PF_Boolean  removeColorMatting;
    A_long      frameOffset;
    PF_Boolean  debugLog;
    PF_LRect    result_rect;    // Stashed from checkout during PreRender.
                                // Used in SmartRender to offset alpha buffer
                                // lookups when AE gives us a sub-region world
                                // (happens when other effects are in the stack).
};

struct AlphaFixInstanceData {
    char        filePath[2048];
    A_long      lastFrame;
    A_long      cachedWidth;
    A_long      cachedHeight;
    PF_Boolean  isValid;
    void*       ffmpegContext;
};

// â”€â”€ FFmpeg loading â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// AlphaFix_EnsureFFmpegLoaded is thread-safe and runs its work exactly once,
// no matter how many threads call it. It installs delay-load hooks and then
// explicitly loads every DLL found in the alphafix.ffmpeg sidecar folder by
// absolute path, so resolution never depends on the process-wide DLL search
// path (which other AE plugins routinely change out from under us).
//
// Returns true when FFmpeg is usable. When it returns false the effect passes
// pixels through untouched instead of crashing.
bool AlphaFix_EnsureFFmpegLoaded();

// Retained for source compatibility. Just forwards to the function above.
bool AlphaFix_LoadFFmpegDLLs();

extern "C" {

    ALPHAFIX_API PF_Err PluginDataEntryFunction(
        PF_PluginDataPtr    inPtr,
        PF_PluginDataCB     inPluginDataCallBackPtr,
        SPBasicSuite*       inSPBasicSuitePtr,
        const char*         inHostName,
        const char*         inHostVersion
    );

    ALPHAFIX_API PF_Err EffectMain(
        PF_Cmd          cmd,
        PF_InData*      in_data,
        PF_OutData*     out_data,
        PF_ParamDef*    params[],
        PF_LayerDef*    output,
        void*           extra
    );

}