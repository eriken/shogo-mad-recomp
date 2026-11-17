// Native replacement for ClientShellDLL/CreditsWin.cpp.
// The original read a "TEXT" resource out of CRES.DLL through Win32. Here the
// engine supplies it through an engine hook named "shogo_cres_text:<name>",
// which returns a NUL-terminated buffer the engine owns.
#include "CreditsWin.h"
#include "cpp_client_de.h"

#include <string.h>

extern CClientDE *g_pClientDE;

char *CreditsWin_GetTextBuffer(char *sName)
{
    if (!g_pClientDE || !sName) return NULL;
    char hook[128] = "shogo_cres_text:";
    strncat(hook, sName, sizeof(hook) - strlen(hook) - 1);
    void *pText = NULL;
    if (g_pClientDE->GetEngineHook(hook, &pText) != DE_OK) return NULL;
    return (char *)pText;
}
