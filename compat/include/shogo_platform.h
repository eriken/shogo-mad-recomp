// Force-included into every Shogo game-code translation unit.
// Supplies the small slice of Win32 / MSVC runtime the 1998 game code expects,
// so the original sources compile unchanged with Clang on macOS.
#ifndef SHOGO_PLATFORM_H
#define SHOGO_PLATFORM_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

// ---- MSVC keywords -------------------------------------------------------
#define __cdecl
#define __stdcall
#define _cdecl
#define WINAPI
#define CALLBACK
#define APIENTRY
#define __declspec(x)
#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif

// ---- Win32 scalar types (sizes match 32-bit Windows) ----------------------
typedef uint32_t DWORD;
typedef uint16_t WORD;
typedef uint8_t BYTE;
typedef int32_t BOOL;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef uint32_t UINT;
typedef int32_t INT;
typedef char CHAR;
typedef unsigned char UCHAR;
typedef float FLOAT;
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef char *PSTR;
typedef DWORD *LPDWORD;
typedef BYTE *LPBYTE;
typedef intptr_t LPARAM;
typedef uintptr_t WPARAM;
typedef intptr_t LRESULT;
typedef long HRESULT;

typedef int16_t SHORT;
typedef uint16_t USHORT;

// Some game headers do `#ifndef DWORD typedef unsigned long DWORD; #endif`.
// Defining the names as macros makes those guards skip the (64-bit) redefinition.
#define DWORD DWORD
#define BOOL BOOL

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifndef NULL
#define NULL 0
#endif
#define MAX_PATH 260
#define _MAX_PATH 260
#define _MAX_DRIVE 3
#define _MAX_DIR 256
#define _MAX_FNAME 256
#define _MAX_EXT 256

// Opaque Win32 handles. The game only stores and passes these around.
typedef struct HWND__ *HWND;
typedef struct HINSTANCE__ *HINSTANCE;
typedef HINSTANCE HMODULE;
typedef void *HANDLE;
typedef struct HKEY__ *HKEY;
typedef struct HCURSOR__ *HCURSOR;
typedef struct HICON__ *HICON;
typedef struct HBITMAP__ *HBITMAP;
typedef struct HDC__ *HDC;

#define LOWORD(l) ((WORD)((uintptr_t)(l) & 0xffff))
#define HIWORD(l) ((WORD)(((uintptr_t)(l) >> 16) & 0xffff))
#define MAKELONG(a, b) ((LONG)(((WORD)(a)) | ((DWORD)((WORD)(b))) << 16))
#define __min(a, b) (((a) < (b)) ? (a) : (b))
#define __max(a, b) (((a) > (b)) ? (a) : (b))

// ---- MSVC C runtime names -------------------------------------------------
#define stricmp strcasecmp
#define _stricmp strcasecmp
#define strnicmp strncasecmp
#define _strnicmp strncasecmp
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define wsprintf sprintf
#define _strcmpi strcasecmp
#define strcmpi strcasecmp

#ifdef __cplusplus
extern "C" {
#endif
char *shogo_strupr(char *s);
char *shogo_strlwr(char *s);
char *shogo_itoa(int value, char *buf, int radix);
char *shogo_ltoa(long value, char *buf, int radix);
#ifdef __cplusplus
}
#endif
#define strupr shogo_strupr
#define _strupr shogo_strupr
#define strlwr shogo_strlwr
#define _strlwr shogo_strlwr
#define itoa shogo_itoa
#define _itoa shogo_itoa
#define ltoa shogo_ltoa
#define _ltoa shogo_ltoa

#ifdef __cplusplus
#include "shogo_ptrtoken.h"
#endif
// MSVC headers pulled these in transitively; the game relies on that.
#include "mbstring.h"

#endif // SHOGO_PLATFORM_H
