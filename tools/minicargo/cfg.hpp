/*
 * mrustc "minicargo" (minimal cargo clone)
 * - By John Hodge (Mutabah)
 *
 * cfg.cpp
 * - Handling of target configuration (in manifest nodes)
 */
#pragma once
#include "stringlist.h"

struct AllowedCfg {
    ::std::vector<std::string>  flags;
    ::std::vector<std::string>  values;

    void add_from_string(const char* s);
};

extern void Cfg_SetTarget(const char* target_name);
extern bool Cfg_Check(const char* cfg_string, const std::vector<std::string>& features, const AllowedCfg& allowed_cfg);
extern void Cfg_ToEnvironment(StringListKV& out);
