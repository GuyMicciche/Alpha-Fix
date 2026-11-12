#include "AEConfig.h"
#include "AlphaFixVersion.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
	#include <AE_General.r>
#endif

resource 'PiPL' (16000) {
    {
        Kind {
            AEEffect
        },
        Name {
            ALPHAFIX_NAME
        },
        Category {
            ALPHAFIX_CATEGORY
        },

#ifdef AE_OS_WIN
    #ifdef AE_PROC_INTELx64
        CodeWin64X86 {"EffectMain"},
    #endif
    #ifdef AE_OS_ARM64
        CodeWinARM64 {"EffectMain"},
    #endif
#else
        CodeMacIntel64 {"EffectMain"},
        CodeMacARM64 {"EffectMain"},
#endif

        AE_PiPL_Version {
            2,
            0
        },
        AE_Effect_Spec_Version {
            PF_PLUG_IN_VERSION,
            PF_PLUG_IN_SUBVERS
        },
        AE_Effect_Version {
            ALPHAFIX_PIPL_VERSION
        },
        AE_Effect_Info_Flags {
            0
        },
        AE_Effect_Global_OutFlags {
            0x2000004
        },
        AE_Effect_Global_OutFlags_2 {
            0x08001400
        },
        AE_Effect_Match_Name {
            ALPHAFIX_MATCH_NAME
        },
        AE_Reserved_Info {
            0
        },
    }
};