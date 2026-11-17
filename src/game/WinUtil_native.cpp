// Native replacement for ClientShellDLL/WinUtil.cpp (Win32 helpers used by the menus).
#include "winutil.h"
#include "shogo_win32.h"
#include "basedefs_de.h"

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace {
bool pathExists(const char *gamePath, bool wantDir)
{
    if (!gamePath || !*gamePath) return false;
    char p[MAX_PATH];
    shogo_native_path(gamePath, p, sizeof(p));
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = '\0';
    struct stat st;
    if (stat(p, &st) != 0) return false;
    return wantDir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode);
}
}

// The intro movies live next to the game data (the engine's working directory).
// Paths are returned in the game's own backslash style.
BOOL CWinUtil::GetMoviesPath(char *strPath)
{
    strPath[0] = '\0';
    if (!FileExist((char *)"intro.smk")) return FALSE;
    strcpy(strPath, ".\\");
    return TRUE;
}

BOOL CWinUtil::DirExist(char *strPath) { return pathExists(strPath, true); }

BOOL CWinUtil::CreateDir(char *strPath)
{
    if (!strPath || !*strPath) return FALSE;
    char p[MAX_PATH];
    shogo_native_path(strPath, p, sizeof(p));
    for (char *s = p + 1; *s; ++s) {
        if (*s != '/') continue;
        *s = '\0';
        mkdir(p, 0755);
        *s = '/';
    }
    mkdir(p, 0755);
    return pathExists(strPath, true);
}

BOOL CWinUtil::FileExist(char *strPath) { return pathExists(strPath, false); }

DWORD CWinUtil::WinGetPrivateProfileString(char *lpAppName, char *lpKeyName, char *lpDefault,
                                           char *lpReturnedString, DWORD nSize, char *lpFileName)
{
    return GetPrivateProfileString(lpAppName, lpKeyName, lpDefault, lpReturnedString, nSize, lpFileName);
}

DWORD CWinUtil::WinWritePrivateProfileString(char *lpAppName, char *lpKeyName, char *lpString,
                                             char *lpFileName)
{
    return WritePrivateProfileString(lpAppName, lpKeyName, lpString, lpFileName);
}

void CWinUtil::DebugOut(char *str) { OutputDebugString(str); }

void CWinUtil::DebugBreak() { __builtin_debugtrap(); }

float CWinUtil::GetTime() { return (float)GetTickCount() / 1000.0f; }

// Only used for debugging on Windows; there is no Win32 focus window here.
char *CWinUtil::GetFocusWindow() { return NULL; }

void CWinUtil::WriteToDebugFile(char *strText)
{
    FILE *f = fopen("shodebug.txt", "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (t)
        fprintf(f, "[%02d/%02d/%02d %02d:%02d:%02d]  ", t->tm_mon + 1, t->tm_mday, (t->tm_year + 1900) % 100,
                t->tm_hour, t->tm_min, t->tm_sec);
    fprintf(f, "%s\n", strText);
    fclose(f);
}
