#pragma once
#include "blot.h"

struct LETTERBOX : public BLOT
{

};

void StartupLetterbox(LETTERBOX *pletterbox);
void PostLetterBoxLoad(LETTERBOX* pletterbox);
void SetLetterboxBlots(LETTERBOX* pletterbox, BLOTS blots);
void DrawLetterBox(LETTERBOX* pletterbox);
void ReassertLetterboxAfterWipe();

extern LETTERBOX g_letterbox;