// Stand-in for MSVC <direct.h>.
#ifndef SHOGO_COMPAT_DIRECT_H
#define SHOGO_COMPAT_DIRECT_H
#include <unistd.h>
#include <sys/stat.h>
#define _mkdir(p) mkdir((p), 0755)
#define _getcwd getcwd
#define _chdir chdir
#endif
