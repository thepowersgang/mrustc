//
//
//
#pragma once

#include <vector>
#include <memory>
#include <cstdint>
#include <cassert>

namespace HIR {
    struct TypeRef;
    struct Path;
}
class Allocation;


class AllocationPtr
{
    friend class Allocation;
    void* m_ptr;
public:

    enum class Ty
    {
        Allocation,
        Function,   // m_ptr is a pointer to the function.
        Unused1,
        Unused2,
    };

private:
    AllocationPtr(Allocation* p):
        m_ptr(p)
    {
    }
public:
    AllocationPtr(): m_ptr(nullptr) {}
    AllocationPtr(AllocationPtr&& x): m_ptr(x.m_ptr) {
        x.m_ptr = nullptr;
    }
    AllocationPtr(const AllocationPtr& x);
    ~AllocationPtr();
    static AllocationPtr new_fcn(::HIR::Path p);

    AllocationPtr& operator=(const AllocationPtr& x) = delete;
    AllocationPtr& operator=(AllocationPtr&& x) {
        this->~AllocationPtr();
        this->m_ptr = x.m_ptr;
        x.m_ptr = nullptr;
        return *this;
    }

    operator bool() const { return m_ptr != 0; }
    bool is_alloc() {
        return *this && get_ty() == Ty::Allocation;
    }
    Allocation& alloc() {
        assert(get_ty() == Ty::Allocation);
        return *static_cast<Allocation*>(get_ptr());
    }
    const Allocation& alloc() const {
        assert(get_ty() == Ty::Allocation);
        return *static_cast<Allocation*>(get_ptr());
    }
    const ::HIR::Path& fcn() const {
        assert(get_ty() == Ty::Function);
        return *static_cast<const ::HIR::Path*>(get_ptr());
    }

    Ty get_ty() const {
        return static_cast<Ty>( reinterpret_cast<uintptr_t>(m_ptr) & 3 );
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const AllocationPtr& x);
private:
    void* get_ptr() const {
        return reinterpret_cast<void*>( reinterpret_cast<uintptr_t>(m_ptr) & ~3 );
    }
};
struct Relocation
{
    // Offset within parent allocation where this relocation is performed.
    // TODO: Size?
    size_t  slot_ofs;
    AllocationPtr   backing_alloc;
};
class Allocation
{
    friend class AllocationPtr;
    size_t  refcount;
    // TODO: Read-only flag?
public:
    static AllocationPtr new_alloc(size_t size);

    const uint8_t* data_ptr() const { return reinterpret_cast<const uint8_t*>(this->data.data()); }
          uint8_t* data_ptr()       { return reinterpret_cast<      uint8_t*>(this->data.data()); }

    ::std::vector<uint64_t> data;
    ::std::vector<uint8_t> mask;
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
    static Value new_fnptr(const ::HIR::Path& fn_path);

    void check_bytes_valid(size_t ofs, size_t size) const;
    void mark_bytes_valid(size_t ofs, size_t size);

    Value read_value(size_t ofs, size_t size) const;
    void read_bytes(size_t ofs, void* dst, size_t count) const;

    void write_value(size_t ofs, Value v);
    void write_bytes(size_t ofs, const void* src, size_t count);

    uint8_t read_u8(size_t ofs) const { uint8_t rv; read_bytes(ofs, &rv, 1); return rv; }
     int8_t read_i8(size_t ofs) const { return static_cast<int8_t>(read_u8(ofs)); }
    uint16_t read_u16(size_t ofs) const { uint16_t rv; read_bytes(ofs, &rv, 2); return rv; }
     int16_t read_i16(size_t ofs) const { return static_cast<int16_t>(read_u16(ofs)); }
    uint32_t read_u32(size_t ofs) const { uint32_t rv; read_bytes(ofs, &rv, 4); return rv; }
     int32_t read_i32(size_t ofs) const { return static_cast<int32_t>(read_u32(ofs)); }
    uint64_t read_u64(size_t ofs) const { uint64_t rv; read_bytes(ofs, &rv, 8); return rv; }
     int64_t read_i64(size_t ofs) const { return static_cast<int64_t>(read_u64(ofs)); }
    float  read_f32(size_t ofs) const { float rv; read_bytes(ofs, &rv, 4); return rv; }
    double read_f64(size_t ofs) const { double rv; read_bytes(ofs, &rv, 8); return rv; }
    uint64_t read_usize(size_t ofs) const;
     int64_t read_isize(size_t ofs) const { return static_cast<int64_t>(read_usize(ofs)); }

    void write_u8 (size_t ofs, uint8_t  v) { write_bytes(ofs, &v, 1); }
    void write_u16(size_t ofs, uint16_t v) { write_bytes(ofs, &v, 2); }
    void write_u32(size_t ofs, uint32_t v) { write_bytes(ofs, &v, 4); }
    void write_u64(size_t ofs, uint64_t v) { write_bytes(ofs, &v, 8); }
    void write_i8 (size_t ofs, int8_t  v) { write_u8 (ofs, static_cast<uint8_t >(v)); }
    void write_i16(size_t ofs, int16_t v) { write_u16(ofs, static_cast<uint16_t>(v)); }
    void write_i32(size_t ofs, int32_t v) { write_u32(ofs, static_cast<uint32_t>(v)); }
    void write_i64(size_t ofs, int64_t v) { write_u64(ofs, static_cast<uint64_t>(v)); }
    void write_f32(size_t ofs, float  v) { write_bytes(ofs, &v, 4); }
    void write_f64(size_t ofs, double v) { write_bytes(ofs, &v, 8); }
private:
    const uint8_t* data_ptr() const { return allocation ? allocation.alloc().data_ptr() + meta.indirect_meta.offset : meta.direct_data.data; }
          uint8_t* data_ptr()       { return allocation ? allocation.alloc().data_ptr() + meta.indirect_meta.offset : meta.direct_data.data; }
};
extern ::std::ostream& operator<<(::std::ostream& os, const Value& v);
