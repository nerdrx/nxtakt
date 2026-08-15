// <sys/stat.h> for the mingw GUI cross build. See ../README.md.
//
// mingw has a perfectly good sys/stat.h -- stat(), struct stat, S_ISDIR are
// all there. It has one incompatibility: MSVC's mkdir() takes a path and no
// mode, so every `mkdir(p, 0755)` in the codebase is "too many arguments".
//
// In C that would need a macro. In C++ it is an overload: the real one-argument
// mkdir stays exactly as declared, and this adds a two-argument sibling that
// drops the mode -- which is the truthful thing to do, since NTFS ACLs have no
// meaningful mapping from 0755 and a new directory inherits its parent's.
#pragma once
#include_next <sys/stat.h>

#if defined(__cplusplus)
#include <sys/types.h>

inline int mkdir(const char* path, mode_t /*mode*/) { return mkdir(path); }
#endif
