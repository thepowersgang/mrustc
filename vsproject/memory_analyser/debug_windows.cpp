//
//
//
#include "debug_windows.h"
#include <iostream>

ReadMemory* ReadMemory::s_self = 0;

ReadMemory::ReadMemory(LPVOID view, const MINIDUMP_MEMORY_LIST* memory, const MINIDUMP_MEMORY64_LIST* memory64)
    : view_base(view)
{
    if(memory)
    {
        for(ULONG32 i = 0; i < memory->NumberOfMemoryRanges; i ++)
        {
            const auto& rng = memory->MemoryRanges[i];
            m_ranges[rng.StartOfMemoryRange] = MemoryRange { rng.Memory.Rva, rng.Memory.DataSize };
        }
    }
    if(memory64)
    {
        DWORD64 base_rva = memory64->BaseRva;
        for(ULONG32 i = 0; i < memory64->NumberOfMemoryRanges; i ++)
        {
            const auto& rng = memory64->MemoryRanges[i];
            m_ranges[rng.StartOfMemoryRange] = MemoryRange { base_rva, rng.DataSize };
            base_rva += rng.DataSize;
        }
    }
    for(const auto& r : m_ranges) {
        ::std::cout << "@" << std::hex << r.first << "+" << r.second.len << " => +0x" << r.second.ofs << "\n";
    }
    s_self = this;
}
/*static*/ BOOL ReadMemory::read(HANDLE /*hProcess*/, DWORD64 qwBaseAddress, PVOID lpBuffer, DWORD nSize, PDWORD lpNumberOfBytesRead)
{
    //std::cout << "> " << std::hex << qwBaseAddress << "+" << nSize << std::endl;

    // Find memory range containing the address
    // - `upper_bound` returns first element after the target value, so shift it backwards to get the element before (or equal to) the target
    auto slot = s_self->m_ranges.upper_bound(qwBaseAddress);
    if( slot != s_self->m_ranges.end() ) {
        -- slot;
    }
    //if( slot != s_self->m_ranges.end() ) {
    //    ::std::cout << std::hex << slot->first << "+" << slot->second.len << std::dec << "\n";
    //}
    if( slot != s_self->m_ranges.end() && qwBaseAddress - slot->first < slot->second.len )
    {
        // Start address is within within the range
        // NOTE: Not bothering to check for if the read spans two ranges, because with 64-bit dumps that shouldn't happen?
        auto ofs = qwBaseAddress - slot->first;
        auto max_size = min(nSize, slot->second.ofs - ofs);
        if(max_size != nSize) {
            std::cout << "> " << std::hex << qwBaseAddress << "+" << nSize << " == ReadMemory::read truncated 0x" << max_size << std::dec << std::endl;
        }
        const auto* src = (const char*)s_self->view_base + slot->second.ofs + ofs;
        memcpy(lpBuffer, src, max_size);
        if(lpNumberOfBytesRead)
            *lpNumberOfBytesRead = static_cast<DWORD>(max_size);
        return TRUE;
    }
    std::cout << "> " << std::hex << qwBaseAddress << "+" << nSize << " == ReadMemory::read failed" << std::dec << std::endl;
    return FALSE;
}