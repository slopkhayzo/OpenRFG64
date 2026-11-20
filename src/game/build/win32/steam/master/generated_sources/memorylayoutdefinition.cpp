// Original: game/build/win32/steam/master/generated_sources/memorylayoutdefinition.cpp
//
// Generated: the game's memory layout, recovered from .data (s_memoryBlocks 0x01348348, 15 blocks). Every block is main
// memory, 1 KB aligned for the RFG blocks and 128 bytes for the GameFramework ones, with protection 0x13 (read, write
// and bit 0x10). A size of 0 leaves the block empty. The indices are what the code locks: 1, 2, 3, 5, 6, 7, 8, 9 in
// rfg_init_keen_memory_allocators, 10 (the default allocator) and 11 (shader objects) in GameFramework::initialize.
// Sizes are decimal in the source (the hex values are 1,000, 80,000,000, ...).

#include "game/build/win32/steam/master/generated_sources/memorylayoutdefinition.hpp"
#include "library/modules/runtime/base/sources/global/memorylayout.hpp"
#include "scaling/scaling.h" // scaling

namespace keen
{
    static const MemoryBlockDefinition s_memoryBlocks[15] = // 0x01348348
    {
        { 1000u, 0x400u, MemoryType_MainMemory, 0u, 0x13u, "RFG_SysMempoolProvider" }, // 0
        { 80000000u, 0x400u, MemoryType_MainMemory, 0u, 0x13u, "RFG_VramMempoolProvider" }, // 1
        { 1700000000u, 0x400u, MemoryType_MainMemory, 0u, 0x13u, "RFG_MempoolProvider" }, // 2
        { 300000u, 0x400u, MemoryType_MainMemory, 0u, 0x13u, "RFG_GpuScratchMem" }, // 3
        { 0u, 0x400u, MemoryType_MainMemory, 0u, 0x13u, "RFG_Graphics" }, // 4
        { 0u, 0x400u, MemoryType_MainMemory, 0u, 0x13u, "RFG_Framebuffers" }, // 5
        { 220000000u, 0x400u, MemoryType_MainMemory, 0u, 0x13u, "RFG_Level" }, // 6
        { 500000000u, 0x400u, MemoryType_MainMemory, 0u, 0x13u, "RFG_D3Dscratch" }, // 7
        { 190000000u, 0x400u, MemoryType_MainMemory, 0u, 0x13u, "RFG_Audio_Bank" }, // 8
        { 20000000u, 0x400u, MemoryType_MainMemory, 0u, 0x13u, "RFG_Audio" }, // 9
        { 180000000u, 0x80u, MemoryType_MainMemory, 0u, 0x13u, "GameFramework_Default" }, // 10
        { 10000000u, 0x80u, MemoryType_MainMemory, 0u, 0x13u, "GameFramework_ShaderObjects" }, // 11
        { 0u, 0x80u, MemoryType_MainMemory, 0u, 0x13u, "GameFramework_Content_Scratch" }, // 12
        { 0u, 0x80u, MemoryType_MainMemory, 0u, 0x13u, "GameFramework_Content_ResourceObjects" }, // 13
        { 64000u, 0x80u, MemoryType_MainMemory, 0u, 0x13u, "GameFramework_DebugTexture" }, // 14
    };

    static const MemoryLayoutDefinition s_memoryLayoutDefinition = { s_memoryBlocks, 15u }; // 0x01348474

    // 0x00CE6120 (:36)
    const MemoryLayoutDefinition& getMemoryLayoutDefinition()
    {
        // scaling (OpenRFG64-memtest): level memory (block 6) is a grow-on-demand region then, just under 2 GB (the streamer
        // compares its free space as an int), only reserved, together with Havok's heap (memorylayout_win32.cpp); the
        // main provider (block 2) is 448 MB smaller, Havok's heap no longer being carved from it.
        static MemoryBlockDefinition blocks[15];
        static MemoryLayoutDefinition definition = { blocks, 15u };
        static bool made = false;
        if (!made)
        {
            for (int i = 0; i < 15; i++)
            {
                blocks[i] = s_memoryBlocks[i];
            }
            if (scaling_enabled())
            {
                blocks[6].size = 0xDFFF0000u; // 3.5 GB: the streamer clamps its sizes, a container may pass 2 GB (stream2_container.cpp)
                blocks[2].size -= 0x1C000000u;
            }
            made = true;
        }
        return s_memoryLayoutDefinition.blockCount == 15u ? definition : s_memoryLayoutDefinition;
    }
}
