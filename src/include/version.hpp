/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * version.hpp
 * - Compiler version number
 */
#pragma once

#include <string>

extern const unsigned int giVersion_Major;
extern const unsigned int giVersion_Minor;
extern const unsigned int giVersion_Patch;
extern const bool gbVersion_GitDirty;
extern const char gsVersion_GitHash[];
extern const char gsVersion_GitShortHash[];
extern const char gsVersion_GitBranch[];
extern const char gsVersion_BuildTime[];

extern ::std::string Version_GetString();
