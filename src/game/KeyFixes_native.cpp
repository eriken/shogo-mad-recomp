// Native replacement for ClientShellDLL/KeyFixes.cpp.
// The original remapped single-character key names to the user's Windows
// keyboard layout. Key names are passed through unchanged for now; layout-aware
// names will come from the engine's input layer.
#include "KeyFixes.h"

#include <string.h>

void ConvertKeyToCurrentKeyboard(char *sNewKey, char *sOldKey, int nNewKeyMaxSize)
{
    if (nNewKeyMaxSize <= 0) return;
    strncpy(sNewKey, sOldKey, (size_t)nNewKeyMaxSize);
    sNewKey[nNewKeyMaxSize - 1] = '\0';
}
