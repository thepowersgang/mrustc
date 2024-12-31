/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * version.cpp
 * - Compiler version number
 */
#include <version.hpp>
#include <sstream>

#define VERSION_MAJOR   0
#define VERSION_MINOR   11
#define VERSION_PATCH   2

#ifdef _WIN32
# define VERSION_GIT_ISDIRTY    1
# define VERSION_GIT_FULLHASH   "unknown"
# define VERSION_GIT_SHORTHASH   "msvc"
# define VERSION_BUILDTIME  "unknown"
# define VERSION_GIT_BRANCH "unknown"
#endif

unsigned int giVersion_Major = VERSION_MAJOR;
unsigned int giVersion_Minor = VERSION_MINOR;
unsigned int giVersion_Patch = VERSION_PATCH;
bool gbVersion_GitDirty = VERSION_GIT_ISDIRTY;
const char* gsVersion_GitHash = VERSION_GIT_FULLHASH;
const char* gsVersion_GitShortHash = VERSION_GIT_SHORTHASH;
const char* gsVersion_BuildTime = VERSION_BUILDTIME;


::std::string Version_GetString()
{
    ::std::stringstream ss;
    ss << "v" << VERSION_MAJOR << "." << VERSION_MINOR << "." << VERSION_PATCH << " " << VERSION_GIT_BRANCH << ":" << VERSION_GIT_SHORTHASH;
    return ss.str();
}
