// Native replacement for ClientShellDLL/NetStart.cpp.
//
// The original file is the multiplayer start/join/host wizard, built from
// Win32 dialog boxes. Multiplayer isn't ported yet, so every entry point
// reports "not available" and the game's main menu treats that as the user
// cancelling the wizard. Single-player doesn't go through this code.

#include "NetStart.h"

#include <string.h>

ServerOptions g_ServerOptions;

namespace {
NetPlayer g_netPlayer;
NetGame g_netGame;
}

DBOOL NetStart_DoWizard(CClientDE *) { return DFALSE; }
DBOOL NetStart_DoWizard(CClientDE *, NetStart *pNetStart)
{
    if (pNetStart) memset(pNetStart, 0, sizeof(*pNetStart));
    return DFALSE;
}
DBOOL NetStart_DoLobbyLaunchWizard(CClientDE *) { return DFALSE; }
DBOOL NetStart_DoConsoleConnect(CClientDE *, char *) { return DFALSE; }
DBOOL NetStart_MinimizeMainWnd(CClientDE *) { return DTRUE; }
DBOOL NetStart_RestoreMainWnd() { return DTRUE; }
DBOOL NetStart_RunServerOptions(CClientDE *, ServerOptions *) { return DFALSE; }
DBOOL NetStart_RunServerOptions(CClientDE *) { return DFALSE; }
DBOOL NetStart_DoSettingsDialog() { return DFALSE; }

NetSession *NetStart_GetSessionList(CClientDE *, char *) { return DNULL; }
void NetStart_FreeSessionList(CClientDE *) {}

NetPlayer *NetStart_GetPlayerStruct() { return &g_netPlayer; }
NetGame *NetStart_GetGameStruct() { return &g_netGame; }
void NetStart_ClearGameStruct() { memset(&g_netGame, 0, sizeof(g_netGame)); }

NetSession *NetStart_GetSessionQueryResults(CClientDE *) { return DNULL; }
void NetStart_UpdateSessionQuery(CClientDE *) {}
void NetStart_EndSessionQuery(CClientDE *) {}
DBOOL NetStart_StartSessionQuery(CClientDE *, char *) { return DFALSE; }
DBOOL NetStart_StartSessionQuery(CClientDE *, CIpMgr *) { return DFALSE; }
