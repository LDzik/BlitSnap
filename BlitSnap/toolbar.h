#pragma once
#include "blitsnap.h"

constexpr wchar_t kToolbarWndClass[] = L"BlitSnap_Toolbar_Class";
constexpr wchar_t kLayersPopupWndClass[] = L"BlitSnap_LayersPopupWnd";

void RegisterToolbarClass(HINSTANCE hInst);
void CreateAndShowToolbar();
void DestroyToolbar();
void SetToolbarVisible(bool visible);
void RepaintToolbar();
bool GetPillAnchorRect(int anchorIndex, RECT& outRect);

// Layers popup controls
bool IsLayersPopupOpen();
void ToggleLayersPopup();
void CloseLayersPopup();
void RepaintLayersPopup();