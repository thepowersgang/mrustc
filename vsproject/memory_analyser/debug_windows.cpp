//
//
//
#include "debug_windows.h"
#include <iostream>

ReadMemory* ReadMemory::s_self = 0;

static const size_t VISITED_CHUNK_SIZE = 32;

ReadMemory::ReadMemory(LPVOID view, DWORD view_size, const MINIDUMP_MEMORY_LIST* memory, const MINIDUMP_MEMORY64_LIST* memory64)
    : m_view_base(view)
    , m_visited_bitmap( ((view_size + VISITED_CHUNK_SIZE-1)/VISITED_CHUNK_SIZE + 7) / 8 )
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
::std::pair<size_t,size_t> ReadMemory::calculate_usage()
{
    size_t  num_used_blocks = 0;
    for(auto v : s_self->m_visited_bitmap) {
        for(int i = 0; i < 8; i ++) {
            if( ((v >> i) & 1) != 0 ) {
                num_used_blocks ++;
            }
        }
    }
    return std::make_pair(num_used_blocks * VISITED_CHUNK_SIZE, s_self->m_visited_bitmap.size() * 8 * VISITED_CHUNK_SIZE);
}
/*static*/ void ReadMemory::mark_seen(DWORD64 qwBaseAddress, DWORD nSize)
{
    auto slot = s_self->m_ranges.upper_bound(qwBaseAddress);
    if( slot != s_self->m_ranges.end() ) {
        -- slot;
    }
    if( slot != s_self->m_ranges.end() && qwBaseAddress - slot->first < slot->second.len )
    {
        auto ofs = qwBaseAddress - slot->first;
        for(size_t i = 0; i < nSize; i ++) {
            auto o = (slot->second.ofs + ofs + i) / VISITED_CHUNK_SIZE;
            s_self->m_visited_bitmap[o / 8] |= (1 << (o%8));
        }
    }
}
/*static*/ BOOL ReadMemory::read(HANDLE hProcess, DWORD64 qwBaseAddress, PVOID lpBuffer, DWORD nSize, PDWORD lpNumberOfBytesRead)
{
    return s_self->read_inner(hProcess, qwBaseAddress, lpBuffer, nSize, lpNumberOfBytesRead);
}
BOOL ReadMemory::read_inner(HANDLE /*hProcess*/, DWORD64 qwBaseAddress, PVOID lpBuffer, DWORD nSize, PDWORD lpNumberOfBytesRead)
{
    //std::cout << "> " << std::hex << qwBaseAddress << "+" << nSize << std::endl;

    // Find memory range containing the address
    // - `upper_bound` returns first element after the target value, so shift it backwards to get the element before (or equal to) the target
    auto slot = m_ranges.upper_bound(qwBaseAddress);
    if( slot != m_ranges.end() ) {
        -- slot;
    }
    //if( slot != s_self->m_ranges.end() ) {
    //    ::std::cout << std::hex << slot->first << "+" << slot->second.len << std::dec << "\n";
    //}
    if( slot != m_ranges.end() && qwBaseAddress - slot->first < slot->second.len )
    {
        // Start address is within within the range
        // NOTE: Not bothering to check for if the read spans two ranges, because with 64-bit dumps that shouldn't happen?
        auto ofs = qwBaseAddress - slot->first;
        auto max_size = min(nSize, slot->second.ofs - ofs);
        if(max_size != nSize) {
            std::cout << "> " << std::hex << qwBaseAddress << "+" << nSize << " == ReadMemory::read truncated 0x" << max_size << std::dec << std::endl;
        }

        // Flag chunks
        {
            for(size_t i = 0; i < nSize; i += VISITED_CHUNK_SIZE) {
                auto o = (slot->second.ofs + ofs + i) / VISITED_CHUNK_SIZE;
                m_visited_bitmap[o / 8] |= (1 << (o%8));
            }
        }

        const auto* src = (const char*)m_view_base + slot->second.ofs + ofs;
        memcpy(lpBuffer, src, max_size);
        if(lpNumberOfBytesRead)
            *lpNumberOfBytesRead = static_cast<DWORD>(max_size);
        return TRUE;
    }
    std::cout << "> " << std::hex << qwBaseAddress << "+" << nSize << " == ReadMemory::read failed" << std::dec << std::endl;
    return FALSE;
}