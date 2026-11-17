#pragma once
#include "blitsnap.h"

void RegisterToolbarClass(HINSTANCE hInst);
void CreateAndShowToolbar();
void DestroyToolbar();
void SetToolbarVisible(bool visible);
void RepaintToolbar();
bool GetPillAnchorRect(int anchorIndex, RECT& outRect);