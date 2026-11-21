// Small fixed-size array that lives on the stack (no heap allocation) for up to N elements, and falls back to a
// std::vector above that. Replaces per-frame std::vector temporaries in the batching code (same behaviour).
#pragma once

#include <vector>

template <class T, int N = 64>
class rl_small_array
{
public:
    rl_small_array(int count, const T& init)
    {
        m_count = count > 0 ? count : 0;
        if (m_count <= N)
        {
            for (int i = 0; i < m_count; i++)
            {
                m_stack[i] = init;
            }
            m_ptr = m_stack;
        }
        else
        {
            m_heap.assign(m_count, init);
            m_ptr = m_heap.data();
        }
    }
    rl_small_array(const rl_small_array& other)
        : m_count(other.m_count)
        , m_heap(other.m_heap)
    {
        for (int i = 0; i < N; i++)
        {
            m_stack[i] = other.m_stack[i];
        }
        m_ptr = m_count <= N ? m_stack : m_heap.data();
    }
    rl_small_array& operator=(const rl_small_array&) = delete;

    T* data() { return m_ptr; }
    const T* data() const { return m_ptr; }
    T& operator[](int i) { return m_ptr[i]; }
    const T& operator[](int i) const { return m_ptr[i]; }

private:
    int m_count;
    T* m_ptr;
    std::vector<T> m_heap;
    T m_stack[N];
};
