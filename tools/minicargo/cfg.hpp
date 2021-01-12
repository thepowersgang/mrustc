/*
 * mrustc "minicargo" (minimal cargo clone)
 * - By John Hodge (Mutabah)
 *
 * cfg.cpp
 * - Handling of target configuration (in manifest nodes)
 */
#pragma once
#include "stringlist.h"

extern void Cfg_SetTarget(const char* target_name);
extern bool Cfg_Check(const char* cfg_string);
extern void Cfg_ToEnvironment(StringListKV& out);
