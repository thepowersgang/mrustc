/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * version.hpp
 * - Compiler version number
 */
#pragma once

#include <string>

extern unsigned int giVersion_Major;
extern unsigned int giVersion_Minor;
extern unsigned int giVersion_Patch;
extern const char* gsVersion_GitHash;
extern const char* gsVersion_GitShortHash;
extern const char* gsVersion_BuildTime;
extern bool gbVersion_GitDirty;

extern ::std::string Version_GetString();
