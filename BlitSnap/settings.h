#pragma once
#include "blitsnap.h"

void RegisterSettingsClass(HINSTANCE hInst);
void ShowSettingsWindow();
bool IsSettingsDialogMessage(MSG* pMsg);