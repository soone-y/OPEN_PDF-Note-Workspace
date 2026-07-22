#pragma once

#include <windows.h>

bool RouteWheelToComboListIfNeeded(const MSG& msg);
void InstallMainWheelRoutingHooks();
void UninstallMainWheelRoutingHooks();
