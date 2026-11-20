// Original: volition/vlib/code/util/endian_type.h (inline members; included at the end of endian_type.h)
//
// Partial: et_ptr_offset get_offset, set_offset, to_pointer, from_pointer, the copy constructor and assignments,
// operator[], operator T* and operator-> only.
#pragma once

#ifdef OPENRFG_X64
#include <intrin.h>
// x64: a self-relative 32-bit offset cannot reach a target more than 2 GB away (the heaps are far apart). Such a
// target goes in a global table and the offset holds 0x7F000000 + its index (a real distance never reaches that).
namespace et_far
{
static const int Base = 0x7F000000;
static const unsigned int Max_entries = 0x400000; // OpenRFG64-memtest: 4 million (1 in OpenRFG64): big maps
static const unsigned int Hash_size = 0x800000;
struct tables
{
    void* targets[Max_entries];
    unsigned int hash[Hash_size]; // index + 1, 0 = empty
    unsigned int count;
    volatile long lock;
};
inline tables& get_tables()
{
    static tables t;
    return t;
}
// memtest (OpenRFG64-memtest): told of each new entry and the field that needed it, under the table's lock (the census
// sorts them by memory region). Must not take an entry itself.
inline void (*On_new_entry)(const void* field, const void* target) = nullptr;
inline int far_index_of(const void* target, const void* field = nullptr)
{
    tables& t = get_tables();
    while (_InterlockedCompareExchange(&t.lock, 1, 0) != 0)
    {
    }
    unsigned long long k = (unsigned long long)target;
    unsigned int h = (unsigned int)((k >> 4) * 0x9E3779B1u) & (Hash_size - 1);
    int result = -1;
    for (;;)
    {
        const unsigned int e = t.hash[h];
        if (e == 0)
        {
            if (t.count < Max_entries)
            {
                t.targets[t.count] = (void*)target;
                t.hash[h] = t.count + 1;
                result = (int)t.count;
                t.count++;
                if (On_new_entry != nullptr)
                {
                    On_new_entry(field, target);
                }
            }
            break;
        }
        if (t.targets[e - 1] == target)
        {
            result = (int)(e - 1);
            break;
        }
        h = (h + 1) & (Hash_size - 1);
    }
    _InterlockedExchange(&t.lock, 0);
    return result;
}
inline void* far_target(int offset)
{
    return get_tables().targets[offset - Base];
}
} // namespace et_far
#endif

// <char,0> 0x00595630 (.h:460)
template<typename T, int NULLABLE>
int et_ptr_offset<T, NULLABLE>::get_offset() const
{
    return m_offset;
}

// <char,0> 0x00595640 (.h:465)
template<typename T, int NULLABLE>
void et_ptr_offset<T, NULLABLE>::set_offset(int offset)
{
    m_offset = offset;
}

// <char,0> 0x00595850 (.h:338)
template<typename T, int NULLABLE>
T* et_ptr_offset<T, NULLABLE>::to_pointer() const
{
    // With NULLABLE = 1 an offset of 0 or -1 means nullptr.
    if (NULLABLE != 0 && (m_offset == 0 || m_offset == -1))
    {
        return nullptr;
    }
#ifdef OPENRFG_X64
    if (m_offset >= et_far::Base)
    {
        return (T*)et_far::far_target(m_offset);
    }
#endif
    return (T*)((char*)this + m_offset);
}

// <char,0> 0x005C0510 (.h:347)
template<typename T, int NULLABLE>
void et_ptr_offset<T, NULLABLE>::from_pointer(T* const value)
{
    // With NULLABLE = 1 nullptr is stored as -1.
    if (NULLABLE != 0 && value == nullptr)
    {
        m_offset = -1;
        return;
    }
#ifdef OPENRFG_X64
    {
        const long long d = (long long)((char*)value - (char*)this);
        if (d > 0x7EFFFFFFLL || d < -0x80000000LL)
        {
            const int index = et_far::far_index_of(value, this);
            if (index >= 0)
            {
                m_offset = et_far::Base + index;
                return;
            }
        }
    }
#endif
    m_offset = (int)((char*)value - (char*)this);
}

// <char,1> 0x005C5CC0 (.h:472)
// A copy points at the same target (the offset rebased on the new address).
template<typename T, int NULLABLE>
et_ptr_offset<T, NULLABLE>::et_ptr_offset(const et_ptr_offset<T, NULLABLE>& value)
{
    from_pointer(value.to_pointer());
}

// <char,1> 0x005C5CF0 (.h:483)
template<typename T, int NULLABLE>
et_ptr_offset<T, NULLABLE>& et_ptr_offset<T, NULLABLE>::operator=(const et_ptr_offset<T, NULLABLE>& value)
{
    from_pointer(value.to_pointer());
    return *this;
}

// <terrain_occluder,0> 0x00935590 (.h:453)
template<typename T, int NULLABLE>
et_ptr_offset<T, NULLABLE>& et_ptr_offset<T, NULLABLE>::operator=(T* const value)
{
    from_pointer(value);
    return *this;
}

// <char,0> 0x00595620 (.h:331)
// An unset offset is -1.
template<typename T, int NULLABLE>
et_ptr_offset<T, NULLABLE>::et_ptr_offset()
    : m_offset(-1)
{
}

// <char,0> 0x005B3500 (.h:383)
template<typename T, int NULLABLE>
et_ptr_offset<T, NULLABLE>::operator T*() const
{
    return to_pointer();
}

// <rl_renderable_instance,0> 0x005D6780 (.h:360)
template<typename T, int NULLABLE>
T* et_ptr_offset<T, NULLABLE>::operator->() const
{
    return to_pointer();
}

// <unsigned char,0> 0x00501F60 (.h:408)
template<typename T, int NULLABLE>
T& et_ptr_offset<T, NULLABLE>::operator[](int i)
{
    return to_pointer()[i];
}

// <int,0> 0x00C948C0 (.h:414)
template<typename T, int NULLABLE>
T& et_ptr_offset<T, NULLABLE>::operator[](int i) const
{
    return to_pointer()[i];
}

// <unsigned int,0> 0x00800D40 (.h:396)
template<typename T, int NULLABLE>
T& et_ptr_offset<T, NULLABLE>::operator[](unsigned int i)
{
    return to_pointer()[i];
}

// <rl_render_block,0> 0x00501F80 (.h:402)
template<typename T, int NULLABLE>
T& et_ptr_offset<T, NULLABLE>::operator[](unsigned int i) const
{
    return to_pointer()[i];
}

// <char> 0x0091E180 (.h:601)
template<typename T>
void et_sized_native_pointer<T>::from_pointer(T* const value)
{
    m_storage.m_pointer_type.m_pointer = value;
}

// <char> 0x005D3110 (.h:590)
template<typename T>
T* et_sized_native_pointer<T>::to_pointer() const
{
    return m_storage.m_pointer_type.m_pointer;
}

// <char> 0x005D6730 (.h:635)
template<typename T>
et_sized_native_pointer<T>::operator T*() const
{
    return to_pointer();
}

// <rl_material_map_data> to unsigned char* 0x00CA2840 (.h:641)
// The stored pointer as another pointer type.
template<typename T>
template<typename U>
et_sized_native_pointer<T>::operator U*() const
{
    return (U*)to_pointer();
}

// <char> 0x00924060 (.h:700)
template<typename T>
et_sized_native_pointer<T>& et_sized_native_pointer<T>::operator=(T* const value)
{
    from_pointer(value);
    return *this;
}

// <const vehicle_component_data::hinge> 0x00B6F180 (.h:612)
template<typename T>
T* et_sized_native_pointer<T>::operator->() const
{
    return to_pointer();
}

// <et_sized_native_pointer<rfg_base_destroyable_shape>> 0x00924090 (.h:666)
template<typename T>
T& et_sized_native_pointer<T>::operator[](int i) const
{
    return to_pointer()[i];
}

// <vfx_effect_template::emitter> 0x005D67B0 (.h:660)
template<typename T>
T& et_sized_native_pointer<T>::operator[](int i)
{
    return to_pointer()[i];
}

// <vehicle_mesh_light> 0x00B6F150 (.h:654)
template<typename T>
T& et_sized_native_pointer<T>::operator[](unsigned int i) const
{
    return to_pointer()[i];
}

// <unsigned int> 0x00B6F100 (.h:648)
template<typename T>
T& et_sized_native_pointer<T>::operator[](unsigned int i)
{
    return to_pointer()[i];
}
