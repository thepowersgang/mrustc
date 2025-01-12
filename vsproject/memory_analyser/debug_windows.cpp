//
//
//
#include "debug_windows.h"
#include <iostream>

ReadMemory* ReadMemory::s_self = 0;

/*static*/ BOOL ReadMemory::read(HANDLE /*hProcess*/, DWORD64 qwBaseAddress, PVOID lpBuffer, DWORD nSize, PDWORD lpNumberOfBytesRead)
{
    //std::cout << "> " << std::hex << qwBaseAddress << "+" << nSize << std::endl;
    // Find memory range containing the address
    if( s_self->memory )
    {
        for(ULONG32 i = 0; i < s_self->memory->NumberOfMemoryRanges; i ++)
        {
            const auto& rng = s_self->memory->MemoryRanges[i];
            //std::cout << "- " << std::hex << rng.StartOfMemoryRange << "+" << rng.Memory.DataSize << std::endl;

            if( rng.StartOfMemoryRange <= qwBaseAddress && qwBaseAddress <= rng.StartOfMemoryRange + rng.Memory.DataSize )
            {
                auto ofs = qwBaseAddress - rng.StartOfMemoryRange;
                auto max_size = min(nSize, rng.Memory.DataSize - ofs);
                const auto* src = (const char*)s_self->view + rng.Memory.Rva + ofs;
                memcpy(lpBuffer, src, max_size);
                if(lpNumberOfBytesRead)
                    *lpNumberOfBytesRead = static_cast<DWORD>(max_size);
                return TRUE;
            }
        }
    }
    if( s_self->memory64 )
    {
        DWORD64 base_rva = s_self->memory64->BaseRva;
        for(ULONG32 i = 0; i < s_self->memory64->NumberOfMemoryRanges; i ++)
        {
            const auto& rng = s_self->memory64->MemoryRanges[i];
            //std::cout << "- " << (void*)base_rva << " " << std::hex << rng.StartOfMemoryRange << "+" << rng.DataSize << std::endl;

            if( rng.StartOfMemoryRange <= qwBaseAddress && qwBaseAddress <= rng.StartOfMemoryRange + rng.DataSize )
            {
                auto ofs = qwBaseAddress - rng.StartOfMemoryRange;
                auto max_size = min(nSize, rng.DataSize - ofs);
                const auto* src = (const char*)s_self->view + base_rva + ofs;
                memcpy(lpBuffer, src, max_size);
                if(lpNumberOfBytesRead)
                    *lpNumberOfBytesRead = static_cast<DWORD>(max_size);
                //if( nSize == 8 )
                //    std::cout << "> " << std::hex << qwBaseAddress << "+" << nSize << " = " << *(void**)lpBuffer << std::endl;
                return TRUE;
            }

            base_rva += rng.DataSize;
        }
    }
    std::cout << "> " << std::hex << qwBaseAddress << "+" << nSize << " == ReadMemory::read failed" << std::dec << std::endl;
    return FALSE;
}