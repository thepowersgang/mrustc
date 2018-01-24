//
//
//
#pragma once

#include <vector>
#include <memory>
#include <cstdint>

namespace HIR {
    struct TypeRef;
}
class Allocation;

class AllocationPtr
{
    friend class Allocation;
    Allocation* m_ptr;
public:
    AllocationPtr(): m_ptr(nullptr) {}

    operator bool() const { return m_ptr != 0; }
    Allocation& operator*() { return *m_ptr; }
    Allocation* operator->() { return m_ptr; }
};
struct Relocation
{
    size_t  slot_ofs;
    AllocationPtr   backing_alloc;
};
class Allocation
{
    size_t  refcount;
public:
    ::std::vector<uint64_t> data;
    ::std::vector<Relocation>   relocations;
};

struct Value
{
    // If NULL, data is direct
    AllocationPtr   allocation;
    union {
        struct {
            size_t  size;
            size_t  offset;
        } indirect_meta;
        struct {
            uint8_t data[2*sizeof(size_t)-3];   // 16-3 = 13, fits in 16 bits of mask
            uint8_t mask[2];
            uint8_t size;
        } direct_data;
    } meta;

    Value();
    Value(::HIR::TypeRef ty);

    void check_bytes_valid(size_t ofs, size_t size) const;
    void mark_bytes_valid(size_t ofs, size_t size);

    Value read_value(size_t ofs, size_t size) const;
    void read_bytes(size_t ofs, void* dst, size_t count) const;

    void write_value(size_t ofs, Value v);
    void write_bytes(size_t ofs, const void* src, size_t count);

    size_t as_usize() const;
};
extern ::std::ostream& operator<<(::std::ostream& os, const Value& v);
