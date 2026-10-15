#include "basic.h"
#if defined(__ANDROID__) && defined(CANE_DIAG)
#include <android/log.h>
#endif
#include "gc.h"
#include <cstdint>
#include <new>

void GetBasicCid(BASIC* pbasic, CID* pcid)
{
	*pcid = pbasic->pvtbasic->cid;
}

bool FIsBasicDerivedFrom(const BASIC* basic, CID cid)
{
    if (!basic) return false;

    VTBASIC *pvt = basic->pvtbasic;

    while (pvt) {
        if (pvt->cid == cid) 
            return true;

        pvt = (VTBASIC*)pvt->pvtSuper;
    }
    return false;
}

void EnsureBasicSidebag(BASIC* pbasic)
{
    if (pbasic == nullptr)
        return;

    CSidebag* const psidebag = pbasic->psidebag.get();
    const std::uintptr_t sidebagAddress = reinterpret_cast<std::uintptr_t>(psidebag);
    // A make_shared<CSidebag> object is only guaranteed 8-byte aligned: libc++ puts it
    // after a 24-byte control block, so it sits at an 8 (mod 16) address. Testing
    // 16-byte alignment here flagged every valid sidebag as corrupt on Android and
    // rebuilt it on each call, silently dropping all but the last option added.
    const bool fCorrupt = psidebag != nullptr &&
        (sidebagAddress < 0x10000u || (sidebagAddress & 0x07u) != 0u);

    if (fCorrupt)
    {
#if defined(__ANDROID__) && defined(CANE_DIAG)
        __android_log_print(ANDROID_LOG_WARN, "cane", "[Sidebag] rebuilding sidebag with suspicious address %p (basic=%p)",
            static_cast<void*>(psidebag), static_cast<void*>(pbasic));
#endif
        // Do not assign/reset a corrupted shared_ptr: that would dereference
        // its equally corrupted control block. Reconstruct it in place.
        std::construct_at(&pbasic->psidebag, PsidebagNew());
    }
    else if (psidebag == nullptr)
    {
        pbasic->psidebag = PsidebagNew();
    }

    if (pbasic->psidebag != nullptr)
        g_gc.AddRootSidebag(pbasic->psidebag.get());
}
