#pragma once

// Loads bundled LibreOffice fonts as process-private GDI fonts.
// This does not install fonts system-wide and does not contact external services.
void LoadLibreOfficePrivateFontsForProcess();
void UnloadLibreOfficePrivateFontsForProcess();
