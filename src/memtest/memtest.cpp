// OpenRFG64 memory test (new code, not recovered). See memtest.h.
//
// Output (in the run folder):
//   memtest.csv          one line per census (default once a second): process, budgets, heaps, tables, counts
//   memtest_pools.csv    every vlib mempool every 10 s: name, used, size, peak
//   memtest_events.log   start-up, the Sledge layer, plugins, game states, pools that fill, the game's failure messages
//   memtest_native.log   everything the game prints (keen::debug::nativePrint)
//   memtest_summary.txt  peaks and what filled first; rewritten every 10 s, so a crash leaves the latest

#include "memtest/memtest.h"
#include "scaling/scaling.h"
#include "volition/rfg/code/havok/havok_memory.h"

// The game's headers first, then windows.h (OpenRFG's order: winsock2 must come before any windows.h).
#include "volition/libmodules/havok/hk2014_2_5_r1/source/common/base/memory/system/hkmemorysystem.h"
#include "volition/libmodules/havok/hk2014_2_5_r1/source/common/base/object/hkreferencedobject.h" // Memtest.HavokCensus
#include "volition/libmodules/havok/hk2014_2_5_r1/source/common/base/memory/allocator/freelist/hkfreelist.h"
#include "volition/libmodules/havok/hk2014_2_5_r1/source/common/base/memory/allocator/freelist/hkfreelistallocator.h"
#include "volition/libmodules/havok/hk2014_2_5_r1/source/common/base/memory/system/freelist/hkfreelistmemorysystem.h"
#include "volition/rfg/code/gameplay/wrecking_crew/wrecking_crew.h"
#include "volition/rfg/code/gamewide/gameseq.h"
#include "volition/rfg/code/interface/localization/localization_shared.h"
#include "volition/rfg/code/interface/loading.h" // game_set_wc_shutdown (Memtest.WcRepeat)
#include "volition/rfg/code/interface/ui_document_manager.h"
#include "volition/rfg/code/level/world.h"
#include "volition/rfg/code/rfg.h"
#include "volition/vlib/code/cfile/cfile_common.h"
#include "volition/vlib/code/memory/mempool_base.h"
#include "volition/vlib/code/memory/split_memmgr.h"
#include "volition/vlib/code/util/endian_type.h"
#include "keen/base/tlsfmemoryallocator.hpp"
#include "keen/base/zonememoryallocator.hpp"
#include "keen/base/mutex.hpp"
#include "library/modules/thirdparty/tlsf/tlsf.h" // the audio bank walk

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <ctype.h>
#include <share.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "psapi.lib")

namespace keen
{
    namespace memory
    {
        // memorysystem.cpp: a block of the layout and its name.
        bool memtestLayoutBlock(unsigned int index, const void** pStart, unsigned long long* size, const char** pName);
    }
}

namespace
{
    // ---- options (parsed before WinMain)
    const char* Wc_map = nullptr;
    const char* Wc_options_hex = nullptr;
    const char* Sledge_dir = nullptr;
    const char* Plugins_dir = nullptr;
    const char* Out_dir_arg = nullptr;
    int Exit_after_s = 0;

    struct options_setup
    {
        options_setup()
        {
            for (int i = 1; i + 1 < __argc; i++)
            {
                const char* arg = __argv[i];
                const char* value = __argv[i + 1];
                if (strcmp(arg, "--wc") == 0) { Wc_map = value; i++; }
                else if (strcmp(arg, "--wc-options") == 0) { Wc_options_hex = value; i++; }
                else if (strcmp(arg, "--sledge") == 0) { Sledge_dir = value; i++; }
                else if (strcmp(arg, "--plugins") == 0) { Plugins_dir = value; i++; }
                else if (strcmp(arg, "--memtest-dir") == 0) { Out_dir_arg = value; i++; }
                else if (strcmp(arg, "--exit-after") == 0) { Exit_after_s = atoi(value); i++; }
            }
        }
    };
    options_setup Options_setup;

    // ---- output
    char Out_dir[MAX_PATH] = ".";
    FILE* Csv = nullptr;
    FILE* Pools_csv = nullptr;
    FILE* Events = nullptr;
    FILE* Native = nullptr;
    SRWLOCK Log_lock = SRWLOCK_INIT;
    bool Initialized = false;
    unsigned long long Start_ms = 0;
    unsigned int Interval_ms = 1000;

    double now_s()
    {
        return Start_ms == 0 ? 0.0 : (double)(GetTickCount64() - Start_ms) / 1000.0;
    }

    FILE* open_out(const char* name, const char* mode)
    {
        char path[MAX_PATH];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", Out_dir, name);
        FILE* f = nullptr;
        // Shared reading: the run script and the agent may read the files while the game runs.
        f = _fsopen(path, mode, _SH_DENYWR);
        return f;
    }

    // ---- the pool registry (mempool_base::base_init registers, ~mempool_base unregisters)
    struct pool_entry
    {
        mempool_base* pool;
        char name[32];
        unsigned int size;
        unsigned int used;
        unsigned int peak_seen;
        double full_at;      // first census time it was at least 99.9 % used (-1: never, -2: full when first seen)
        bool dynamic;
        bool valid;
        bool seen;           // read at least once
    };
    const int Max_pools = 4096;
    pool_entry Pools[Max_pools];
    int Pool_count = 0;
    int Pool_registry_overflow = 0;
    SRWLOCK Pool_lock = SRWLOCK_INIT;
    uintptr_t Image_lo = 0;
    uintptr_t Image_hi = 0;

    // A registered pool read under SEH: the pool may have been released without its destructor (pools inside pools),
    // so its vtable must lie in the program image and an access violation only drops it.
    bool read_pool(mempool_base* pool, unsigned int* used, unsigned int* size, unsigned int* peak, bool* dynamic, char* name)
    {
        __try
        {
            const uintptr_t vptr = *(const uintptr_t*)pool;
            if (vptr < Image_lo || vptr >= Image_hi)
            {
                return false;
            }
            *used = pool->space_used();
            *size = pool->space_max();
            *peak = pool->peak_used();
            *dynamic = pool->is_dynamic();
            const char* n = pool->get_name();
            int i = 0;
            for (; i < 31 && n[i] != '\0'; i++)
            {
                name[i] = n[i];
            }
            name[i] = '\0';
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // ---- counters fed by the recovered code
    volatile long long Crt_bytes = 0;
    volatile long long Crt_blocks = 0;
    volatile long long Crt_peak = 0;
    volatile long Strpool_fallbacks = 0;
    volatile long Strpool_unregistered = 0;
    volatile long Failure_messages = 0;
    volatile long Havok_oom = 0;
    volatile long Level_failures = 0;
    int Peak_cm_models = 0;
    unsigned int Cm_high_water = 0; // scaling: the highest collision model index handed out, + 1
    int Peak_cm_side = 0;           // scaling: the most indices kept past the chunks' 16 bits
    int Peak_movers = 0;
    int Peak_zone_handles = 0;
    int Peak_render;
    long long Havok_system_used; // memtest_debris: Havok's system allocator in use (its block, 512 MB on x64)
    long long Havok_solver_used;
    int Render_max;
    memtest_debris_counts Debris;
    unsigned int Peak_level = 0;
    unsigned int Level_size = 0;
    char Level_first_failure[160] = "";

    // ---- peaks for the summary
    unsigned long long Peak_private = 0;
    unsigned long long Peak_ws = 0;
    double Over_4gb_at = -1.0;
    long long Peak_havok_inuse = 0;
    long long Havok_total = 0;
    unsigned int Peak_far = 0;
    int Peak_objects = 0;
    int Max_objects = 0;
    unsigned int Peak_provider = 0;
    unsigned int Provider_total = 0;
    unsigned int Frame = 0;
    int Last_state = -100;
    unsigned long long Last_census_ms = 0;
    unsigned long long Last_pools_ms = 0;
    unsigned long long Last_summary_ms = 0;
    char Last_failure[256] = "";

    // ---- the Wrecking Crew start
    bool Wc_selected = false;
    bool Wc_started = false;
    unsigned long long Wc_loaded_since = 0;
    unsigned long long Wc_started_ms = 0;

    // ---- the Sledge layer
    struct overlay_file
    {
        char name[128];       // the base name the game asks for
        char keen_path[320];  // /sledgeN\relative\path
        unsigned long long size;
    };
    const int Max_overlay_files = 512;
    overlay_file Overlay_files[Max_overlay_files];
    int Overlay_file_count = 0;
    char Mount_native[16][MAX_PATH];
    char Mount_name[16][32];
    int Mount_count = 0;
    memtest_wc_map_addition Wc_additions[256]; // as many as the game's map list (wrecking_crew.cpp)
    int Wc_addition_count = 0;
    volatile long Overlay_hits = 0;

    void log_line(FILE* f, const char* text)
    {
        if (f == nullptr)
        {
            return;
        }
        AcquireSRWLockExclusive(&Log_lock);
        fprintf(f, "%9.3f %s\n", now_s(), text);
        fflush(f);
        ReleaseSRWLockExclusive(&Log_lock);
    }

    const char* state_name(int s)
    {
        switch (s)
        {
        case 0: return "MAINMENU";
        case 1: return "GAMEPLAY";
        case 2: return "LOAD";
        case 19: return "WRECKING_CREW_MAIN_MENU";
        case 59: return "WC_INIT";
        case 60: return "WC_SHUTDOWN";
        default: return "";
        }
    }

    // ---- Sledge folder
    bool read_text(const char* path, char** out, size_t* out_size)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, path, "rb") != 0 || f == nullptr)
        {
            return false;
        }
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char* buf = (char*)malloc((size_t)n + 1);
        const size_t got = fread(buf, 1, (size_t)n, f);
        fclose(f);
        buf[got] = '\0';
        *out = buf;
        *out_size = got;
        return true;
    }

    void add_overlay_files(int mount, const char* root, const char* rel)
    {
        char pattern[MAX_PATH];
        _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s%s%s\\*", root, rel[0] ? "\\" : "", rel);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            return;
        }
        do
        {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            {
                continue;
            }
            char child[MAX_PATH];
            _snprintf_s(child, sizeof(child), _TRUNCATE, "%s%s%s", rel, rel[0] ? "\\" : "", fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                add_overlay_files(mount, root, child);
            }
            else if (Overlay_file_count < Max_overlay_files)
            {
                overlay_file& o = Overlay_files[Overlay_file_count++];
                strncpy_s(o.name, fd.cFileName, _TRUNCATE);
                _snprintf_s(o.keen_path, sizeof(o.keen_path), _TRUNCATE, "%s\\%s", Mount_name[mount], child);
                o.size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                memtest_event("sledge: file %s -> %s (%llu bytes)", o.name, o.keen_path, o.size);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    // The quoted string after "key" and "=": file = "wcavenue" -> wcavenue.
    bool lua_string_field(const char* from, const char* to, const char* key, char* out, size_t out_size)
    {
        const size_t klen = strlen(key);
        for (const char* p = from; p + klen < to; p++)
        {
            if (strncmp(p, key, klen) != 0 || (p > from && (isalnum((unsigned char)p[-1]) || p[-1] == '_')))
            {
                continue;
            }
            const char* q = p + klen;
            while (q < to && (*q == ' ' || *q == '\t'))
            {
                q++;
            }
            if (q >= to || *q != '=')
            {
                continue;
            }
            q++;
            while (q < to && (*q == ' ' || *q == '\t'))
            {
                q++;
            }
            if (q >= to || *q != '"')
            {
                continue;
            }
            q++;
            const char* e = q;
            while (e < to && *e != '"')
            {
                e++;
            }
            if (e >= to)
            {
                return false;
            }
            const size_t n = (size_t)(e - q) < out_size - 1 ? (size_t)(e - q) : out_size - 1;
            memcpy(out, q, n);
            out[n] = '\0';
            return true;
        }
        return false;
    }

    // What the avenue mod's mod.lua asks of Sledge, read from its text: the files it registers (every file in its
    // files\ folder) and the Wrecking Crew map entries of its MAPS table ({ file = "X", copy_of = "Y" }) added to the
    // table named in its register_xml_edit call.
    void apply_mod(const char* id, const char* mod_dir)
    {
        char files_dir[MAX_PATH];
        _snprintf_s(files_dir, sizeof(files_dir), _TRUNCATE, "%s\\files", mod_dir);
        const DWORD attr = GetFileAttributesA(files_dir);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0 && Mount_count < 16)
        {
            const int m = Mount_count++;
            strncpy_s(Mount_native[m], files_dir, _TRUNCATE);
            _snprintf_s(Mount_name[m], sizeof(Mount_name[m]), _TRUNCATE, "/sledge%d", m);
            add_overlay_files(m, files_dir, "");
        }
        char lua_path[MAX_PATH];
        _snprintf_s(lua_path, sizeof(lua_path), _TRUNCATE, "%s\\mod.lua", mod_dir);
        char* lua = nullptr;
        size_t lua_size = 0;
        if (!read_text(lua_path, &lua, &lua_size))
        {
            memtest_event("sledge: %s has no mod.lua", id);
            return;
        }
        // The table its xml edit goes to.
        int dlc_id = -2;
        const char* edit = strstr(lua, "register_xml_edit(\"");
        if (edit != nullptr)
        {
            const char* t = edit + strlen("register_xml_edit(\"");
            if (_strnicmp(t, "dlc", 3) == 0 && isdigit((unsigned char)t[3]) && isdigit((unsigned char)t[4]) && strncmp(t + 5, "_wrecking_crew.xtbl", 19) == 0)
            {
                dlc_id = (t[3] - '0') * 10 + (t[4] - '0');
            }
            else if (strncmp(t, "wrecking_crew.xtbl", 18) == 0)
            {
                dlc_id = -1;
            }
        }
        // Each { file = "X", copy_of = "Y" } entry.
        int found = 0;
        for (const char* p = strstr(lua, "copy_of"); p != nullptr; p = strstr(p + 7, "copy_of"))
        {
            const char* open = p;
            while (open > lua && *open != '{')
            {
                open--;
            }
            const char* close = strchr(p, '}');
            if (close == nullptr || *open != '{')
            {
                continue;
            }
            memtest_wc_map_addition a = {};
            if (!lua_string_field(open, close, "file", a.file, sizeof(a.file)) || !lua_string_field(open, close, "copy_of", a.copy_of, sizeof(a.copy_of)))
            {
                continue;
            }
            if (dlc_id == -2)
            {
                memtest_event("sledge: %s: map %s found but no wrecking crew table edit; ignored", id, a.file);
                continue;
            }
            a.dlc_id = dlc_id;
            char name[192] = "";
            if (lua_string_field(open, close, "name", name, sizeof(name)))
            {
                MultiByteToWideChar(CP_UTF8, 0, name, -1, a.name, (int)(sizeof(a.name) / sizeof(a.name[0])));
                a.name[sizeof(a.name) / sizeof(a.name[0]) - 1] = L'\0';
            }
            lua_string_field(open, close, "image", a.image, sizeof(a.image));
            if (Wc_addition_count < (int)(sizeof(Wc_additions) / sizeof(Wc_additions[0])))
            {
                Wc_additions[Wc_addition_count++] = a;
                found++;
                memtest_event("sledge: %s: Wrecking Crew map %s (a copy of %s's entry%s%s%s%s) in %s", id, a.file, a.copy_of,
                              name[0] ? ", named " : "", name, a.image[0] ? ", picture " : "", a.image,
                              dlc_id < 0 ? "wrecking_crew.xtbl" : "a dlc wrecking crew table");
            }
        }
        if (strstr(lua, "register_") != nullptr && found == 0 && Mount_count == 0)
        {
            memtest_event("sledge: %s: nothing this layer understands (only register_file and the Wrecking Crew map entries are supported)", id);
        }
        free(lua);
    }

    void load_sledge(const char* dir)
    {
        char toml_path[MAX_PATH];
        _snprintf_s(toml_path, sizeof(toml_path), _TRUNCATE, "%s\\sledge.toml", dir);
        char* toml = nullptr;
        size_t toml_size = 0;
        if (!read_text(toml_path, &toml, &toml_size))
        {
            memtest_event("sledge: no %s", toml_path);
            return;
        }
        // [sledge.mods.'<id>'] followed by enabled = true|false (option sections, [sledge.mods.'<id>'.options], are skipped).
        char current[128] = "";
        char* line = toml;
        while (line != nullptr && *line != '\0')
        {
            char* next = strchr(line, '\n');
            if (next != nullptr)
            {
                *next++ = '\0';
            }
            while (*line == ' ' || *line == '\t')
            {
                line++;
            }
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' '))
            {
                line[--len] = '\0';
            }
            if (line[0] == '[')
            {
                current[0] = '\0';
                const char* prefix = "[sledge.mods.'";
                if (strncmp(line, prefix, strlen(prefix)) == 0)
                {
                    const char* id = line + strlen(prefix);
                    const char* q = strchr(id, '\'');
                    if (q != nullptr && strcmp(q, "']") == 0)
                    {
                        const size_t n = (size_t)(q - id) < sizeof(current) - 1 ? (size_t)(q - id) : sizeof(current) - 1;
                        memcpy(current, id, n);
                        current[n] = '\0';
                    }
                }
            }
            else if (current[0] != '\0' && strncmp(line, "enabled", 7) == 0)
            {
                const bool enabled = strstr(line, "true") != nullptr;
                memtest_event("sledge: mod %s %s", current, enabled ? "enabled" : "disabled");
                if (enabled)
                {
                    char mod_dir[MAX_PATH];
                    _snprintf_s(mod_dir, sizeof(mod_dir), _TRUNCATE, "%s\\mods\\%s", dir, current);
                    apply_mod(current, mod_dir);
                }
            }
            line = next;
        }
        free(toml);
    }

    void load_plugins(const char* dir)
    {
        char pattern[MAX_PATH];
        _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*.asi", dir);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            memtest_event("plugins: none in %s", dir);
            return;
        }
        do
        {
            char path[MAX_PATH];
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", dir, fd.cFileName);
            HMODULE module = LoadLibraryA(path);
            if (module == nullptr)
            {
                memtest_event("plugins: %s did not load (error %lu)", path, GetLastError());
                continue;
            }
            memtest_event("plugins: %s loaded at %p", path, (void*)module);
            // Ultimate ASI Loader's convention: an exported InitializeASI is called once the plugin is in.
            typedef void (*initialize_fn)();
            if (initialize_fn init = (initialize_fn)GetProcAddress(module, "InitializeASI"))
            {
                init();
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    // ---- the census
    struct keen_alloc_sample
    {
        unsigned int used;
        unsigned int peak;
        unsigned int total;
    };

    template<typename T>
    keen_alloc_sample sample_allocator(const T& a)
    {
        keen_alloc_sample s;
        s.used = a.getAllocatedSize();
        s.peak = a.getMaxAllocatedSize();
        s.total = a.getTotalSize();
        return s;
    }

    void havok_stats(long long* in_use, long long* peak, long long* total)
    {
        *in_use = 0;
        *peak = 0;
        *total = 0;
        hkMemorySystem* system = hkMemorySystem::getInstancePtr();
        if (system == nullptr)
        {
            return;
        }
        hkMemorySystem::MemoryStatistics stats;
        system->getMemoryStatistics(stats);
        // hkFreeListMemorySystem::getMemoryStatistics: System (the block's allocator), Heap (the free lists on top of
        // it: Havok's objects), Debug, Solver. Their sum counted the heap twice; the heap and the system are apart now.
        for (int i = 0; i < stats.m_entries.getSize(); i++)
        {
            const hkMemorySystem::MemoryStatistics::Entry& e = stats.m_entries[i];
            if (i == 1)
            {
                *in_use = e.m_allocatorStats.m_inUse;
                *peak = e.m_allocatorStats.m_peakInUse;
            }
            else if (i == 0)
            {
                Havok_system_used = e.m_allocatorStats.m_inUse;
                if (e.m_allocatorStats.m_totalAvailable > 0) // -1: an allocator without a limit
                {
                    *total = e.m_allocatorStats.m_totalAvailable;
                }
                if (havok_memory_heap_page_limit() > 0) // scaling: the heap grows in pages up to this
                {
                    *total = havok_memory_heap_page_limit();
                }
            }
            else if (i == 3)
            {
                Havok_solver_used = e.m_allocatorStats.m_inUse;
            }
        }
    }

    double mb(unsigned long long bytes)
    {
        return (double)bytes / 1048576.0;
    }

    void write_summary(const char* reason);
    unsigned long long scaling_committed_total();
    const char* pool_label(const char* name, char* out, size_t size);

    void census()
    {
        const double t = now_s();
        PROCESS_MEMORY_COUNTERS_EX pmc = {};
        pmc.cb = sizeof(pmc);
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
        MEMORYSTATUSEX ms = {};
        ms.dwLength = sizeof(ms);
        GlobalMemoryStatusEx(&ms);
        const unsigned long long va_used = ms.ullTotalVirtual - ms.ullAvailVirtual;
        if (pmc.PrivateUsage > Peak_private)
        {
            Peak_private = pmc.PrivateUsage;
        }
        if (pmc.WorkingSetSize > Peak_ws)
        {
            Peak_ws = pmc.WorkingSetSize;
        }
        if (Over_4gb_at < 0.0 && pmc.PrivateUsage > 4ull * 1024 * 1024 * 1024)
        {
            Over_4gb_at = t;
            memtest_event("process: private bytes passed 4 GB (%.0f MB)", mb(pmc.PrivateUsage));
        }

        keen_alloc_sample provider = sample_allocator(Rfg_mempool_provider_keen_allocator);
        const keen_alloc_sample vram = sample_allocator(Rfg_vram_mempool_provider_keen_allocator);
        const keen_alloc_sample bank = sample_allocator(Rfg_audio_bank_keen_allocator);
        // The keen allocators' own counters are not kept (they read 0, as in rfg.exe); the main block is a zone, whose
        // top is what it has handed out.
        provider.used = Rfg_mempool_provider_keen_allocator.getAllocator().getUsedSize();
        if (provider.used > Peak_provider)
        {
            Peak_provider = provider.used;
        }
        provider.peak = Peak_provider;
        Provider_total = provider.total;

        // The pools: totals, the fullest, the first time each becomes full.
        int pools = 0;
        unsigned long long pools_used = 0;
        double fullest_pct = 0.0;
        char fullest[32] = "-";
        AcquireSRWLockExclusive(&Pool_lock);
        for (int i = 0; i < Pool_count; i++)
        {
            pool_entry& e = Pools[i];
            unsigned int used, size, peak;
            bool dynamic;
            char name[32];
            if (!read_pool(e.pool, &used, &size, &peak, &dynamic, name))
            {
                e.valid = false;
                continue;
            }
            e.valid = true;
            memcpy(e.name, name, sizeof(name));
            e.used = used;
            e.size = size;
            e.dynamic = dynamic;
            if (used > e.peak_seen)
            {
                e.peak_seen = used;
            }
            pools++;
            pools_used += used;
            if (size >= 0x100000 && !dynamic)
            {
                const double pct = 100.0 * (double)used / (double)size;
                if (pct > fullest_pct)
                {
                    fullest_pct = pct;
                    memcpy(fullest, name, sizeof(name));
                }
                if (pct >= 99.9 && e.full_at == -1.0)
                {
                    // A pool full the first time it is seen took its memory whole at creation (alloc_all, single
                    // allocations): that is its design, not a pool running out.
                    e.full_at = e.seen ? t : -2.0;
                    if (e.seen)
                    {
                        char line[160];
                        _snprintf_s(line, sizeof(line), _TRUNCATE, "pool full: %s (%u of %u bytes)", name, used, size);
                        log_line(Events, line);
                    }
                }
            }
            e.seen = true;
        }
        ReleaseSRWLockExclusive(&Pool_lock);

        long long hk_in_use, hk_peak, hk_total;
        havok_stats(&hk_in_use, &hk_peak, &hk_total);
        if (hk_in_use > Peak_havok_inuse)
        {
            Peak_havok_inuse = hk_in_use;
        }
        if (hk_peak > Peak_havok_inuse)
        {
            Peak_havok_inuse = hk_peak;
        }
        Havok_total = hk_total;

        unsigned int far_count = 0;
#ifdef OPENRFG_X64
        far_count = et_far::get_tables().count;
#endif
        if (far_count > Peak_far)
        {
            Peak_far = far_count;
        }
        const int objects = World.get_num_objects();
        Max_objects = World.get_max_world_objects();
        if (objects > Peak_objects)
        {
            Peak_objects = objects;
        }
        // Level memory (layout block 6, RFG_Level): the avenue's wall in rfg.exe (two allocations fail there).
        const unsigned int level_used = Level_memmgr.space_used();
        Level_size = Level_memmgr.space_max();
        const unsigned int level_largest = Level_memmgr.largest_allocation_possible();
        if (level_used > Peak_level)
        {
            Peak_level = level_used;
        }
        const int cm_models = openrfg_num_collision_models();
        if (cm_models > Peak_cm_models)
        {
            Peak_cm_models = cm_models;
        }
        Cm_high_water = openrfg_collision_models_high_water();
        const int cm_side = openrfg_chunk_cm_side_entries();
        if (cm_side > Peak_cm_side)
        {
            Peak_cm_side = cm_side;
        }
        const int movers = openrfg_general_movers_in_use();
        if (movers > Peak_movers)
        {
            Peak_movers = movers;
        }
        int zones = 0;
        const int zone_handles = World.memtest_zone_handles_used(&zones);
        if (zone_handles > Peak_zone_handles)
        {
            Peak_zone_handles = zone_handles;
        }
        memtest_debris_counts dc = {};
        memtest_debris_numbers(&dc);
        if (dc.render_instances > Peak_render)
        {
            Peak_render = dc.render_instances;
        }
        Render_max = dc.render_instances_max;
        Debris = dc;

        if (Csv != nullptr)
        {
            fprintf(Csv, "%.1f,%u,%d,%.1f,%.1f,%.1f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%.1f,%s,%.1f,%.1f,%.1f,%.1f,%.1f,%lld,%.1f,%u,%u,%ld,%ld,%d,%d,%ld,%ld",
                    t, Frame, gameseq_get_state(), mb(pmc.PrivateUsage), mb(pmc.PeakPagefileUsage), mb(pmc.WorkingSetSize),
                    (double)va_used / (1024.0 * 1024.0 * 1024.0), mb(provider.used), mb(provider.peak), mb(provider.total),
                    mb(vram.used), mb(vram.total), mb(bank.used), mb(bank.total), pools, mb(pools_used), fullest, fullest_pct,
                    mb((unsigned long long)hk_in_use), mb((unsigned long long)hk_peak), mb((unsigned long long)hk_total),
                    mb((unsigned long long)Crt_bytes), Crt_blocks, mb((unsigned long long)Crt_peak), far_count,
#ifdef OPENRFG_X64
                    et_far::Max_entries,
#else
                    0u,
#endif
                    Strpool_fallbacks, Strpool_unregistered, objects, Max_objects, Failure_messages, Overlay_hits);
            // level memory (layout block 6)
            fprintf(Csv, ",%.1f,%.1f,%.1f,%.1f,%ld,%d,%d,%d,%d,%d,%d", mb(level_used), mb(Peak_level), mb(Level_size), mb(level_largest),
                    Level_failures, cm_models, openrfg_max_collision_models(), movers, openrfg_general_movers_capacity(), zone_handles, zones);
            // the debris (memtest_debris.cpp)
            fprintf(Csv, ",%d,%d,%d,%u,%u,%u,%u,%u,%u,%u,%.1f,%.1f,%u,%u,%u", dc.render_instances, dc.render_instances_max, dc.zone_free,
                    dc.kept, dc.gated, dc.fades_blocked, dc.vaporize_refused, dc.recycled, dc.stream_skips, dc.blasts,
                    mb((unsigned long long)Havok_system_used), mb((unsigned long long)Havok_solver_used), dc.island_trims, dc.bodies_trimmed,
                    dc.havok_low_memory);
            // the grow-on-demand memory (src/scaling)
            fprintf(Csv, ",%.1f,%.1f,%u\n", mb(scaling_committed_total()), mb((unsigned long long)havok_memory_heap_page_bytes(nullptr)),
                    scaling_fault_count());
            fflush(Csv);
        }
    }

    void write_pools()
    {
        if (Pools_csv == nullptr)
        {
            return;
        }
        const double t = now_s();
        AcquireSRWLockExclusive(&Pool_lock);
        for (int i = 0; i < Pool_count; i++)
        {
            const pool_entry& e = Pools[i];
            if (e.valid)
            {
                fprintf(Pools_csv, "%.1f,%s,%u,%u,%u,%d\n", t, e.name, e.used, e.size, e.peak_seen, e.dynamic ? 1 : 0);
            }
        }
        ReleaseSRWLockExclusive(&Pool_lock);
        fflush(Pools_csv);
    }

    int compare_pool_fill(const void* a, const void* b)
    {
        const pool_entry* x = *(const pool_entry* const*)a;
        const pool_entry* y = *(const pool_entry* const*)b;
        const double fx = x->size ? (double)x->peak_seen / x->size : 0.0;
        const double fy = y->size ? (double)y->peak_seen / y->size : 0.0;
        return fx < fy ? 1 : fx > fy ? -1 : 0;
    }

    // ---- the grow-on-demand memory (src/scaling)
    unsigned long long scaling_committed_total()
    {
        unsigned long long total = 0;
        scaling_region_info info;
        for (int i = 0; scaling_region(i, &info); i++)
        {
            total += info.committed;
        }
        return total;
    }

    // ---- far-offset census: each new entry of the far table (endian_type.inl), by the memory area of the field that
    // needed it and of its target: a scaling region, a layout block, the program's image, or another system allocation.
    struct far_area
    {
        int kind; // 0 none, 1 scaling region, 2 layout block, 3 a module's image, 4 another allocation
        unsigned long long id;
    };
    struct far_pair
    {
        far_area field;
        far_area target;
        unsigned int count;
        const void* first_field;
        const void* first_target;
    };
    far_pair Far_pairs[64];
    int Far_pair_count;
    unsigned int Far_unsorted;

    far_area far_area_of(const void* p)
    {
        far_area a = {0, 0};
        if (p == nullptr)
        {
            return a;
        }
        scaling_region_info info;
        for (int i = 0; scaling_region(i, &info); i++)
        {
            if ((const char*)p >= (const char*)info.base && (const char*)p < (const char*)info.base + info.reserved)
            {
                a.kind = 1;
                a.id = (unsigned long long)i;
                return a;
            }
        }
        const void* start = nullptr;
        unsigned long long size = 0;
        const char* name = nullptr;
        for (unsigned int i = 0; keen::memory::memtestLayoutBlock(i, &start, &size, &name); i++)
        {
            if (start != nullptr && (const char*)p >= (const char*)start && (const char*)p < (const char*)start + size)
            {
                a.kind = 2;
                a.id = i;
                return a;
            }
        }
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
        {
            return a;
        }
        a.kind = mbi.Type == MEM_IMAGE ? 3 : 4;
        a.id = (unsigned long long)mbi.AllocationBase;
        return a;
    }

    // Under the far table's lock.
    // The far entries a level takes can go when it is gone (memtest_level_load_start) if their field or their target
    // lies in memory its unload gives back: the "big containers" and "level memory" regions. Entries taken since the
    // level's load began with neither (lasting ones) keep the level's entries.
    unsigned int Far_lasting_since_mark = 0;

    bool transient_area(const far_area& a)
    {
        static int Big = -2;
        static int Level = -2;
        if (Big == -2)
        {
            Big = -1;
            Level = -1;
            scaling_region_info info;
            for (int i = 0; scaling_region(i, &info); i++)
            {
                if (info.name != nullptr && strcmp(info.name, "big containers") == 0)
                {
                    Big = i;
                }
                else if (info.name != nullptr && strcmp(info.name, "level memory") == 0)
                {
                    Level = i;
                }
            }
        }
        return a.kind == 1 && ((int)a.id == Big || (int)a.id == Level);
    }

    void note_far_entry(const void* field, const void* target)
    {
        const far_area f = far_area_of(field);
        const far_area t = far_area_of(target);
        if (!transient_area(f) && !transient_area(t))
        {
            Far_lasting_since_mark++;
        }
        for (int i = 0; i < Far_pair_count; i++)
        {
            far_pair& p = Far_pairs[i];
            if (p.field.kind == f.kind && p.field.id == f.id && p.target.kind == t.kind && p.target.id == t.id)
            {
                p.count++;
                return;
            }
        }
        if (Far_pair_count < (int)(sizeof(Far_pairs) / sizeof(Far_pairs[0])))
        {
            far_pair& p = Far_pairs[Far_pair_count++];
            p.field = f;
            p.target = t;
            p.count = 1;
            p.first_field = field;
            p.first_target = target;
            return;
        }
        Far_unsorted++;
    }

    const char* far_area_label(const far_area& a, char* out, size_t size)
    {
        switch (a.kind)
        {
        case 1:
        {
            scaling_region_info info;
            if (scaling_region((int)a.id, &info))
            {
                char label[160];
                const char* name = strstr(info.name, "object_pool<") != nullptr ? pool_label(info.name, label, sizeof(label)) : info.name;
                snprintf(out, size, "region \"%s\" (%.0f MB at %p)", name, mb(info.reserved), info.base);
                return out;
            }
            break;
        }
        case 2:
        {
            const void* start = nullptr;
            unsigned long long bytes = 0;
            const char* name = nullptr;
            if (keen::memory::memtestLayoutBlock((unsigned int)a.id, &start, &bytes, &name))
            {
                snprintf(out, size, "layout block %u %s (%.0f MB at %p)", (unsigned int)a.id, name != nullptr ? name : "", mb(bytes), start);
                return out;
            }
            break;
        }
        case 3:
        {
            char path[MAX_PATH] = "";
            GetModuleFileNameA((HMODULE)a.id, path, sizeof(path));
            const char* file = strrchr(path, '\\');
            snprintf(out, size, "the image of %s (%p)", file != nullptr ? file + 1 : path, (const void*)a.id);
            return out;
        }
        case 4:
        {
            // The allocation's size: its regions while they belong to it.
            unsigned long long bytes = 0;
            const char* p = (const char*)a.id;
            MEMORY_BASIC_INFORMATION mbi;
            while (VirtualQuery(p + bytes, &mbi, sizeof(mbi)) != 0 && mbi.AllocationBase == (void*)a.id && mbi.RegionSize > 0)
            {
                bytes += mbi.RegionSize;
            }
            snprintf(out, size, "an allocation of %.1f MB at %p", mb(bytes), (const void*)a.id);
            return out;
        }
        default:
            break;
        }
        snprintf(out, size, "(nowhere known)");
        return out;
    }

    void write_far_census(FILE* f)
    {
        if (Far_pair_count == 0)
        {
            return;
        }
        // Largest first.
        far_pair sorted[64];
        const int n = Far_pair_count;
        memcpy(sorted, Far_pairs, sizeof(far_pair) * (size_t)n);
        for (int i = 1; i < n; i++)
        {
            for (int j = i; j > 0 && sorted[j].count > sorted[j - 1].count; j--)
            {
                const far_pair swap = sorted[j];
                sorted[j] = sorted[j - 1];
                sorted[j - 1] = swap;
            }
        }
        fprintf(f, "  far entries by where the field and its target lie (the first of each: field, target):\n");
        char a[200];
        char b[200];
        for (int i = 0; i < n; i++)
        {
            fprintf(f, "  %10u  %s\n              -> %s   [%p -> %p]\n", sorted[i].count, far_area_label(sorted[i].field, a, sizeof(a)),
                    far_area_label(sorted[i].target, b, sizeof(b)), sorted[i].first_field, sorted[i].first_target);
        }
        if (Far_unsorted > 0)
        {
            fprintf(f, "  %10u  in other pairs\n", Far_unsorted);
        }
    }

    void write_scaling_summary(FILE* f)
    {
        int pages = 0;
        const long long havok = havok_memory_heap_page_bytes(&pages);
        fprintf(f, "grow-on-demand memory (%s)\n", scaling_enabled() ? "on" : "off: OPENRFG_SCALING=0");
        if (havok > 0 || pages > 0)
        {
            fprintf(f, "  %-40s %10.1f MB in %d pages (at most 2 GB)\n", "Havok heap", mb((unsigned long long)havok), pages);
        }
        scaling_region_info info;
        char label[160];
        for (int i = 0; scaling_region(i, &info); i++)
        {
            if (info.committed >= (1u << 20) && strcmp(info.name, "Havok heap") != 0) // Havok's: the page line above
            {
                const char* name = strstr(info.name, "object_pool<") != nullptr ? pool_label(info.name, label, sizeof(label)) : info.name;
                fprintf(f, "  %-40s %10.1f MB committed of %.1f MB reserved\n", name, mb(info.committed), mb(info.reserved));
            }
        }
        fprintf(f, "  %d regions, %u granules committed on first touch, %u commits refused by the system\n", scaling_region_count(),
                scaling_fault_count(), scaling_commit_failures());
        scaling_pool_info pools[512];
        const int grown = scaling_grown_pools(pools, 512);
        for (int i = 0; i < grown; i++)
        {
            fprintf(f, "  pool grew: %-60s %u -> %u objects\n", pool_label(pools[i].name, label, sizeof(label)), pools[i].initial,
                    pools[i].objects);
        }
        const int exhausted = scaling_exhausted_pools(pools, 512);
        for (int i = 0; i < exhausted; i++)
        {
            fprintf(f, "  pool FULL: %-60s at %u objects\n", pool_label(pools[i].name, label, sizeof(label)), pools[i].objects);
        }
        fprintf(f, "\n");
    }

    // Growth events: a region (or Havok's heap) passing the next 64 MB step, or doubling below 64 MB.
    unsigned long long Scaling_reported[4096];
    long long Havok_pages_reported;

    bool grew(unsigned long long now, unsigned long long& reported)
    {
        const unsigned long long step = 64ull << 20;
        const bool passed = reported < step ? now >= 2 * reported && now >= (1ull << 20) && now > reported : now / step > reported / step;
        if (passed)
        {
            reported = now;
        }
        return passed;
    }

    // "object_pool<struct glass_shard,750,1>" out of a pool's __FUNCSIG__.
    const char* pool_label(const char* name, char* out, size_t size)
    {
        if (name == nullptr)
        {
            return "(a pool without a name)";
        }
        const char* p = strstr(name, "object_pool<");
        const char* e = p != nullptr ? strstr(p, ">::") : nullptr;
        if (p == nullptr || e == nullptr)
        {
            return name;
        }
        const size_t n = (size_t)(e - p + 1) < size - 1 ? (size_t)(e - p + 1) : size - 1;
        memcpy(out, p, n);
        out[n] = '\0';
        return out;
    }

    int Exhausted_reported;
    unsigned int Grown_reported[512];
    const char* Grown_names[512];
    int Grown_names_count;

    void report_pools()
    {
        scaling_pool_info pools[512];
        char label[160];
        const int exhausted = scaling_exhausted_pools(pools, 512);
        for (; Exhausted_reported < exhausted; Exhausted_reported++)
        {
            memtest_event("pool: %s is full (%u objects)", pool_label(pools[Exhausted_reported].name, label, sizeof(label)),
                          pools[Exhausted_reported].objects);
        }
        const int grown = scaling_grown_pools(pools, 512);
        for (int i = 0; i < grown; i++)
        {
            int k = 0;
            while (k < Grown_names_count && Grown_names[k] != pools[i].name)
            {
                k++;
            }
            if (k == Grown_names_count)
            {
                if (Grown_names_count >= 512)
                {
                    continue;
                }
                Grown_names[Grown_names_count] = pools[i].name;
                Grown_reported[Grown_names_count] = pools[i].initial;
                Grown_names_count++;
            }
            if (pools[i].objects >= 2 * Grown_reported[k])
            {
                memtest_event("scaling: pool %s grew %u -> %u objects", pool_label(pools[i].name, label, sizeof(label)), pools[i].initial,
                              pools[i].objects);
                Grown_reported[k] = pools[i].objects;
            }
        }
    }

    void report_scaling()
    {
        report_pools();
        scaling_region_info info;
        for (int i = 0; i < 4096 && scaling_region(i, &info); i++)
        {
            if (grew(info.committed, Scaling_reported[i]))
            {
                memtest_event("scaling: %s grew to %.1f MB committed (of %.1f MB reserved)", info.name, mb(info.committed), mb(info.reserved));
            }
        }
        int pages = 0;
        unsigned long long havok = (unsigned long long)havok_memory_heap_page_bytes(&pages);
        unsigned long long reported = (unsigned long long)Havok_pages_reported;
        if (grew(havok, reported))
        {
            Havok_pages_reported = (long long)reported;
            memtest_event("scaling: Havok's heap grew to %.1f MB (%d page%s)", mb(havok), pages, pages == 1 ? "" : "s");
        }
    }

    void write_summary(const char* reason)
    {
        FILE* f = open_out("memtest_summary.txt", "w");
        if (f == nullptr)
        {
            return;
        }
        fprintf(f, "OpenRFG64 memtest summary (%s) at %.1f s, frame %u\n\n", reason, now_s(), Frame);
        fprintf(f, "process\n");
        fprintf(f, "  peak private bytes      %10.1f MB%s\n", mb(Peak_private), Peak_private > 4ull << 30 ? "  (more than a 32-bit process can address)" : "");
        fprintf(f, "  passed 4 GB at          %10s\n", Over_4gb_at < 0 ? "never" : "");
        if (Over_4gb_at >= 0)
        {
            fprintf(f, "                          %10.1f s\n", Over_4gb_at);
        }
        fprintf(f, "  peak working set        %10.1f MB\n\n", mb(Peak_ws));
        fprintf(f, "game budgets\n");
        fprintf(f, "  RFG_MempoolProvider     %10.1f of %.1f MB at the peak (every game pool is carved from it)\n", mb(Peak_provider), mb(Provider_total));
        fprintf(f, "  Havok heap              %10.1f of %.1f MB at the peak%s\n", mb((unsigned long long)Peak_havok_inuse), mb((unsigned long long)Havok_total),
                Havok_oom ? "  ** OUT OF MEMORY **" : "");
        fprintf(f, "  C runtime heap (x64)    %10.1f MB at the peak (dlmalloc's replacement: unbounded)\n", mb((unsigned long long)Crt_peak));
        fprintf(f, "  level memory (block 6)  %10.1f of %.1f MB at the peak, %ld failed allocations%s%s\n", mb(Peak_level), mb(Level_size),
                Level_failures, Level_first_failure[0] ? "; the first: " : "", Level_first_failure);
        fprintf(f, "  world objects           %10d of %d at the peak (indices are 16-bit: at most 65,535)\n", Peak_objects, Max_objects);
        fprintf(f, "  collision model table   %10d of %d at the peak; indices handed out up to %u, %d at most kept past the chunks' 16 bits\n",
                Peak_cm_models, openrfg_max_collision_models(), Cm_high_water, Peak_cm_side);
        unsigned int first_miss = 0;
        const unsigned int cm_misses = openrfg_cm_misses(&first_miss);
        if (cm_misses != 0)
        {
            fprintf(f, "  ** %u Havok bodies asked for a collision model that is not there (the first: index %u) **\n", cm_misses, first_miss);
        }
        fprintf(f, "  debris (general movers) %10d of %d at the peak (the pool links its elements with 16-bit indices)\n", Peak_movers,
                openrfg_general_movers_capacity());
        fprintf(f, "  zone object handles     %10d of 7280 at the peak in the fullest zone (a fixed bitmap per zone)\n", Peak_zone_handles);
        fprintf(f, "  destroyable render inst.%10d of %d at the peak%s\n\n", Peak_render, Render_max,
                scaling_enabled() ? " (they grow when they run out)" : "");
        fprintf(f, "debris (rfgphys's keep_debris in the source: %s)\n", Debris.keep ? "on" : "off");
        fprintf(f, "  kept from the game's deletion at shatter %u, deleted by the zone budget %u, fades refused %u, vaporizing refused %u\n",
                Debris.kept, Debris.gated, Debris.fades_blocked, Debris.vaporize_refused);
        fprintf(f, "  recycled by the zone budget %u, streaming met a parent that is gone %u, demolition vehicle bombs %u\n",
                Debris.recycled, Debris.stream_skips, Debris.blasts);
        fprintf(f, "  compiles cancelled with the request pool empty %u\n", Debris.recompiles_cancelled);
        fprintf(f, "  Havok: islands trimmed by the watchdog %u (%u bodies destroyed: solver memory), low-memory calls %u\n\n",
                Debris.island_trims, Debris.bodies_trimmed, Debris.havok_low_memory);
        write_scaling_summary(f);
        fprintf(f, "32-bit structures\n");
#ifdef OPENRFG_X64
        fprintf(f, "  far offset table        %10u of %u entries at the peak%s\n", Peak_far, et_far::Max_entries,
                Peak_far >= et_far::Max_entries ? "  ** FULL: offsets are being truncated **" : "");
        write_far_census(f);
#endif
        fprintf(f, "  string table fallbacks  %10ld records outside their arena, %ld tables without an arena\n\n", Strpool_fallbacks, Strpool_unregistered);
        fprintf(f, "failure messages from the game: %ld%s%s\n\n", Failure_messages, Last_failure[0] ? ", the last: " : "", Last_failure);
        // The pools by how full they got.
        pool_entry* sorted[Max_pools];
        int n = 0;
        AcquireSRWLockExclusive(&Pool_lock);
        for (int i = 0; i < Pool_count; i++)
        {
            if (Pools[i].valid && Pools[i].size >= 0x10000 && !Pools[i].dynamic)
            {
                sorted[n++] = &Pools[i];
            }
        }
        qsort(sorted, (size_t)n, sizeof(sorted[0]), compare_pool_fill);
        fprintf(f, "game pools by peak fill (%d pools of 64 KB or more; registry %d%s)\n", n, Pool_count, Pool_registry_overflow ? ", overflowed" : "");
        fprintf(f, "  %-32s %12s %12s %7s  %s\n", "pool", "peak", "size", "fill", "full at");
        for (int i = 0; i < n && i < 40; i++)
        {
            const pool_entry& e = *sorted[i];
            char full[32] = "";
            if (e.full_at >= 0)
            {
                _snprintf_s(full, sizeof(full), _TRUNCATE, "%.1f s", e.full_at);
            }
            else if (e.full_at == -2.0)
            {
                strcpy_s(full, "whole from the start");
            }
            fprintf(f, "  %-32s %12u %12u %6.1f%%  %s\n", e.name, e.peak_seen, e.size, e.size ? 100.0 * e.peak_seen / e.size : 0.0, full);
        }
        ReleaseSRWLockExclusive(&Pool_lock);
        fclose(f);
    }

    // ---- the Wrecking Crew start (memtest_frame): the menu loaded for a second -> options -> match begin.
    int hex_value(char c)
    {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    }

    int env_int(const char* name, int fallback)
    {
        char value[32] = "";
        const DWORD n = GetEnvironmentVariableA(name, value, sizeof(value));
        return n > 0 && n < sizeof(value) ? atoi(value) : fallback;
    }

    // The "big containers" region's committed memory (a big map's World container) and the far table's entries:
    // logged at each match start and round end (Memtest.WcRepeat), to see what an unload gives back.
    double big_committed_mb()
    {
        scaling_region_info info;
        for (int i = 0; scaling_region(i, &info); i++)
        {
            if (info.name != nullptr && strcmp(info.name, "big containers") == 0)
            {
                return (double)info.committed / 1048576.0;
            }
        }
        return 0.0;
    }

    unsigned int far_entries()
    {
#ifdef OPENRFG_X64
        return et_far::get_tables().count;
#else
        return 0;
#endif
    }

    // ---- Memtest.HavokCensus=1 (OPENRFG_MEMTEST_HAVOKCENSUS): Havok's live counted objects (hkReferencedObjectTracker:
    // told of every counted object's construction and of every destruction), by class, before each Wrecking Crew load
    // and at each round's end: what a level's unload leaves in Havok's heap. The set lives on the C runtime heap.
    SRWLOCK Hk_lock = SRWLOCK_INIT;
    std::unordered_set<const hkReferencedObject*>* Hk_live = nullptr;
    unsigned long long Hk_constructed = 0;
    unsigned long long Hk_destructed = 0;

    void hk_track(const hkReferencedObject* object, bool alive)
    {
        AcquireSRWLockExclusive(&Hk_lock);
        if (alive)
        {
            Hk_live->insert(object);
            Hk_constructed++;
        }
        else if (Hk_live->erase(object) != 0)
        {
            Hk_destructed++;
        }
        ReleaseSRWLockExclusive(&Hk_lock);
    }

    // Havok's heap by where its memory is: the system allocator (hkLargeBlockAllocator: the free lists' blocks and every
    // allocation past MAX_FREELIST_SIZE), each free list's blocks, live and free elements (havok_memory_init's setup).
    void hk_heap_breakdown(const char* when)
    {
        hkFreeListMemorySystem* system = static_cast<hkFreeListMemorySystem*>(hkMemorySystem::getInstancePtr());
        if (system == nullptr || system->m_heapAllocator == nullptr || system->m_systemAllocator == nullptr)
        {
            return;
        }
        hkFreeListAllocator* heap = static_cast<hkFreeListAllocator*>(system->m_heapAllocator);
        hkMemoryAllocator::MemoryStatistics sys;
        system->m_systemAllocator->getMemoryStatistics(sys);
        long long blocks = 0;
        long long live = 0;
        long long free_elements = 0;
        const double mb = 1048576.0;
        for (int i = 0; i < heap->m_numFreeLists + 1; i++)
        {
            const hkFreeList* list = i == heap->m_numFreeLists ? &heap->m_blockAllocator.m_freeList : heap->m_freeLists[i];
            if (list == nullptr)
            {
                continue;
            }
            hkMemoryAllocator::MemoryStatistics s;
            list->getMemoryStatistics(s);
            blocks += s.m_allocated;
            live += s.m_inUse;
            free_elements += s.m_available;
            if (s.m_allocated >= 4 * 1048576)
            {
                memtest_event("  free list of %u-byte elements: %.1f MB of blocks, %.1f MB live, %.1f MB free", list->m_elementSize,
                              (double)s.m_allocated / mb, (double)s.m_inUse / mb, (double)s.m_available / mb);
            }
        }
        memtest_event("havok heap (%s): system allocator %.1f MB in use; free lists %.1f MB of blocks (%.1f MB live, %.1f MB free); "
                      "allocations past the free lists %.1f MB",
                      when, (double)sys.m_inUse / mb, (double)blocks / mb, (double)live / mb, (double)free_elements / mb,
                      (double)(sys.m_inUse - blocks) / mb);
    }

    // ---- Memtest.HkCheck=N (OPENRFG_MEMTEST_HKCHECK): every N seconds, at the start of a level's gameplay exit and
    // before the garbage collections of its Havok shutdown and of the next level's load, every free list of Havok's
    // heap allocator is walked under its lock. Each free element must be an element of one of that list's active blocks, at an element's start, outside
    // the current block's unused top, and the walk must end within the list's count. A campaign session crashed in the
    // shutdown's garbage collection, sorting a free list (hkFreeList::findGarbage under gamestate_gp_exit, 2026-11-22):
    // a free element's link led nowhere. The first bad link of a list is described once: what it leads to (another
    // list's element: a free with the wrong size; nothing; a cycle: a double free), and the bytes of the element that
    // holds it, i.e. what was written into a free element (an object's vtable names its class).
    int Hk_check_s = 0;
    unsigned long long Last_hk_check_ms = 0;
    unsigned int Hk_checks = 0;
    unsigned int Hk_bad_checks = 0;
    volatile long Hk_wrong_frees = 0; // frees whose block is another list's (hk_free_hook)
    bool Hk_list_reported[hkFreeListAllocator::MAX_FREELISTS + 1] = {};

    struct hk_range
    {
        uintptr_t begin;
        uintptr_t end;
        int list;
        bool free_block; // a block of the list's m_freeBlocks (all of it free, none of it on m_free)
    };

    bool hk_read_bytes(const void* p, void* out, size_t n)
    {
        __try
        {
            memcpy(out, p, n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The range holding p (the ranges sorted and apart), or -1.
    int hk_find_range(const std::vector<hk_range>& ranges, uintptr_t p)
    {
        size_t lo = 0;
        size_t hi = ranges.size();
        while (lo < hi)
        {
            const size_t mid = (lo + hi) / 2;
            if (ranges[mid].end <= p)
            {
                lo = mid + 1;
            }
            else
            {
                hi = mid;
            }
        }
        return lo < ranges.size() && ranges[lo].begin <= p ? (int)lo : -1;
    }

    // A block list's element ranges. False if a block header could not be read or the list does not end.
    bool hk_add_blocks(std::vector<hk_range>& ranges, const hkFreeList* list, int index, const hkFreeList::Block* b, bool free_blocks)
    {
        for (int n = 0; b != nullptr; n++)
        {
            hkFreeList::Block blk;
            if (n >= 50000000 || !hk_read_bytes(b, &blk, sizeof(blk)))
            {
                return false;
            }
            const uintptr_t begin = (uintptr_t)blk.m_elements;
            ranges.push_back({begin, begin + (uintptr_t)blk.m_numElements * list->m_elementSize, index, free_blocks});
            b = blk.m_next;
        }
        return true;
    }

    // The bytes of a free element, as 8-byte words; those pointing into the program named by their offset in it.
    void hk_describe_bytes(uintptr_t at, unsigned int size, char* out, size_t out_size)
    {
        size_t len = 0;
        out[0] = 0;
        const unsigned int words = (size < 64 ? size : 64) / 8;
        for (unsigned int w = 0; w < words && len + 40 < out_size; w++)
        {
            unsigned long long v = 0;
            if (!hk_read_bytes((const void*)(at + w * 8), &v, 8))
            {
                len += snprintf(out + len, out_size - len, " (unreadable)");
                break;
            }
            if (v >= Image_lo && v < Image_hi)
            {
                len += snprintf(out + len, out_size - len, " exe+0x%llX", v - Image_lo);
            }
            else
            {
                len += snprintf(out + len, out_size - len, " %016llX", v);
            }
        }
    }

    unsigned int hk_list_count(const hkFreeListAllocator* heap)
    {
        return (unsigned int)heap->m_numFreeLists + 1;
    }

    const hkFreeList* hk_list_at(const hkFreeListAllocator* heap, unsigned int i)
    {
        return i == (unsigned int)heap->m_numFreeLists ? &heap->m_blockAllocator.m_freeList : heap->m_freeLists[i];
    }

    // How many of a free list's elements lie in other lists' blocks, written when the count changed since the last check.
    unsigned int Hk_foreign_last[hkFreeListAllocator::MAX_FREELISTS + 1] = {};

    void hk_report_foreign(const char* when, const hkFreeList* list, unsigned int i, unsigned int foreign, unsigned int at_start,
                           const unsigned int* sizes, const unsigned int* counts)
    {
        if (foreign == Hk_foreign_last[i])
        {
            return;
        }
        Hk_foreign_last[i] = foreign;
        char by_size[160] = "";
        size_t len = 0;
        for (int k = 0; k < 8 && sizes[k] != 0 && len + 24 < sizeof(by_size); k++)
        {
            len += (size_t)snprintf(by_size + len, sizeof(by_size) - len, " %u in %u-byte blocks", counts[k], sizes[k]);
        }
        memtest_event("hkcheck (%s): the %u-byte free list holds %u elements of other lists' blocks (%u of them at an element's start):%s",
                      when, list->m_elementSize, foreign, at_start, foreign != 0 ? by_size : " none now");
    }

    // Walks one free list. Returns false if a link was bad (the first bad link of a list described once per session).
    // Elements of another list's blocks (a free with the wrong size) are counted and the walk goes on through them
    // (their link is the list's own), the first three of a check described with their own bytes; the walk stops at a
    // link it cannot follow.
    bool hk_walk_free_list(const hkFreeListAllocator* heap, unsigned int i, const std::vector<hk_range>& ranges, const char* when,
                           unsigned long long& walked)
    {
        const hkFreeList* list = hk_list_at(heap, i);
        const unsigned long long limit = (unsigned long long)list->m_numFreeElements + 1;
        uintptr_t prev = 0; // the element whose link leads here (0: the list's head)
        uintptr_t e = (uintptr_t)list->m_free;
        unsigned long long steps = 0;
        unsigned int foreign = 0;      // elements of other lists' blocks
        unsigned int foreign_at_start = 0; // of them, at the start of one of that list's elements
        unsigned int foreign_by_size[8] = {};
        unsigned int foreign_sizes[8] = {};
        while (e != 0)
        {
            const char* problem = nullptr;
            unsigned int other_size = 0;
            steps++;
            if (steps > limit)
            {
                problem = "one more than the list counts free (a cycle: an element freed twice)";
            }
            else if (list->m_align != 0 && (e & (list->m_align - 1)) != 0)
            {
                problem = "misaligned";
            }
            else
            {
                const int r = hk_find_range(ranges, e);
                if (r < 0)
                {
                    problem = "no free list's element (outside every block)";
                }
                else if ((unsigned int)ranges[r].list != i)
                {
                    other_size = hk_list_at(heap, (unsigned int)ranges[r].list)->m_elementSize;
                    const bool at_start = other_size != 0 && (e - ranges[r].begin) % other_size == 0;
                    foreign++;
                    foreign_at_start += at_start ? 1 : 0;
                    for (int k = 0; k < 8; k++)
                    {
                        if (foreign_sizes[k] == other_size || foreign_sizes[k] == 0)
                        {
                            foreign_sizes[k] = other_size;
                            foreign_by_size[k]++;
                            break;
                        }
                    }
                    if (foreign <= 3)
                    {
                        char own[640];
                        hk_describe_bytes(e, other_size < 64 ? other_size : 64, own, sizeof(own));
                        memtest_event("hkcheck (%s): the %u-byte free list's element %llu, 0x%llX, lies in a block of the %u-byte list (%s, "
                                      "%llu bytes into its block%s); its bytes:%s",
                                      when, list->m_elementSize, steps, (unsigned long long)e, other_size,
                                      at_start ? "at the start of one of its elements" : "inside one of its elements",
                                      (unsigned long long)(e - ranges[r].begin), ranges[r].free_block ? ", a free block" : "", own);
                    }
                }
                else if (ranges[r].free_block)
                {
                    problem = "inside one of the list's free blocks";
                }
                else if ((e - ranges[r].begin) % list->m_elementSize != 0)
                {
                    problem = "not at an element's start";
                }
                else if (e >= (uintptr_t)list->m_top && e < (uintptr_t)list->m_blockEnd)
                {
                    problem = "in the current block's unused top (handed out twice)";
                }
            }
            uintptr_t next = 0;
            if (problem == nullptr && !hk_read_bytes((const void*)e, &next, sizeof(next)))
            {
                problem = "unreadable";
            }
            if (problem != nullptr)
            {
                if (!Hk_list_reported[i])
                {
                    Hk_list_reported[i] = true;
                    char bytes[640];
                    hk_describe_bytes(prev != 0 ? prev : (uintptr_t)&list->m_free, prev != 0 ? list->m_elementSize : 8, bytes,
                                      sizeof(bytes));
                    memtest_event("hkcheck (%s): the free list of %u-byte elements is broken at its element %llu of %u counted free: "
                                  "0x%llX is %s. The link to it is in %s 0x%llX, whose bytes are:%s",
                                  when, list->m_elementSize, steps, list->m_numFreeElements, (unsigned long long)e, problem,
                                  prev != 0 ? "the free element" : "the list's head", (unsigned long long)(prev != 0 ? prev : (uintptr_t)&list->m_free),
                                  bytes);
                }
                walked += steps;
                hk_report_foreign(when, list, i, foreign, foreign_at_start, foreign_sizes, foreign_by_size);
                return false;
            }
            prev = e;
            e = next;
        }
        walked += steps;
        hk_report_foreign(when, list, i, foreign, foreign_at_start, foreign_sizes, foreign_by_size);
        return foreign == 0;
    }

    void hk_check_free_lists(const char* when, bool always_log)
    {
        hkMemorySystem* instance = hkMemorySystem::getInstancePtr();
        hkFreeListMemorySystem* system = static_cast<hkFreeListMemorySystem*>(instance);
        if (system == nullptr || system->m_heapAllocator == nullptr)
        {
            return;
        }
        hkFreeListAllocator* heap = static_cast<hkFreeListAllocator*>(system->m_heapAllocator);
        const unsigned long long t0 = GetTickCount64();
        std::vector<hk_range> ranges;
        ranges.reserve(1 << 16);
        unsigned int bad = 0;
        unsigned int unreadable_blocks = 0;
        unsigned long long walked = 0;
        heap->m_criticalSection.enter();
        const unsigned int lists = hk_list_count(heap);
        if (Hk_checks == 0)
        {
            char sizes[400] = "";
            size_t len = 0;
            for (unsigned int i = 0; i < lists && len + 16 < sizeof(sizes); i++)
            {
                const hkFreeList* list = hk_list_at(heap, i);
                len += (size_t)snprintf(sizes + len, sizeof(sizes) - len, " %u%s", list != nullptr ? list->m_elementSize : 0,
                                        i == lists - 1 ? " (the block headers')" : "");
            }
            memtest_event("hkcheck: Havok's heap has %u free lists, their element sizes:%s", lists, sizes);
        }
        for (unsigned int i = 0; i < lists; i++)
        {
            const hkFreeList* list = hk_list_at(heap, i);
            if (list == nullptr)
            {
                continue;
            }
            if (!hk_add_blocks(ranges, list, (int)i, list->m_activeBlocks, false) ||
                !hk_add_blocks(ranges, list, (int)i, list->m_freeBlocks, true))
            {
                unreadable_blocks++;
                if (!Hk_list_reported[i])
                {
                    Hk_list_reported[i] = true;
                    memtest_event("hkcheck (%s): the block list of the %u-byte free list is broken (a block header unreadable, "
                                  "or no end)", when, list->m_elementSize);
                }
            }
        }
        std::sort(ranges.begin(), ranges.end(), [](const hk_range& a, const hk_range& b) { return a.begin < b.begin; });
        // Blocks of two lists (or two blocks of one) in the same memory: the large block allocator handed it out twice.
        static int overlaps_logged = 0;
        unsigned int overlaps = 0;
        for (size_t k = 1; k < ranges.size(); k++)
        {
            if (ranges[k].begin < ranges[k - 1].end)
            {
                overlaps++;
                if (overlaps_logged++ < 5)
                {
                    memtest_event("hkcheck (%s): blocks overlap: [0x%llX, 0x%llX) of the %u-byte list%s and [0x%llX, 0x%llX) of the %u-byte "
                                  "list%s",
                                  when, (unsigned long long)ranges[k - 1].begin, (unsigned long long)ranges[k - 1].end,
                                  hk_list_at(heap, (unsigned int)ranges[k - 1].list)->m_elementSize, ranges[k - 1].free_block ? " (free)" : "",
                                  (unsigned long long)ranges[k].begin, (unsigned long long)ranges[k].end,
                                  hk_list_at(heap, (unsigned int)ranges[k].list)->m_elementSize, ranges[k].free_block ? " (free)" : "");
                }
            }
        }
        bad += overlaps != 0 ? 1 : 0;
        for (unsigned int i = 0; i < lists; i++)
        {
            if (hk_list_at(heap, i) != nullptr && !hk_walk_free_list(heap, i, ranges, when, walked))
            {
                bad++;
            }
        }
        heap->m_criticalSection.leave();
        Hk_checks++;
        if (bad != 0 || unreadable_blocks != 0)
        {
            Hk_bad_checks++;
        }
        if (always_log || bad != 0 || unreadable_blocks != 0 || Hk_checks <= 3 || Hk_checks % 30 == 0)
        {
            memtest_event("hkcheck (%s): %u free lists, %zu blocks, %llu free elements walked in %llu ms: %s (checks %u, %u of them "
                          "with a broken list; wrong-size frees so far %ld)",
                          when, lists, ranges.size(), walked, GetTickCount64() - t0,
                          bad == 0 && unreadable_blocks == 0 ? "all good" : "BROKEN", Hk_checks, Hk_bad_checks, Hk_wrong_frees);
        }
    }

    // ---- Memtest.HkCheck: every block freed into Havok's heap (hkMemtestFreeHook) is checked against the free list
    // whose block holds it. The first check of a campaign load found the 16-byte list holding an element of a 48-byte
    // block 6 s after the start: a block freed with a smaller size than it was allocated with (the thread memory files a
    // size of 0 under 16 bytes). When that block's list later gives the block back, the 16-byte list points into memory
    // that is used again: the free list sort of a garbage collection then follows garbage (the crash on quitting). Each
    // free whose block is another list's is written once per call stack, with the stack as offsets into the program
    // (tools/re/pdb_addr.py names them). The blocks are indexed, and the index is rebuilt whenever a list's element
    // total changed (it gained or lost a block).
    SRWLOCK Hk_index_lock = SRWLOCK_INIT;
    std::vector<hk_range>* Hk_index = nullptr; // the heap's blocks, sorted (CRT heap: no Havok memory)
    unsigned long long Hk_index_signature = 0;
    SRWLOCK Hk_stacks_lock = SRWLOCK_INIT;
    unsigned long long Hk_stacks_seen[256] = {};
    int Hk_stacks_count = 0;

    // Changes when a list gains or loses a block (its element total), read without the lock: a hint only.
    unsigned long long hk_blocks_signature(const hkFreeListAllocator* heap)
    {
        unsigned long long sig = 0;
        const unsigned int lists = hk_list_count(heap);
        for (unsigned int i = 0; i < lists; i++)
        {
            const hkFreeList* list = hk_list_at(heap, i);
            if (list != nullptr)
            {
                sig = sig * 1000003ull + list->m_totalNumElements;
            }
        }
        return sig;
    }

    void hk_rebuild_index(hkFreeListAllocator* heap)
    {
        std::vector<hk_range> fresh;
        fresh.reserve(Hk_index != nullptr ? Hk_index->size() + 256 : 4096);
        heap->m_criticalSection.enter();
        const unsigned int lists = hk_list_count(heap);
        for (unsigned int i = 0; i < lists; i++)
        {
            const hkFreeList* list = hk_list_at(heap, i);
            if (list != nullptr)
            {
                hk_add_blocks(fresh, list, (int)i, list->m_activeBlocks, false);
                hk_add_blocks(fresh, list, (int)i, list->m_freeBlocks, true);
            }
        }
        const unsigned long long sig = hk_blocks_signature(heap);
        heap->m_criticalSection.leave();
        std::sort(fresh.begin(), fresh.end(), [](const hk_range& a, const hk_range& b) { return a.begin < b.begin; });
        AcquireSRWLockExclusive(&Hk_index_lock);
        Hk_index->swap(fresh);
        Hk_index_signature = sig;
        ReleaseSRWLockExclusive(&Hk_index_lock);
    }

    // The list whose block holds p (-1: none) from the index.
    int hk_index_list_of(uintptr_t p, bool& free_block)
    {
        AcquireSRWLockShared(&Hk_index_lock);
        const int r = hk_find_range(*Hk_index, p);
        const int list = r >= 0 ? (*Hk_index)[r].list : -1;
        free_block = r >= 0 && (*Hk_index)[r].free_block;
        ReleaseSRWLockShared(&Hk_index_lock);
        return list;
    }

    void hk_free_hook(void* p, int nbytes, int list_bytes)
    {
        hkFreeListMemorySystem* system = static_cast<hkFreeListMemorySystem*>(hkMemorySystem::getInstancePtr());
        if (system == nullptr || system->m_heapAllocator == nullptr || Hk_index == nullptr)
        {
            return;
        }
        hkFreeListAllocator* heap = static_cast<hkFreeListAllocator*>(system->m_heapAllocator);
        const hkFreeList* expected = list_bytes >= 0 && list_bytes <= hkFreeListAllocator::MAX_FREELIST_SIZE
                                         ? heap->m_sizeToFreeList[(list_bytes + 15) >> 4]
                                         : nullptr;
        if (hk_blocks_signature(heap) != Hk_index_signature)
        {
            hk_rebuild_index(heap);
        }
        bool free_block = false;
        const int actual = hk_index_list_of((uintptr_t)p, free_block);
        const hkFreeList* actual_list = actual >= 0 ? hk_list_at(heap, (unsigned int)actual) : nullptr;
        if ((actual_list == expected && !free_block) || (actual_list == nullptr && expected == nullptr))
        {
            return;
        }
        const long count = _InterlockedIncrement(&Hk_wrong_frees);
        void* frames[14] = {};
        const USHORT n = RtlCaptureStackBackTrace(1, 14, frames, nullptr);
        unsigned long long key = 1469598103934665603ull;
        for (USHORT k = 0; k < n && k < 8; k++)
        {
            key = (key ^ (unsigned long long)(uintptr_t)frames[k]) * 1099511628211ull;
        }
        bool fresh_stack = false;
        AcquireSRWLockExclusive(&Hk_stacks_lock);
        bool seen = false;
        for (int k = 0; k < Hk_stacks_count; k++)
        {
            seen |= Hk_stacks_seen[k] == key;
        }
        if (!seen && Hk_stacks_count < 256)
        {
            Hk_stacks_seen[Hk_stacks_count++] = key;
            fresh_stack = true;
        }
        ReleaseSRWLockExclusive(&Hk_stacks_lock);
        if (!fresh_stack)
        {
            return;
        }
        char stack[600] = "";
        size_t len = 0;
        for (USHORT k = 0; k < n && len + 32 < sizeof(stack); k++)
        {
            const uintptr_t a = (uintptr_t)frames[k];
            len += (size_t)snprintf(stack + len, sizeof(stack) - len, a >= Image_lo && a < Image_hi ? " exe+0x%llX" : " 0x%llX",
                                    (unsigned long long)(a >= Image_lo && a < Image_hi ? a - Image_lo : a));
        }
        char bytes[640];
        hk_describe_bytes((uintptr_t)p, 48, bytes, sizeof(bytes));
        memtest_event("hkcheck: wrong-size free %ld: 0x%llX freed as %d bytes (filed under the %u-byte list) lies in a %s of the %s; "
                      "the caller's stack:%s; its bytes:%s",
                      count, (unsigned long long)(uintptr_t)p, nbytes, expected != nullptr ? expected->m_elementSize : 0,
                      free_block ? "free block" : "block", actual_list != nullptr ? "list below" : "no free list (a large block)", stack, bytes);
        if (actual_list != nullptr)
        {
            memtest_event("hkcheck: wrong-size free %ld: its block is the %u-byte list's", count, actual_list->m_elementSize);
        }
    }

    // The audio bank allocator (RFG_Audio_Bank, TLSF over 190 MB) by its blocks, under its lock: whether a level's
    // banks are still there when the next one loads (its keen counters are not kept: they read 0 in use).
    struct tlsf_sum
    {
        unsigned long long used = 0;
        unsigned long long free_bytes = 0;
        unsigned long long largest_free = 0;
        unsigned int used_blocks = 0;
        unsigned int free_blocks = 0;
        char big[192] = "";
        size_t big_len = 0;
    };

    void tlsf_sum_walker(void* ptr, size_t size, int used, void* user)
    {
        tlsf_sum& s = *static_cast<tlsf_sum*>(user);
        if (used != 0)
        {
            s.used += size;
            s.used_blocks++;
            if (size >= 4 * 1048576 && s.big_len < sizeof(s.big) - 24)
            {
                const int w = snprintf(s.big + s.big_len, sizeof(s.big) - s.big_len, " %.1f", (double)size / 1048576.0);
                s.big_len += w > 0 ? (size_t)w : 0;
            }
        }
        else
        {
            s.free_bytes += size;
            s.free_blocks++;
            s.largest_free = size > s.largest_free ? size : s.largest_free;
        }
        (void)ptr;
    }

    void audio_bank_walk(const char* when)
    {
        keen::MutexLock lock(Rfg_audio_bank_keen_allocator.getMutex());
        void* pool = Rfg_audio_bank_keen_allocator.getAllocator().getPool();
        if (pool == nullptr)
        {
            return;
        }
        tlsf_sum s;
        tlsf_walk_heap(pool, tlsf_sum_walker, &s);
        memtest_event("audio banks (%s): %u blocks in use, %.1f MB (those of 4 MB or more:%s MB); %u free blocks, %.1f MB, the largest %.1f MB",
                      when, s.used_blocks, (double)s.used / 1048576.0, s.big_len > 0 ? s.big : " none", s.free_blocks,
                      (double)s.free_bytes / 1048576.0, (double)s.largest_free / 1048576.0);
    }

    void hk_census(const char* when)
    {
        if (Hk_live == nullptr)
        {
            return;
        }
        struct row
        {
            size_t count = 0;
            unsigned long long bytes = 0; // the objects' memory sizes (0 for AUTO_MEMSIZE: made by new, size unknown)
            size_t auto_size = 0;
            const hkReferencedObject* sample = nullptr;
        };
        std::unordered_map<const void*, row> by_vtable;
        size_t total = 0;
        unsigned long long total_bytes = 0;
        AcquireSRWLockShared(&Hk_lock);
        for (const hkReferencedObject* o : *Hk_live)
        {
            row& r = by_vtable[*reinterpret_cast<const void* const*>(o)];
            const int size = o->getMemorySizeAndFlags();
            r.count++;
            if (size == hkReferencedObject::AUTO_MEMSIZE)
            {
                r.auto_size++;
            }
            else
            {
                r.bytes += (unsigned int)size;
                total_bytes += (unsigned int)size;
            }
            r.sample = o;
            total++;
        }
        const unsigned long long constructed = Hk_constructed;
        const unsigned long long destructed = Hk_destructed;
        std::vector<row> rows;
        rows.reserve(by_vtable.size());
        for (const auto& kv : by_vtable)
        {
            rows.push_back(kv.second);
        }
        std::sort(rows.begin(), rows.end(), [](const row& a, const row& b) { return a.count > b.count; });
        memtest_event("havok census (%s): %zu live counted objects in %zu classes, %.1f MB of known sizes (%llu counted constructed, %llu destructed so far)",
                      when, total, rows.size(), (double)total_bytes / 1048576.0, constructed, destructed);
        for (size_t i = 0; i < rows.size() && i < 24; i++)
        {
            memtest_event("  %9zu %-56s %8.1f MB of known sizes, %zu made by new", rows[i].count, typeid(*rows[i].sample).name(),
                          (double)rows[i].bytes / 1048576.0, rows[i].auto_size);
        }
        ReleaseSRWLockShared(&Hk_lock); // the samples stay alive while the names are read
        hk_heap_breakdown(when);
    }

    void wc_drive()
    {
        if (Wc_map == nullptr || Wc_started)
        {
            return;
        }
        if (gameseq_get_state() != GS_WRECKING_CREW_MAIN_MENU || openrfg_wc_menu_state() != 1 /* MMS_WC_LOADED */)
        {
            Wc_loaded_since = 0;
            return;
        }
        const unsigned long long now = GetTickCount64();
        if (Wc_loaded_since == 0)
        {
            Wc_loaded_since = now;
            return;
        }
        if (now - Wc_loaded_since < 1000)
        {
            return;
        }
        // The chosen map's picture (a Sledge map's own: a container its mod's *_ui_images.asm_pc declares), loaded
        // once before the match as the menu loads it, to show that it does: up to 5 s.
        {
            static int picture_state = 0; // 0 not asked, 1 loading, 2 done
            static unsigned long long picture_since = 0;
            const char* image = nullptr;
            for (int i = 0; i < wrecking_crew_get_num_maps(); i++)
            {
                const char* file = wrecking_crew_get_map_file_name(i);
                if (file != nullptr && _stricmp(file, Wc_map) == 0)
                {
                    image = wrecking_crew_get_map_image_name(i);
                }
            }
            if (image != nullptr && picture_state == 0)
            {
                picture_state = ui_request_image(image) ? 1 : 2;
                picture_since = now;
                if (picture_state == 2)
                {
                    memtest_event("wc: %s's picture %s: not requested (no such container)", Wc_map, image);
                }
                return;
            }
            if (image != nullptr && picture_state == 1)
            {
                const stream2_status_val status = ui_get_image_status(image);
                if (status == ST_LOADING && now - picture_since < 5000)
                {
                    return;
                }
                memtest_event("wc: %s's picture %s: %s after %llu ms", Wc_map, image,
                              status == ST_LOADED ? "loaded" : status == ST_ERROR ? "error" : status == ST_LOADING ? "still loading" : "not loaded",
                              now - picture_since);
                ui_release_image(image);
                picture_state = 2;
            }
        }
        Wc_started = true;
        // The menu's picture bookkeeping (wcmm_do_state_change): loading, it requested the picture of map_index;
        // unloading, it releases those of map_index and old_map_index. The options below would overwrite both.
        const int menu_map_index = Wrecking_crew_options.map_index;
        const int menu_old_map_index = Wrecking_crew_options.old_map_index;
        // Total chaos, 1 player, weapon 4 (thermobaric rockets), no time limit: rfgphys's avenue default.
        const char* hex = Wc_options_hex != nullptr ? Wc_options_hex : "030000000100000013000000120000000400000000000000FFFFFFFF010000000500000000010000FFFFFFFF";
        unsigned char bytes[sizeof(wrecking_crew_options)];
        memcpy(bytes, &Wrecking_crew_options, sizeof(bytes));
        for (size_t i = 0; i < sizeof(bytes) && hex[2 * i] != '\0' && hex[2 * i + 1] != '\0'; i++)
        {
            const int hi = hex_value(hex[2 * i]);
            const int lo = hex_value(hex[2 * i + 1]);
            if (hi < 0 || lo < 0)
            {
                break;
            }
            bytes[i] = (unsigned char)(hi << 4 | lo);
        }
        memcpy(&Wrecking_crew_options, bytes, sizeof(bytes));
        const int count = wrecking_crew_get_num_maps();
        int index = -1;
        for (int i = 0; i < count; i++)
        {
            const char* file = wrecking_crew_get_map_file_name(i);
            if (file != nullptr && _stricmp(file, Wc_map) == 0)
            {
                index = i;
            }
        }
        if (index < 0)
        {
            char list[1024] = "";
            for (int i = 0; i < count; i++)
            {
                const char* file = wrecking_crew_get_map_file_name(i);
                strncat_s(list, file ? file : "?", _TRUNCATE);
                strncat_s(list, " ", _TRUNCATE);
            }
            memtest_event("wc: map %s is not in the Wrecking Crew list (%d maps: %s); not starting", Wc_map, count, list);
            return;
        }
        // The list as the menu shows it: each map's file and name (the Sledge maps' own names included).
        {
            char line[900] = "";
            for (int i = 0; i < count; i++)
            {
                const char* file = wrecking_crew_get_map_file_name(i);
                const char* key = wrecking_crew_get_map_display_name(i);
                const wchar_t* text = key != nullptr ? localization_get_display_string_opt(key) : nullptr;
                char name[96] = "?";
                if (text != nullptr)
                {
                    WideCharToMultiByte(CP_UTF8, 0, text, -1, name, (int)sizeof(name), nullptr, nullptr);
                    name[sizeof(name) - 1] = '\0';
                }
                char item[192];
                _snprintf_s(item, sizeof(item), _TRUNCATE, "%d %s \"%s\"; ", i, file != nullptr ? file : "?", name);
                if (strlen(line) + strlen(item) >= sizeof(line))
                {
                    memtest_event("wc: maps: %s", line);
                    line[0] = '\0';
                }
                strncat_s(line, item, _TRUNCATE);
            }
            if (line[0] != '\0')
            {
                memtest_event("wc: maps: %s", line);
            }
        }
        // The map is chosen as the menu's own selection change would: the picture it requested (its map_index's) is
        // released and the chosen map's requested, so its release on unloading is the chosen map's and balances.
        // Setting map_index alone (and old_map_index from the options) left the first map's picture requested for
        // good and released two it never asked for; back in the menu, the first map's bitmap still pointed into
        // memory that a later picture took (bmcheck repaired it: wc1's mp_thumb_complex.tga, a hand session).
        Wrecking_crew_options.old_map_index = menu_old_map_index;
        if (menu_map_index != index)
        {
            if (menu_map_index >= 0 && menu_map_index < count)
            {
                ui_release_image(wrecking_crew_get_map_image_name(menu_map_index));
            }
            ui_request_image(wrecking_crew_get_map_image_name(index));
        }
        Wrecking_crew_options.map_index = index;
        Wrecking_crew_options.current_challenge_id = -1;
        memtest_event("wc: starting %s (map %d of %d), mode %d, %d player(s), weapon %d, free play %d", Wc_map, index, count,
                      (int)Wrecking_crew_options.mode, Wrecking_crew_options.num_players, Wrecking_crew_options.weapon_index,
                      Wrecking_crew_options.free_play ? 1 : 0);
        memtest_event("wc: before this load: big containers %.1f MB committed, far table %u entries", big_committed_mb(), far_entries());
        hk_census("before this load");
        audio_bank_walk("before this load");
        Wc_started_ms = now;
        wrecking_crew_match_begin();
    }

    // Memtest.WcRepeat=N (OPENRFG_MEMTEST_WCREPEAT): N more rounds after the first. A round lasts Memtest.WcRoundS
    // seconds (OPENRFG_MEMTEST_WCROUNDS, default 60) of the match, then quits to the main menu as the pause menu does,
    // and --wc starts the match again: the level unloads and loads again (what it gives back, what it takes anew).
    int Wc_repeats = -1;
    bool Wc_menu_again = false;

    void wc_repeat(unsigned long long now)
    {
        if (Wc_repeats < 0)
        {
            Wc_repeats = env_int("OPENRFG_MEMTEST_WCREPEAT", 0);
        }
        static const int round_s = env_int("OPENRFG_MEMTEST_WCROUNDS", 60);
        if (Wc_repeats <= 0 || Wc_started_ms == 0 || gameseq_get_state() != GS_GAMEPLAY ||
            now - Wc_started_ms < (unsigned long long)round_s * 1000ull)
        {
            return;
        }
        Wc_repeats--;
        Wc_started = false;
        Wc_started_ms = 0;
        Wc_loaded_since = 0;
        Wc_menu_again = true;
        memtest_event("wc: round over (%d more): to the main menu; big containers %.1f MB committed, far table %u entries",
                      Wc_repeats, big_committed_mb(), far_entries());
        hk_census("at the round's end");
        audio_bank_walk("at the round's end");
        game_set_wc_shutdown(GS_MAINMENU);
    }
}

bool memtest_wc_menu_again()
{
    if (!Wc_menu_again)
    {
        return false;
    }
    Wc_menu_again = false;
    memtest_event("wc: main menu: Wrecking Crew again (after the level's unload: big containers %.1f MB committed, far table %u entries)",
                  big_committed_mb(), far_entries());
    return true;
}

// ---- lifecycle

void memtest_init()
{
    if (Initialized)
    {
        return;
    }
    Initialized = true;
    Start_ms = GetTickCount64();
    if (env_int("OPENRFG_MEMTEST_HAVOKCENSUS", 0) != 0) // before the game makes its Havok objects
    {
        Hk_live = new std::unordered_set<const hkReferencedObject*>();
        Hk_live->reserve(1 << 22);
        hkReferencedObjectTracker = hk_track;
    }
    Hk_check_s = env_int("OPENRFG_MEMTEST_HKCHECK", 0);
    if (Hk_check_s > 0) // every Havok free checked against the list whose block holds it (before Havok starts)
    {
        Hk_index = new std::vector<hk_range>();
        hkMemtestFreeHook = hk_free_hook;
    }
    scaling_static_init_done(); // scaling: pools made from now on keep the game's fixed storage
#ifdef OPENRFG_X64
    et_far::On_new_entry = note_far_entry;
#endif
    const unsigned char* base = (const unsigned char*)GetModuleHandleA(nullptr);
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    Image_lo = (uintptr_t)base;
    Image_hi = Image_lo + nt->OptionalHeader.SizeOfImage;

    const char* dir = Out_dir_arg;
    char env_dir[MAX_PATH];
    if (dir == nullptr && GetEnvironmentVariableA("OPENRFG_RUN_DIR", env_dir, sizeof(env_dir)) > 0)
    {
        dir = env_dir;
    }
    if (dir != nullptr)
    {
        strncpy_s(Out_dir, dir, _TRUNCATE);
    }
    char interval[32];
    if (GetEnvironmentVariableA("OPENRFG_MEMTEST_INTERVALMS", interval, sizeof(interval)) > 0 && atoi(interval) >= 100)
    {
        Interval_ms = (unsigned int)atoi(interval);
    }
    Events = open_out("memtest_events.log", "w");
    Native = open_out("memtest_native.log", "w");
    Csv = open_out("memtest.csv", "w");
    Pools_csv = open_out("memtest_pools.csv", "w");
    if (Csv != nullptr)
    {
        fprintf(Csv, "t_s,frame,state,private_mb,private_peak_mb,working_set_mb,va_used_gb,provider_used_mb,provider_peak_mb,provider_total_mb,"
                     "vram_pool_used_mb,vram_pool_total_mb,audio_bank_used_mb,audio_bank_total_mb,pools,pools_used_mb,fullest_pool,fullest_pct,"
                     "havok_inuse_mb,havok_peak_mb,havok_total_mb,crt_heap_mb,crt_blocks,crt_peak_mb,far_entries,far_max,strpool_fallbacks,"
                     "strpool_no_arena,objects,max_objects,failure_messages,overlay_hits,level_used_mb,level_peak_mb,level_size_mb,"
                     "level_largest_free_mb,level_failures,collision_models,collision_models_max,general_movers,general_movers_max,"
                     "zone_handles_used,zones,render_instances,render_instances_max,zone_free,debris_kept,debris_deleted_by_budget,"
                     "fades_blocked,vaporize_refused,debris_recycled,stream_parent_gone,demolish_bombs,havok_system_mb,havok_solver_mb,"
                     "island_trims,bodies_trimmed,havok_low_memory,scaling_committed_mb,havok_pages_mb,scaling_faults\n");
        fflush(Csv);
    }
    if (Pools_csv != nullptr)
    {
        fprintf(Pools_csv, "t_s,pool,used,size,peak,dynamic\n");
        fflush(Pools_csv);
    }
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, sizeof(exe));
    memtest_event("memtest: %s, %s build of %s %s, census every %u ms, output in %s", exe,
#ifdef OPENRFG_X64
                  "64-bit",
#else
                  "32-bit",
#endif
                  __DATE__, __TIME__, Interval_ms, Out_dir);
    memtest_event("memtest: command line: %s", GetCommandLineA());
    if (const char* ini = memtest_config_ini_path())
    {
        memtest_event("memtest: config.ini redirected to %s", ini);
    }
    if (Sledge_dir != nullptr)
    {
        load_sledge(Sledge_dir);
    }
    if (Plugins_dir != nullptr)
    {
        load_plugins(Plugins_dir);
    }
    if (Wc_map != nullptr)
    {
        memtest_event("wc: will start a Wrecking Crew match on %s from the main menu", Wc_map);
    }
    // The summary when the game ends normally (the player quits), from what the census recorded while it ran. No last
    // census: by now main() has ended and the memory system is destroyed, and a pool destroyed inside another one keeps
    // its base class's vtable (in the image, so read_pool's check passes) and a call into it is a pure virtual call:
    // abort(), which no __try catches (the first quit of a session, 2026-11-19).
    atexit([]() {
        if (!Initialized)
        {
            return;
        }
        write_pools();
        write_summary("the game ended");
        memtest_event("memtest: the game ended at frame %u", Frame);
    });
}

void memtest_frame()
{
    if (!Initialized)
    {
        return;
    }
    Frame++;
    const int state = (int)gameseq_get_state();
    if (state != Last_state)
    {
        PROCESS_MEMORY_COUNTERS_EX pmc = {};
        pmc.cb = sizeof(pmc);
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
        memtest_event("state: %d %s -> %d %s (frame %u, private %.0f MB)", Last_state, state_name(Last_state), state, state_name(state), Frame, mb(pmc.PrivateUsage));
        Last_state = state;
    }
    wc_drive();
    report_scaling();
    memtest_ui_frame();
    const unsigned long long now = GetTickCount64();
    wc_repeat(now);
    if (Hk_check_s > 0 && now - Last_hk_check_ms >= (unsigned long long)Hk_check_s * 1000ull)
    {
        Last_hk_check_ms = now;
        hk_check_free_lists("periodic", false);
    }
    if (now - Last_census_ms >= Interval_ms)
    {
        Last_census_ms = now;
        census();
    }
    if (now - Last_pools_ms >= 10000)
    {
        Last_pools_ms = now;
        write_pools();
    }
    if (now - Last_summary_ms >= 10000)
    {
        Last_summary_ms = now;
        write_summary("running");
    }
    if (Exit_after_s > 0 && Wc_started_ms != 0 && now - Wc_started_ms >= (unsigned long long)Exit_after_s * 1000ull)
    {
        census();
        write_pools();
        write_summary("--exit-after");
        memtest_event("memtest: --exit-after %d s reached: ending the process", Exit_after_s);
        TerminateProcess(GetCurrentProcess(), 0);
    }
}

void memtest_level_event(const char* what)
{
    memtest_event("level: %s", what);
}

void memtest_havok_check(const char* when)
{
    if (Initialized && Hk_check_s > 0)
    {
        hk_check_free_lists(when, true);
    }
}

#ifdef OPENRFG_X64
namespace
{
    unsigned int Far_level_mark = 0; // the far table's count when the level began to load
    unsigned int Far_exit_count = 0; // its count when that level's gameplay ended
    unsigned int Far_mark_level_used = 0; // level memory in use when the level began to load
    bool Far_level_exited = false;
}
#endif

// The far entries a level took go when the next one begins to load: its gameplay ended (gamestate_gp_exit) and its
// containers were unloaded since (stream2 unloads them after the exit queued them). Only when no entry was taken
// since its gameplay ended (the menus took none in the runs), none of the level's lies outside the memory its unload
// gave back (note_far_entry: a field or a target in big containers or level memory), and the big containers have
// nothing committed any more; otherwise they stay, and the events say why. Then the mark for this level.
void memtest_level_load_start()
{
#ifdef OPENRFG_X64
    et_far::tables& t = et_far::get_tables();
    if (Far_level_exited)
    {
        Far_level_exited = false;
        const unsigned int taken = Far_exit_count - Far_level_mark;
        const double big_mb = big_committed_mb();
        if (env_int("OPENRFG_MEMTEST_FARREUSE", 1) == 0)
        {
        }
        else if (t.count != Far_exit_count)
        {
            memtest_event("far table: %u entries taken since the last level's gameplay ended: its %u kept", t.count - Far_exit_count, taken);
        }
        else if (Far_lasting_since_mark != 0)
        {
            memtest_event("far table: %u of the last level's %u entries lie outside the memory its unload gave back: all kept", Far_lasting_since_mark, taken);
        }
        else if (big_mb > 0.0)
        {
            memtest_event("far table: the big containers still hold %.1f MB: the last level's %u entries kept", big_mb, taken);
        }
        else if (Level_memmgr.space_used() > Far_mark_level_used)
        {
            memtest_event("far table: level memory holds %.1f MB more than when the last level began to load: its %u entries kept",
                          mb(Level_memmgr.space_used() - Far_mark_level_used), taken);
        }
        else if (taken > 0)
        {
            const unsigned int dropped = et_far::release_from(Far_level_mark);
            memtest_event("far table: the last level's %u entries released (%u kept)", dropped, Far_level_mark);
        }
    }
    Far_level_mark = t.count;
    Far_lasting_since_mark = 0;
    Far_mark_level_used = Level_memmgr.space_used();
#endif
}

void memtest_level_gameplay_end()
{
#ifdef OPENRFG_X64
    Far_exit_count = et_far::get_tables().count;
    Far_level_exited = true;
#endif
}

// ---- what the recovered code reports

void memtest_pool_register(mempool_base* pool)
{
    AcquireSRWLockExclusive(&Pool_lock);
    for (int i = 0; i < Pool_count; i++)
    {
        if (Pools[i].pool == pool)
        {
            ReleaseSRWLockExclusive(&Pool_lock);
            return;
        }
    }
    if (Pool_count < Max_pools)
    {
        pool_entry& e = Pools[Pool_count++];
        memset(&e, 0, sizeof(e));
        e.pool = pool;
        e.full_at = -1.0;
    }
    else
    {
        Pool_registry_overflow++;
    }
    ReleaseSRWLockExclusive(&Pool_lock);
}

void memtest_pool_unregister(mempool_base* pool)
{
    AcquireSRWLockExclusive(&Pool_lock);
    for (int i = 0; i < Pool_count; i++)
    {
        if (Pools[i].pool == pool)
        {
            Pools[i] = Pools[--Pool_count];
            break;
        }
    }
    ReleaseSRWLockExclusive(&Pool_lock);
}

void memtest_native_print(const char* text)
{
    if (Native == nullptr || text == nullptr)
    {
        return;
    }
    AcquireSRWLockExclusive(&Log_lock);
    fputs(text, Native);
    const size_t n = strlen(text);
    if (n == 0 || text[n - 1] != '\n')
    {
        fputc('\n', Native);
    }
    fflush(Native);
    ReleaseSRWLockExclusive(&Log_lock);
    // Messages that mean something ran out.
    static const char* const patterns[] = {"out of memory", "Out of memory", "overflow", "Overflow", "Cannot alloc", "alloc failed",
                                           "allocation fail", "allocationFailure", "full!", "is full", "exhausted", "not enough memory",
                                           "Too many", "too many", "increase MAX", "Failed to alloc", "failed to alloc"};
    for (const char* p : patterns)
    {
        if (strstr(text, p) != nullptr)
        {
            InterlockedIncrement(&Failure_messages);
            strncpy_s(Last_failure, text, _TRUNCATE);
            for (char* c = Last_failure; *c; c++)
            {
                if (*c == '\n' || *c == '\r')
                {
                    *c = ' ';
                }
            }
            char line[300];
            _snprintf_s(line, sizeof(line), _TRUNCATE, "game: %s", Last_failure);
            log_line(Events, line);
            break;
        }
    }
}

void memtest_event(const char* format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args);
    va_end(args);
    log_line(Events, text);
}

void memtest_crt_alloc(size_t bytes)
{
    const long long now = InterlockedAdd64(&Crt_bytes, (long long)bytes);
    InterlockedIncrement64(&Crt_blocks);
    long long peak = Crt_peak;
    while (now > peak)
    {
        const long long seen = InterlockedCompareExchange64(&Crt_peak, now, peak);
        if (seen == peak)
        {
            break;
        }
        peak = seen;
    }
}

void memtest_crt_free(size_t bytes)
{
    InterlockedAdd64(&Crt_bytes, -(long long)bytes);
    InterlockedDecrement64(&Crt_blocks);
}

void memtest_strpool_fallback()
{
    InterlockedIncrement(&Strpool_fallbacks);
}

void memtest_strpool_arena_unregistered()
{
    InterlockedIncrement(&Strpool_unregistered);
}

void memtest_havok_out_of_memory(unsigned int size, bool final_failure)
{
    if (!final_failure)
    {
        memtest_event("havok: cannot allocate %u bytes: the game frees what it can", size);
        return;
    }
    InterlockedIncrement(&Havok_oom);
    memtest_event("havok: OUT OF MEMORY (%u bytes): rfg.exe would hang here; the census is written and the process ends", size);
    census();
    write_pools();
    write_summary("Havok out of memory");
    TerminateProcess(GetCurrentProcess(), 3);
}

void memtest_level_alloc_failed(const void* manager, unsigned int size, const char* name)
{
    const bool level = manager == &Level_memmgr;
    if (level)
    {
        InterlockedIncrement(&Level_failures);
    }
    // Read the manager's figures without its lock: approximate, for the log only.
    const split_memmgr* m = static_cast<const split_memmgr*>(manager);
    split_memmgr* mm = const_cast<split_memmgr*>(m);
    char line[200];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%s: alloc of %u bytes (%s) FAILED: %u of %u bytes used, largest possible %u",
                level ? "level memory" : "split_memmgr", size, name ? name : "-", mm->space_used(), mm->space_max(),
                mm->largest_allocation_possible());
    if (level && Level_first_failure[0] == '\0')
    {
        strncpy_s(Level_first_failure, line, _TRUNCATE);
    }
    log_line(Events, line);
}

// ---- for the plugins

// The target of a self-relative 32-bit offset field of the game's data (et_ptr_offset). On x64 such a field may hold an
// index into the far table (a target more than 2 GB away, endian_type.inl), which a plugin cannot read itself; the
// converted rfgphys calls this (GetProcAddress on the program). Null-able conventions (0 or -1) are the caller's.
extern "C" __declspec(dllexport) const void* openrfg_et_ptr_target(const void* field)
{
    const int offset = *static_cast<const int*>(field);
#ifdef OPENRFG_X64
    if (offset >= et_far::Base)
    {
        return et_far::far_target(offset);
    }
#endif
    return static_cast<const char*>(field) + offset;
}

// ---- settings

const char* memtest_config_ini_path()
{
    static char path[MAX_PATH];
    static int state = 0;
    if (state == 0)
    {
        state = GetEnvironmentVariableA("OPENRFG_CONFIG_INI", path, sizeof(path)) > 0 ? 1 : 2;
    }
    return state == 1 ? path : nullptr;
}

uint64_t memtest_budget(const char* name, uint64_t default_bytes)
{
    char var[96];
    char value[64];
    _snprintf_s(var, sizeof(var), _TRUNCATE, "OPENRFG_MEMTEST_%s", name);
    if (GetEnvironmentVariableA(var, value, sizeof(value)) == 0)
    {
        return default_bytes;
    }
    char* end = nullptr;
    unsigned long long v = _strtoui64(value, &end, 0);
    if (end != nullptr)
    {
        if (*end == 'k' || *end == 'K') v <<= 10;
        else if (*end == 'm' || *end == 'M') v <<= 20;
        else if (*end == 'g' || *end == 'G') v <<= 30;
    }
    memtest_event("budget: %s = %llu bytes (default %llu)", name, v, (unsigned long long)default_bytes);
    return v;
}

// ---- the Wrecking Crew start

const char* memtest_wc_map()
{
    return Wc_map;
}

// Memtest.NoHud=1 (OPENRFG_MEMTEST_NOHUD): gameplay draws the world alone, for a map's picture (no HUD, no Wrecking
// Crew panels, no menus, no FPS counter).
bool memtest_hide_hud()
{
    static int on = -1;
    if (on < 0)
    {
        char value[8] = "";
        on = GetEnvironmentVariableA("OPENRFG_MEMTEST_NOHUD", value, sizeof(value)) > 0 && value[0] == '1' ? 1 : 0;
    }
    return on != 0;
}

// Unattended runs (OPENRFG_MEMTEST_WCCONTINUE=1, set by the run script with -IgnoreFocus): the match's "continue"
// screens are passed after 1.5 s, the first one before the player's turn among them (the match waits there).
bool memtest_wc_continue()
{
    static int on = -1;
    if (on < 0)
    {
        char value[8] = "";
        on = GetEnvironmentVariableA("OPENRFG_MEMTEST_WCCONTINUE", value, sizeof(value)) > 0 && value[0] == '1' ? 1 : 0;
    }
    if (on == 0)
    {
        return false;
    }
    static unsigned long long since = 0;
    const unsigned long long now = GetTickCount64();
    if (since == 0 || now - since > 5000)
    {
        since = now; // a new wait (no call for a while: the last one ended)
        return false;
    }
    if (now - since < 1500)
    {
        return false;
    }
    since = 0;
    memtest_event("wc: continue pressed (unattended run), Wrecking Crew state %d", (int)wrecking_crew_get_state());
    return true;
}

// ---- the Sledge layer

const char* memtest_overlay_mount(int i, const char** mount_name)
{
    if (i < 0 || i >= Mount_count)
    {
        return nullptr;
    }
    *mount_name = Mount_name[i];
    return Mount_native[i];
}

bool memtest_overlay_find(cf_found_info* found_info, const char* filename)
{
    if (Overlay_file_count == 0 || filename == nullptr)
    {
        return false;
    }
    const char* base = filename;
    for (const char* p = filename; *p; p++)
    {
        if (*p == '\\' || *p == '/' || *p == ':')
        {
            base = p + 1;
        }
    }
    for (int i = 0; i < Overlay_file_count; i++)
    {
        const overlay_file& o = Overlay_files[i];
        if (_stricmp(base, o.name) != 0)
        {
            continue;
        }
        strncpy(found_info->m_full_name, o.keen_path, cf_found_info::CFFI_FULL_NAME_ARRAY_SIZE);
        found_info->m_full_name[cf_found_info::CFFI_FULL_NAME_ARRAY_SIZE - 1] = '\0';
        found_info->m_size = (unsigned int)o.size;
        found_info->m_searched_system = CF_SEARCH_STANDARD;
        found_info->m_media_type = CF_IO_MEDIA_HDD;
        found_info->vpe = nullptr;
        if (InterlockedIncrement(&Overlay_hits) <= 50)
        {
            memtest_event("sledge: %s served from %s", filename, o.keen_path);
        }
        return true;
    }
    return false;
}

int memtest_wc_map_additions(const memtest_wc_map_addition** additions)
{
    *additions = Wc_additions;
    return Wc_addition_count;
}

int memtest_sledge_ui_image_asms(const char** names, int max_names)
{
    static const char suffix[] = "_ui_images.asm_pc";
    const size_t slen = sizeof(suffix) - 1;
    int n = 0;
    for (int i = 0; i < Overlay_file_count && n < max_names; i++)
    {
        const size_t len = strlen(Overlay_files[i].name);
        if (len > slen && _stricmp(Overlay_files[i].name + len - slen, suffix) == 0)
        {
            names[n++] = Overlay_files[i].name;
        }
    }
    return n;
}
