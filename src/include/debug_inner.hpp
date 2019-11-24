/*
 */
#pragma once
#include <ctime>
#include <initializer_list>

extern void debug_init_phases(const char* env_var_name, std::initializer_list<const char*> il);

class DebugTimedPhase
{
    const char* m_name;
    clock_t m_start;
public:
    DebugTimedPhase(const char* name);
    ~DebugTimedPhase();
};
