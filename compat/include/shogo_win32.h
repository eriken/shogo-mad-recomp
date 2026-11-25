// Small native implementations of Win32 calls that the game's glue code needs.
// Paths given to these accept Windows-style backslashes.
#ifndef SHOGO_WIN32_H
#define SHOGO_WIN32_H

#include "shogo_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

// Convert a game path ("Save\\Slot1.sav", "c:\\foo") into a POSIX path in out.
// Backslashes become slashes and a leading drive letter is dropped. The
// player's files (Save\\..., autoexec.cfg) lead into the folder the
// SHOGO_PLAYER_DIR environment variable names, when it's set.
void shogo_native_path(const char *in, char *out, size_t outSize);

// INI files, with the Win32 semantics the game relies on:
// missing file/section/key returns lpDefault; writing a NULL value deletes the key.
DWORD GetPrivateProfileString(const char *lpAppName, const char *lpKeyName, const char *lpDefault,
                              char *lpReturnedString, DWORD nSize, const char *lpFileName);
BOOL WritePrivateProfileString(const char *lpAppName, const char *lpKeyName, const char *lpString,
                               const char *lpFileName);

DWORD GetTickCount(void);
// Toggle/press state of a virtual key. Returns 0 (not pressed, not toggled) until
// the engine's input layer provides real state.
SHORT GetKeyState(int nVirtKey);
void OutputDebugString(const char *str);

#ifdef __cplusplus
}
#endif

#endif
