/*
 * MiniCargo - mrustc's minimal clone of cargo
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * debug.cpp
 * - Debugging helpers
 */
#include <set>
#include <iostream>
#include "debug.h"
#include <mutex>

static int giIndentLevel = 0;
static const char* gsDebugPhase = "";
static ::std::set<::std::string> gmDisabledDebug;
static ::std::mutex gDebugLock;

void Debug_SetPhase(const char* phase_name)
{
    gsDebugPhase = phase_name;
}
bool Debug_IsEnabled()
{
    if( gmDisabledDebug.find(gsDebugPhase) != gmDisabledDebug.end() )
        return false;
    return true;
}
void Debug_DisablePhase(const char* phase_name)
{
    gmDisabledDebug.insert( ::std::string(phase_name) );
}
void Debug_Print(::std::function<void(::std::ostream& os)> cb)
{
    if( !Debug_IsEnabled() )
        return ;
    ::std::unique_lock<::std::mutex>    _lh { gDebugLock };

    ::std::cout << gsDebugPhase << "- ";
    for(auto i = giIndentLevel; i --; )
        ::std::cout << " ";
    cb(::std::cout);
    ::std::cout << ::std::endl;
}
void Debug_EnterScope(const char* name, dbg_cb_t cb)
{
    if( !Debug_IsEnabled() )
        return ;
    ::std::unique_lock<::std::mutex>    _lh { gDebugLock };

    ::std::cout << gsDebugPhase << "- ";
    for(auto i = giIndentLevel; i --; )
        ::std::cout << " ";
    ::std::cout << ">>> " << name << "(";
    cb(::std::cout);
    ::std::cout << ")" << ::std::endl;
    giIndentLevel ++;
}
void Debug_LeaveScope(const char* name, dbg_cb_t cb)
{
    if( !Debug_IsEnabled() )
        return ;
    ::std::unique_lock<::std::mutex>    _lh { gDebugLock };

    ::std::cout << gsDebugPhase << "- ";
    giIndentLevel --;
    for(auto i = giIndentLevel; i --; )
        ::std::cout << " ";
    ::std::cout << "<<< " << name << ::std::endl;
}
