// Original: volition/render_lib/code/rl_factory_rtti.cpp
//
// The class registries of render_lib: every rl_class_factory_info and rl_class_dynamic_info registers itself in a
// circular list during static initialization. ::factory_init (rl_renderer_core.cpp) walks the factory list.

#include "volition/render_lib/code/rl_factory_rtti.h"
#include "scaling/scaling.h" // scaling

#include <stdlib.h>

linked_list_p<rl_class_factory_info>* rl_class_factory_info::ms_class_factories = nullptr; // 0x01B7BCFC
int rl_class_factory_info::ms_num_exhausted_factories = 0; // 0x01B7BD00
linked_list_p<rl_class_dynamic_info>* rl_class_dynamic_info::ms_dynamic_classes = nullptr; // 0x01B7BD0C

// Appends elem_p to the circular list (linked_list_p::append, inline in both register_class functions).
template<typename T>
static void append_to_class_list(linked_list_p<T>*& list_p, T* elem_p)
{
    if (list_p == nullptr)
    {
        // The list is malloc'ed: it is created during static initialization, before any allocator exists.
        list_p = (linked_list_p<T>*)malloc(sizeof(linked_list_p<T>));
        if (list_p != nullptr)
        {
            new (list_p) linked_list_p<T>();
        }
    }
    list_p->append(elem_p);
}

// 0x004DC8B0 (.h:518)
void rl_class_factory_info::register_class(rl_class_factory_info* class_factory_info_p)
{
    append_to_class_list(ms_class_factories, class_factory_info_p);
}

// 0x004DC910 (.h:535)
rl_class_factory_info::rl_class_factory_info(const char* class_name, unsigned int class_id, void (*factory_init)(mempool&), int class_size, const pool_list<rl_base_object*>* local_object_pool_p, int objects_max)
{
    m_class_name = class_name;
    m_class_id = class_id;
    m_class_size = class_size;
    m_factory_init = factory_init;
    m_factory_mempool_p = nullptr;
    m_local_object_pool_p = local_object_pool_p;
    m_objects_max = objects_max;
    m_display_exhausted_factories_warning = true;
    register_class(this);
    m_object_data_base = nullptr;
    m_object_data_size = 0;
}

// ---- scaling (OpenRFG64-memtest, new): factory classes that grow ------------------------------------------------

namespace
{
    struct factory_growth
    {
        rl_class_factory_info* info;
        int ceiling;
        void (*on_grow)(const char*, int, int);
        unsigned char* base;  // the grown objects, one after the other (reserved for the ceiling)
        size_t reserved;
        int added;            // objects in the region
    };
    factory_growth Growths[32];
    int Growth_count;

    factory_growth* growth_of(const rl_class_factory_info* info)
    {
        for (int i = 0; i < Growth_count; i++)
        {
            if (Growths[i].info == info)
            {
                return &Growths[i];
            }
        }
        return nullptr;
    }
}

void rl_factory_set_growth(rl_class_factory_info& info, int ceiling, void (*on_grow)(const char*, int, int))
{
    if (!scaling_enabled() || ceiling <= info.m_objects_max)
    {
        return;
    }
    factory_growth* g = growth_of(&info);
    if (g == nullptr)
    {
        if (Growth_count >= (int)(sizeof(Growths) / sizeof(Growths[0])))
        {
            return;
        }
        g = &Growths[Growth_count++];
        g->info = &info;
    }
    g->ceiling = ceiling;
    g->on_grow = on_grow;
}

int rl_factory_pool_capacity(const rl_class_factory_info& info)
{
    const factory_growth* g = growth_of(&info);
    return g != nullptr && g->ceiling > info.m_objects_max ? g->ceiling : info.m_objects_max;
}

// A quarter more each time (at least 1,024), committed in the class's region; reserved for the ceiling at the first.
int rl_factory_grow_objects(rl_class_factory_info& info, int class_size, unsigned char** objects)
{
    factory_growth* g = growth_of(&info);
    if (g == nullptr || info.m_objects_max >= g->ceiling || info.m_factory_mempool_p == nullptr)
    {
        return 0;
    }
    if (g->base == nullptr)
    {
        g->reserved = (size_t)(g->ceiling - info.m_objects_max) * (size_t)class_size;
        g->base = (unsigned char*)scaling_reserve(info.m_class_name, g->reserved, 0);
        if (g->base == nullptr)
        {
            g->ceiling = 0;
            return 0;
        }
    }
    int step = info.m_objects_max / 4 > 1024 ? info.m_objects_max / 4 : 1024;
    if (step > g->ceiling - info.m_objects_max)
    {
        step = g->ceiling - info.m_objects_max;
    }
    unsigned char* first = g->base + (size_t)g->added * (size_t)class_size;
    if ((size_t)(g->added + step) * (size_t)class_size > g->reserved || !scaling_commit(first, (size_t)step * (size_t)class_size))
    {
        return 0;
    }
    const int before = info.m_objects_max;
    g->added += step;
    info.m_objects_max += step;
    if (g->on_grow != nullptr)
    {
        g->on_grow(info.m_class_name, before, info.m_objects_max);
    }
    *objects = first;
    return step;
}

bool rl_factory_owns(const rl_class_factory_info& info, const void* object)
{
    const factory_growth* g = growth_of(&info);
    return g != nullptr && g->base != nullptr && (const unsigned char*)object >= g->base &&
           (const unsigned char*)object < g->base + g->reserved;
}

// 0x004B6880 (.h:542)
bool rl_class_factory_info::get_display_exhausted_factories_warning()
{
    return m_display_exhausted_factories_warning;
}

// 0x004B6890 (.h:546)
void rl_class_factory_info::increment_num_exhausted_factories()
{
    ms_num_exhausted_factories++;
}

// 0x004B68A0 (.h:547)
void rl_class_factory_info::decrement_num_exhausted_factories()
{
    ms_num_exhausted_factories--;
}

// 0x004DC970 (.h:564)
void rl_class_dynamic_info::register_class(rl_class_dynamic_info* class_dynamic_info_p)
{
    append_to_class_list(ms_dynamic_classes, class_dynamic_info_p);
}

// 0x004DC9D0 (.h:575)
rl_class_dynamic_info::rl_class_dynamic_info(const char* class_name, int class_size)
{
    m_class_name = class_name;
    m_class_size = class_size;
    register_class(this);
    m_objects_peak = 0;
    m_objects_allocated = 0;
}
