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

#include "debug.hpp"
#include "u128.hpp"

namespace HIR {
    struct TypeRef;
    struct Path;
}
class Allocation;
struct Value;
struct ValueRef;

struct FfiLayout
{
    struct Range {
        size_t  len;
        bool    is_valid;
        bool    is_writable;
    };
    ::std::vector<Range>    ranges;

    static FfiLayout new_const_bytes(size_t s);

    size_t get_size() const {
        size_t rv = 0;
        for(const auto& r : ranges)
            rv += r.len;
        return rv;
    }
    bool is_valid_read(size_t o, size_t s) const;
};
struct FFIPointer
{
    // FFI pointers require the following:
    // - A tag indicating where they're valid/from
    // - A data format (e.g. size of allocation, internal data format)
    //   - If the data format is unspecified (null) then it's a void pointer
    // - An actual pointer
    // TODO: Add extra metadata

    // Pointer value, returned by the FFI
    void*   ptr_value;
    // Tag name, used for validty checking by FFI hooks
    const char* tag_name;
    ::std::shared_ptr<FfiLayout>    layout;

    static FFIPointer new_void(const char* name, const void* v) {
        return FFIPointer { const_cast<void*>(v), name, ::std::make_shared<FfiLayout>() };
    }
    static FFIPointer new_const_bytes(const char* name, const void* s, size_t size) {
        return FFIPointer { const_cast<void*>(s), name, ::std::make_shared<FfiLayout>(FfiLayout::new_const_bytes(size)) };
    };

    size_t get_size() const {
        return (layout ? layout->get_size() : 0);
    }
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
    static RelocationPtr new_fcn(::HIR::Path p);    // TODO: What if it's a FFI function? Could be encoded in here.
    static RelocationPtr new_string(const ::std::string* s);    // NOTE: The string must have a stable pointer
    static RelocationPtr new_ffi(FFIPointer info);

    RelocationPtr& operator=(const RelocationPtr& x) = delete;
    RelocationPtr& operator=(RelocationPtr&& x) {
        this->~RelocationPtr();
        this->m_ptr = x.m_ptr;
        x.m_ptr = nullptr;
        return *this;
    }

    bool operator==(const RelocationPtr& x) const {
        return m_ptr == x.m_ptr;
    }
    bool operator!=(const RelocationPtr& x) const {
        return !(*this == x);
    }
    bool operator<(const RelocationPtr& x) const {
        // HACK: Just compare the pointers (won't be predictable... but should be stable)
        return m_ptr < x.m_ptr;
    }

    size_t get_size() const;
    size_t get_base() const;

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
    U128 read_u128(size_t ofs) const { U128 rv; read_bytes(ofs, &rv, 16); return rv; }
    int8_t read_i8(size_t ofs) const { return static_cast<int8_t>(read_u8(ofs)); }
    int16_t read_i16(size_t ofs) const { return static_cast<int16_t>(read_u16(ofs)); }
    int32_t read_i32(size_t ofs) const { return static_cast<int32_t>(read_u32(ofs)); }
    int64_t read_i64(size_t ofs) const { return static_cast<int64_t>(read_u64(ofs)); }
    I128 read_i128(size_t ofs) const { I128 rv; read_bytes(ofs, &rv, 16); return rv; }
    float  read_f32(size_t ofs) const { float rv; read_bytes(ofs, &rv, 4); return rv; }
    double read_f64(size_t ofs) const { double rv; read_bytes(ofs, &rv, 8); return rv; }
    uint64_t read_usize(size_t ofs) const;
    int64_t read_isize(size_t ofs) const { return static_cast<int64_t>(read_usize(ofs)); }

    /// De-reference a pointer (of target type `ty`) at the given offset, and return a reference to it
    ValueRef deref(size_t ofs, const ::HIR::TypeRef& ty);

    bool read_ptr_ofs(size_t ofs, size_t& v, RelocationPtr& reloc) const {
        reloc = get_relocation(ofs);
        v = read_usize(ofs);
        if(reloc)
        {
            auto base = reloc.get_base();
            auto size = reloc.get_size();
            if(v < base)
                return false;
            v -= base;
            // TODO: Check size?
        }
        return true;
    }

    /// Read a pointer that should be a function pointer
    const ::HIR::Path& read_pointer_fcn(size_t rd_ofs) const;
    /// Read a pointer that must be FFI with the specified tag (or NULL)
    void* read_pointer_tagged_null(size_t rd_ofs, const char* tag) const;
    /// Read a pointer that must be FFI with the specified tag (cannot be NULL)
    void* read_pointer_tagged_nonnull(size_t rd_ofs, const char* tag) const {
        auto rv = read_pointer_tagged_null(rd_ofs, tag);
        if(!rv)
            LOG_FATAL("Accessing NUL pointer");
        return rv;
    }
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
            LOG_FATAL("Attempting to get an uninit pointer to immutable data");
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
    void write_u128(size_t ofs, U128 v) { write_bytes(ofs, &v, 16); }
    void write_i8 (size_t ofs, int8_t  v) { write_u8 (ofs, static_cast<uint8_t >(v)); }
    void write_i16(size_t ofs, int16_t v) { write_u16(ofs, static_cast<uint16_t>(v)); }
    void write_i32(size_t ofs, int32_t v) { write_u32(ofs, static_cast<uint32_t>(v)); }
    void write_i64(size_t ofs, int64_t v) { write_u64(ofs, static_cast<uint64_t>(v)); }
    void write_i128(size_t ofs, I128 v) { write_bytes(ofs, &v, 16); }
    void write_f32(size_t ofs, float  v) { write_bytes(ofs, &v, 4); }
    void write_f64(size_t ofs, double v) { write_bytes(ofs, &v, 8); }
    void write_usize(size_t ofs, uint64_t v);
    void write_isize(size_t ofs, int64_t v) { write_usize(ofs, static_cast<uint64_t>(v)); }
    virtual void write_ptr(size_t ofs, size_t ptr_ofs, RelocationPtr reloc) = 0;
    void write_ptr_ofs(size_t ofs, size_t ptr_ofs, RelocationPtr reloc) {
        ptr_ofs += reloc.get_base();
        write_ptr(ofs, ptr_ofs, std::move(reloc));
    }
};

class Allocation:
    public ValueCommonWrite
{
    friend class AllocationHandle;

    static uint64_t s_next_index;

    ::std::string   m_tag;
    size_t  refcount;
    size_t  m_size;
    uint64_t m_index;
    // TODO: Read-only flag?
    bool is_freed = false;

    ::std::vector<uint64_t> m_data;
public:
    ::std::vector<uint8_t> m_mask;
    ::std::vector<Relocation>   relocations;
public:
    virtual ~Allocation() {}
    static AllocationHandle new_alloc(size_t size, ::std::string tag);

    const uint8_t* data_ptr() const { return reinterpret_cast<const uint8_t*>(this->m_data.data()); }
          uint8_t* data_ptr()       { return reinterpret_cast<      uint8_t*>(this->m_data.data()); }
    size_t size() const { return m_size; }
    const ::std::string& tag() const { return m_tag; }

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
        for(auto& v : m_mask)
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

    void set_reloc(size_t ofs, size_t len, RelocationPtr reloc);
    friend ::std::ostream& operator<<(::std::ostream& os, const Allocation* x);
};
extern ::std::ostream& operator<<(::std::ostream& os, const Allocation& x);

// TODO: Rename to be `StackSlot`?
struct Value:
    public ValueCommonWrite
{
//private:
    union Inner {
        bool    is_alloc;
        struct Alloc {
            bool    is_alloc;
            AllocationHandle    alloc;

            Alloc(AllocationHandle alloc):
                is_alloc(true),
                alloc(::std::move(alloc))
            {
            }
        } alloc;
        // Sizeof = 4+8+8 = 20b (plus vtable?)
        struct Direct {
            bool    is_alloc;
            uint8_t size;
            uint8_t mask[2];
            uint8_t data[2*8];
            RelocationPtr   reloc_0;    // Relocation only for 0+POINTER_SIZE

            Direct(size_t size):
                is_alloc(false),
                size(static_cast<uint8_t>(size)),
                mask{0,0}
            {
            }
        } direct;

        Inner():
            direct(0)
        {
        }
        ~Inner() {
            if(is_alloc) {
                alloc.~Alloc();
            }
            else {
                direct.~Direct();
            }
        }
        Inner(const Inner& x) {
            if(x.is_alloc) {
                new(&this->alloc) Alloc(x.alloc);
            }
            else {
                new(&this->direct) Direct(x.direct);
            }
        }
        Inner(Inner&& x) {
            if(x.is_alloc) {
                new(&this->alloc) Alloc(::std::move(x.alloc));
            }
            else {
                new(&this->direct) Direct(::std::move(x.direct));
            }
        }
        Inner& operator=(Inner&& x) {
            this->~Inner();
            new(this) Inner(x);
            return *this;
        }
        Inner& operator=(const Inner& x) {
            this->~Inner();
            new(this) Inner(x);
            return *this;
        }
    } m_inner;

public:
    Value();
    Value(::HIR::TypeRef ty);
    ~Value() {
    }

    Value(const Value&) = default;
    Value(Value&&) = default;
    Value& operator=(const Value& x) = delete;
    Value& operator=(Value&& x) = default;

    static Value with_size(size_t size, bool have_allocation);
    static Value new_fnptr(const ::HIR::Path& fn_path);
    static Value new_ffiptr(FFIPointer ffi);
    static Value new_pointer_ofs(::HIR::TypeRef ty, uint64_t ofs, RelocationPtr r);
    static Value new_pointer(::HIR::TypeRef ty, uint64_t v, RelocationPtr r);
    static Value new_usize(uint64_t v);
    static Value new_isize(int64_t v);
    static Value new_u32(uint32_t v);
    static Value new_i32(int32_t v);
    static Value new_i64(int64_t v);

    AllocationHandle borrow(::std::string loc) {
        if( !m_inner.is_alloc )
            create_allocation(/*loc*/);
        return m_inner.alloc.alloc;
    }
    void ensure_allocation() {
        if( !m_inner.is_alloc )
            create_allocation();
    }
    void create_allocation();
    size_t size() const { return m_inner.is_alloc ? m_inner.alloc.alloc->size() : m_inner.direct.size; }
    const uint8_t* data_ptr() const { return m_inner.is_alloc ? m_inner.alloc.alloc->data_ptr() : m_inner.direct.data; }
          uint8_t* data_ptr()       { return m_inner.is_alloc ? m_inner.alloc.alloc->data_ptr() : m_inner.direct.data; }
    const uint8_t* get_mask() const { return m_inner.is_alloc ? m_inner.alloc.alloc->m_mask.data() : m_inner.direct.mask; }
          uint8_t* get_mask_mut()   { return m_inner.is_alloc ? m_inner.alloc.alloc->m_mask.data() : m_inner.direct.mask; }

    RelocationPtr get_relocation(size_t ofs) const override {
        if( m_inner.is_alloc )
            return m_inner.alloc.alloc->get_relocation(ofs);
        else if(ofs == 0)
        {
            return m_inner.direct.reloc_0;
        }
        else
        {
            return RelocationPtr();
        }
    }
    void set_reloc(size_t ofs, size_t size, RelocationPtr p) /*override*/ {
        if( m_inner.is_alloc )
        {
            m_inner.alloc.alloc->set_reloc(ofs, size, ::std::move(p));
        }
        else if( ofs == 0 /*&& size == POINTER_SIZE*/ )
        {
            m_inner.direct.reloc_0 = ::std::move(p);
        }
        else
        {
            this->create_allocation();
            assert( m_inner.is_alloc );
            m_inner.alloc.alloc->set_reloc(ofs, size, ::std::move(p));
        }
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

    static bool in_bounds(size_t ofs, size_t size, size_t max_size) {
        if( size == 0 ) {
            return ofs <= max_size;
        }
        if( ofs > 0 && !(ofs < max_size) )
            return false;
        if( !(size <= max_size) )
            return false;
        return ofs + size <= max_size;
    }

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
                if( !in_bounds(ofs, size, m_alloc.alloc().size()) )
                {
                    LOG_NOTICE("ValueRef exceeds bounds of " << m_alloc << " - " << ofs << "+" << size << " > " << m_alloc.alloc().size());
                }
                break;
            case RelocationPtr::Ty::StdString:
                if( !in_bounds(ofs, size, m_alloc.str().size()) )
                {
                    LOG_NOTICE("ValueRef exceeds bounds of string - " << ofs << "+" << size << " > " << m_alloc.str().size());
                }
                break;
            case RelocationPtr::Ty::FfiPointer:
                if( !in_bounds(ofs, size, m_alloc.ffi().get_size()) )
                {
                    LOG_NOTICE("ValueRef exceeds bounds of FFI buffer - " << ofs << "+" << size << " > " << m_alloc.ffi().get_size());
                }
                break;
            case RelocationPtr::Ty::Function:
                LOG_TODO("");
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
                return m_alloc.alloc().get_relocation(m_offset + ofs);
            else
                return RelocationPtr();
        }
        else if( m_value )
        {
            return m_value->get_relocation(m_offset + ofs);
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
                LOG_TODO("");
            }
        }
        else if( m_value ) {
            return m_value->data_ptr() + m_offset;
        }
        else {
            return nullptr;
        }
    }

    // TODO: Remove these two (move to a helper?)
    uint8_t* data_ptr_mut() {
        if( m_alloc ) {
            switch(m_alloc.get_ty())
            {
            case RelocationPtr::Ty::Allocation:
                return m_alloc.alloc().data_ptr() + m_offset;
                break;
            default:
                LOG_TODO("");
            }
        }
        else if( m_value ) {
            return m_value->data_ptr() + m_offset;
        }
        else {
            return nullptr;
        }
    }
    void mark_bytes_valid(size_t ofs, size_t size);

    void read_bytes(size_t ofs, void* dst, size_t size) const {
        if( size == 0 )
            return ;
        LOG_ASSERT(in_bounds(ofs, size, m_size), "read_bytes(" << ofs << "+" << size << " > " << m_size <<")");
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
                LOG_TODO("");
            }
        }
        else {
            m_value->read_bytes(m_offset + ofs, dst, size);
        }
    }
    void check_bytes_valid(size_t ofs, size_t size) const {
        if( size == 0 )
            return ;
        LOG_ASSERT(in_bounds(ofs, size, m_size), "check_bytes_valid(" << ofs << "+" << size << " > " << m_size <<")");
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
                LOG_TODO("");
            }
        }
        else {
            m_value->check_bytes_valid(m_offset + ofs, size);
        }
    }

    bool compare(size_t offset, const void* other, size_t other_len) const;
};
extern ::std::ostream& operator<<(::std::ostream& os, const ValueRef& v);
//struct ValueRefMut:
//    public ValueCommonWrite
//{
//};
