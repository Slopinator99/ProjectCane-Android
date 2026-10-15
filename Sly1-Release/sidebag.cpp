#include "sidebag.h"
#if defined(__ANDROID__) && defined(CANE_DIAG)
#include <android/log.h>
#endif

std::shared_ptr <CSidebag> PsidebagNew()
{
    return std::make_shared <CSidebag>();
}

CRef CSidebag::RefAddBinding(int n, CRef* pref)
{
	CRef ret;
	ret.SetTag(TAGK_Void);

#if defined(__ANDROID__) && defined(CANE_DIAG)
	if (m_csbb >= 16)
		__android_log_print(ANDROID_LOG_WARN, "cane", "[Sidebag] DROP (full) sidebag=%p n=%d", (void*)this, n);
#endif
	if (pref == nullptr || m_csbb < 0 || m_csbb >= 16)
		return ret;

    int index = m_csbb++;

    m_asbb[index].n = n;
    m_asbb[index].ref = *pref;

    return ret;
}

int CSidebag::FFindBinding(int n, CRef* pref)
{
    for (int i = m_csbb - 1; i >= 0; --i)
    {
        if (m_asbb[i].n == n)
        {
            if (pref)
                *pref = m_asbb[i].ref;

            return 1;
        }
    }

    return 0;
}

CRef CSidebag::RefSetBinding(int n, CRef* pref)
{
	CRef ret;
	ret.SetTag(TAGK_Void);

	if (pref == nullptr || m_csbb < 0 || m_csbb > 16)
		return ret;

    for (int i = m_csbb - 1; i >= 0; --i)
    {
        if (m_asbb[i].n == n)
        {
            m_asbb[i].ref = *pref;
            break;
        }
    }

    return ret;
}

void CSidebag::CloneTo(CSidebag* psidebagClone)
{
    if (!psidebagClone)
        return;

    psidebagClone->m_csbb = m_csbb;

    for (int i = 0; i < m_csbb && i < 16; ++i)
    {
        psidebagClone->m_asbb[i].n = m_asbb[i].n;
        m_asbb[i].ref.CloneTo(&psidebagClone->m_asbb[i].ref, nullptr);
    }
}
