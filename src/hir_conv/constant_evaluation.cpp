/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_conv/constant_evaluation.cpp
 * - Minimal (integer only) constant evaluation
 */
#include "main_bindings.hpp"
#include <hir/hir.hpp>
#include <hir/expr.hpp>
#include <hir/visitor.hpp>
#include <algorithm>
#include <mir/mir.hpp>
#include <hir_typeck/common.hpp>    // Monomorph
#include <mir/helpers.hpp>
#include <trans/target.hpp>
#include <hir/expr_state.hpp>
#include <int128.h> // 128 bit integer support

#include "constant_evaluation.hpp"
#include <trans/monomorphise.hpp>   // For handling monomorph of MIR in provided associated constants
#include <trans/codegen.hpp>    // For encoding as part of transmute

namespace {
    struct Defer {};

    struct NewvalState
        : public HIR::Evaluator::Newval
    {
        const ::HIR::Module&   mod;
        const ::HIR::ItemPath&  mod_path;
        ::std::string   name_prefix;
        unsigned int next_item_idx;

        NewvalState(const ::HIR::Module& mod, const ::HIR::ItemPath& mod_path, ::std::string prefix):
            mod(mod),
            mod_path(mod_path),
            name_prefix(prefix),
            next_item_idx(0)
        {
        }

        ::HIR::Path new_static(::HIR::TypeRef type, EncodedLiteral value) override
        {
            ASSERT_BUG(Span(), type != HIR::TypeRef(), "");
            auto name = RcString::new_interned(FMT(name_prefix << next_item_idx));
            next_item_idx ++;
            auto rv = mod_path.get_simple_path() + name.c_str();
            auto s = ::HIR::Static( ::HIR::Linkage(), false, mv$(type), ::HIR::ExprPtr() );
            s.m_value_res = ::std::move(value);
            s.m_value_generated = true;
            s.m_save_literal = true;
            DEBUG(rv << ": " << s.m_type << " = " << s.m_value_res);

            const_cast<::HIR::Module&>(mod).m_inline_statics.push_back( ::std::make_pair( mv$(name), box$(s) ) );
            return rv;
        }
    };

    TAGGED_UNION(EntPtr, NotFound,
        (NotFound, struct{}),
        (Function, const ::HIR::Function*),
        (Static, const ::HIR::Static*),
        (Constant, const ::HIR::Constant*),
        (Struct, const ::HIR::Struct*)
        );
    enum class EntNS {
        //Type,
        Value
    };
}

namespace MIR { namespace eval {
    class Allocation;
    class Constant;
    class StaticRef;
    class RelocPtr;
    template<typename T> class RefCountPtr
    {
        friend class RelocPtr;
    protected:
        T*  m_ptr;
    public:
        ~RefCountPtr() {
            if(m_ptr)
            {
                m_ptr->reference_count -= 1;
                if(m_ptr->reference_count == 0)  {
                    this->dealloc(m_ptr);
                }
            }
            m_ptr = nullptr;
        }
        RefCountPtr(const RefCountPtr& x):
            m_ptr(x.m_ptr)
        {
            if(m_ptr)
            {
                x.m_ptr->reference_count += 1;
            }
        }
        RefCountPtr(RefCountPtr&& x): m_ptr(x.m_ptr) { x.m_ptr = nullptr; }

        RefCountPtr(): m_ptr(nullptr) {}

        operator bool() const { return m_ptr != 0; }
        T* operator->() { return m_ptr; }
        T& operator*() { return *m_ptr; }
    protected:
        void dealloc(T* p); // Note: Should be overridden for each type
    };
    /// "Statically allocated" constant data
    class ConstantPtr final: public RefCountPtr<Constant>
    {
    public:
        static ConstantPtr allocate(const void* data, size_t len);
    };
    /// Mutable allocation
    class AllocationPtr final: public RefCountPtr<Allocation>
    {
    public:
        static AllocationPtr allocate(const ::MIR::TypeResolve& state, const ::HIR::TypeRef& ty);
        static AllocationPtr allocate_ro(const void* data, size_t len);
    };
    /// Reference to a `static`
    class StaticRefPtr final: public RefCountPtr<StaticRef>
    {
    public:
        static StaticRefPtr allocate(::HIR::Path p, const EncodedLiteral* lit);
    };

    /// Common interface for data storage
    class IValue
    {
    public:
        virtual void fmt_ident(std::ostream& os) const = 0;
        virtual void fmt(::std::ostream& os, size_t ofs, size_t len) const = 0;

        virtual size_t size() const = 0;
        virtual const uint8_t* get_bytes(size_t ofs, size_t len, bool check_mask) const = 0;
        virtual void read_mask(uint8_t* dst, size_t dst_ofs, size_t ofs, size_t len) const = 0;

        virtual bool is_writable() const = 0;
        virtual void write_bytes(size_t ofs, const void* data, size_t len) = 0;
        virtual void write_mask_from(size_t ofs, const IValue& src, size_t src_ofs, size_t len) = 0;

        virtual RelocPtr get_reloc(size_t ofs) const = 0;
        virtual void set_reloc(size_t ofs, RelocPtr ptr) = 0;
    };
    /// Pointer wrapping a reference-counted allocation
    class RelocPtr
    {
        uintptr_t   ptr;
        enum Tag {
            TAG_Allocation = 0,
            TAG_Constant,
            TAG_StaticRef,
        };
    public:
        ~RelocPtr();
        RelocPtr(const RelocPtr& x): ptr(0) { *this = x; }
        RelocPtr(RelocPtr&& x): ptr(x.ptr) { x.ptr = 0; }
        RelocPtr& operator=(const RelocPtr& x);
        RelocPtr& operator=(RelocPtr&& x) { this->~RelocPtr(); this->ptr = x.ptr; x.ptr = 0; return *this; }

        RelocPtr(): ptr(0) {}
        RelocPtr(AllocationPtr p): ptr(0) { set(reinterpret_cast<uintptr_t>(p.m_ptr), TAG_Allocation); p.m_ptr = nullptr; }
        RelocPtr(ConstantPtr   p): ptr(0) { set(reinterpret_cast<uintptr_t>(p.m_ptr), TAG_Constant  ); p.m_ptr = nullptr; }
        RelocPtr(StaticRefPtr  p): ptr(0) { set(reinterpret_cast<uintptr_t>(p.m_ptr), TAG_StaticRef ); p.m_ptr = nullptr; }

        operator bool() const { return ptr != 0; }
        bool operator==(const RelocPtr& x) { return ptr == x.ptr; }

              IValue& as_value()       { return *as_value_ptr(); }
        const IValue& as_value() const { return *as_value_ptr(); }

        Allocation* as_allocation() const { return (ptr != 0 && (ptr&3) == TAG_Allocation) ? reinterpret_cast<Allocation*>(ptr - TAG_Allocation) : nullptr; }
        Constant  * as_constant  () const { return (ptr != 0 && (ptr&3) == TAG_Constant  ) ? reinterpret_cast<Constant  *>(ptr - TAG_Constant  ) : nullptr; }
        StaticRef * as_staticref () const { return (ptr != 0 && (ptr&3) == TAG_StaticRef ) ? reinterpret_cast<StaticRef *>(ptr - TAG_StaticRef ) : nullptr; }

        friend std::ostream& operator<<(std::ostream& os, const RelocPtr& ptr) {
            if(ptr.ptr) {
                ptr.as_value_ptr()->fmt_ident(os);
            }
            else {
                os << "NULL";
            }
            return os;
        }
    private:
        IValue* as_value_ptr() const;
        void set(uintptr_t ptr, Tag tag) {
            assert(this->ptr == 0);
            assert( (ptr & 3) == 0 );
            assert( tag < 4 );
            this->ptr = ptr | tag;
        }
    };
    /// Helper: Print a 2-digit hex value (without updating the stream state)
    void putb_hex(std::ostream& os, uint8_t v) {
        char tmp[3];
        tmp[0] = "0123456789ABCDEF"[v >> 4];
        tmp[1] = "0123456789ABCDEF"[v & 0xF];
        tmp[2] = '\0';
        os << tmp;
    }
    /// Constant data
    class Constant final: public IValue
    {
        friend class ConstantPtr;
        friend class RefCountPtr<Constant>;
        unsigned    reference_count;
        unsigned    const length;
        const uint8_t*  const data;

        Constant(const void* data, size_t len)
            : reference_count(1)
            , length(len)
            , data(reinterpret_cast<const uint8_t*>(data))
        {
        }
    public:
        void fmt_ident(std::ostream& os) const override {
            os << "C:" << (const void*)this->data;
        }
        void fmt(::std::ostream& os, size_t ofs, size_t len) const override {
            assert(ofs <= length);
            assert(ofs+len <= length);
            for(size_t i = 0; i < len; i ++) {
                if(i != 0 && (ofs+i) % 8 == 0)
                    os << " ";
                putb_hex(os, this->data[ofs + i]);
            }
        }

        size_t size() const { return length; }
        const uint8_t* get_bytes(size_t ofs, size_t len, bool /*check_mask*/) const override
        {
            if( !(ofs <= length) || !(len <= length) || !(ofs+len <= length) )
                return nullptr;
            return data + ofs;
        }
        void read_mask(uint8_t* dst, size_t dst_ofs, size_t /*ofs*/, size_t len) const {
            dst += dst_ofs / 8;
            dst_ofs %= 8;
            if( dst_ofs != 0 )
            {
                // Do a single-bit fill
                while(len --)
                    dst[dst_ofs/8] |= 1 << (dst_ofs%8);
            }
            else
            {
                memset(dst, 0xFF, len / 8);
                dst[len / 8] |= 0xFF >> (8 - len%8);
            }
        }

        bool is_writable() const override { return false; }
        void write_bytes(size_t ofs, const void* data, size_t len) override { abort(); }
        void write_mask_from(size_t ofs, const IValue& src, size_t src_ofs, size_t len) override { abort(); }

        RelocPtr get_reloc(size_t ofs) const override { return RelocPtr(); }
        void set_reloc(size_t ofs, RelocPtr ptr) override { abort(); }

    };
    class Allocation final: public IValue
    {
        friend class AllocationPtr;
        friend class RefCountPtr<Allocation>;
    public:
        struct Reloc {
            size_t  offset;
            RelocPtr    ptr;
        };
    private:
        unsigned    reference_count;
        unsigned    length;
        bool    is_readonly;
        ::HIR::TypeRef  m_type;
        std::vector<Reloc>  relocations;
        uint8_t data[0];

        Allocation(size_t len, const ::HIR::TypeRef& ty)
            : reference_count(1)
            , length(len)
            , is_readonly(false)
            , m_type(ty.clone())
        {
            memset(data, 0, len + (len + 7) / 8);
        }
    public:
        void fmt_ident(std::ostream& os) const override {
            os << "A:" << this;
        }
        void fmt(::std::ostream& os, size_t ofs, size_t len) const override {
            assert(ofs <= length);
            assert(ofs+len <= length);
            for(size_t i = 0; i < len; i ++) {
                auto j = ofs + i;
                if(i != 0 && j % 8 == 0)
                    os << " ";
                for(const auto& r : relocations)
                    if( r.offset == j )
                        os << "{" << r.ptr << "}";
                if( get_mask()[j/8] & (1 << j%8) ) {
                    putb_hex(os, data[j]);
                }
                else {
                    os << "--";
                }
            }
        }
        size_t size() const override { return length; }
        const uint8_t* get_bytes(size_t ofs, size_t len, bool check_mask) const override
        {
            if( !(ofs <= length) || !(len <= length) || !(ofs+len <= length) )
                return nullptr;

            if( check_mask )
            {
                const auto* m = this->get_mask();
                size_t mo = ofs, ml = len;
                for( ; mo % 8 != 0 && ml > 0; mo ++, ml --)
                    if( !(m[mo/8] & (1 << (mo % 8))) )
                        return nullptr;
                for( ; ml >= 8; mo += 8, ml -= 8)
                    if( !(m[mo/8] == 0xFF) )
                        return nullptr;
                for( ; ml % 8 != 0 && ml > 0; mo ++, ml --)
                    if( !(m[mo/8] & (1 << (mo % 8))) )
                        return nullptr;
            }

            return this->data + ofs;
        }
        void read_mask(uint8_t* dst, size_t dst_ofs, size_t ofs, size_t len) const {
            assert(ofs <= length);
            assert(len <= length);
            assert(ofs+len <= length);

            dst += dst_ofs / 8;
            const auto* src = get_mask() + ofs / 8;
            dst_ofs %= 8;
            ofs %= 8;
            if( dst_ofs != 0 || ofs != 0 )
            {
                // If the entries are unaligned, then use a bit-by-bit copy
                for(size_t i = 0; i < len; i ++)
                {
                    size_t s = ofs + i;
                    size_t d = dst_ofs + i;
                    if( src[s/8] & (1 << s%8) )
                        dst[d/8] |= (1 << d%8);
                    else
                        dst[d/8] &= ~(1 << d%8);
                }
            }
            else
            {
                for(; len >= 8; len -= 8)
                    *dst++ = *src++;
                // Tail entires (partial byte)
                if(len > 0)
                {
                    uint8_t mask = (0xFF >> len);
                    *dst = (*dst & ~mask) | (*src & mask);
                }
            }
        }

        bool is_writable() const override {
            return !is_readonly;
        }
        void write_bytes(size_t ofs, const void* data, size_t len) override
        {
            assert(ofs <= length);
            assert(len <= length);
            assert(ofs+len <= length);
            // Set the mask
            {
                auto* m = this->get_mask();
                size_t mo = ofs, ml = len;
                for( ; mo % 8 != 0 && ml > 0; mo ++, ml --)
                    m[mo/8] |= (1 << (mo % 8));
                for( ; ml >= 8; mo += 8, ml -= 8)
                    m[mo/8] = 0xFF;
                for( ; ml % 8 != 0 && ml > 0; mo ++, ml --)
                    m[mo/8] |= (1 << (mo % 8));
            }
            // Write data
            memcpy(this->data + ofs, data, len);
            // Clear impacted relocations
            auto it = std::remove_if(this->relocations.begin(), this->relocations.end(), [&](const Reloc& r){ return (ofs <= r.offset && r.offset < ofs+len); });
            this->relocations.resize( it - this->relocations.begin() );
        }
        void write_mask_from(size_t ofs, const IValue& src, size_t src_ofs, size_t len) override {
            assert(ofs <= length);
            assert(len <= length);
            assert(ofs+len <= length);
            src.read_mask(get_mask(), ofs, src_ofs, len);
        }

        RelocPtr get_reloc(size_t ofs) const override {
            for(const auto& r : this->relocations)
                if(r.offset == ofs)
                    return r.ptr;
            return RelocPtr();
        }
        void set_reloc(size_t ofs, RelocPtr ptr) override {
            assert(ofs % (Target_GetPointerBits()/8) == 0);
            auto it = std::lower_bound(this->relocations.begin(), this->relocations.end(), ofs, [](const Reloc& r, size_t ofs){ return r.offset < ofs; });
            if(it != this->relocations.end() && it->offset == ofs) {
                if(ptr)
                    it->ptr = std::move(ptr);
                else
                    this->relocations.erase(it);
            }
            else {
                if( ptr )
                    this->relocations.insert(it, Reloc { ofs, std::move(ptr) });
                else
                    ;
            }
        }


        const ::HIR::TypeRef& get_type() const { return m_type; }
        const std::vector<Reloc>& get_relocations() const { return relocations; }
    private:
              uint8_t* get_mask()       { return data + length; }
        const uint8_t* get_mask() const { return data + length; }
    };
    class StaticRef final: public IValue
    {
        friend class RefCountPtr<StaticRef>;
        friend class StaticRefPtr;

        unsigned reference_count;
        ::HIR::Path m_path;
        const EncodedLiteral*  m_encoded;

        StaticRef(::HIR::Path p, const EncodedLiteral* lit = nullptr)
            : reference_count(1)
            , m_path(std::move(p))
            , m_encoded(lit)
        {
        }

    public:
        void fmt_ident(std::ostream& os) const override {
            os << this->m_path;
        }
        void fmt(::std::ostream& os, size_t ofs, size_t len) const override {
            os << "[" << m_path << "]";
            if(m_encoded)
                os << EncodedLiteralSlice(*m_encoded).slice(ofs, len);
        }

        size_t size() const { return m_encoded ? m_encoded->bytes.size() : 0; }
        const uint8_t* get_bytes(size_t ofs, size_t len, bool check_mask) const override {
            if(m_encoded) {
                assert(ofs <= m_encoded->bytes.size());
                assert(len <= m_encoded->bytes.size());
                assert(ofs+len <= m_encoded->bytes.size());
                return m_encoded->bytes.data() + ofs;
            }
            else {
                if(len == 0 && ofs==0) { static uint8_t null; return &null; }
                return nullptr;
            }
        }
        void read_mask(uint8_t* dst, size_t dst_ofs, size_t ofs, size_t len) const override {
            dst += dst_ofs / 8;
            dst_ofs %= 8;
            if( dst_ofs != 0 )
            {
                // Do a single-bit fill
                while(len --)
                    dst[dst_ofs/8] |= 1 << (dst_ofs%8);
            }
            else
            {
                memset(dst, 0xFF, len / 8);
                dst[len / 8] |= 0xFF >> (8 - len%8);
            }
        }

        bool is_writable() const override {
            return false;
        }
        void write_bytes(size_t ofs, const void* data, size_t len) override {
            abort();
        }
        void write_mask_from(size_t ofs, const IValue& src, size_t src_ofs, size_t len) override {
            abort();
        }

        RelocPtr get_reloc(size_t ofs) const override {
            if(m_encoded) {
                for(const auto& r : m_encoded->relocations)
                {
                    if(r.ofs == ofs) {
                        RelocPtr    reloc;
                        if( r.p ) {
                            //return RelocPtr(get_staticref(r.p->clone()));
                            TODO(Span(), "Convert relocation pointer - " << *r.p);
                        }
                        else {
                            return RelocPtr(AllocationPtr::allocate_ro(r.bytes.data(), r.bytes.size()));
                        }
                    }
                }
            }
            return RelocPtr();
        }
        void set_reloc(size_t ofs, RelocPtr ptr) override {
            abort();
        }

        const ::HIR::Path& path() const { return m_path; }
    };
#if 0   // TODO: Add this sometime?
    class InlineValue
    {
        RelocationPtr   reloc_0;
        uin64_t data[2];
        uint16_t    len;
        uint16_t    mask;
    };
#endif
    /// Reference to a value
    class ValueRef
    {
        RelocPtr    storage;
        uint32_t    ofs;
        uint32_t    len;

    public:
        ValueRef(): storage(), ofs(0), len(0)
        {
        }

        ValueRef(RelocPtr alloc, size_t ofs=0)
            : storage(alloc)
            , ofs(ofs)
            , len(alloc ? alloc.as_value().size() - ofs : 0)
        {
            assert(ofs <= alloc.as_value().size());
        }

        ValueRef slice(size_t ofs, size_t len) {
            assert(ofs <= this->len);
            assert(ofs+len <= this->len);

            ValueRef    rv;
            rv.storage = storage;
            rv.ofs = this->ofs + ofs;
            rv.len = len;
            return rv;
        }
        ValueRef slice(size_t ofs) {
            assert(ofs <= this->len);
            return slice(ofs, this->len - ofs);
        }

        bool is_valid() const { return storage; }
        RelocPtr get_storage() const { return storage; }
        size_t get_ofs() const { return ofs; }
        size_t get_len() const { return len; }

        void copy_from(const MIR::TypeResolve& state, const ValueRef& other) {
            size_t len = std::min(this->len, other.len);
            // Check that there's no overlap
            if(this->storage == other.storage) {
                if(this->ofs < other.ofs) {
                    MIR_ASSERT(state, this->ofs + len <= other.ofs, "Overlapping copy_from: " << other.ofs << "+" << len << " and " << this->ofs << "+" << len);
                }
                else {
                    MIR_ASSERT(state, other.ofs + len <= this->ofs, "Overlapping copy_from: " << other.ofs << "+" << len << " and " << this->ofs << "+" << len);
                }
            }
            // Copy the data (don't check the source mask when getting the source pointer)
            const auto* src = other.storage.as_value().get_bytes(other.ofs, len, /*check_mask*/false);
            MIR_ASSERT(state, src, "Invalid read " << other.storage << " - " << other.ofs << "+" << len);
            storage.as_value().write_bytes(this->ofs, src, len);
            // Copy the mask data
            storage.as_value().write_mask_from(this->ofs, other.storage.as_value(), other.ofs, len);
            // Copy relocations
            for(size_t i = 0; i < len; i ++) {
                if(auto r = other.storage.as_value().get_reloc(other.ofs + i)) {
                    storage.as_value().set_reloc(this->ofs + i, std::move(r));
                }
            }
        }

        void write_bytes(const MIR::TypeResolve& state, const void* data, size_t len) {
            MIR_ASSERT(state, storage, "Writing to invalid slot");
            MIR_ASSERT(state, storage.as_value().is_writable(), "Writing to read-only slot");
            if(len > 0)
            {
                storage.as_value().write_bytes(ofs, data, len);
            }
        }

        void write_byte(const MIR::TypeResolve& state, uint8_t v) {
            write_bytes(state, &v, 1);
        }
        void write_float(const MIR::TypeResolve& state, unsigned bits, double v) {
            switch(bits)
            {
            case 32: { float  v_f32 = v; write_bytes(state, &v_f32, sizeof(v_f32)); } break;
            case 64: { double v_f64 = v; write_bytes(state, &v_f64, sizeof(v_f64)); } break;
            default:
                MIR_BUG(state, "Unexpected float size: " << bits);
            }
        }
        void write_uint(const MIR::TypeResolve& state, unsigned bits, uint64_t v) {
            if(Target_GetCurSpec().m_arch.m_big_endian) MIR_TODO(state, "Handle big endian in constant evaluate");
            if( bits <= 64 ) {
                write_bytes(state, &v, (bits+7)/8);
            }
            else {
                assert(bits == 128);
                uint64_t vs[2] = {v, 0};
                write_bytes(state, vs, 128/8);
            }
        }
        void write_sint(const MIR::TypeResolve& state, unsigned bits, int64_t v) {
            if(Target_GetCurSpec().m_arch.m_big_endian) MIR_TODO(state, "Handle big endian in constant evaluate");
            if( bits <= 64 ) {
                write_bytes(state, &v, (bits+7)/8);
            }
            else {
                assert(bits == 128);
                int64_t vs[2] = {v, -1};
                write_bytes(state, vs, 128/8);
            }
        }
        void write_ptr(const MIR::TypeResolve& state, uint64_t val, RelocPtr reloc) {
            write_uint(state, Target_GetPointerBits(), val);
            storage.as_value().set_reloc(ofs, std::move(reloc));
        }
        void set_reloc(RelocPtr reloc) {
            storage.as_value().set_reloc(ofs, std::move(reloc));
        }

        void read_bytes(const MIR::TypeResolve& state, void* data, size_t len) const {
            MIR_ASSERT(state, storage, "");
            MIR_ASSERT(state, len >= 1, "");
            const auto* src = storage.as_value().get_bytes(ofs, len, /*check_mask*/true);
            memcpy(data, src, len);
        }
        double read_float(const ::MIR::TypeResolve& state, unsigned bits) const {
            switch(bits)
            {
            case 32: { float  v_f32 = 0; read_bytes(state, &v_f32, sizeof(v_f32)); return v_f32; } break;
            case 64: { double v_f64 = 0; read_bytes(state, &v_f64, sizeof(v_f64)); return v_f64; } break;
            default:
                MIR_BUG(state, "Unexpected float size: " << bits);
            }
        }
        U128 read_uint(const ::MIR::TypeResolve& state, unsigned bits) const {
            if(Target_GetCurSpec().m_arch.m_big_endian) MIR_TODO(state, "Handle big endian in constant evaluate");
            assert(bits <= 128);
            if(bits > 64) {
                uint64_t    lo = 0;
                uint64_t    hi = 0;
                read_bytes(state, &lo, 8);
                read_bytes(state, &hi, (bits+7)/8 - 8);
                return U128(lo, hi);
            }
            else {
                uint64_t    rv = 0;
                read_bytes(state, &rv, (bits+7)/8);
                return rv;
            }
        }
        S128 read_sint(const ::MIR::TypeResolve& state, unsigned bits) const {
            auto v = read_uint(state, bits);
            if( v.bit(bits-1) ) {
                // Apply sign extension
                if( bits <= 64 ) {
                    auto v64 = static_cast<uint64_t>(v);
                    v64 |= UINT64_MAX << bits;
                    v = U128(v64, UINT64_MAX);
                }
                else {
                    assert(bits == 128);
                }
            }
            return S128(v);
        }
        uint64_t read_usize(const ::MIR::TypeResolve& state) const {
            return read_uint(state, Target_GetPointerBits());
        }
        std::pair<uint64_t, RelocPtr> read_ptr(const ::MIR::TypeResolve& state) const {
            return std::make_pair(read_usize(state), storage.as_value().get_reloc(ofs));
        }

        friend std::ostream& operator<<(std::ostream& os, const ValueRef& vr);
    };

    std::ostream& operator<<(std::ostream& os, const ValueRef& vr) {
        if(!vr.storage) {
            os << "ValueRef(null)";
        }
        else {
            os << "ValueRef(";
            vr.storage.as_value().fmt(os, vr.ofs, vr.len);
            os << ")";
        }
        return os;
    }
    std::ostream& operator<<(std::ostream& os, const AllocationPtr& ap) {
        os << ValueRef(ap);
        return os;
    }

    // ---
    template<>
    void RefCountPtr<Constant>::dealloc(Constant* v)
    {
        delete v;
    }
    ConstantPtr ConstantPtr::allocate(const void* data, size_t len)
    {
        ConstantPtr rv;
        rv.m_ptr = new Constant(data, len);
        return rv;
    }
    // ---
    template<>
    void RefCountPtr<Allocation>::dealloc(Allocation* v)
    {
        free(v);
    }
    AllocationPtr AllocationPtr::allocate(const ::MIR::TypeResolve& state, const ::HIR::TypeRef& ty)
    {
        size_t len;
        if( !Target_GetSizeOf(Span(), state.m_resolve, ty, len) )    throw Defer();
        auto* rv_raw = reinterpret_cast<Allocation*>( malloc(sizeof(Allocation) + len + ((len+7) / 8)) );
        AllocationPtr   rv;
        rv.m_ptr = new(rv_raw) Allocation(len, ty);
        return rv;
    }
    AllocationPtr AllocationPtr::allocate_ro(const void* data, size_t len)
    {
        auto* rv_raw = reinterpret_cast<Allocation*>( malloc(sizeof(Allocation) + len + ((len+7) / 8)) );
        AllocationPtr   rv;
        rv.m_ptr = new(rv_raw) Allocation(len, HIR::TypeRef());
        rv->write_bytes(0, data, len);
        rv->is_readonly = true;
        return rv;
    }
    // ---
    template<>
    void RefCountPtr<StaticRef>::dealloc(StaticRef* v)
    {
        delete v;
    }
    StaticRefPtr StaticRefPtr::allocate(::HIR::Path p, const EncodedLiteral* lit)
    {
        StaticRefPtr rv;
        rv.m_ptr = new StaticRef(std::move(p), lit);
        return rv;
    }

    // --- RelocPtr ---
    RelocPtr::~RelocPtr()
    {
        switch(ptr & 3)
        {
        case TAG_Allocation: { AllocationPtr p; p.m_ptr = this->as_allocation(); } break;
        case TAG_Constant  : { ConstantPtr   p; p.m_ptr = this->as_constant  (); } break;
        case TAG_StaticRef : { StaticRefPtr  p; p.m_ptr = this->as_staticref (); } break;
        case 3: assert("Unexpected tag");
        }
    }
    RelocPtr& RelocPtr::operator=(const RelocPtr& x)
    {
        switch(x.ptr & 3)
        {
        case TAG_Allocation: { AllocationPtr p; p.m_ptr = x.as_allocation(); auto p2 = p; p.m_ptr=nullptr; p2.m_ptr=nullptr; } break;
        case TAG_Constant  : { ConstantPtr   p; p.m_ptr = x.as_constant  (); auto p2 = p; p.m_ptr=nullptr; p2.m_ptr=nullptr; } break;
        case TAG_StaticRef : { StaticRefPtr  p; p.m_ptr = x.as_staticref (); auto p2 = p; p.m_ptr=nullptr; p2.m_ptr=nullptr; } break;
        case 3: assert("Unexpected tag");
        }
        this->~RelocPtr();
        this->ptr = x.ptr;
        return *this;
    }
    IValue* RelocPtr::as_value_ptr() const
    {
        assert(ptr);
        switch(ptr & 3)
        {
        case TAG_Allocation: assert(as_allocation()); return as_allocation();
        case TAG_Constant  : assert(as_constant  ()); return as_constant  ();
        case TAG_StaticRef : assert(as_staticref ()); return as_staticref ();
        case 3: assert(!"Unexpected tag 3");
        }
        abort();
    }
}}

namespace {
    /// Get the offset for a given field path
    size_t get_offset(const Span& sp, const StaticTraitResolve& resolve, const TypeRepr* r, const TypeRepr::FieldPath& out_path)
    {
        assert(out_path.index < r->fields.size());
        size_t ofs = r->fields[out_path.index].offset;

        const auto* ty = &r->fields[out_path.index].ty;
        for(const auto& f : out_path.sub_fields)
        {
            r = Target_GetTypeRepr(sp, resolve, *ty);
            if(!r)
                throw Defer();
            assert(f < r->fields.size());
            ofs += r->fields[f].offset;
            ty = &r->fields[f].ty;
        }

        return ofs;
    }

    EntPtr get_ent_fullpath(const Span& sp, const ::StaticTraitResolve& resolve, const ::HIR::Path& path, EntNS ns, MonomorphState& out_ms)
    {
        if(const auto* gp = path.m_data.opt_Generic()) {
            const auto& name = gp->m_path.m_components.back();
            const auto& mod = resolve.m_crate.get_mod_by_path(sp, gp->m_path, /*ignore_last*/true);
            // TODO: This pointer will be invalidated...
            for(const auto& is : mod.m_inline_statics) {
                if(is.first == name)
                    return &*is.second;
            }
        }
        auto v = resolve.get_value(sp, path, out_ms);
        TU_MATCH_HDRA( (v), { )
        TU_ARMA(NotFound, e)
            return EntPtr();
        TU_ARMA(NotYetKnown, e)
            TODO(sp, "Handle NotYetKnown - " << path);
        TU_ARMA(Constant, e)
            return e;
        TU_ARMA(Static, e)
            return e;
        TU_ARMA(Function, e)
            return e;
        TU_ARMA(EnumConstructor, e)
            TODO(sp, "Handle EnumConstructor - " << path);
        TU_ARMA(EnumValue, e)
            TODO(sp, "Handle EnumValue - " << path);
        TU_ARMA(StructConstructor, e)
            TODO(sp, "Handle StructConstructor - " << path);
        TU_ARMA(StructConstant, e)
            TODO(sp, "Handle StructConstant - " << path);
        }
        throw "";
    }
    const ::HIR::Function& get_function(const Span& sp, const ::StaticTraitResolve& resolve, const ::HIR::Path& path, MonomorphState& out_ms)
    {
        auto rv = get_ent_fullpath(sp, resolve, path, EntNS::Value, out_ms);
        if(rv.is_Function()) {
            return *rv.as_Function();
        }
        else {
            TODO(sp, "Could not find function for " << path << " - " << rv.tag_str());
        }
    }
}   // namespace <anon>

namespace HIR {

    ::MIR::eval::AllocationPtr Evaluator::evaluate_constant_mir(
        const ::HIR::ItemPath& ip, const ::MIR::Function& fcn, MonomorphState ms,
        ::HIR::TypeRef exp, const ::HIR::Function::args_t& arg_defs,
        ::std::vector<::MIR::eval::AllocationPtr> args
        )
    {
        TRACE_FUNCTION_F("exp=" << exp << ", args=" << args);

        ::MIR::TypeResolve  state { this->root_span, this->resolve, FMT_CB(ss, ss<<ip), exp, arg_defs, fcn };

        using namespace ::MIR::eval;

        struct LocalState {
            ::MIR::TypeResolve& state;
            const MonomorphState& ms;

            HIR::TypeRef    ret_type;
            AllocationPtr   retval;

            ::std::vector<AllocationPtr>&  args;

            ::std::vector<HIR::TypeRef>   local_types;
            ::std::vector<AllocationPtr>  locals;

            LocalState(::MIR::TypeResolve& state, const MonomorphState& ms, ::std::vector<AllocationPtr>& args):
                state(state),
                ms(ms),
                ret_type( ms.monomorph_type(state.sp, state.m_ret_type) ),
                retval( AllocationPtr::allocate(state, ret_type) ),
                args(args)
            {
                local_types.reserve( state.m_fcn.locals.size() );
                locals     .reserve( state.m_fcn.locals.size() );
                for(size_t i = 0; i < state.m_fcn.locals.size(); i ++)
                {
                    local_types.push_back( ms.monomorph_type(state.sp, state.m_fcn.locals[i]) );
                    locals.push_back( AllocationPtr::allocate(state, local_types.back()) );
                }

                state.m_monomorphed_rettype = &ret_type;
                state.m_monomorphed_locals = &local_types;
            }

            StaticRefPtr get_staticref_mono(const ::HIR::Path& p)
            {
                // NOTE: Value won't need to be monomorphed, as it shouldn't be generic
                return get_staticref( ms.monomorph_path(state.sp, p) );
            }
            StaticRefPtr get_staticref(::HIR::Path p)
            {
                // If there's any mention of generics in this path, then return Literal::Defer
                if( visit_path_tys_with(p, [&](const auto& ty)->bool { return ty.data().is_Generic(); }) )
                {
                    DEBUG("Return Literal::Defer for constastatic " << p << " which references a generic parameter");
                    throw Defer();
                }
                MonomorphState  const_ms;
                auto ent = get_ent_fullpath(state.sp, state.m_resolve.m_crate, p, EntNS::Value,  const_ms);
                if(ent.is_Static())
                {
                    const auto& s = *ent.as_Static();

                    if( !s.m_value_generated )
                    {
                        // If there's no MIR and no HIR then this is an external static (which can only be borrowed)
                        if( !s.m_value && !s.m_value.m_mir )
                            return StaticRefPtr::allocate(std::move(p), nullptr);

                        auto& item = const_cast<::HIR::Static&>(s);

                        // Challenge: Adding items to the module might invalidate an iterator.
                        ::HIR::ItemPath mod_ip { item.m_value.m_state->m_mod_path };
                        auto nvs = NewvalState(item.m_value.m_state->m_module, mod_ip, FMT("static" << &item << "#"));
                        auto eval = ::HIR::Evaluator(item.m_value.span(), state.m_resolve.m_crate, nvs);
                        DEBUG("- Evaluate " << p);
                        try
                        {
                            item.m_value_res = eval.evaluate_constant(::HIR::ItemPath(p), item.m_value, item.m_type.clone());
                            item.m_value_generated = true;
                        }
                        catch(const Defer& )
                        {
                            MIR_BUG(state, p << " Defer during value generation");
                        }
                    }
                    return StaticRefPtr::allocate(std::move(p), &s.m_value_res);
                }
                else
                    return StaticRefPtr::allocate(std::move(p), nullptr);
            }

            ValueRef get_lval(const ::MIR::LValue& lv, ValueRef* meta=nullptr)
            {
                const ::HIR::TypeRef*   typ = nullptr;
                ValueRef metadata;
                ValueRef val;
                //TRACE_FUNCTION_FR(lv, val);
                TU_MATCH_HDRA( (lv.m_root), {)
                TU_ARMA(Return, e) {
                    typ = &ret_type;
                    val = ValueRef(retval);
                    }
                TU_ARMA(Local, e) {
                    MIR_ASSERT(state, e < locals.size(), "Local index out of range - " << e << " >= " << locals.size());
                    typ = &local_types[e];
                    val = ValueRef(locals[e]);
                    }
                TU_ARMA(Argument, e) {
                    MIR_ASSERT(state, e < args.size(), "Argument index out of range - " << e << " >= " << args.size());
                    typ = &state.m_args[e].second;
                    val = ValueRef(args[e]);
                    }
                TU_ARMA(Static, e) {
                    val = ValueRef(get_staticref_mono(e));
                    }
                }

                for(const auto& w : lv.m_wrappers)
                {
                    assert(typ);
                    DEBUG(w << " " << val);
                    TU_MATCH_HDRA( (w), {)
                    TU_ARMA(Field, e) {
                        auto* repr = Target_GetTypeRepr(state.sp, state.m_resolve, *typ);
                        MIR_ASSERT(state, repr, "No repr for " << *typ);
                        MIR_ASSERT(state, e < repr->fields.size(), "LValue::Field index out of range");
                        if(repr->size != SIZE_MAX) {
                            metadata = ValueRef();
                        }
                        typ = &repr->fields[e].ty;

                        size_t sz, al;
                        if( !Target_GetSizeAndAlignOf(state.sp, state.m_resolve, *typ,  sz, al) )
                            throw Defer();
                        if( sz == SIZE_MAX ) {
                            val = val.slice(repr->fields[e].offset);
                        }
                        else {
                            val = val.slice(repr->fields[e].offset, sz);
                        }
                        }
                    TU_ARMA(Deref, e) {
                        // 
                        if( const auto* te = typ->data().opt_Pointer() ) {
                            typ = &te->inner;
                        }
                        else if( const auto* te = typ->data().opt_Borrow() ) {
                            typ = &te->inner;
                        }
                        else {
                        }
                        // If the inner type is unsized
                        size_t sz, al;
                        if( !Target_GetSizeAndAlignOf(state.sp, state.m_resolve, *typ,  sz, al) )
                            throw Defer();
                        if( sz == SIZE_MAX ) {
                            // Read metadata
                            DEBUG("Reading metadata");
                            metadata = val.slice(Target_GetPointerBits()/8);
                        }
                        auto p = val.read_ptr(state);
                        MIR_ASSERT(state, p.first >= EncodedLiteral::PTR_BASE, "Null (<PTR_BASE) pointer deref");
                        MIR_ASSERT(state, p.first % al == 0, "Unaligned pointer deref");
                        DEBUG("> " << ValueRef(p.second) << " - " << (p.first - EncodedLiteral::PTR_BASE) << " " << sz << " " << *typ);
                        // TODO: Determine size using metadata?
                        if(sz == SIZE_MAX) {
                            val = ValueRef(p.second, p.first - EncodedLiteral::PTR_BASE);
                        }
                        else {
                            val = ValueRef(p.second, p.first - EncodedLiteral::PTR_BASE).slice(0, sz);
                        }
                        }
                    TU_ARMA(Index, e) {
                        // Check the inner type
                        size_t  size;
                        if( const auto* te = typ->data().opt_Array() ) {
                            typ = &te->inner;
                            size = te->size.as_Known();
                        }
                        else if( const auto* te = typ->data().opt_Slice() ) {
                            typ = &te->inner;
                            // Get metadata
                            size = metadata.read_usize(state);
                        }
                        else {
                        }
                        metadata = ValueRef();
                        size_t sz, al;
                        if( !Target_GetSizeAndAlignOf(state.sp, state.m_resolve, *typ,  sz, al) )
                            throw Defer();
                        MIR_ASSERT(state, sz < SIZE_MAX, "Unsized type on index output - " << *typ);
                        MIR_ASSERT(state, e < locals.size(), "LValue::Index index local out of range");
                        size_t  index = ValueRef(locals[e]).read_usize(state);
                        MIR_ASSERT(state, index < size, "LValue::Index index out of range - " << index << " >= " << size);
                        val = val.slice(index * sz, sz);
                        }
                    TU_ARMA(Downcast, e) {
                        auto* repr = Target_GetTypeRepr(state.sp, state.m_resolve, *typ);
                        MIR_ASSERT(state, repr, "No repr for " << *typ);
                        MIR_ASSERT(state, e < repr->fields.size(), "LValue::Downcast index out of range");
                        if(repr->size != SIZE_MAX) {
                            metadata = ValueRef();
                        }
                        val = val.slice(repr->fields[e].offset, size_of_or_bug(repr->fields[e].ty));
                        }
                    }
                }
                if(meta)
                    *meta = std::move(metadata);
                return val;
            }

            const EncodedLiteral& get_const(const ::HIR::Path& in_p, ::HIR::TypeRef* out_ty)
            {
                auto p = ms.monomorph_path(state.sp, in_p);
                // If there's any mention of generics in this path, then return Literal::Defer
                if( visit_path_tys_with(p, [&](const auto& ty)->bool { return ty.data().is_Generic(); }) )
                {
                    DEBUG("Return Literal::Defer for constant " << p << " which references a generic parameter");
                    throw Defer();
                }
                MonomorphState  const_ms;
                auto ent = get_ent_fullpath(state.sp, state.m_resolve.m_crate, p, EntNS::Value,  const_ms);
                MIR_ASSERT(state, ent.is_Constant(), "MIR Constant::Const(" << p << ") didn't point to a Constant - " << ent.tag_str());
                const auto& c = *ent.as_Constant();
                if( c.m_value_state == HIR::Constant::ValueState::Unknown )
                {
                    auto& item = const_cast<::HIR::Constant&>(c);
                    // Challenge: Adding items to the module might invalidate an iterator.
                    ::HIR::ItemPath mod_ip { item.m_value.m_state->m_mod_path };
                    auto nvs = NewvalState(item.m_value.m_state->m_module, mod_ip, FMT("const" << &c << "#"));
                    auto eval = ::HIR::Evaluator(item.m_value.span(), state.m_resolve.m_crate, nvs);
                    // TODO: Does this need to set generics?
                    DEBUG("- Evaluate " << p);
                    try
                    {
                        item.m_value_res = eval.evaluate_constant(::HIR::ItemPath(p), item.m_value, item.m_type.clone());
                        item.m_value_state = HIR::Constant::ValueState::Known;
                    }
                    catch(const Defer& )
                    {
                        item.m_value_state = HIR::Constant::ValueState::Generic;
                    }
                }
                if(out_ty)
                    *out_ty = const_ms.monomorph_type(state.sp, c.m_type);
                if( c.m_value_state == HIR::Constant::ValueState::Generic )
                {
                    auto it = c.m_monomorph_cache.find(p);
                    if( it == c.m_monomorph_cache.end() )
                    {
                        auto& item = const_cast<::HIR::Constant&>(c);
                        // Challenge: Adding items to the module might invalidate an iterator.
                        ::HIR::ItemPath mod_ip { item.m_value.m_state->m_mod_path };
                        auto nvs = NewvalState(item.m_value.m_state->m_module, mod_ip, FMT("const" << &c << "#"));
                        auto eval = ::HIR::Evaluator(item.m_value.span(), state.m_resolve.m_crate, nvs);
                        // TODO: Does this need to set generics?

                        DEBUG("- Evaluate monomorphed " << p);
                        DEBUG("> const_ms=" << const_ms);
                        auto ty = const_ms.monomorph_type( item.m_value.span(), item.m_type );
                        auto val = eval.evaluate_constant(::HIR::ItemPath(p), item.m_value, std::move(ty), std::move(const_ms));

                        auto insert_res = item.m_monomorph_cache.insert(std::make_pair(p.clone(), std::move(val)));
                        it = insert_res.first;
                    }

                    return it->second;
                }
                else
                {
                    return c.m_value_res;
                }
            }

            void write_const(ValueRef dst, const ::MIR::Constant& c)
            {
                TU_MATCH_HDR( (c), {)
                TU_ARM(c, Int, e2) {
                    dst.write_sint(state, dst.get_len() * 8, e2.v);
                    }
                TU_ARM(c, Uint, e2) {
                    dst.write_uint(state, dst.get_len() * 8, e2.v);
                    }
                TU_ARM(c, Float, e2) {
                    dst.write_float(state, dst.get_len() * 8, e2.v);
                    }
                TU_ARM(c, Bool, e2) {
                    dst.write_uint(state, 1, e2.v);
                    }
                TU_ARM(c, Bytes, e2) {
                    dst.write_ptr(state, EncodedLiteral::PTR_BASE, ConstantPtr::allocate(e2.data(), e2.size()));
                    }
                TU_ARM(c, StaticString, e2) {
                    dst.write_ptr(state, EncodedLiteral::PTR_BASE, ConstantPtr::allocate(e2.data(), e2.size()));
                    dst.slice(Target_GetPointerBits()/8).write_uint(state, Target_GetPointerBits(), e2.size());
                    }
                TU_ARM(c, Const, e2) {
                    ::HIR::TypeRef  ty;
                    const auto& encoded = get_const(*e2.p, &ty);
                    DEBUG(*e2.p << " = " << encoded);

                    // Write the encoded value into the destination
                    dst.write_bytes(state, encoded.bytes.data(), encoded.bytes.size());
                    for(const auto& r : encoded.relocations)
                    {
                        RelocPtr    reloc;
                        if( r.p ) {
                            reloc = RelocPtr(get_staticref(r.p->clone()));
                        }
                        else {
                            reloc = RelocPtr(AllocationPtr::allocate_ro(r.bytes.data(), r.bytes.size()));
                        }
                        dst.slice(r.ofs, r.len).set_reloc(std::move(reloc));
                    }
                    }
                TU_ARM(c, Generic, e2) {
                    throw Defer();
                    }
                TU_ARM(c, ItemAddr, e2) {
                    dst.write_ptr(state, EncodedLiteral::PTR_BASE, get_staticref_mono(*e2));
                    //MIR_TODO(state, "ItemAddr");
                    }
                }
            }

            /// Write a borrow of the given lvalue
            void write_borrow(ValueRef dst, ::HIR::BorrowType bt, const ::MIR::LValue& lv)
            {
                ValueRef    meta;
                auto val = this->get_lval(lv, &meta);
                dst.write_ptr(state, EncodedLiteral::PTR_BASE + val.get_ofs(), val.get_storage());
                if(meta.is_valid())
                {
                    auto ptr_size = Target_GetPointerBits() / 8;
                    dst.slice(ptr_size).copy_from( state, meta );
                }
            }

            void write_param(ValueRef dst, const ::MIR::Param& p)
            {
                TU_MATCH_HDRA( (p), { )
                TU_ARMA(LValue, e)
                    dst.copy_from( state, this->get_lval(e) );
                TU_ARMA(Borrow, e)
                    write_borrow(dst, e.type, e.val);
                TU_ARMA(Constant, e)
                    write_const(dst, e);
                }
            }

            /// Read a floating point value from a MIR::Param
            double read_param_float(unsigned bits, const ::MIR::Param& p)
            {
                TU_MATCH_HDRA( (p), { )
                TU_ARMA(LValue, e)
                    return this->get_lval(e).read_float(state, bits);
                TU_ARMA(Borrow, e)
                    MIR_BUG(state, "Expected a float, got a MIR::Param::Borrow");
                TU_ARMA(Constant, e) {
                    if(e.is_Const()) {
                        const auto& val = get_const(*e.as_Const().p, nullptr);
                        // TODO: Check the type from get_const
                        return EncodedLiteralSlice(val).read_float();
                    }
                    MIR_ASSERT(state, e.is_Float(), "Expected a float, got " << e);
                    return e.as_Float().v;
                    }
                }
                abort();
            }

            uint64_t read_param_uint(unsigned bits, const ::MIR::Param& p)
            {
                TU_MATCH_HDRA( (p), { )
                TU_ARMA(LValue, e)
                    return this->get_lval(e).read_uint(state, bits);
                TU_ARMA(Borrow, e)
                    MIR_BUG(state, "Expected an integer, got a MIR::Param::Borrow");
                TU_ARMA(Constant, e) {
                    if(e.is_Const()) {
                        const auto& val = get_const(*e.as_Const().p, nullptr);
                        // TODO: Check the type from get_const
                        return EncodedLiteralSlice(val).read_uint();
                    }
                    if(e.is_Int())
                        return e.as_Int().v;
                    MIR_ASSERT(state, e.is_Uint(), "Expected an integer, got " << e);
                    return e.as_Uint().v;
                    }
                }
                abort();
            }
            int64_t read_param_sint(unsigned bits, const ::MIR::Param& p)
            {
                TU_MATCH_HDRA( (p), { )
                TU_ARMA(LValue, e)
                    return this->get_lval(e).read_sint(state, bits);
                TU_ARMA(Borrow, e)
                    MIR_BUG(state, "Expected an integer, got a MIR::Param::Borrow");
                TU_ARMA(Constant, e) {
                    if(e.is_Const()) {
                        const auto& val = get_const(*e.as_Const().p, nullptr);
                        // TODO: Check the type from get_const
                        return EncodedLiteralSlice(val).read_sint();
                    }
                    MIR_ASSERT(state, e.is_Int(), "Expected an integer, got " << e);
                    return e.as_Int().v;
                    }
                }
                abort();
            }

            size_t size_of_or_bug(const ::HIR::TypeRef& ty) const
            {
                size_t rv;
                if( !Target_GetSizeOf(state.sp, state.m_resolve, ty, /*out*/rv) )
                    MIR_BUG(state, "");
                return rv;
            }
        };
        LocalState  local_state( state, ms, args );

        struct TypeInfo {
            enum {
                Other,
                Float,
                Signed,
                Unsigned,
            } ty;
            unsigned bits;

            static TypeInfo for_primitive(::HIR::CoreType te) {
                switch(te)
                {
                case ::HIR::CoreType::I8:   return TypeInfo { Signed  , 8 };
                case ::HIR::CoreType::U8:   return TypeInfo { Unsigned, 8 };
                case ::HIR::CoreType::I16:  return TypeInfo { Signed  , 16 };
                case ::HIR::CoreType::U16:  return TypeInfo { Unsigned, 16 };
                case ::HIR::CoreType::I32:  return TypeInfo { Signed  , 32 };
                case ::HIR::CoreType::U32:  return TypeInfo { Unsigned, 32 };
                case ::HIR::CoreType::I64:  return TypeInfo { Signed  , 64 };
                case ::HIR::CoreType::U64:  return TypeInfo { Unsigned, 64 };
                case ::HIR::CoreType::I128: return TypeInfo { Signed  , 128 };
                case ::HIR::CoreType::U128: return TypeInfo { Unsigned, 128 };

                case ::HIR::CoreType::Isize: return TypeInfo { Signed  , Target_GetPointerBits() };
                case ::HIR::CoreType::Usize: return TypeInfo { Unsigned, Target_GetPointerBits() };
                case ::HIR::CoreType::Char: return TypeInfo { Unsigned, 21 };
                case ::HIR::CoreType::Bool: return TypeInfo { Unsigned, 1 };


                case ::HIR::CoreType::F32:  return TypeInfo { Float, 32 };
                case ::HIR::CoreType::F64:  return TypeInfo { Float, 64 };

                case ::HIR::CoreType::Str:  return TypeInfo { Other, 0 };
                }
                return TypeInfo { Other, 0 };
            }
            static TypeInfo for_type(const ::HIR::TypeRef& ty) {
                if(!ty.data().is_Primitive())
                    return TypeInfo { Other, 0 };
                return for_primitive(ty.data().as_Primitive());
            }

            uint64_t mask(uint64_t v) const {
                if(bits < 64)
                {
                    uint64_t mask_val = (1ull << bits) - 1;
                    return v & mask_val;
                }
                return v;
            }
            uint64_t mask(int64_t v) const {
                if( v < 0 ) {
                    // Negate, mask, and re-negate
                    return static_cast<uint64_t>( -static_cast<int64_t>( mask(static_cast<uint64_t>(-v)) ));
                }
                else {
                    return mask(static_cast<uint64_t>(v));
                }
            }
            double mask(double v) const {
                return v;
            }
        };

        unsigned int cur_block = 0;
        for(;;)
        {
            const auto& block = fcn.blocks[cur_block];
            for(const auto& stmt : block.statements)
            {
                state.set_cur_stmt(cur_block, &stmt - &block.statements.front());
                DEBUG(state << stmt);

                if( ! stmt.is_Assign() ) {
                    // NOTE: `const FOO: &Foo = &Foo { ... }` does't get rvalue promoted because of challenges in
                    // running promotion during early generation
                    // HACK: Ignore drops... for now
                    if( stmt.is_Drop() )
                        continue ;
                    MIR_TODO(state, "Non-assign statement - " << stmt);
                    continue ;
                }

                const auto& sa = stmt.as_Assign();

                auto dst = local_state.get_lval(sa.dst);
                TU_MATCH_HDRA( (sa.src), {)
                TU_ARMA(Use, e) {
                    dst.copy_from( state, local_state.get_lval(e) );
                    }
                TU_ARMA(Constant, e) {
                    local_state.write_const(dst, e);
                    }
                TU_ARMA(Borrow, e) {
                    local_state.write_borrow( dst, e.type, e.val );
                    }
                TU_ARMA(Cast, e) {
                    ::HIR::TypeRef  tmp;
                    const auto& src_ty = state.get_lvalue_type(tmp, e.val);

                    auto inval = local_state.get_lval(e.val);

                    TU_MATCH_HDRA( (e.type.data()), {)
                    default:
                        // NOTE: Can be an unsizing!
                        MIR_TODO(state, "RValue::Cast to " << e.type << ", val = " << inval);
                    TU_ARMA(Path, te) {
                        bool done = false;
                        // CoerceUnsized cast
                        if(te.binding.is_Struct())
                        {
                            const HIR::Struct& str = *te.binding.as_Struct();
                            if( src_ty.data().is_Path() && src_ty.data().as_Path().binding.is_Struct() && src_ty.data().as_Path().binding.as_Struct() == &str )
                            {
                                if( str.m_struct_markings.coerce_unsized != HIR::StructMarkings::Coerce::None )
                                {
                                    dst.copy_from( state, inval );
                                    done = true;
                                }
                            }
                        }
                        if(!done )
                        {
                            MIR_TODO(state, "RValue::Cast to " << e.type << ", val = " << inval);
                        }
                        }
                    TU_ARMA(Primitive, te) {
                        auto ti = TypeInfo::for_primitive(te);
                        auto src_ti = TypeInfo::for_type(src_ty);
                        switch(ti.ty)
                        {
                        // Integers mask down
                        case TypeInfo::Signed:
                        case TypeInfo::Unsigned:
                            switch(src_ti.ty)
                            {
                            case TypeInfo::Signed: {
                                auto v = inval.read_sint(state, src_ti.bits);
                                dst.write_uint( state, ti.bits, v );
                                } break;
                            case TypeInfo::Unsigned:
                                dst.write_uint( state, ti.bits, inval.read_uint(state, src_ti.bits) );
                                break;
                            case TypeInfo::Float:
                                dst.write_uint( state, ti.bits, static_cast<int64_t>(inval.read_float(state, src_ti.bits)) );
                                break;
                            case TypeInfo::Other: {
                                MIR_ASSERT(state, TU_TEST1(src_ty.data(), Path, .binding.is_Enum()), "Constant cast Variant to integer with invalid type - " << src_ty);
                                MIR_ASSERT(state, src_ty.data().as_Path().binding.as_Enum(), "Enum binding pointer not set! - " << src_ty);
                                const HIR::Enum& enm = *src_ty.data().as_Path().binding.as_Enum();
                                MIR_ASSERT(state, enm.is_value(), "Constant cast Variant to integer with non-value enum - " << src_ty);
                                const auto* repr = Target_GetTypeRepr(state.sp, state.m_resolve, src_ty);
                                if(!repr)   throw Defer();
                                auto& ve = repr->variants.as_Values();

                                auto v = inval.slice( repr->get_offset(state.sp, state.m_resolve, ve.field), ve.field.size).read_uint(state, ve.field.size*8);
                                // TODO: Ensure that this is a valid variant?
                                dst.write_uint( state, ti.bits, v );
                                } break;
                            }
                            break;
                        case TypeInfo::Float:
                            switch(src_ti.ty)
                            {
                            case TypeInfo::Signed:
                                dst.write_float(state, ti.bits, static_cast<double>(static_cast<int64_t>(inval.read_uint(state, src_ti.bits))) );
                                break;
                            case TypeInfo::Unsigned:
                                dst.write_float(state, ti.bits, static_cast<double>(inval.read_uint(state, src_ti.bits)) );
                                break;
                            case TypeInfo::Float:
                                dst.write_float(state, ti.bits, inval.read_float(state, src_ti.bits) );
                                break;
                            case TypeInfo::Other:
                                MIR_TODO(state, "Cast " << src_ty << " to float");
                            }
                            break;
                        default:
                            MIR_TODO(state, "RValue::Cast to " << e.type << ", val = " << inval);
                        }
                        }
                    // Allow casting any integer value to a pointer (TODO: Ensure that the pointer is sized?)
                    TU_ARMA(Pointer, te) {
                        // This might be a cast fat to thin, so restrict th input size
                        dst.copy_from( state, inval.slice(0, std::min(inval.get_len(), dst.get_len())) );
                        }
                    TU_ARMA(Borrow, te) {
                        // TODO: What can cast TO a borrow? - Non-converted dyn unsizes .. but they require vtables,
                        // which aren't available yet!
                        if( const auto* tep = te.inner.data().opt_TraitObject() )
                        {
                            auto vtable_path = ::HIR::Path(src_ty.data().as_Borrow().inner.clone(), tep->m_trait.m_path.clone(), "vtable#");
                            dst.copy_from( state, inval );
                            dst.slice(Target_GetPointerBits()/8).write_ptr(state, EncodedLiteral::PTR_BASE, local_state.get_staticref(std::move(vtable_path)));
                        }
                        else
                        {
                            MIR_BUG(state, "Cast to " << e.type << " from " << src_ty);
                        }
                        }
                    }
                    }
                TU_ARMA(BinOp, e) {
                    ::HIR::TypeRef tmp;
                    const auto& ty_l = state.get_param_type(tmp, e.val_l);
                    auto ti = TypeInfo::for_type(ty_l);
                    // NOTE: Shifts can use any integer as the RHS, so give them special handling
                    if(e.op == ::MIR::eBinOp::BIT_SHL || e.op == ::MIR::eBinOp::BIT_SHR )
                    {
                        ::HIR::TypeRef  tmp_r;
                        const auto& ty_r = state.get_param_type(tmp_r, e.val_r);
                        auto ti_r = TypeInfo::for_type(ty_r);

                        auto r = ti_r.ty == TypeInfo::Unsigned
                            ? local_state.read_param_uint(ti_r.bits, e.val_r)
                            : local_state.read_param_sint(ti_r.bits, e.val_r);
                        switch(ti.ty)
                        {
                        case TypeInfo::Unsigned: {
                            auto l = local_state.read_param_uint(ti.bits, e.val_l);
                            switch(e.op)
                            {
                            case ::MIR::eBinOp::BIT_SHL: dst.write_uint(state, ti.bits, ti.mask(l << r));  break;
                            case ::MIR::eBinOp::BIT_SHR: dst.write_uint(state, ti.bits, ti.mask(l >> r));  break;
                            default:    MIR_BUG(state, "This block should only be active for SHL/SHR");
                            }
                            break; }
                        case TypeInfo::Signed: {
                            auto l = local_state.read_param_sint(ti.bits, e.val_l);
                            switch(e.op)
                            {
                            case ::MIR::eBinOp::BIT_SHL: dst.write_uint(state, ti.bits, ti.mask(l << r));  break;
                            case ::MIR::eBinOp::BIT_SHR: dst.write_uint(state, ti.bits, ti.mask(l >> r));  break;
                            default:    MIR_BUG(state, "This block should only be active for SHL/SHR");
                            }
                            break; }
                        default:
                            MIR_BUG(state, "Invalid use of BIT_SHL/BIT_SHR on " << ty_l);
                        }
                        // Skip the rest of this arm (breaks both loops in `TU_ARMA`)
                        break ;
                    }
                    switch(ti.ty)
                    {
                    case TypeInfo::Float: {
                        auto l = local_state.read_param_float(ti.bits, e.val_l);
                        auto r = local_state.read_param_float(ti.bits, e.val_r);
                        switch(e.op)
                        {
                        case ::MIR::eBinOp::ADD:    dst.write_float(state, ti.bits, l + r);  break;
                        case ::MIR::eBinOp::SUB:    dst.write_float(state, ti.bits, l - r);  break;
                        case ::MIR::eBinOp::MUL:    dst.write_float(state, ti.bits, l * r);  break;
                        case ::MIR::eBinOp::DIV:    dst.write_float(state, ti.bits, l / r);  break;
                        case ::MIR::eBinOp::MOD:
                        case ::MIR::eBinOp::ADD_OV:
                        case ::MIR::eBinOp::SUB_OV:
                        case ::MIR::eBinOp::MUL_OV:
                        case ::MIR::eBinOp::DIV_OV:
                            MIR_TODO(state, "RValue::BinOp - " << sa.src << ", val = " << l << " , " << r);

                        case ::MIR::eBinOp::BIT_OR :
                        case ::MIR::eBinOp::BIT_AND:
                        case ::MIR::eBinOp::BIT_XOR:
                        case ::MIR::eBinOp::BIT_SHL:
                        case ::MIR::eBinOp::BIT_SHR:
                            MIR_TODO(state, "RValue::BinOp - " << sa.src << ", val = " << l << " , " << r);
                        case ::MIR::eBinOp::EQ: dst.write_byte(state, l == r);  break;
                        case ::MIR::eBinOp::NE: dst.write_byte(state, l != r);  break;
                        case ::MIR::eBinOp::GT: dst.write_byte(state, l >  r);  break;
                        case ::MIR::eBinOp::GE: dst.write_byte(state, l >= r);  break;
                        case ::MIR::eBinOp::LT: dst.write_byte(state, l <  r);  break;
                        case ::MIR::eBinOp::LE: dst.write_byte(state, l <= r);  break;
                        }
                        break; };
                    case TypeInfo::Unsigned: {
                        auto l = local_state.read_param_uint(ti.bits, e.val_l);
                        auto r = local_state.read_param_uint(ti.bits, e.val_r);
                        switch(e.op)
                        {
                        case ::MIR::eBinOp::ADD:    dst.write_uint(state, ti.bits, ti.mask(l + r));  break;
                        case ::MIR::eBinOp::SUB:    dst.write_uint(state, ti.bits, ti.mask(l - r));  break;
                        case ::MIR::eBinOp::MUL:    dst.write_uint(state, ti.bits, ti.mask(l * r));  break;
                        case ::MIR::eBinOp::DIV:    dst.write_uint(state, ti.bits, ti.mask(l / r));  break;
                        case ::MIR::eBinOp::MOD:    dst.write_uint(state, ti.bits, ti.mask(l % r));  break;
                        case ::MIR::eBinOp::ADD_OV:
                        case ::MIR::eBinOp::SUB_OV:
                        case ::MIR::eBinOp::MUL_OV:
                        case ::MIR::eBinOp::DIV_OV:
                            MIR_TODO(state, "RValue::BinOp - " << sa.src << ", val = " << l << " , " << r);

                        case ::MIR::eBinOp::BIT_OR : dst.write_uint(state, ti.bits, l | r);  break;
                        case ::MIR::eBinOp::BIT_AND: dst.write_uint(state, ti.bits, l & r);  break;
                        case ::MIR::eBinOp::BIT_XOR: dst.write_uint(state, ti.bits, l ^ r);  break;
                        case ::MIR::eBinOp::BIT_SHL: dst.write_uint(state, ti.bits, ti.mask(l << r));  break;
                        case ::MIR::eBinOp::BIT_SHR: dst.write_uint(state, ti.bits, ti.mask(l >> r));  break;

                        case ::MIR::eBinOp::EQ: dst.write_byte(state, l == r);  break;
                        case ::MIR::eBinOp::NE: dst.write_byte(state, l != r);  break;
                        case ::MIR::eBinOp::GT: dst.write_byte(state, l >  r);  break;
                        case ::MIR::eBinOp::GE: dst.write_byte(state, l >= r);  break;
                        case ::MIR::eBinOp::LT: dst.write_byte(state, l <  r);  break;
                        case ::MIR::eBinOp::LE: dst.write_byte(state, l <= r);  break;
                        }
                        break; }
                    case TypeInfo::Signed: {
                        auto l = local_state.read_param_sint(ti.bits, e.val_l);
                        auto r = local_state.read_param_sint(ti.bits, e.val_r);
                        switch(e.op)
                        {
                        case ::MIR::eBinOp::ADD:    dst.write_uint( state, ti.bits, ti.mask(l + r) );  break;
                        case ::MIR::eBinOp::SUB:    dst.write_uint( state, ti.bits, ti.mask(l - r) );  break;
                        case ::MIR::eBinOp::MUL:    dst.write_uint( state, ti.bits, ti.mask(l * r) );  break;
                        case ::MIR::eBinOp::DIV:    dst.write_uint( state, ti.bits, ti.mask(l / r) );  break;
                        case ::MIR::eBinOp::MOD:    dst.write_uint( state, ti.bits, ti.mask(l % r) );  break;
                        case ::MIR::eBinOp::ADD_OV:
                        case ::MIR::eBinOp::SUB_OV:
                        case ::MIR::eBinOp::MUL_OV:
                        case ::MIR::eBinOp::DIV_OV:
                            MIR_TODO(state, "RValue::BinOp - " << sa.src << ", val = " << l << " , " << r);

                        case ::MIR::eBinOp::BIT_OR : dst.write_uint( state, ti.bits, l | r);  break;
                        case ::MIR::eBinOp::BIT_AND: dst.write_uint( state, ti.bits, l & r );  break;
                        case ::MIR::eBinOp::BIT_XOR: dst.write_uint( state, ti.bits, l ^ r );  break;
                        case ::MIR::eBinOp::BIT_SHL: dst.write_uint( state, ti.bits, ti.mask(l << r) );  break;
                        case ::MIR::eBinOp::BIT_SHR: dst.write_uint( state, ti.bits, ti.mask(l >> r) );  break;

                        case ::MIR::eBinOp::EQ: dst.write_byte(state, l == r);  break;
                        case ::MIR::eBinOp::NE: dst.write_byte(state, l != r);  break;
                        case ::MIR::eBinOp::GT: dst.write_byte(state, l >  r);  break;
                        case ::MIR::eBinOp::GE: dst.write_byte(state, l >= r);  break;
                        case ::MIR::eBinOp::LT: dst.write_byte(state, l <  r);  break;
                        case ::MIR::eBinOp::LE: dst.write_byte(state, l <= r);  break;
                        }
                        break; }
                    case TypeInfo::Other:
                        MIR_BUG(state, "BinOp on " << ty_l);
                    }
                    }
                TU_ARMA(UniOp, e) {
                    ::HIR::TypeRef tmp;
                    const auto& ty_l = state.get_lvalue_type(tmp, e.val);
                    auto ti = TypeInfo::for_type(ty_l);

                    switch(ti.ty)
                    {
                    case TypeInfo::Unsigned:
                    case TypeInfo::Signed: {
                        auto i = local_state.get_lval(e.val).read_uint(state, ti.bits);
                        switch( e.op )
                        {
                        case ::MIR::eUniOp::INV:    i = ti.mask(~i);    break;
                        case ::MIR::eUniOp::NEG:    i = ~i + 1u; break;
                        }
                        dst.write_uint(state, ti.bits, i);
                        break; }
                    case TypeInfo::Float: {
                        auto v = local_state.get_lval(e.val).read_float(state, ti.bits);
                        switch( e.op )
                        {
                        case ::MIR::eUniOp::INV:    MIR_BUG(state, "Invalid invert of Float");
                        case ::MIR::eUniOp::NEG:    v = -v; break;
                        }
                        dst.write_float(state, ti.bits, v);
                        break; }
                    case TypeInfo::Other:
                        MIR_BUG(state, "UniOp on " << ty_l);
                    }
                    }
                TU_ARMA(DstMeta, e) {
                    MIR_TODO(state, "RValue::DstMeta");
                    }
                TU_ARMA(DstPtr, e) {
                    MIR_TODO(state, "RValue::DstPtr");
                    }
                TU_ARMA(MakeDst, e) {
                    size_t ptr_size = Target_GetPointerBits() / 8;
                    local_state.write_param(dst.slice(0, ptr_size),  e.ptr_val);
                    local_state.write_param(dst.slice(ptr_size),  e.meta_val);
                    }
                TU_ARMA(Tuple, e) {
                    ::HIR::TypeRef tmp;
                    const auto& ty = state.get_lvalue_type(tmp, sa.dst);
                    auto* repr = Target_GetTypeRepr(state.sp, state.m_resolve, ty);
                    if(!repr)   throw Defer();
                    MIR_ASSERT(state, repr->fields.size() == e.vals.size(), "");
                    for(size_t i = 0; i < e.vals.size(); i ++)
                    {
                        size_t sz = local_state.size_of_or_bug(repr->fields[i].ty);
                        local_state.write_param(dst.slice(repr->fields[i].offset, sz), e.vals[i]);
                    }
                    }
                TU_ARMA(Struct, e) {
                    ::HIR::TypeRef tmp;
                    const auto& ty = state.get_lvalue_type(tmp, sa.dst);
                    auto* repr = Target_GetTypeRepr(state.sp, state.m_resolve, ty);
                    if(!repr)   throw Defer();
                    MIR_ASSERT(state, repr->fields.size() == e.vals.size(), "");
                    for(size_t i = 0; i < e.vals.size(); i ++)
                    {
                        size_t sz = local_state.size_of_or_bug(repr->fields[i].ty);
                        auto local_dst = dst.slice(repr->fields[i].offset, sz);
                        local_state.write_param(local_dst, e.vals[i]);
                        DEBUG("@" << repr->fields[i].offset << " = " << local_dst);
                    }
                    }
                TU_ARMA(SizedArray, e) {
                    if( e.count > 0 )
                    {
                        ::HIR::TypeRef tmp;
                        const auto& ty = state.get_lvalue_type(tmp, sa.dst);
                        const auto& ity = ty.data().as_Array().inner;
                        size_t sz = local_state.size_of_or_bug(ity);

                        local_state.write_param( dst.slice(0, sz), e.val );
                        for(unsigned int i = 1; i < e.count; i++)
                            dst.slice(sz*i, sz).copy_from( state, dst.slice(0, sz) );
                    }
                    }
                TU_ARMA(Array, e) {
                    ::HIR::TypeRef tmp;
                    const auto& ty = state.get_lvalue_type(tmp, sa.dst);
                    const auto& ity = ty.data().as_Array().inner;
                    size_t sz = local_state.size_of_or_bug(ity);

                    size_t ofs = 0;
                    for(const auto& v : e.vals)
                    {
                        local_state.write_param( dst.slice(ofs, sz), v );
                        ofs += sz;
                    }
                    }
                TU_ARMA(UnionVariant, e) {
                    // TODO: Write some hidden information to contain the variant?
                    local_state.write_param( dst, e.val );
                    }
                TU_ARMA(EnumVariant, e) {
                    ::HIR::TypeRef tmp;
                    const auto& ty = state.get_lvalue_type(tmp, sa.dst);
                    auto* enm_repr = Target_GetTypeRepr(state.sp, state.m_resolve, ty);
                    if(!enm_repr)
                        throw Defer();
                    if( e.vals.size() > 0 )
                    {
                        auto ofs        = enm_repr->fields[e.index].offset;
                        const auto& ity = enm_repr->fields[e.index].ty;
                        auto* repr = Target_GetTypeRepr(state.sp, state.m_resolve, ity);
                        if(!repr)   throw Defer();
                        for(size_t i = 0; i < e.vals.size(); i ++)
                        {
                            size_t sz = local_state.size_of_or_bug(repr->fields[i].ty);
                            auto local_dst = dst.slice(ofs + repr->fields[i].offset, sz);
                            local_state.write_param(local_dst, e.vals[i]);
                            DEBUG("@" << (ofs + repr->fields[i].offset) << " = " << local_dst);
                        }
                    }

                    TU_MATCH_HDRA( (enm_repr->variants), {)
                    TU_ARMA(None, ve) {
                        }
                    TU_ARMA(NonZero, ve) {
                        // No tag to write, just leave as zeroes
                        if( e.index == ve.zero_variant ) {
                            auto ofs = get_offset(state.sp, resolve, enm_repr, ve.field);
                            auto saved_ofs = ofs;
                            for(size_t i = 0; i+8 <= ve.field.size; i += 8 ) {
                                dst.slice(ofs, 8).write_uint(state, 64, 0);
                                ofs += 8;
                            }
                            if(ve.field.size % 8 > 0)
                            {
                                dst.slice(ofs, 8).write_uint(state, (ve.field.size % 8) * 8, 0);
                            }
                            DEBUG("@" << ofs << " = " << dst.slice(saved_ofs, ve.field.size) << " NonZero");
                        }
                        else {
                            // No tag, already filled
                        }
                        }
                    TU_ARMA(Linear, ve) {
                        if( ve.is_niche(e.index) ) {
                            // No need to write tag, as this variant is the niche
                        }
                        else {
                            auto ofs = get_offset(state.sp, resolve, enm_repr, ve.field);
                            MIR_ASSERT(state, ve.field.size < 64/8, "");
                            dst.slice(ofs, ve.field.size).write_uint(state, ve.field.size * 8, ve.offset + e.index);
                        }
                        }
                    TU_ARMA(Values, ve) {
                        const auto& fld = enm_repr->fields[ve.field.index];
                        auto ti = TypeInfo::for_type(fld.ty);
                        MIR_ASSERT(state, ti.ty == TypeInfo::Signed || ti.ty == TypeInfo::Unsigned, "EnumVariant: Values not integer - " << fld.ty);
                        dst.slice(fld.offset, (ti.bits+7)/8).write_uint(state, ti.bits, ve.values.at(e.index));
                        }
                    }
                    }
                }

                DEBUG(sa.dst << " := " << dst);
            }
            state.set_cur_stmt_term(cur_block);
            DEBUG(state << block.terminator);
            TU_MATCH_HDRA( (block.terminator), {)
            default:
                MIR_BUG(state, "Unexpected terminator - " << block.terminator);
            TU_ARMA(Goto, e) {
                cur_block = e;
                }
            TU_ARMA(Return, e) {
                return std::move(local_state.retval);
                }
            TU_ARMA(Call, e) {
                auto dst = local_state.get_lval(e.ret_val);
                if( const auto* te = e.fcn.opt_Intrinsic() )
                {
                    if( te->name == "size_of" ) {
                        auto ty = ms.monomorph_type(state.sp, te->params.m_types.at(0));
                        size_t  size_val;
                        if( Target_GetSizeOf(state.sp, this->resolve, ty, size_val) )
                            dst.write_uint(state, Target_GetPointerBits(), size_val);
                        else
                            throw Defer();
                    }
                    else if( te->name == "min_align_of" ) {
                        auto ty = ms.monomorph_type(state.sp, te->params.m_types.at(0));
                        size_t  align_val;
                        if( Target_GetAlignOf(state.sp, this->resolve, ty, align_val) )
                            dst.write_uint(state, Target_GetPointerBits(), align_val);
                        else
                            throw Defer();
                    }
                    else if( te->name == "bswap" ) {
                        auto ty = ms.monomorph_type(state.sp, te->params.m_types.at(0));
                        MIR_ASSERT(state, ty.data().is_Primitive(), "bswap with non-primitive " << ty);
                        auto ti = TypeInfo::for_type(ty);
                        auto val = local_state.read_param_uint(ti.bits, e.args.at(0));
                        struct H {
                            static uint16_t bswap16(uint16_t v) {
                                return (v >> 8) | (v << 8);
                            }
                            static uint32_t bswap32(uint32_t v) {
                                return bswap16(v >> 16) | (static_cast<uint32_t>(bswap16(static_cast<uint16_t>(v))) << 16);
                            }
                            static uint64_t bswap64(uint64_t v) {
                                return bswap32(v >> 32) | (static_cast<uint64_t>(bswap32(static_cast<uint32_t>(v))) << 32);
                            }
                        };
                        uint64_t rv;
                        switch(ty.data().as_Primitive())
                        {
                        case ::HIR::CoreType::I8:
                        case ::HIR::CoreType::U8:
                            rv = val;
                            break;
                        case ::HIR::CoreType::I16:
                        case ::HIR::CoreType::U16:
                            rv = H::bswap16(val);
                            break;
                        case ::HIR::CoreType::I32:
                        case ::HIR::CoreType::U32:
                            rv = H::bswap32(val);
                            break;
                        case ::HIR::CoreType::I64:
                        case ::HIR::CoreType::U64:
                            rv = H::bswap64(val);
                            break;
                        default:
                            MIR_TODO(state, "Handle bswap with " << ty);
                        }
                        dst.write_uint(state, ti.bits, rv);
                    }
                    else if( te->name == "transmute" ) {
                        local_state.write_param(dst, e.args.at(0));
                    }
                    else {
                        MIR_TODO(state, "Call intrinsic \"" << te->name << "\" - " << block.terminator);
                    }
                }
                else if( const auto* te = e.fcn.opt_Path() )
                {
                    const auto& fcnp_raw = *te;
                    auto fcnp = ms.monomorph_path(state.sp, fcnp_raw);

                    MonomorphState  fcn_ms;
                    auto& fcn = get_function(this->root_span, this->resolve.m_crate, fcnp, fcn_ms);

                    // Argument values
                    ::std::vector<AllocationPtr>  call_args;
                    call_args.reserve( e.args.size() );
                    for(const auto& a : e.args)
                    {
                        ::HIR::TypeRef  tmp;
                        const auto& ty = state.get_param_type(tmp, a);
                        call_args.push_back(AllocationPtr::allocate(state, ty));
                        auto vr = ValueRef(call_args.back());
                        local_state.write_param( vr, a );
                    }

                    // Monomorphised argument types
                    ::HIR::Function::args_t arg_defs;
                    for(const auto& a : fcn.m_args)
                        arg_defs.push_back( ::std::make_pair(::HIR::Pattern(), fcn_ms.monomorph_type(this->root_span, a.second)) );

                    // TODO: Set m_const during parse and check here

                    // Call by invoking evaluate_constant on the function
                    {
                        TRACE_FUNCTION_F("Call const fn " << fcnp << " args={ " << call_args << " }");
                        auto fcn_ip = ::HIR::ItemPath(fcnp);
                        const auto* mir = this->resolve.m_crate.get_or_gen_mir( fcn_ip, fcn );
                        MIR_ASSERT(state, mir, "No MIR for function " << fcnp);
                        auto ret_ty = fcn_ms.monomorph_type(this->root_span, fcn.m_return);
                        auto rv = evaluate_constant_mir(fcn_ip, *mir, mv$(fcn_ms), mv$(ret_ty), arg_defs, mv$(call_args));
                        dst.copy_from( state, ValueRef(rv) );
                    }
                }
                else
                {
                    MIR_BUG(state, "Unexpected terminator - " << block.terminator);
                }
                DEBUG(e.ret_val << " := " << dst);
                cur_block = e.ret_block;
                }
            }
        } // for(;;) - Blocks
    }

    EncodedLiteral Evaluator::allocation_to_encoded(const ::HIR::TypeRef& ty, const ::MIR::eval::Allocation& a)
    {
        //const auto* a_bytes = a.get_bytes(0, a.size(), true);
        const auto* a_bytes = a.get_bytes(0, a.size(), false);  // NOTE: Read the uninitialised bytes (they _should_ be zeroes)
        ASSERT_BUG(this->root_span, a_bytes, "Unable to get entire allocation - " << FMT_CB(ss, a.fmt(ss, 0, a.size())));
        EncodedLiteral  rv;
        rv.bytes.insert(rv.bytes.begin(), a_bytes, a_bytes + a.size());
        for(const auto& r : a.get_relocations())
        {
            if( const auto* inner_alloc = r.ptr.as_allocation() ) {
                // Create a new static
                if( inner_alloc->is_writable() )
                {
                    auto inner_val = allocation_to_encoded(inner_alloc->get_type(), *inner_alloc);
                    auto item_path = nvs.new_static( inner_alloc->get_type().clone(), mv$(inner_val) );

                    rv.relocations.push_back(Reloc::new_named(r.offset, Target_GetPointerBits()/8, mv$(item_path)));
                }
                else
                {
                    // string
                    auto size = inner_alloc->size();
                    auto ptr = inner_alloc->get_bytes(0, size, true);
                    rv.relocations.push_back(Reloc::new_bytes(r.offset, Target_GetPointerBits()/8, ::std::string(ptr, ptr+size)));
                }
            }
            else if( const auto* sr = r.ptr.as_staticref() ) {
                // Just emit a path
                rv.relocations.push_back(Reloc::new_named(r.offset, Target_GetPointerBits()/8, sr->path().clone()));
            }
            else if( const auto* c = r.ptr.as_constant() ) {
                // string
                auto size = c->size();
                auto ptr = c->get_bytes(0, size, true);
                rv.relocations.push_back(Reloc::new_bytes(r.offset, Target_GetPointerBits()/8, ::std::string(ptr, ptr+size)));
            }
            else {
                BUG(this->root_span, "");
            }
        }
        return rv;
    }

    EncodedLiteral Evaluator::evaluate_constant(const ::HIR::ItemPath& ip, const ::HIR::ExprPtr& expr, ::HIR::TypeRef exp, MonomorphState ms/*={}*/)
    {
        TRACE_FUNCTION_F(ip);
        const auto* mir = this->resolve.m_crate.get_or_gen_mir(ip, expr, exp);

        if( mir ) {
            ::HIR::TypeRef  ty_self { "Self", GENERIC_Self };
            // Might want to have a fully-populated MonomorphState for expanding inside impl blocks
            // HACK: Generate a roughly-correct one
            const auto& top_ip = ip.get_top_ip();
            if( top_ip.trait && !top_ip.ty ) {
                ms.self_ty = ty_self.clone();
            }

            auto rv_raw = evaluate_constant_mir(ip, *mir, mv$(ms), exp.clone(), {}, {});
            ASSERT_BUG(this->root_span, rv_raw, "evaluate_constant_mir returned null allocation");
            DEBUG(ip << " = " << ::MIR::eval::ValueRef(rv_raw));

            return this->allocation_to_encoded(exp, *rv_raw);
        }
        else {
            BUG(this->root_span, "Attempting to evaluate constant expression with no associated code");
        }
    }
}   // namespace HIR

namespace {
    class Expander:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;
        const ::HIR::Module*  m_mod;
        const ::HIR::ItemPath*  m_mod_path;
        MonomorphState  m_monomorph_state;
        bool m_recurse_types;

        const ::HIR::GenericParams* m_impl_params;
        const ::HIR::GenericParams* m_item_params;

    public:
        Expander(const ::HIR::Crate& crate):
            m_crate(crate),
            m_mod(nullptr),
            m_mod_path(nullptr)
            ,m_recurse_types(false)
            ,m_impl_params(nullptr)
            ,m_item_params(nullptr)
        {}

        ::HIR::Evaluator get_eval(const Span& sp, NewvalState& nvs) const
        {
            auto eval = ::HIR::Evaluator { sp, m_crate, nvs };
            if(m_impl_params)   eval.resolve.set_impl_generics_raw(*m_impl_params);
            if(m_item_params)   eval.resolve.set_item_generics_raw(*m_item_params);
            return eval;
        }


        void visit_module(::HIR::ItemPath p, ::HIR::Module& mod) override
        {
            auto saved_mp = m_mod_path;
            auto saved_m = m_mod;
            m_mod = &mod;
            m_mod_path = &p;

            ::HIR::Visitor::visit_module(p, mod);

            m_mod = saved_m;
            m_mod_path = saved_mp;
        }
        void visit_function(::HIR::ItemPath p, ::HIR::Function& f) override
        {
            TRACE_FUNCTION_F(p);
            m_item_params = &f.m_params;
            ::HIR::Visitor::visit_function(p, f);
            m_item_params = nullptr;
        }

        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            static Span sp;
            TRACE_FUNCTION_F("impl" << impl.m_params.fmt_args() << " " << trait_path << impl.m_trait_args << " for " << impl.m_type);

            auto mp = ::HIR::ItemPath(impl.m_src_module);
            m_mod_path = &mp;
            m_mod = &m_crate.get_mod_by_path(sp, impl.m_src_module);

            ::HIR::PathParams   pp_impl;
            for(const auto& tp : impl.m_params.m_types)
                pp_impl.m_types.push_back( ::HIR::TypeRef(tp.m_name, pp_impl.m_types.size()) );
            m_monomorph_state.pp_impl = &pp_impl;
            m_impl_params = &impl.m_params;

            ::HIR::Visitor::visit_trait_impl(trait_path, impl);

            assert(m_impl_params);
            m_impl_params = nullptr;
            m_monomorph_state.pp_impl = nullptr;

            m_mod = nullptr;
            m_mod_path = nullptr;
        }
        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            static Span sp;
            TRACE_FUNCTION_F("impl" << impl.m_params.fmt_args() << " " << impl.m_type);

            auto mp = ::HIR::ItemPath(impl.m_src_module);
            m_mod_path = &mp;
            m_mod = &m_crate.get_mod_by_path(sp, impl.m_src_module);

            ::HIR::PathParams   pp_impl;
            for(const auto& tp : impl.m_params.m_types)
                pp_impl.m_types.push_back( ::HIR::TypeRef(tp.m_name, pp_impl.m_types.size()) );
            m_monomorph_state.pp_impl = &pp_impl;
            m_impl_params = &impl.m_params;

            ::HIR::Visitor::visit_type_impl(impl);

            assert(m_impl_params);
            m_impl_params = nullptr;
            m_monomorph_state.pp_impl = nullptr;

            m_mod = nullptr;
            m_mod_path = nullptr;
        }

        void visit_type(::HIR::TypeRef& ty) override
        {
            ::HIR::Visitor::visit_type(ty);

            if(auto* e = ty.data_mut().opt_Array())
            {
                TRACE_FUNCTION_FR(ty, ty);
                if( e->size.is_Unevaluated() )
                {
                    const auto& expr_ptr = *e->size.as_Unevaluated();
                    auto ty_name = FMT("ty_" << &ty << "#");

                    auto nvs = NewvalState { *m_mod, *m_mod_path, ty_name };
                    auto eval = get_eval(expr_ptr->span(), nvs);
                    try
                    {
                        auto val = eval.evaluate_constant(::HIR::ItemPath(*m_mod_path, ty_name.c_str()), expr_ptr, ::HIR::CoreType::Usize);
                        // TODO: Read a usize out of the literal
                        e->size = EncodedLiteralSlice(val).read_uint();
                        DEBUG("Array " << ty << " - size = " << e->size.as_Known());
                    }
                    catch(const Defer& )
                    {
                        const auto* tn = dynamic_cast<const HIR::ExprNode_ConstParam*>(&*expr_ptr);
                        if(tn) {
                            e->size = HIR::GenericRef(tn->m_name, tn->m_binding);
                        }
                        else {
                            //TODO(expr_ptr->span(), "Handle defer for array sizes");
                        }
                    }
                }
                else
                {
                    DEBUG("Array " << ty << " - size = " << e->size);
                }
            }

            if( m_recurse_types )
            {
                m_recurse_types = false;
                if( const auto* te = ty.data().opt_Path() )
                {
                    TU_MATCH_HDRA( (te->binding), {)
                    TU_ARMA(Unbound, _) {
                        }
                    TU_ARMA(Opaque, _) {
                        }
                    TU_ARMA(Struct, pbe) {
                        // If this struct hasn't been visited already, visit it
                        auto saved_ip = m_impl_params;
                        m_impl_params = nullptr;
                        this->visit_struct(te->path.m_data.as_Generic().m_path, const_cast<::HIR::Struct&>(*pbe));
                        m_impl_params = saved_ip;
                        }
                    TU_ARMA(Union, pbe) {
                        }
                    TU_ARMA(Enum, pbe) {
                        }
                    TU_ARMA(ExternType, pbe) {
                        }
                    }
                }
                m_recurse_types = true;
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override
        {
            TRACE_FUNCTION_F(p);
            m_item_params = &item.m_params;

            m_recurse_types = true;
            ::HIR::Visitor::visit_constant(p, item);
            m_recurse_types = false;

            // NOTE: Consteval needed here for MIR match generation to work
            if( item.m_value || item.m_value.m_mir )
            {
                auto nvs = NewvalState { *m_mod, *m_mod_path, FMT(p.get_name() << "#") };
                auto eval = get_eval(item.m_value.span(), nvs);
                try
                {
                    item.m_value_res = eval.evaluate_constant(p, item.m_value, item.m_type.clone(), m_monomorph_state.clone());
                    //check_lit_type(item.m_value.span(), item.m_type, item.m_value_res);
                    item.m_value_state = ::HIR::Constant::ValueState::Known;
                }
                catch(const Defer&)
                {
                    item.m_value_state = ::HIR::Constant::ValueState::Generic;
                }


                DEBUG("constant: " << item.m_type <<  " = " << item.m_value_res);
            }
            else
            {
                DEBUG("constant? " << item.m_value);
            }

            m_item_params = nullptr;
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override
        {
            //m_item_params = &item.m_params;

            m_recurse_types = true;
            ::HIR::Visitor::visit_static(p, item);
            m_recurse_types = false;

            if( item.m_value )
            {
                auto nvs = NewvalState { *m_mod, *m_mod_path, FMT(p.get_name() << "#") };
                auto eval = get_eval(item.m_value.span(), nvs);
                try
                {
                    item.m_value_res = eval.evaluate_constant(p, item.m_value, item.m_type.clone());
                    item.m_value_generated = true;
                }
                catch(const Defer&)
                {
                    ERROR(item.m_value->span(), E0000, "Defer top-level static?");
                }

                DEBUG("static: " << item.m_type <<  " = " << item.m_value_res);
            }

            //m_item_params = nullptr;
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            static Span sp;
            assert(!m_impl_params);
            m_impl_params = &item.m_params;

            visit_enum_inner(m_crate, p, *m_mod, *m_mod_path, p.get_name(), item);
            ::HIR::Visitor::visit_enum(p, item);

            assert(m_impl_params);
            m_impl_params = nullptr;
        }
        void visit_struct(::HIR::ItemPath p, ::HIR::Struct& item) override {
            assert(!m_impl_params);
            m_impl_params = &item.m_params;
            if( item.const_eval_state != HIR::ConstEvalState::Complete )
            {
                ASSERT_BUG(Span(), item.const_eval_state == HIR::ConstEvalState::None, "Constant evaluation loop involving " << p);
                item.const_eval_state = HIR::ConstEvalState::Active;
                ::HIR::Visitor::visit_struct(p, item);
                item.const_eval_state = HIR::ConstEvalState::Complete;
            }
            assert(m_impl_params);
            m_impl_params = nullptr;
        }

        void visit_expr(::HIR::ExprPtr& expr) override
        {
            struct Visitor:
                public ::HIR::ExprVisitorDef
            {
                Expander& m_exp;

                Visitor(Expander& exp):
                    m_exp(exp)
                {}

                void visit_type(::HIR::TypeRef& ty) override {
                    // Need to evaluate array sizes
                    DEBUG("expr type " << ty);
                    m_exp.visit_type(ty);
                }
                void visit_path_params(::HIR::PathParams& pp) override {
                    // Explicit call to handle const params (eventually)
                    m_exp.visit_path_params(pp);
                }

                void visit(::HIR::ExprNode_ArraySized& node) override {
                    assert( node.m_size );
                    auto name = FMT("array_" << &node << "#");
                    auto nvs = NewvalState { *m_exp.m_mod, *m_exp.m_mod_path, name };
                    auto eval = m_exp.get_eval(node.m_size->span(), nvs);
                    try
                    {
                        auto val = eval.evaluate_constant( ::HIR::ItemPath(*m_exp.m_mod_path, name.c_str()), node.m_size, ::HIR::CoreType::Usize );
                        node.m_size_val = static_cast<size_t>(EncodedLiteralSlice(val).read_uint());
                        DEBUG("Array literal [?; " << node.m_size_val << "]");
                    }
                    catch(const Defer& )
                    {
                        TODO(node.span(), "Defer ArraySized");
                    }
                }
            };

            if( expr.get() != nullptr )
            {
                Visitor v { *this };
                //m_recurse_types = true;
                (*expr).visit(v);
                //m_recurse_types = false;
            }
        }


        static void visit_enum_inner(const ::HIR::Crate& crate, const ::HIR::ItemPath& p, const ::HIR::Module& mod, const ::HIR::ItemPath& mod_path, const char* name, ::HIR::Enum& item)
        {
            if( auto* e = item.m_data.opt_Value() )
            {
                auto ty = ::HIR::Enum::get_repr_type(item.m_tag_repr);
                uint64_t i = 0;
                for(auto& var : e->variants)
                {

                    if( var.expr )
                    {
                        auto nvs = NewvalState { mod, mod_path, FMT(name << "_" << var.name << "#") };
                        auto eval = ::HIR::Evaluator { var.expr->span(), crate, nvs };
                        eval.resolve.set_impl_generics_raw(item.m_params);
                        try
                        {
                            auto val = eval.evaluate_constant(p, var.expr, ty.clone());
                            DEBUG("enum variant: " << p << "::" << var.name << " = " << val);
                            i = EncodedLiteralSlice(val).read_sint();
                        }
                        catch(const Defer&)
                        {
                            BUG(var.expr->span(), "");
                        }
                    }
                    var.val = i;
                    if(!var.expr)
                    {
                        DEBUG("enum variant: " << p << "::" << var.name << " = " << var.val << " (auto)");
                    }
                    i ++;
                }
                e->evaluated = true;
            }
        }
    };

    class ExpanderApply:
        public ::HIR::Visitor
    {

    public:
        ExpanderApply()
        {
        }

        void visit_module(::HIR::ItemPath p, ::HIR::Module& mod) override
        {
            if( ! mod.m_inline_statics.empty() )
            {
                for(auto& v : mod.m_inline_statics)
                {
                    // ::std::unique_ptr<VisEnt<ValueItem>>
                    ::std::unique_ptr<::HIR::VisEnt<::HIR::ValueItem>>  iv;
                    iv.reset( new ::HIR::VisEnt<::HIR::ValueItem> { ::HIR::Publicity::new_none(), ::HIR::ValueItem::make_Static(mv$(*v.second)) } );
                    mod.m_value_items.insert(::std::make_pair( v.first, mv$(iv) ));
                }
                mod.m_inline_statics.clear();
            }

            ::HIR::Visitor::visit_module(p, mod);

        }
    };
}   // namespace

void ConvertHIR_ConstantEvaluate(::HIR::Crate& crate)
{
    Expander    exp { crate };
    exp.visit_crate( crate );

    ExpanderApply().visit_crate(crate);
    for(auto& new_ty_pair : crate.m_new_types)
    {
        crate.m_root_module.m_mod_items.insert( mv$(new_ty_pair) );
    }
}
void ConvertHIR_ConstantEvaluate_Expr(const ::HIR::Crate& crate, const ::HIR::ItemPath& ip, ::HIR::ExprPtr& expr_ptr)
{
    TRACE_FUNCTION_F(ip);
    // Check innards but NOT the value
    Expander    exp { crate };
    exp.visit_expr( expr_ptr );
}
void ConvertHIR_ConstantEvaluate_Enum(const ::HIR::Crate& crate, const ::HIR::ItemPath& ip, const ::HIR::Enum& enm)
{
    auto mod_path = ip.get_simple_path();
    auto item_name = mod_path.m_components.back();
    mod_path.m_components.pop_back();
    const auto& mod = crate.get_mod_by_path(Span(), mod_path);

    auto& item = const_cast<::HIR::Enum&>(enm);

    Expander::visit_enum_inner(crate, ip, mod, mod_path, item_name.c_str(), item);
}
