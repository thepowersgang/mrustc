/*
 * mrustc Standalone MIRI
 * - by John Hodge (Mutabah)
 *
 * value.hpp
 * - Runtime values
 */
#pragma once

#include <vector>
#include <memory>
#include <cstdint>
#include <cstring>	// memcpy
#include <cassert>

namespace HIR {
    struct TypeRef;
    struct Path;
}
class Allocation;
struct Value;
struct ValueRef;

struct FFIPointer
{
    const char* source_function;
    void*   ptr_value;
    size_t  size;
};

class AllocationHandle
{
    friend class Allocation;
    friend class RelocationPtr;
    Allocation* m_ptr;

private:
    AllocationHandle(Allocation* p):
        m_ptr(p)
    {
    }
public:
    AllocationHandle(): m_ptr(nullptr) {}
    AllocationHandle(AllocationHandle&& x): m_ptr(x.m_ptr) {
        x.m_ptr = nullptr;
    }
    AllocationHandle(const AllocationHandle& x);
    ~AllocationHandle();

    AllocationHandle& operator=(const AllocationHandle& x) = delete;
    AllocationHandle& operator=(AllocationHandle&& x) {
        this->~AllocationHandle();
        this->m_ptr = x.m_ptr;
        x.m_ptr = nullptr;
        return *this;
    }

    operator bool() const { return m_ptr != 0; }
    const Allocation& operator*() const { assert(m_ptr); return *m_ptr; }
          Allocation& operator*()       { assert(m_ptr); return *m_ptr; }
    const Allocation* operator->() const { assert(m_ptr); return m_ptr; }
          Allocation* operator->()       { assert(m_ptr); return m_ptr; }
};

// TODO: Split into RelocationPtr and AllocationHandle
class RelocationPtr
{
    void* m_ptr;

public:
    enum class Ty
    {
        Allocation,
        Function,   // m_ptr is a pointer to the function.
        StdString,
        FfiPointer,
    };

    RelocationPtr(): m_ptr(nullptr) {}
    RelocationPtr(RelocationPtr&& x): m_ptr(x.m_ptr) {
        x.m_ptr = nullptr;
    }
    RelocationPtr(const RelocationPtr& x);
    ~RelocationPtr();
    static RelocationPtr new_alloc(AllocationHandle h);
    static RelocationPtr new_fcn(::HIR::Path p);
    static RelocationPtr new_string(const ::std::string* s);    // NOTE: The string must have a stable pointer
    static RelocationPtr new_ffi(FFIPointer info);

    RelocationPtr& operator=(const RelocationPtr& x) = delete;
    RelocationPtr& operator=(RelocationPtr&& x) {
        this->~RelocationPtr();
        this->m_ptr = x.m_ptr;
        x.m_ptr = nullptr;
        return *this;
    }

    size_t get_size() const;

    operator bool() const { return m_ptr != 0; }
    bool is_alloc() const {
        return *this && get_ty() == Ty::Allocation;
    }
    Allocation& alloc() {
        assert(*this);
        assert(get_ty() == Ty::Allocation);
        return *static_cast<Allocation*>(get_ptr());
    }
    const Allocation& alloc() const {
        assert(*this);
        assert(get_ty() == Ty::Allocation);
        return *static_cast<Allocation*>(get_ptr());
    }
    const ::HIR::Path& fcn() const {
        assert(*this);
        assert(get_ty() == Ty::Function);
        return *static_cast<const ::HIR::Path*>(get_ptr());
    }
    const ::std::string& str() const {
        assert(*this);
        assert(get_ty() == Ty::StdString);
        return *static_cast<const ::std::string*>(get_ptr());
    }
    const FFIPointer& ffi() const {
        assert(*this);
        assert(get_ty() == Ty::FfiPointer);
        return *static_cast<const FFIPointer*>(get_ptr());
    }

    Ty get_ty() const {
        return static_cast<Ty>( reinterpret_cast<uintptr_t>(m_ptr) & 3 );
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const RelocationPtr& x);
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
    RelocationPtr   backing_alloc;
};

// TODO: Split write and read
struct ValueCommonRead
{
    virtual RelocationPtr get_relocation(size_t ofs) const = 0;
    virtual void read_bytes(size_t ofs, void* dst, size_t count) const = 0;

    uint8_t read_u8(size_t ofs) const { uint8_t rv; read_bytes(ofs, &rv, 1); return rv; }
    uint16_t read_u16(size_t ofs) const { uint16_t rv; read_bytes(ofs, &rv, 2); return rv; }
    uint32_t read_u32(size_t ofs) const { uint32_t rv; read_bytes(ofs, &rv, 4); return rv; }
    uint64_t read_u64(size_t ofs) const { uint64_t rv; read_bytes(ofs, &rv, 8); return rv; }
    int8_t read_i8(size_t ofs) const { return static_cast<int8_t>(read_u8(ofs)); }
    int16_t read_i16(size_t ofs) const { return static_cast<int16_t>(read_u16(ofs)); }
    int32_t read_i32(size_t ofs) const { return static_cast<int32_t>(read_u32(ofs)); }
    int64_t read_i64(size_t ofs) const { return static_cast<int64_t>(read_u64(ofs)); }
    float  read_f32(size_t ofs) const { float rv; read_bytes(ofs, &rv, 4); return rv; }
    double read_f64(size_t ofs) const { double rv; read_bytes(ofs, &rv, 8); return rv; }
    uint64_t read_usize(size_t ofs) const;
    int64_t read_isize(size_t ofs) const { return static_cast<int64_t>(read_usize(ofs)); }

    /// Read a pointer from the value, requiring at least `req_valid` valid bytes, saves avaliable space in `size`
    void* read_pointer_unsafe(size_t rd_ofs, size_t req_valid, size_t& size, bool& is_mut) const;
    /// Read a pointer, requiring `req_len` valid bytes
    const void* read_pointer_const(size_t rd_ofs, size_t req_len) const {
        size_t  tmp;
        bool is_mut;
        return read_pointer_unsafe(rd_ofs, req_len, tmp, is_mut);
    }
    /// Read a pointer, not requiring that the target be initialised
    void* read_pointer_uninit(size_t rd_ofs, size_t& out_size) {
        bool is_mut;
        void* rv = read_pointer_unsafe(rd_ofs, 0, out_size, is_mut);
        if(!is_mut)
            throw "";
            //LOG_FATAL("Attempting to get an uninit pointer to immutable data");
        return rv;
    }
    /// Read a pointer and return a ValueRef to it (mutable data)
    ValueRef read_pointer_valref_mut(size_t rd_ofs, size_t size);
};
struct ValueCommonWrite:
    public ValueCommonRead
{
    virtual void write_bytes(size_t ofs, const void* src, size_t count) = 0;

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
    void write_usize(size_t ofs, uint64_t v);
    void write_isize(size_t ofs, int64_t v) { write_usize(ofs, static_cast<uint64_t>(v)); }
    virtual void write_ptr(size_t ofs, size_t ptr_ofs, RelocationPtr reloc) = 0;
};

class Allocation:
    public ValueCommonWrite
{
    friend class AllocationHandle;
    size_t  refcount;
    // TODO: Read-only flag?
    bool is_freed = false;
public:
    static AllocationHandle new_alloc(size_t size);

    const uint8_t* data_ptr() const { return reinterpret_cast<const uint8_t*>(this->data.data()); }
          uint8_t* data_ptr()       { return reinterpret_cast<      uint8_t*>(this->data.data()); }
    size_t size() const { return this->data.size() * 8; }

    ::std::vector<uint64_t> data;
    ::std::vector<uint8_t> mask;
    ::std::vector<Relocation>   relocations;

    RelocationPtr get_relocation(size_t ofs) const override {
        for(const auto& r : relocations) {
            if(r.slot_ofs == ofs)
                return r.backing_alloc;
        }
        return RelocationPtr();
    }
    void mark_as_freed() {
        is_freed = true;
        relocations.clear();
        for(auto& v : mask)
            v = 0;
    }

    void resize(size_t new_size);

    void check_bytes_valid(size_t ofs, size_t size) const;
    void mark_bytes_valid(size_t ofs, size_t size);

    Value read_value(size_t ofs, size_t size) const;
    void read_bytes(size_t ofs, void* dst, size_t count) const override;

    void write_value(size_t ofs, Value v);
    void write_bytes(size_t ofs, const void* src, size_t count) override;
    void write_ptr(size_t ofs, size_t ptr_ofs, RelocationPtr reloc) override;
};
extern ::std::ostream& operator<<(::std::ostream& os, const Allocation& x);

struct Value:
    public ValueCommonWrite
{
    // If NULL, data is direct
    AllocationHandle    allocation;
    struct {
        // NOTE: Can't pack the mask+size tighter, need 4 bits of size (8-15) leaving 12 bits of mask
        uint8_t data[2*8-3];   // 13 data bytes, plus 16bit mask, plus size = 16 bytes
        uint8_t mask[2];
        uint8_t size;
    } direct_data;

    Value();
    Value(::HIR::TypeRef ty);

    static Value with_size(size_t size, bool have_allocation);
    static Value new_fnptr(const ::HIR::Path& fn_path);
    static Value new_ffiptr(FFIPointer ffi);
    static Value new_pointer(::HIR::TypeRef ty, uint64_t v, RelocationPtr r);
    static Value new_usize(uint64_t v);
    static Value new_isize(int64_t v);
    static Value new_u32(uint32_t v);
    static Value new_i32(int32_t v);

    void create_allocation();
    size_t size() const { return allocation ? allocation->size() : direct_data.size; }
    const uint8_t* data_ptr() const { return allocation ? allocation->data_ptr() : direct_data.data; }
          uint8_t* data_ptr()       { return allocation ? allocation->data_ptr() : direct_data.data; }

    RelocationPtr get_relocation(size_t ofs) const override {
        if( this->allocation && this->allocation )
            return this->allocation->get_relocation(ofs);
        else
            return RelocationPtr();
    }

    void check_bytes_valid(size_t ofs, size_t size) const;
    void mark_bytes_valid(size_t ofs, size_t size);

    Value read_value(size_t ofs, size_t size) const;
    void read_bytes(size_t ofs, void* dst, size_t count) const override;

    void write_value(size_t ofs, Value v);
    void write_bytes(size_t ofs, const void* src, size_t count) override;

    void write_ptr(size_t ofs, size_t ptr_ofs, RelocationPtr reloc) override;
};
extern ::std::ostream& operator<<(::std::ostream& os, const Value& v);

// A read-only reference to a value (to write, you have to go through it)
struct ValueRef:
    public ValueCommonRead
{
    // Either an AllocationHandle, or a Value pointer
    RelocationPtr   m_alloc;
    Value*  m_value;
    size_t  m_offset;   // Offset within the value
    size_t  m_size; // Size in bytes of the referenced value
    ::std::shared_ptr<Value>    m_metadata;

    ValueRef(RelocationPtr ptr, size_t ofs, size_t size):
        m_alloc(ptr),
        m_value(nullptr),
        m_offset(ofs),
        m_size(size)
    {
        if( m_alloc )
        {
            switch(m_alloc.get_ty())
            {
            case RelocationPtr::Ty::Allocation:
                assert(ofs < m_alloc.alloc().size());
                assert(size <= m_alloc.alloc().size());
                assert(ofs+size <= m_alloc.alloc().size());
                break;
            case RelocationPtr::Ty::StdString:
                assert(ofs < m_alloc.str().size());
                assert(size <= m_alloc.str().size());
                assert(ofs+size <= m_alloc.str().size());
                break;
            case RelocationPtr::Ty::FfiPointer:
                assert(ofs < m_alloc.ffi().size);
                assert(size <= m_alloc.ffi().size);
                assert(ofs+size <= m_alloc.ffi().size);
                break;
            default:
                throw "TODO";
            }
        }
    }
    ValueRef(Value& val):
        ValueRef(val, 0, val.size())
    {
    }
    ValueRef(Value& val, size_t ofs, size_t size):
        m_value(&val),
        m_offset(ofs),
        m_size(size)
    {
    }

    RelocationPtr get_relocation(size_t ofs) const override {
        if(m_alloc)
        {
            if( m_alloc.is_alloc() )
                return m_alloc.alloc().get_relocation(ofs);
            else
                return RelocationPtr();
        }
        else if( m_value )
        {
            return m_value->get_relocation(ofs);
        }
        else
        {
            return RelocationPtr();
        }
    }
    Value read_value(size_t ofs, size_t size) const;
    const uint8_t* data_ptr() const {
        if( m_alloc ) {
            switch(m_alloc.get_ty())
            {
            case RelocationPtr::Ty::Allocation:
                return m_alloc.alloc().data_ptr() + m_offset;
                break;
            case RelocationPtr::Ty::StdString:
                return reinterpret_cast<const uint8_t*>(m_alloc.str().data() + m_offset);
            default:
                throw "TODO";
            }
        }
        else if( m_value ) {
            return m_value->data_ptr() + m_offset;
        }
        else {
            return nullptr;
        }
    }
    void read_bytes(size_t ofs, void* dst, size_t size) const {
        if( size == 0 )
            return ;
        assert(ofs < m_size);
        assert(size <= m_size);
        assert(ofs+size <= m_size);
        if( m_alloc ) {
            switch(m_alloc.get_ty())
            {
            case RelocationPtr::Ty::Allocation:
                m_alloc.alloc().read_bytes(m_offset + ofs, dst, size);
                break;
            case RelocationPtr::Ty::StdString:
                assert(m_offset+ofs <= m_alloc.str().size() && size <= m_alloc.str().size() && m_offset+ofs+size <= m_alloc.str().size());
                ::std::memcpy(dst, m_alloc.str().data() + m_offset + ofs, size);
                break;
            default:
                //ASSERT_BUG(m_alloc.is_alloc(), "read_value on non-data backed Value - " << );
                throw "TODO";
            }
        }
        else {
            m_value->read_bytes(m_offset + ofs, dst, size);
        }
    }
    void check_bytes_valid(size_t ofs, size_t size) const {
        if( size == 0 )
            return ;
        assert(ofs < m_size);
        assert(size <= m_size);
        assert(ofs+size <= m_size);
        if( m_alloc ) {
            switch(m_alloc.get_ty())
            {
            case RelocationPtr::Ty::Allocation:
                m_alloc.alloc().check_bytes_valid(m_offset + ofs, size);
                break;
            case RelocationPtr::Ty::StdString:
                assert(m_offset+ofs <= m_alloc.str().size() && size <= m_alloc.str().size() && m_offset+ofs+size <= m_alloc.str().size());
                break;
            default:
                //ASSERT_BUG(m_alloc.is_alloc(), "read_value on non-data backed Value - " << );
                throw "TODO";
            }
        }
        else {
            m_value->check_bytes_valid(m_offset + ofs, size);
        }
    }

    bool compare(const void* other, size_t other_len) const;
};
extern ::std::ostream& operator<<(::std::ostream& os, const ValueRef& v);
