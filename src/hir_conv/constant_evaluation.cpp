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
    static const ::HIR::TypeRef  ty_Self = ::HIR::TypeRef::new_self();

    void ConvertHIR_ConstantEvaluate_Static(const ::HIR::Crate& crate, const ::HIR::GenericParams* impl_params, const ::HIR::ItemPath& ip, ::HIR::Static& e);
    void ConvertHIR_ConstantEvaluate_FcnSig(const ::HIR::Crate& crate, const ::HIR::GenericParams* impl_params, const ::HIR::ItemPath& ip, ::HIR::Function& fcn);

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
        static AllocationPtr allocate(const StaticTraitResolve& resolve, const ::MIR::TypeResolve& state, const ::HIR::TypeRef& ty);
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
        virtual uint8_t* ext_write_bytes(size_t ofs, size_t len) = 0;
        void write_bytes(size_t ofs, const void* data, size_t len) {
            memcpy(ext_write_bytes(ofs, len), data, len);
        }
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
        uint8_t* ext_write_bytes(size_t ofs, size_t len) override { abort(); }
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
        uint8_t data[1];

        Allocation(size_t len, const ::HIR::TypeRef& ty)
            : reference_count(1)
            , length(len)
            , is_readonly(false)
            , m_type(ty.clone())
        {
            memset(data, 0, len + (len + 7) / 8);
        }
        Allocation(const Allocation&) = delete;
        Allocation& operator=(const Allocation&) = delete;
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
                    uint8_t mask = (0xFF >> (8-len));
                    *dst = (*dst & ~mask) | (*src & mask);
                }
            }
        }

        bool is_writable() const override {
            return !is_readonly;
        }
        uint8_t* ext_write_bytes(size_t ofs, size_t len) override
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
            // Clear impacted relocations
            auto it = std::remove_if(this->relocations.begin(), this->relocations.end(), [&](const Reloc& r){ return (ofs <= r.offset && r.offset < ofs+len); });
            this->relocations.resize( it - this->relocations.begin() );
            return this->data + ofs;
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
            if(m_encoded) {
                os << EncodedLiteralSlice(*m_encoded).slice(ofs, len);
            }
            else {
                os << "?";
            }
        }

        size_t size() const { return m_encoded ? m_encoded->bytes.size() : 0; }
        const uint8_t* get_bytes(size_t ofs, size_t len, bool check_mask) const override {
            if(m_encoded) {
                assert(ofs <= m_encoded->bytes.size());
                assert(len <= m_encoded->bytes.size());
                assert(ofs+len <= m_encoded->bytes.size());
                if(m_encoded->bytes.size() == 0) {
                    // Empty vectors can have a null data pointer
                    return reinterpret_cast<const uint8_t*>("");
                }
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
        uint8_t* ext_write_bytes(size_t ofs, size_t len) override {
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
                            return RelocPtr(StaticRefPtr::allocate(r.p->clone(), nullptr));
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
            ASSERT_BUG(Span(), ofs <= this->len && ofs+len <= this->len, "ValueRef::slice: " << ofs << "+" << len << " out of range (" << this->len << ")");

            ValueRef    rv;
            rv.storage = storage;
            rv.ofs = this->ofs + ofs;
            rv.len = len;
            return rv;
        }
        ValueRef slice(size_t ofs) {
            ASSERT_BUG(Span(), ofs <= this->len, "ValueRef::slice: " << ofs << " out of range (" << this->len << ")");
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
        uint8_t* ext_write_bytes(const MIR::TypeResolve& state, size_t len) {
            MIR_ASSERT(state, storage, "Writing to invalid slot");
            MIR_ASSERT(state, storage.as_value().is_writable(), "Writing to read-only slot");
            if(len > 0) {
                return storage.as_value().ext_write_bytes(ofs, len);
            }
            else {
                static uint8_t empty_buf;
                return &empty_buf;
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
            assert(bits <= 64);
            write_uint(state, bits, U128(v));
        }
        void write_uint(const MIR::TypeResolve& state, unsigned bits, U128 v) {
            auto n_bytes = (bits+7)/8;
            if(Target_GetCurSpec().m_arch.m_big_endian) {
                v.to_be_bytes(ext_write_bytes(state, n_bytes), n_bytes);
            }
            else {
                v.to_le_bytes(ext_write_bytes(state, n_bytes), n_bytes);
            }
        }
        void write_sint(const MIR::TypeResolve& state, unsigned bits, S128 v) {
            auto n_bytes = (bits+7)/8;
            if(Target_GetCurSpec().m_arch.m_big_endian) {
                v.get_inner().to_be_bytes(ext_write_bytes(state, n_bytes), n_bytes);
            }
            else {
                v.get_inner().to_le_bytes(ext_write_bytes(state, n_bytes), n_bytes);
            }
        }
        void write_ptr(const MIR::TypeResolve& state, uint64_t val, RelocPtr reloc) {
            write_uint(state, Target_GetPointerBits(), U128(val));
            storage.as_value().set_reloc(ofs, std::move(reloc));
        }
        void set_reloc(RelocPtr reloc) {
            storage.as_value().set_reloc(ofs, std::move(reloc));
        }

        const uint8_t* ext_read_bytes(const MIR::TypeResolve& state, size_t len) const {
            MIR_ASSERT(state, storage, "");
            MIR_ASSERT(state, len >= 1, "");
            const auto* src = storage.as_value().get_bytes(ofs, len, /*check_mask*/true);
            MIR_ASSERT(state, src, "Invalid read: " << ofs << "+" << len << " (in " << *this << ")");
            return src;
        }
        void read_bytes(const MIR::TypeResolve& state, void* data, size_t len) const {
            const auto* src = ext_read_bytes(state, len);
            assert(src);
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
            assert(bits <= 128);
            auto n_bytes = (bits+7)/8;
            U128    rv;
            if(Target_GetCurSpec().m_arch.m_big_endian) {
                rv.from_be_bytes(ext_read_bytes(state, n_bytes), n_bytes);
            }
            else {
                rv.from_le_bytes(ext_read_bytes(state, n_bytes), n_bytes);
            }
            return rv;
        }
        S128 read_sint(const ::MIR::TypeResolve& state, unsigned bits) const {
            auto n_bytes = (bits+7)/8;
            S128    rv;
            if(Target_GetCurSpec().m_arch.m_big_endian) {
                rv.from_be_bytes(ext_read_bytes(state, n_bytes), n_bytes);
            }
            else {
                rv.from_le_bytes(ext_read_bytes(state, n_bytes), n_bytes);
            }
            return rv;
        }
        uint64_t read_usize(const ::MIR::TypeResolve& state) const {
            return read_uint(state, Target_GetPointerBits()).truncate_u64();
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
            os << "ValueRef({" << vr.ofs << "+" << vr.len << "}";
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
    AllocationPtr AllocationPtr::allocate(const StaticTraitResolve& resolve, const ::MIR::TypeResolve& state, const ::HIR::TypeRef& ty)
    {
        size_t len;
        if( !Target_GetSizeOf(Span(), resolve, ty, len) )    throw Defer();
        auto* rv_raw = reinterpret_cast<Allocation*>( malloc(sizeof(Allocation) + len + ((len+7) / 8)) );
        AllocationPtr   rv;
        // TODO: Include the current location from `state` in the allocation header
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
}}  // namespace MIR::eval

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

    EntPtr get_ent_fullpath(const Span& sp, const ::StaticTraitResolve& resolve, const ::HIR::Path& path, EntNS ns, MonomorphState& out_ms, const ::HIR::GenericParams** out_impl_params_def=nullptr)
    {
        if(const auto* gp = path.m_data.opt_Generic()) {
            const auto& name = gp->m_path.components().back();
            const auto& mod = resolve.m_crate.get_mod_by_path(sp, gp->m_path, /*ignore_last*/true);
            // TODO: This pointer will be invalidated...
            for(const auto& is : mod.m_inline_statics) {
                if(is.first == name)
                    return &*is.second;
            }
        }
        auto v = resolve.get_value(sp, path, out_ms, false, out_impl_params_def);
        TU_MATCH_HDRA( (v), { )
        TU_ARMA(NotFound, e)
            return EntPtr();
        TU_ARMA(NotYetKnown, e)
            return EntPtr();
            //TODO(sp, "Handle NotYetKnown - " << path);
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
    const ::HIR::Function& get_function(const Span& sp, const ::StaticTraitResolve& resolve, const ::HIR::Path& path, MonomorphState& out_ms, const ::HIR::GenericParams*& out_impl_params_def)
    {
        auto rv = get_ent_fullpath(sp, resolve, path, EntNS::Value, out_ms, &out_impl_params_def);
        if(const auto* fcn_p = rv.opt_Function()) {
            const HIR::Function& fcn = **fcn_p;
            const auto& ep = fcn.m_code;
            if( ep && ep.m_state->stage < ::HIR::ExprState::Stage::ConstEval)
            {
                auto prev = ep.m_state->stage;
                ep.m_state->stage = ::HIR::ExprState::Stage::ConstEvalRequest;
                // Run consteval on the arguments and return type
                ConvertHIR_ConstantEvaluate_FcnSig(resolve.m_crate, out_impl_params_def, path, const_cast<HIR::Function&>(fcn));
                ep.m_state->stage = prev;
            }
            return fcn;
        }
        else {
            TODO(sp, "Could not find function for " << path << " - " << rv.tag_str());
        }
    }


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

        U128 mask(U128 v) const {
            if(bits < 64)
            {
                uint64_t mask_val = (static_cast<uint64_t>(1ull) << bits) - 1;
                assert(mask_val != 0);
                return U128(v.get_lo() & mask_val);
            }
            else if( bits == 64 )
            {
                return U128(v.get_lo());
            }
            else if( bits < 128 )
            {
                U128 mask_val = (U128(1) << bits) - 1u;
                assert(mask_val != 0);
                return U128(v & mask_val);
            }
            else if( bits == 128 )
            {
                return v;
            }
            else
            {
                throw "";
            }
        }
        U128 mask(S128 v) const {
            if( v < 0 ) {
                // Negate, mask, and re-negate
                return ( -S128( mask( (-v).get_inner() ) )).get_inner();
            }
            else {
                return mask((v).get_inner());
            }
        }
        double mask(double v) const {
            return v;
        }
    };

    const unsigned TERM_RET_PUSHED = UINT_MAX-1;
    const unsigned TERM_RET_RETURN = UINT_MAX;
}   // namespace <anon>

namespace MIR { namespace eval {

    class CallStackEntry
    {
    public:
        const unsigned  frame_index;
        const std::vector<std::pair< HIR::Pattern, HIR::TypeRef>> arg_defs;
        const HIR::TypeRef  ret_type;

        // MIR Resolve Helper
        const StaticTraitResolve&  root_resolve;
        StaticTraitResolve  resolve;
        ::MIR::TypeResolve state;
        // Monomorphiser from the function
        MonomorphState ms;

        ::MIR::eval::AllocationPtr   retval;

        ::std::vector<::MIR::eval::AllocationPtr>   args;

        ::std::vector<HIR::TypeRef>   local_types;
        ::std::vector<::MIR::eval::AllocationPtr>  locals;

        // ---
        CallStackEntry(const CallStackEntry& ) = delete;
        CallStackEntry(CallStackEntry&& ) = delete;

        CallStackEntry(
            unsigned frame_index,
            const Span& root_span,
            const StaticTraitResolve& resolve,
            ::FmtLambda path_str,
            // Pre-monomorphised function signature (as this may be a `static`)
            HIR::TypeRef  exp_ty,
            std::vector<std::pair< HIR::Pattern, HIR::TypeRef>>   arg_defs,
            // Function/Body code
            const MIR::Function& fcn,
            // Monomorphisation rules
            MonomorphState ms,
            ::std::vector<AllocationPtr> args,
            const ::HIR::GenericParams* item_params_def,
            const ::HIR::GenericParams* impl_params_def
        )
            : frame_index(frame_index)
            , arg_defs(std::move(arg_defs))
            , ret_type(std::move(exp_ty))
            , root_resolve(resolve)
            , resolve(resolve.m_crate)
            , state { root_span, this->resolve, std::move(path_str), this->ret_type, this->arg_defs, fcn }
            , ms(std::move(ms))
            , retval( AllocationPtr::allocate(root_resolve, state, ret_type) )
            , args(args)
        {
            this->resolve.set_both_generics_raw(impl_params_def, item_params_def);
            local_types.reserve( state.m_fcn.locals.size() );
            locals     .reserve( state.m_fcn.locals.size() );
            for(size_t i = 0; i < state.m_fcn.locals.size(); i ++)
            {
                local_types.push_back( state.m_resolve.monomorph_expand(state.sp, state.m_fcn.locals[i], this->ms) );
                locals.push_back( AllocationPtr::allocate(root_resolve, state, local_types.back()) );
            }

            state.m_monomorphed_rettype = &ret_type;
            state.m_monomorphed_locals = &local_types;
        }

        HIR::TypeRef monomorph_expand(const HIR::TypeRef& ty) const
        {
            return this->resolve.monomorph_expand(this->state.sp, ty, this->ms);
        }

        StaticRefPtr get_staticref_mono(const ::HIR::Path& p, HIR::TypeRef* out_ty=nullptr) const
        {
            // NOTE: Value won't need to be monomorphed, as it shouldn't be generic
            return get_staticref( ms.monomorph_path(state.sp, p), out_ty );
        }
        StaticRefPtr get_staticref(::HIR::Path p, HIR::TypeRef* out_ty=nullptr) const
        {
            // If there's any mention of generics in this path, then return Literal::Defer
            if( visit_path_tys_with(p, [&](const auto& ty)->bool { return ty.data().is_Generic(); }) )
            {
                DEBUG("Return Literal::Defer for constastatic " << p << " which references a generic parameter");
                throw Defer();
            }
            MonomorphState  const_ms;

            const HIR::GenericParams* impl_params_def = nullptr;
            auto ent = get_ent_fullpath(state.sp, root_resolve, p, EntNS::Value,  const_ms, &impl_params_def);
            if(ent.is_Static())
            {
                const auto& s = *ent.as_Static();

                if( !s.m_value_generated )
                {
                    // If there's no MIR and no HIR then this is an external static (which can only be borrowed)
                    if( !s.m_value && !s.m_value.m_mir ) {
                        DEBUG("No value and no mir");
                        return StaticRefPtr::allocate(std::move(p), nullptr);
                    }

                    auto& item = const_cast<::HIR::Static&>(s);

                    static ::std::set<::HIR::Static*>   s_non_recurse;
                    if( s_non_recurse.count(&item) == 0 ) {
                        s_non_recurse.insert(&item);
                        ConvertHIR_ConstantEvaluate_Static(resolve.m_crate, impl_params_def, p, item);
                        s_non_recurse.erase( s_non_recurse.find(&item) );
                    }
                    else {
                        DEBUG("Recursion detected");
                    }
                }

                if( !s.m_value_generated )
                {
                    auto& item = const_cast<::HIR::Static&>(s);

                    // Challenge: Adding items to the module might invalidate an iterator.
                    ::HIR::ItemPath mod_ip { item.m_value.m_state->m_mod_path };
                    auto nvs = NewvalState(item.m_value.m_state->m_module, mod_ip, FMT("static" << &item << "#"));
                    auto eval = ::HIR::Evaluator(item.m_value.span(), root_resolve.m_crate, nvs);
                    DEBUG("- Evaluate " << p);
                    try
                    {
                        item.m_value_generated = true;
                        item.m_value_res = eval.evaluate_constant(::HIR::ItemPath(p), item.m_value, item.m_type.clone());
                        item.m_value_generated = true;
                    }
                    catch(const Defer& )
                    {
                        MIR_BUG(state, p << " Defer during value generation");
                    }
                    DEBUG(p << " = " << item.m_value_res);
                }
                if(out_ty) {
                    // Does this need monomorph? No, becuase the value is known and thus not generic?
                    *out_ty = s.m_type.clone();
                }
                return StaticRefPtr::allocate(std::move(p), &s.m_value_res);
            }
            else
            {
                DEBUG(ent.tag_str() << " " << p);
                if( out_ty ) {
                    MIR_TODO(state, "Get type for " << ent.tag_str() << " (" << p << ")");
                }
                return StaticRefPtr::allocate(std::move(p), nullptr);
            }
        }

        ValueRef get_lval(const ::MIR::LValue& lv, ValueRef* meta=nullptr)
        {
            ::HIR::TypeRef  tmp_ty;
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
                val = ValueRef(get_staticref_mono(e, lv.m_wrappers.empty() ? nullptr : &tmp_ty));
                if( !lv.m_wrappers.empty() ) {
                    MIR_ASSERT(state, tmp_ty != HIR::TypeRef(), "Type not set?");
                }
                typ = &tmp_ty;
                }
            }

            for(const auto& w : lv.m_wrappers)
            {
                MIR_ASSERT(state, typ, "Type not set when unwrapping - " << lv);
                DEBUG(w << " " << val << ": " << *typ);
                TU_MATCH_HDRA( (w), {)
                TU_ARMA(Field, e) {
                    if( typ->data().is_Slice() || typ->data().is_Array() ) {
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
                            throw "";
                        }
                        metadata = ValueRef();
                        size_t sz, al;
                        if( !Target_GetSizeAndAlignOf(state.sp, root_resolve, *typ,  sz, al) )
                            throw Defer();
                        MIR_ASSERT(state, sz < SIZE_MAX, "Unsized type on index output - " << *typ);
                        size_t  index = e;
                        // HACK: Allow one-past-end for `[foo, ref bar @ ...]` support
                        if( index == size ) {
                            val = val.slice(index * sz, 0);
                        }
                        else {
                            MIR_ASSERT(state, index < size, "LValue::Index index out of range - " << index << " >= " << size);
                            val = val.slice(index * sz, sz);
                        }
                        continue;
                    }
                    auto* repr = Target_GetTypeRepr(state.sp, this->root_resolve, *typ);
                    MIR_ASSERT(state, repr, "No repr for " << *typ);
                    MIR_ASSERT(state, e < repr->fields.size(), "LValue::Field index out of range");
                    if(repr->size != SIZE_MAX) {
                        metadata = ValueRef();
                    }
                    auto ofs = repr->fields[e].offset;
                    typ = &repr->fields[e].ty;

                    size_t sz, al;
                    if( !Target_GetSizeAndAlignOf(state.sp, root_resolve, *typ,  sz, al) )
                        throw Defer();
                    if( sz == SIZE_MAX ) {
                        val = val.slice(ofs);
                    }
                    else {
                        val = val.slice(ofs, sz);
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
                        MIR_BUG(state, "Deref of unsupported type - " << *typ);
                    }
                    // If the inner type is unsized
                    size_t sz, al;
                    if( !Target_GetSizeAndAlignOf(state.sp, root_resolve, *typ,  sz, al) )
                        throw Defer();
                    if( sz == SIZE_MAX ) {
                        // Read metadata
                        DEBUG("Reading metadata");
                        metadata = val.slice(Target_GetPointerBits()/8);
                    }
                    auto p = val.read_ptr(state);
                    MIR_ASSERT(state, p.first >= EncodedLiteral::PTR_BASE, "Null (<PTR_BASE) pointer deref");
                    MIR_ASSERT(state, p.first % al == 0, "Unaligned pointer deref");
                    DEBUG("> " << ValueRef(p.second) << " - o=" << (p.first - EncodedLiteral::PTR_BASE) << " sz=" << sz << " " << *typ);
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
                        MIR_BUG(state, "Index of unsupported type - " << *typ);
                    }
                    metadata = ValueRef();
                    size_t sz, al;
                    if( !Target_GetSizeAndAlignOf(state.sp, root_resolve, *typ,  sz, al) )
                        throw Defer();
                    MIR_ASSERT(state, sz < SIZE_MAX, "Unsized type on index output - " << *typ);
                    MIR_ASSERT(state, e < locals.size(), "LValue::Index index local out of range");
                    size_t  index = ValueRef(locals[e]).read_usize(state);
                    MIR_ASSERT(state, index < size, "LValue::Index index out of range - " << index << " >= " << size);
                    val = val.slice(index * sz, sz);
                    }
                TU_ARMA(Downcast, e) {
                    auto* repr = Target_GetTypeRepr(state.sp, this->root_resolve, *typ);
                    MIR_ASSERT(state, repr, "No repr for " << *typ);
                    MIR_ASSERT(state, e < repr->fields.size(), "LValue::Downcast index out of range");
                    if(repr->size != SIZE_MAX) {
                        metadata = ValueRef();
                    }
                    typ = &repr->fields[e].ty;
                    val = val.slice(repr->fields[e].offset, size_of_or_bug(*typ));
                    }
                }
            }
            if(meta)
                *meta = std::move(metadata);
            return val;
        }

        const EncodedLiteral& get_const(const ::HIR::Path& in_p, ::HIR::TypeRef* out_ty) const
        {
            auto p = ms.monomorph_path(state.sp, in_p);
            root_resolve.expand_associated_types_path(state.sp, p);
            // If there's any mention of generics in this path, then return Literal::Defer
            if( visit_path_tys_with(p, [&](const auto& ty)->bool { return ty.data().is_Generic(); }) )
            {
                DEBUG("Return Literal::Defer for constant " << p << " which references a generic parameter");
                throw Defer();
            }
            MonomorphState  const_ms;
            const HIR::GenericParams* impl_params_def = nullptr;
            auto ent = get_ent_fullpath(state.sp, root_resolve, p, EntNS::Value,  const_ms, &impl_params_def);
            MIR_ASSERT(state, ent.is_Constant(), "MIR Constant::Const(" << p << ") didn't point to a Constant - " << ent.tag_str());
            const auto& c = *ent.as_Constant();
            if( c.m_value_state == HIR::Constant::ValueState::Unknown )
            {
                auto& item = const_cast<::HIR::Constant&>(c);
                // Challenge: Adding items to the module might invalidate an iterator.
                ::HIR::ItemPath mod_ip { item.m_value.m_state->m_mod_path };
                auto nvs = NewvalState(item.m_value.m_state->m_module, mod_ip, FMT("const" << &c << "#"));
                auto eval = ::HIR::Evaluator(item.m_value.span(), root_resolve.m_crate, nvs);
                eval.resolve.set_both_generics_raw(impl_params_def, &c.m_params);
                DEBUG("- Evaluate " << p);
                try
                {
                    item.m_value_res = eval.evaluate_constant(::HIR::ItemPath(p), item.m_value, item.m_type.clone(), const_ms.clone());
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
                    auto eval = ::HIR::Evaluator(item.m_value.span(), root_resolve.m_crate, nvs);
                    eval.resolve.set_both_generics_raw(impl_params_def, &c.m_params);

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

        void write_encoded(ValueRef dst, const EncodedLiteral& encoded)
        {
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
                assert(e2.p);
                const auto& encoded = get_const(*e2.p, &ty);
                DEBUG(*e2.p << " = " << encoded);

                write_encoded(dst, encoded);
            }
            TU_ARM(c, Generic, e2) {
                auto v = ms.get_value(state.sp, e2);
                TU_MATCH_HDRA( (v), { )
                default:
                    MIR_TODO(state, "Handle expanded generic: " << v);
                TU_ARMA(Generic, _) {
                    throw Defer();
                }
                TU_ARMA(Evaluated, ve) {
                    DEBUG(e2 << " = " << *ve);
                    write_encoded(dst, *ve);
                }
                }
            }
            TU_ARM(c, Function, e2) {
            }
            TU_ARM(c, ItemAddr, e2) {
                assert(e2);
                dst.write_ptr(state, EncodedLiteral::PTR_BASE, get_staticref_mono(*e2));
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

        const EncodedLiteral& get_const(const HIR::ConstGeneric& v, EncodedLiteral& tmp) const
        {
            TU_MATCH_HDRA( (v), {)
            TU_ARMA(Infer, ve) {
                MIR_BUG(state, "Encountered Infer value in constant?");
                }
            TU_ARMA(Generic, ve) {
                throw Defer {};
                }
            TU_ARMA(Unevaluated, ve) {
                MIR_TODO(state, "Evaluate const - " << v);

#if 0
                //::HIR::ItemPath mod_ip { item.m_value.m_state->m_mod_path };
                //auto nvs = NewvalState(item.m_value.m_state->m_module, mod_ip, FMT("const" << &c << "#"));
                auto eval = ::HIR::Evaluator(item.m_value.span(), root_resolve.m_crate, nvs);
                // TODO: Does this need to set generics?
                try
                {
                    item.m_value_res = eval.evaluate_constant(::HIR::ItemPath(p), item.m_value, item.m_type.clone());
                    item.m_value_state = HIR::Constant::ValueState::Known;
                }
                catch(const Defer& )
                {
                    item.m_value_state = HIR::Constant::ValueState::Generic;
                }
#endif
                }
            TU_ARMA(Evaluated, ve) {
                return *ve;
                }
            }
            throw "";
        }

        /// Read a floating point value from a MIR::Param
        double read_param_float(unsigned bits, const ::MIR::Param& p) const
        {
            TU_MATCH_HDRA( (p), {)
            TU_ARMA(LValue, e)
                return const_cast<CallStackEntry*>(this)->get_lval(e).read_float(state, bits);
            TU_ARMA(Borrow, e)
                MIR_BUG(state, "Expected a float, got a MIR::Param::Borrow");
            TU_ARMA(Constant, e) {
                if(e.is_Const()) {
                    const auto& val = get_const(*e.as_Const().p, nullptr);
                    // TODO: Check the type from get_const
                    return EncodedLiteralSlice(val).read_float();
                }
                if(e.is_Generic()) {
                    auto ve = ms.get_value(state.sp, e.as_Generic());
                    EncodedLiteral  el_tmp;
                    const auto& el = get_const(ve, el_tmp);
                    return EncodedLiteralSlice(el).read_float();
                }
                MIR_ASSERT(state, e.is_Float(), "Expected a float, got " << e);
                return e.as_Float().v;
                }
            }
            abort();
        }

        U128 read_param_uint(unsigned bits, const ::MIR::Param& p) const
        {
            TU_MATCH_HDRA( (p), { )
            TU_ARMA(LValue, e)
                return const_cast<CallStackEntry*>(this)->get_lval(e).read_uint(state, bits);
            TU_ARMA(Borrow, e)
                MIR_BUG(state, "Expected an integer, got a MIR::Param::Borrow");
            TU_ARMA(Constant, e) {
                if(e.is_Const()) {
                    const auto& val = get_const(*e.as_Const().p, nullptr);
                    // TODO: Check the type from get_const
                    return EncodedLiteralSlice(val).read_uint();
                }
                if(e.is_Generic()) {
                    auto ve = ms.get_value(state.sp, e.as_Generic());
                    EncodedLiteral  el_tmp;
                    const auto& el = get_const(ve, el_tmp);
                    return EncodedLiteralSlice(el).read_uint();
                }
                if(e.is_Int())
                    return e.as_Int().v.get_inner();
                if(e.is_Bool())
                    return U128( e.as_Bool().v ? 1 : 0 );
                MIR_ASSERT(state, e.is_Uint(), "Expected an integer, got " << e.tag_str() << " " << e);
                return U128( e.as_Uint().v );
                }
            }
            abort();
        }
        S128 read_param_sint(unsigned bits, const ::MIR::Param& p) const
        {
            TU_MATCH_HDRA( (p), { )
            TU_ARMA(LValue, e)
                return const_cast<CallStackEntry*>(this)->get_lval(e).read_sint(state, bits);
            TU_ARMA(Borrow, e)
                MIR_BUG(state, "Expected an integer, got a MIR::Param::Borrow");
            TU_ARMA(Constant, e) {
                if(e.is_Const()) {
                    const auto& val = get_const(*e.as_Const().p, nullptr);
                    // TODO: Check the type from get_const
                    return EncodedLiteralSlice(val).read_sint();
                }
                if(e.is_Generic()) {
                    auto ve = ms.get_value(state.sp, e.as_Generic());
                    EncodedLiteral  el_tmp;
                    const auto& el = get_const(ve, el_tmp);
                    return EncodedLiteralSlice(el).read_sint();
                }
                MIR_ASSERT(state, e.is_Int(), "Expected an integer, got " << e.tag_str() << " " << e);
                return S128( e.as_Int().v );
                }
            }
            abort();
        }

        std::pair<uint64_t, RelocPtr> read_param_ptr(const ::MIR::Param& p) const
        {
            TU_MATCH_HDRA( (p), {)
            TU_ARMA(LValue, e) {
                return const_cast<CallStackEntry*>(this)->get_lval(e).read_ptr(state);
                }
            TU_ARMA(Borrow, e) {
                MIR_TODO(state, "read_param_ptr - " << p);
                }
            TU_ARMA(Constant, e) {
                if( !e.is_ItemAddr() ) {
                    MIR_BUG(state, "Invalid argument for pointer: " << p);
                }
                // TODO: Look up the static
                return ::std::make_pair(EncodedLiteral::PTR_BASE, RelocPtr(get_staticref_mono(*e.as_ItemAddr())));
                }
            }
            abort();
        }

        size_t size_of_or_bug(const ::HIR::TypeRef& ty) const
        {
            size_t rv;
            if( !Target_GetSizeOf(state.sp, root_resolve, ty, /*out*/rv) )
                MIR_BUG(state, "No size for " << ty);
            return rv;
        }
    };

} } // namespace ::MIR::eval

namespace {
    ::std::pair<::MIR::eval::ValueRef, ::MIR::eval::ValueRef> get_tuple_t_bool(const ::MIR::eval::CallStackEntry& local_state, ::MIR::eval::ValueRef& src, const HIR::TypeRef& t)
    {
        auto tuple_t = ::HIR::TypeRef::new_tuple({ t.clone(), ::HIR::TypeRef(::HIR::CoreType::Bool) });
        auto* repr = Target_GetTypeRepr(local_state.state.sp, local_state.root_resolve, tuple_t);
        MIR_ASSERT(local_state.state, repr, "No repr for " << tuple_t);
        auto s = local_state.size_of_or_bug(t);
        return std::make_pair(
            src.slice(repr->fields[0].offset, s),
            src.slice(repr->fields[1].offset, 1)
            );
    }
    bool do_arith_checked(
        ::MIR::eval::CallStackEntry& local_state,
        const HIR::TypeRef& ty,
        ::MIR::eval::ValueRef& dst,
        const ::MIR::Param& val_l,
        ::MIR::eBinOp op,
        const ::MIR::Param& val_r,
        // Should the output be saturated
        bool saturate=false
    )
    {
        auto ti = TypeInfo::for_type(ty);
        const auto& state = local_state.state;
        bool did_overflow = false;

        // NOTE: Shifts can use any integer as the RHS, so give them special handling
        if(op == ::MIR::eBinOp::BIT_SHL || op == ::MIR::eBinOp::BIT_SHR )
        {
            ::HIR::TypeRef  tmp_r;
            const auto& ty_r = local_state.state.get_param_type(tmp_r, val_r);
            auto ti_r = TypeInfo::for_type(ty_r);

            auto r = ti_r.ty == TypeInfo::Unsigned
                ? local_state.read_param_uint(ti_r.bits, val_r)
                : local_state.read_param_sint(ti_r.bits, val_r).get_inner();
            auto amt = r.truncate_u64();
            if( amt > ti.bits ) {
                DEBUG("Shift out of range - " << r << " > " << ti.bits);
                did_overflow = true;
                amt = 0;
            }
            switch(ti.ty)
            {
            case TypeInfo::Unsigned: {
                auto l = local_state.read_param_uint(ti.bits, val_l);
                switch(op)
                {
                case ::MIR::eBinOp::BIT_SHL: dst.write_uint(state, ti.bits, ti.mask(l << amt));  break;
                case ::MIR::eBinOp::BIT_SHR: dst.write_uint(state, ti.bits, ti.mask(l >> amt));  break;
                default:    MIR_BUG(state, "This block should only be active for SHL/SHR");
                }
                break; }
            case TypeInfo::Signed: {
                auto l = local_state.read_param_sint(ti.bits, val_l);
                switch(op)
                {
                case ::MIR::eBinOp::BIT_SHL: dst.write_uint(state, ti.bits, ti.mask(l << amt));  break;
                case ::MIR::eBinOp::BIT_SHR: dst.write_uint(state, ti.bits, ti.mask(l >> amt));  break;
                default:    MIR_BUG(state, "This block should only be active for SHL/SHR");
                }
                break; }
            default:
                MIR_BUG(state, "Invalid use of BIT_SHL/BIT_SHR on " << ty);
            }
            return did_overflow;
        }
        {
            ::HIR::TypeRef  tmp_r;
            MIR_ASSERT(state, ty == local_state.state.get_param_type(tmp_r, val_r), "BinOp with mismatched types");
        }

        switch(ti.ty)
        {
        case TypeInfo::Float: {
            auto l = local_state.read_param_float(ti.bits, val_l);
            auto r = local_state.read_param_float(ti.bits, val_r);
            switch(op)
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
                MIR_TODO(state, "do_arith float unimplemented - val = " << l << " , " << r);

            case ::MIR::eBinOp::BIT_OR :
            case ::MIR::eBinOp::BIT_AND:
            case ::MIR::eBinOp::BIT_XOR:
                MIR_BUG(state, "do_arith float with bitwise - val = " << l << " , " << r);
            case ::MIR::eBinOp::BIT_SHL:
            case ::MIR::eBinOp::BIT_SHR:
                MIR_BUG(state, "Bitshifts should be handled in caller");
            case ::MIR::eBinOp::EQ: dst.write_byte(state, l == r);  break;
            case ::MIR::eBinOp::NE: dst.write_byte(state, l != r);  break;
            case ::MIR::eBinOp::GT: dst.write_byte(state, l >  r);  break;
            case ::MIR::eBinOp::GE: dst.write_byte(state, l >= r);  break;
            case ::MIR::eBinOp::LT: dst.write_byte(state, l <  r);  break;
            case ::MIR::eBinOp::LE: dst.write_byte(state, l <= r);  break;
            }
            break; };
        case TypeInfo::Unsigned: {
            auto l = local_state.read_param_uint(ti.bits, val_l);
            auto r = local_state.read_param_uint(ti.bits, val_r);
            switch(op)
            {
            case ::MIR::eBinOp::ADD: {
                auto res = ti.mask(l + r);
                did_overflow = res < l;
                if( did_overflow && saturate ) {
                    res = ti.mask(~U128());
                }
                dst.write_uint(state, ti.bits, res);
                break; }
            case ::MIR::eBinOp::SUB: {
                auto res = ti.mask(l - r);
                did_overflow = res > l;
                if( did_overflow && saturate ) {
                    res = ti.mask(U128(0));
                }
                dst.write_uint(state, ti.bits, res);
                break; }
            case ::MIR::eBinOp::MUL: {
                auto res =  ti.mask(l * r);
                if( l != 0 && r != 0 ) {
                    did_overflow = res < l || res < r;
                }
                if( did_overflow && saturate ) {
                    res = ti.mask(~U128());
                }
                dst.write_uint(state, ti.bits, res);
                break; }
            case ::MIR::eBinOp::DIV:
                // Early-prevent division by zero
                if( r == 0 ) {
                    dst.write_uint(state, ti.bits, U128(0));
                    return true;
                }
                dst.write_uint(state, ti.bits, ti.mask(l / r));
                break;
            case ::MIR::eBinOp::MOD:
                // Early-prevent division by zero
                if( r == 0 ) {
                    dst.write_uint(state, ti.bits, U128(0));
                    return true;
                }
                dst.write_uint(state, ti.bits, ti.mask(l % r)); 
                break;
            case ::MIR::eBinOp::ADD_OV:
            case ::MIR::eBinOp::SUB_OV:
            case ::MIR::eBinOp::MUL_OV:
            case ::MIR::eBinOp::DIV_OV:
                MIR_TODO(state, "do_arith unsigned - val = " << l << " , " << r);

            case ::MIR::eBinOp::BIT_OR : dst.write_uint(state, ti.bits, l | r);  break;
            case ::MIR::eBinOp::BIT_AND: dst.write_uint(state, ti.bits, l & r);  break;
            case ::MIR::eBinOp::BIT_XOR: dst.write_uint(state, ti.bits, l ^ r);  break;
            case ::MIR::eBinOp::BIT_SHL:
            case ::MIR::eBinOp::BIT_SHR:
                MIR_BUG(state, "Bitshifts should be handled in caller");

            case ::MIR::eBinOp::EQ: dst.write_byte(state, l == r);  break;
            case ::MIR::eBinOp::NE: dst.write_byte(state, l != r);  break;
            case ::MIR::eBinOp::GT: dst.write_byte(state, l >  r);  break;
            case ::MIR::eBinOp::GE: dst.write_byte(state, l >= r);  break;
            case ::MIR::eBinOp::LT: dst.write_byte(state, l <  r);  break;
            case ::MIR::eBinOp::LE: dst.write_byte(state, l <= r);  break;
            }
            break; }
        case TypeInfo::Signed: {
            auto l = local_state.read_param_sint(ti.bits, val_l);
            auto r = local_state.read_param_sint(ti.bits, val_r);
            switch(op)
            {
            case ::MIR::eBinOp::ADD: {
                // Convert to raw/unsigned repr
                auto v1u = l.get_inner();
                auto v2u = r.get_inner();
                // Then convert into a sign and absolute value
                auto v1s = (l < 0);
                auto v2s = (r < 0);
                auto v1a = v1s ? ~v1u + 1 : v1u;
                auto v2a = v2s ? ~v2u + 1 : v2u;

                // Determine the sign
                // - Equal has the same sign
                // - V2 negative is negative if |v2| > |v1|
                // - V1 negative is negative if |v2| < |v1|
                bool res_sign = (v1s == v2s) ? v1s : (v2s ? v1a < v2a : v1a > v2a);
                auto res = S128(v1u + v2u);
                did_overflow = ((res < 0) != res_sign);
                if( did_overflow && saturate ) {
                    auto v = U128(0) << (ti.bits-1);
                    res = res_sign ? S128(v) : S128(v - 1);
                }
                dst.write_sint(state, ti.bits, res);
                break; }
            case ::MIR::eBinOp::SUB: {
                auto res = l - r;
                // If the masked value isn't equal to the non-masked, then it's an overflow.
                // TODO: What about 128 bit arith?
                did_overflow = res.get_inner() != ti.mask(res);
                if(did_overflow && saturate) {
                    MIR_TODO(state, "do_arith signed sub overflow - saturate");
                }
                dst.write_uint( state, ti.bits, ti.mask(res) );
                break; }
            case ::MIR::eBinOp::MUL: {
                auto res = l * r;
                if( l != 0 && r != 0 ) {
                    if( res.u_abs() < l.u_abs() || res.u_abs() < r.u_abs() ) {
                        did_overflow = true;
                    }
                }
                if(did_overflow && saturate) {
                    MIR_TODO(state, "do_arith signed mul overflow - saturate");
                }
                dst.write_uint( state, ti.bits, ti.mask(res) );
                break; }
            case ::MIR::eBinOp::DIV:
                if( r == 0 ) {
                    dst.write_uint(state, ti.bits, U128(0));
                    return true;
                }
                dst.write_sint(state, ti.bits, ti.mask(l / r));
                break;
            case ::MIR::eBinOp::MOD:
                if( r == 0 ) {
                    dst.write_uint(state, ti.bits, U128(0));
                    return true;
                }
                dst.write_sint(state, ti.bits, ti.mask(l % r)); 
                break;
            case ::MIR::eBinOp::ADD_OV:
            case ::MIR::eBinOp::SUB_OV:
            case ::MIR::eBinOp::MUL_OV:
            case ::MIR::eBinOp::DIV_OV:
                MIR_TODO(state, "do_arith signed - val = " << l << " , " << r);

            case ::MIR::eBinOp::BIT_OR : dst.write_uint( state, ti.bits, (l | r).get_inner() );  break;
            case ::MIR::eBinOp::BIT_AND: dst.write_uint( state, ti.bits, (l & r).get_inner() );  break;
            case ::MIR::eBinOp::BIT_XOR: dst.write_uint( state, ti.bits, (l ^ r).get_inner() );  break;
            case ::MIR::eBinOp::BIT_SHL:
            case ::MIR::eBinOp::BIT_SHR:
                MIR_BUG(state, "Bitshifts should be handled in caller");

            case ::MIR::eBinOp::EQ: dst.write_byte(state, l == r);  break;
            case ::MIR::eBinOp::NE: dst.write_byte(state, l != r);  break;
            case ::MIR::eBinOp::GT: dst.write_byte(state, l >  r);  break;
            case ::MIR::eBinOp::GE: dst.write_byte(state, l >= r);  break;
            case ::MIR::eBinOp::LT: dst.write_byte(state, l <  r);  break;
            case ::MIR::eBinOp::LE: dst.write_byte(state, l <= r);  break;
            }
            break; }
        case TypeInfo::Other:
            if( false
                || TU_TEST2(ty.data(), Borrow, .inner.data(), Slice, .inner == HIR::CoreType::U8)
                || TU_TEST1(ty.data(), Borrow, .inner == HIR::CoreType::Str)
                ) {
                struct P {
                    ::MIR::eval::RelocPtr   reloc;
                    const void* data;
                    size_t  len;
                    P(::MIR::eval::CallStackEntry& local_state, const ::MIR::Param& p)
                    {
                        auto vr = local_state.get_lval(p.as_LValue());
                        auto ptr = vr.read_ptr(local_state.state);
                        this->len = vr.slice(Target_GetPointerBits()/8).read_usize(local_state.state);
                        this->data = ptr.second.as_value().get_bytes(ptr.first - EncodedLiteral::PTR_BASE, this->len, true);
                        MIR_ASSERT(local_state.state, this->data, "Invalid pointer " << p << " : " << vr << " = " << ptr.second << " @ " << ptr.first << "+" << this->len);
                        this->reloc = std::move(ptr.second);
                    }
                };
                auto ptr_l = P(local_state, val_l);
                auto ptr_r = P(local_state, val_r);
                int cmp = memcmp(ptr_l.data, ptr_r.data, std::min(ptr_l.len, ptr_l.len));
                if( cmp == 0 ) {
                    if( ptr_l.len != ptr_r.len ) {
                        cmp = ptr_l.len < ptr_r.len ? -1 : 1;
                    }
                }
                switch(op)
                {
                case ::MIR::eBinOp::EQ: dst.write_byte(state, cmp == 0);  break;
                case ::MIR::eBinOp::NE: dst.write_byte(state, cmp != 0);  break;
                case ::MIR::eBinOp::GT: dst.write_byte(state, cmp >  0);  break;
                case ::MIR::eBinOp::GE: dst.write_byte(state, cmp >= 0);  break;
                case ::MIR::eBinOp::LT: dst.write_byte(state, cmp <  0);  break;
                case ::MIR::eBinOp::LE: dst.write_byte(state, cmp <= 0);  break;
                default:
                    MIR_BUG(state, "BinOp " << int(op) << " on " << ty << " - Byte slice or &str");
                }
                break;
            }
            else {
                MIR_BUG(state, "BinOp on " << ty);
            }
        }
        return did_overflow;
    }
}

namespace HIR {

    using namespace ::MIR::eval;

    unsigned int Evaluator::s_next_eval_index = 0;

    Evaluator::CsePtr::~CsePtr()
    {
        if( m_inner ) {
            delete m_inner;
            m_inner = nullptr;
        }
    }

    void Evaluator::push_stack_entry(
        ::FmtLambda print_path, const ::MIR::Function& fcn, MonomorphState ms,
        ::HIR::TypeRef exp, ::HIR::Function::args_t arg_defs,
        ::std::vector<::MIR::eval::AllocationPtr> args,
        const ::HIR::GenericParams* item_params_def,
        const ::HIR::GenericParams* impl_params_def
    )
    {
        this->call_stack.push_back(new CallStackEntry(
            this->num_frames,
            this->root_span, this->resolve,
            std::move(print_path), std::move(exp), std::move(arg_defs), fcn, std::move(ms),
            std::move(args),
            item_params_def, impl_params_def
        ));
        this->num_frames += 1;
    }

    AllocationPtr Evaluator::run_until_stack_empty()
    {
        const unsigned MAX_BLOCK_COUNT = 1'000'000;
        const unsigned MAX_STMT_COUNT = 4'000'000;
        assert( !this->call_stack.empty() );
        unsigned int num_stmts_run = 0;
        unsigned int idx;
        for(idx = 0; idx < MAX_BLOCK_COUNT; idx += 1)
        {
            if( num_stmts_run > MAX_STMT_COUNT ) {
                break;
            }

            auto& state = this->call_stack.back()->state;
            const auto& bb = state.m_fcn.blocks[state.get_cur_block()];
            for(const auto& stmt : bb.statements)
            {
                state.set_cur_stmt(state.get_cur_block(), &stmt - bb.statements.data());
                this->run_statement(*this->call_stack.back(), stmt);
                num_stmts_run += 1;
            }
            state.set_cur_stmt_term(state.get_cur_block());
            auto next_block = run_terminator(*this->call_stack.back(), bb.terminator);
            num_stmts_run += 1;
            switch(next_block)
            {
            case TERM_RET_PUSHED:
                continue;
            case TERM_RET_RETURN: {
                MIR::eval::AllocationPtr rv = std::move(this->call_stack.back()->retval);
                this->call_stack.pop_back();
                if( this->call_stack.empty() == 1 ) {
                    return rv;
                }
                else {
                    auto& next_state = *this->call_stack.back();
                    const auto& term = next_state.state.m_fcn.blocks[next_state.state.get_cur_block()].terminator;
                    const auto& te = term.as_Call();
                    auto dst = next_state.get_lval(te.ret_val);
                    dst.copy_from(next_state.state, ValueRef(rv));
                    next_state.state.set_cur_stmt(te.ret_block, 0);
                }
                break; }
            default:
                state.set_cur_stmt(next_block, 0);
            }
        }
        ERROR(this->root_span, E0000, "Constant evaluation ran for too long - " << num_stmts_run << " statements, " << idx << " blocks");
    }

    void Evaluator::run_statement(::MIR::eval::CallStackEntry& local_state, const ::MIR::Statement& stmt)
    {
        const auto& state = local_state.state;
        DEBUG("E" << this->eval_index << " F" << local_state.frame_index << " " << state << stmt);

        TU_MATCH_HDRA( (stmt), { )
        TU_ARMA(Assign, e) {
            // Fall through
            }
        TU_ARMA(Drop, se) {
            // HACK: Ignore drops... for now
            return;
            }
        TU_ARMA(ScopeEnd, se) {
            // Just ignore, it's a hint
            return;
            }
        TU_ARMA(SetDropFlag, se) {
            // Ignore drop flags, we're ignoring drops
            if( se.other == UINT_MAX ) {
                //MIR_TODO(state, "Set df$" << se.idx << " = " << se.new_val);
            }
            else {
                //MIR_TODO(state, "Set df$" << se.idx << " = " << (se.new_val ? "!" : "") << "df$" << se.other);
            }
            return;
            }
        TU_ARMA(Asm, se) { MIR_TODO(state, "Non-assign statement - " << stmt); }
        TU_ARMA(Asm2, se) { MIR_TODO(state, "Non-assign statement - " << stmt); }
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
                MIR_TODO(state, "RValue::Cast to " << e.type << " from " << src_ty << ", val = " << inval);
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
                        dst.write_uint( state, ti.bits, v.get_inner() );
                        } break;
                    case TypeInfo::Unsigned:
                        MIR_ASSERT(state, !src_ty.data().is_NamedFunction(), "");
                        dst.write_uint( state, ti.bits, inval.read_uint(state, src_ti.bits) );
                        break;
                    case TypeInfo::Float:
                        if( ti.ty == TypeInfo::Signed )
                            dst.write_uint( state, ti.bits, static_cast<int64_t>(inval.read_float(state, src_ti.bits)) );
                        else
                            dst.write_uint( state, ti.bits, static_cast<uint64_t>(inval.read_float(state, src_ti.bits)) );
                        break;
                    case TypeInfo::Other: {
                        MIR_ASSERT(state, TU_TEST1(src_ty.data(), Path, .binding.is_Enum()), "Constant cast Variant to integer with invalid type - " << src_ty);
                        MIR_ASSERT(state, src_ty.data().as_Path().binding.as_Enum(), "Enum binding pointer not set! - " << src_ty);
                        const HIR::Enum& enm = *src_ty.data().as_Path().binding.as_Enum();
                        MIR_ASSERT(state, enm.is_value(), "Constant cast Variant to integer with non-value enum - " << src_ty);
                        const auto* repr = Target_GetTypeRepr(state.sp, resolve, src_ty);
                        if(!repr)   throw Defer();
                        auto& ve = repr->variants.as_Values();

                        auto v = inval.slice( repr->get_offset(state.sp, resolve, ve.field), ve.field.size).read_uint(state, ve.field.size*8);
                        // TODO: Ensure that this is a valid variant?
                        dst.write_uint( state, ti.bits, v );
                        } break;
                    }
                    break;
                case TypeInfo::Float:
                    switch(src_ti.ty)
                    {
                    // NOTE: Subtle rounding differences between f32 and f64
                    case TypeInfo::Signed: {
                        auto v = S128(inval.read_uint(state, src_ti.bits));
                        dst.write_float(state, ti.bits, ti.bits == 32 ? v.to_float() : v.to_double() );
                        break; }
                    case TypeInfo::Unsigned: {
                        auto v = inval.read_uint(state, src_ti.bits);
                        dst.write_float(state, ti.bits, ti.bits == 32 ? v.to_float() : v.to_double() );
                        break; }
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
                break;
            // Allow casting any integer value to a pointer (TODO: Ensure that the pointer is sized?)
            case HIR::TypeData::TAG_Pointer:
            case HIR::TypeData::TAG_Function:
                if( const auto* e = src_ty.data().opt_NamedFunction() ) {
                    dst.write_ptr(state, EncodedLiteral::PTR_BASE, local_state.get_staticref_mono(e->path));
                }
                else {
                    dst.copy_from( state, inval.slice(0, std::min(inval.get_len(), dst.get_len())) );
                }
                break;
            }
            }
        TU_ARMA(BinOp, e) {
            ::HIR::TypeRef tmp;
            const auto& ty_l = state.get_param_type(tmp, e.val_l);
            //auto ti = TypeInfo::for_type(ty_l);
            bool did_overflow = do_arith_checked(local_state, ty_l, dst, e.val_l, e.op, e.val_r);
            switch(e.op)
            {
            case ::MIR::eBinOp::DIV:
            case ::MIR::eBinOp::MOD:
                if(did_overflow) {
                    MIR_BUG(state, "Division/modulo by zero!");
                }
                break;
            case ::MIR::eBinOp::BIT_SHL:
            case ::MIR::eBinOp::BIT_SHR:
                if(did_overflow) {
                    MIR_BUG(state, "Bit shift out of range");
                }
                break;
            default:
                break;
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
            auto v = local_state.get_lval(e.val);
            size_t ptr_size = Target_GetPointerBits() / 8;
            dst.copy_from(state, v.slice(ptr_size));
            }
        TU_ARMA(DstPtr, e) {
            auto v = local_state.get_lval(e.val);
            size_t ptr_size = Target_GetPointerBits() / 8;
            dst.copy_from(state, v.slice(0, ptr_size));
            }
        TU_ARMA(MakeDst, e) {
            if( TU_TEST2(e.meta_val, Constant, ,ItemAddr, .get() == nullptr) ) {

                ::HIR::TypeRef  tmp;
                const auto& src_ty = state.get_param_type(tmp, e.ptr_val);
                ::HIR::TypeRef  tmp2;
                const auto& dst_ty = state.get_lvalue_type(tmp2, sa.dst);

                TU_MATCH_HDRA( (dst_ty.data()), {)
                default:
                    // NOTE: Can be an unsizing!
                    MIR_TODO(state, "RValue::MakeDst Coerce to " << dst_ty);
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
                                done = true;
                            }
                        }
                    }
                    if(!done )
                    {
                        MIR_TODO(state, "RValue::MakeDst Coerce to " << dst_ty);
                    }
                    }
                TU_ARMA(Borrow, te) {
                    const auto* dynamic_type_d = &te.inner;
                    const auto* dynamic_type_s = &src_ty.data().as_Borrow().inner;
                    for(;;)
                    {
                        if( const auto* tep = dynamic_type_d->data().opt_Path() )
                        {
                            MIR_ASSERT(state, tep->binding.is_Struct(), "RValue::MakeDst to " << *dynamic_type_d);
                            const auto& sm = tep->binding.as_Struct()->m_struct_markings;
                            dynamic_type_d = &tep->path.m_data.as_Generic().m_params.m_types.at(sm.unsized_param);
                            dynamic_type_s = &dynamic_type_s->data().as_Path().path.m_data.as_Generic().m_params.m_types.at(sm.unsized_param);
                        }
                        else
                        {
                            break;
                        }
                    }
                    // TODO: What can cast TO a borrow? - Non-converted dyn unsizes .. but they require vtables,
                    // which aren't available yet!
                    if( const auto* tep = dynamic_type_d->data().opt_TraitObject() )
                    {
                        static const RcString rcstring_vtable = RcString::new_interned("vtable#");
                        auto vtable_path = ::HIR::Path(dynamic_type_s->clone(), tep->m_trait.m_path.clone(), rcstring_vtable);
                        dst.slice(Target_GetPointerBits()/8).write_ptr(state, EncodedLiteral::PTR_BASE, local_state.get_staticref(std::move(vtable_path)));
                    }
                    else if( /*const auto* tep =*/ dynamic_type_d->data().opt_Slice() )
                    {
                        auto size = dynamic_type_s->data().as_Array().size.as_Known();
                        dst.slice(Target_GetPointerBits()/8).write_uint(state, Target_GetPointerBits(), size);
                    }
                    else
                    {
                        MIR_BUG(state, "RValue::MakeDst to " << dst_ty << " from " << src_ty << " - " << *dynamic_type_d << " from " << *dynamic_type_s);
                    }
                    }
                }

                if( const auto* p = e.ptr_val.opt_Borrow() ) {
                    local_state.write_borrow( dst, p->type, p->val );
                }
                else if( const auto* c = e.ptr_val.opt_Constant() ) {
                    local_state.write_const(dst, *c);
                }
                else {
                    auto inval = local_state.get_lval(e.ptr_val.as_LValue());
                    dst.slice(0,Target_GetPointerBits()/8).copy_from( state, inval );
                }
            }
            else {
                size_t ptr_size = Target_GetPointerBits() / 8;
                local_state.write_param(dst.slice(0, ptr_size),  e.ptr_val);
                local_state.write_param(dst.slice(ptr_size),  e.meta_val);
            }
            }
        TU_ARMA(Tuple, e) {
            ::HIR::TypeRef tmp;
            const auto& ty = state.get_lvalue_type(tmp, sa.dst);
            auto* repr = Target_GetTypeRepr(state.sp, resolve, ty);
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
            auto* repr = Target_GetTypeRepr(state.sp, resolve, ty);
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
            size_t count = 0;
            TU_MATCH_HDRA( (e.count), {)
            TU_ARMA(Known, v) {
                count = v;
                }
            TU_ARMA(Unevaluated, v) {
                const auto* vp = &v;
                HIR::ConstGeneric   tmp_v;
                if(const auto* ve = v.opt_Generic() ) {
                    vp = &(tmp_v = local_state.ms.get_value(state.sp, *ve));
                }
                EncodedLiteral tmp_val;
                count = local_state.get_const(*vp, tmp_val).read_usize(0);
                }
            }

            if( count > 0 )
            {
                ::HIR::TypeRef tmp;
                const auto& ty = state.get_lvalue_type(tmp, sa.dst);
                const auto& ity = ty.data().as_Array().inner;
                size_t sz = local_state.size_of_or_bug(ity);

                local_state.write_param( dst.slice(0, sz), e.val );
                for(unsigned int i = 1; i < count; i++)
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
            auto* enm_repr = Target_GetTypeRepr(state.sp, resolve, ty);
            if(!enm_repr)
                throw Defer();
            if( e.vals.size() > 0 )
            {
                auto ofs        = enm_repr->fields[e.index].offset;
                const auto& ity = enm_repr->fields[e.index].ty;
                auto* repr = Target_GetTypeRepr(state.sp, resolve, ity);
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
                        dst.slice(ofs, ve.field.size % 8).write_uint(state, (ve.field.size % 8) * 8, 0);
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

        DEBUG("> E" << this->eval_index << " F" << local_state.frame_index << " " << sa.dst << " := " << dst);
    }

    unsigned Evaluator::run_terminator(::MIR::eval::CallStackEntry& local_state, const ::MIR::Terminator& terminator)
    {
        const auto& state = local_state.state;
        DEBUG("E" << this->eval_index << " F" << local_state.frame_index << " " << state << terminator);

        TU_MATCH_HDRA( (terminator), {)
        default:
            MIR_BUG(state, "Unexpected terminator - " << terminator);
        TU_ARMA(Goto, e) {
            return e;
            }
        TU_ARMA(Return, e) {
            return TERM_RET_RETURN;
            }
        TU_ARMA(If, e) {
            bool res = U128(0) != local_state.get_lval(e.cond).read_uint(state, 1);
            DEBUG(state << " IF " << res);
            return res ? e.bb_true : e.bb_false;
            }
        TU_ARMA(Switch, e) {
            HIR::TypeRef    tmp;
            const auto& ty = state.get_lvalue_type(tmp, e.val);
            auto* enm_repr = Target_GetTypeRepr(state.sp, resolve, ty);
            auto lit = local_state.get_lval(e.val);

            // TODO: Share code with `MIR_Cleanup_LiteralToRValue`/`PatternRulesetBuilder::append_from_lit`
            unsigned var_idx = 0;
            TU_MATCH_HDRA( (enm_repr->variants), { )
            TU_ARMA(None, e) {
                }
            TU_ARMA(Linear, ve) {
                auto v = lit.slice( enm_repr->get_offset(state.sp, resolve, ve.field), ve.field.size).read_uint(state, 8*ve.field.size).truncate_u64();
                if( v < ve.offset ) {
                    var_idx = ve.field.index;
                    DEBUG("VariantMode::Linear - Niche #" << var_idx);
                }
                else {
                    var_idx = v - ve.offset;
                    DEBUG("VariantMode::Linear - Other #" << var_idx);
                }
                }
            TU_ARMA(Values, ve) {
                auto v = lit.slice( enm_repr->get_offset(state.sp, resolve, ve.field), ve.field.size).read_uint(state, 8*ve.field.size).truncate_u64();
                auto it = std::find(ve.values.begin(), ve.values.end(), v);
                ASSERT_BUG(state.sp, it != ve.values.end(), "Invalid enum tag: " << v << " for " << ty);
                var_idx = it - ve.values.begin();
                }
            TU_ARMA(NonZero, ve) {
                size_t ofs = enm_repr->get_offset(state.sp, resolve, ve.field);
                bool is_nonzero = false;
                for(size_t i = 0; i < ve.field.size; i ++) {
                    if( lit.slice(ofs+i, 1).read_uint(state, 8).truncate_u64() != 0 ) {
                        is_nonzero = true;
                        break;
                    }
                }

                var_idx = (is_nonzero ? 1 - ve.zero_variant : ve.zero_variant);
                }
            }
            DEBUG(state << " = " << var_idx);
            MIR_ASSERT(state, var_idx < e.targets.size(), "Switch " << var_idx << " out of range in target list (" << e.targets.size() << ")");
            return e.targets[var_idx];
            }
        TU_ARMA(SwitchValue, e) {
            HIR::TypeRef    tmp;
            const auto& ty = state.get_lvalue_type(tmp, e.val);
                auto ti = TypeInfo::for_type(ty);
            auto lit = local_state.get_lval(e.val);

            unsigned target_idx = ~0u;
            TU_MATCH_HDRA( (e.values), { )
            default:
                MIR_TODO(state, "SwitchValue - " << e.values.tag_str());
            TU_ARMA(Unsigned, vals) {
                auto v = lit.read_uint(state, ti.bits/8);
                for(size_t i = 0; i < vals.size(); i ++) {
                    if(v == vals[i]) {
                        target_idx = i;
                        break;
                    }
                }
                }
            }
            if( target_idx == ~0u ) {
                return e.def_target;
            }
            else {
                return e.targets[target_idx];
            }
            }
        TU_ARMA(Call, e) {
            const auto& ms = local_state.ms;
            if( const auto* te = e.fcn.opt_Intrinsic() )
            {
                auto dst = local_state.get_lval(e.ret_val);
                if( te->name == "size_of" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    size_t  size_val;
                    if( Target_GetSizeOf(state.sp, this->resolve, ty, size_val) )
                        dst.write_uint(state, Target_GetPointerBits(), U128(size_val));
                    else
                        throw Defer();
                }
                else if( te->name == "min_align_of" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    size_t  align_val;
                    if( Target_GetAlignOf(state.sp, this->resolve, ty, align_val) )
                        dst.write_uint(state, Target_GetPointerBits(), U128(align_val));
                    else
                        throw Defer();
                }
                else if( te->name == "type_name" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    auto name = FMT(ty);
                    dst.write_ptr(state, EncodedLiteral::PTR_BASE, AllocationPtr::allocate_ro(name.data(), name.size()));
                    dst.slice(Target_GetPointerBits()/8).write_uint(state, Target_GetPointerBits(), name.size());
                }
                else if( te->name == "type_id" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    dst.write_ptr(state, EncodedLiteral::PTR_BASE, StaticRefPtr::allocate(HIR::Path(mv$(ty), "#type_id"), nullptr));
                }
                else if( te->name == "needs_drop" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    dst.write_uint(state, 8, resolve.type_needs_drop_glue(state.sp, ty) ? 1 : 0);
                }
                else if( te->name == "caller_location" ) {
                    auto ty_path = resolve.m_crate.get_lang_item_path(state.sp, "panic_location");
                    auto ty = HIR::TypeRef::new_path(ty_path, &resolve.m_crate.get_struct_by_path(state.sp, ty_path));
                    auto* repr = Target_GetTypeRepr(state.sp, resolve, ty);
                    MIR_ASSERT(state, repr, "No repr for panic::Location?");
                    MIR_ASSERT(state, repr->fields.size() == 3, "Unexpected item count in panic::Location");
                    auto val = RelocPtr(AllocationPtr::allocate(resolve, state, ty));
                    dst.write_ptr(state, EncodedLiteral::PTR_BASE, val);
                    auto rv = ValueRef(val);
                    auto pb = Target_GetPointerBits()/8;
                    rv.slice(repr->fields[0].offset+ 0, pb).write_ptr(state, EncodedLiteral::PTR_BASE, ConstantPtr::allocate("", 0)); // file.ptr
                    rv.slice(repr->fields[0].offset+pb, pb).write_uint(state, Target_GetPointerBits(), 0);    // file.len
                    rv.slice(repr->fields[1].offset, 4).write_uint(state, 32, 0);  // line: u32
                    rv.slice(repr->fields[2].offset, 4).write_uint(state, 32, 0);  // col: u32
                }
                // ---
                else if( te->name == "ctpop" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    MIR_ASSERT(state, ty.data().is_Primitive(), "ctpop with non-primitive " << ty);
                    auto ti = TypeInfo::for_type(ty);
                    //MIR_ASSERT(state, ti.type == TypeInfo::Unsigned, "`ctpop` with non-unsigned " << ty);
                    auto val = local_state.read_param_uint(ti.bits, e.args.at(0));
#ifdef _MSC_VER
                    unsigned rv = __popcnt(val.get_lo() & 0xFFFFFFFF) + __popcnt(val.get_lo() >> 32)
                        + __popcnt(val.get_hi() & 0xFFFFFFFF) + __popcnt(val.get_hi() >> 32);
#else
                    unsigned rv = __builtin_popcountll(val.get_lo()) + __builtin_popcountll(val.get_hi());
#endif
                    dst.write_uint(state, ti.bits, U128(rv));
                }
                // - CounT Trailing Zeros
                else if( te->name == "cttz" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    MIR_ASSERT(state, ty.data().is_Primitive(), "`cttz` with non-primitive " << ty);
                    auto ti = TypeInfo::for_type(ty);
                    MIR_ASSERT(state, ti.ty == TypeInfo::Unsigned, "`cttz` with non-unsigned " << ty);
                    auto val = local_state.read_param_uint(ti.bits, e.args.at(0));
                    unsigned rv = 0;
                    if(val == U128(0)) {
                        rv = ti.bits;
                    }
                    else {
                        while( (val & 1) == U128(0) ) {
                            val >>= 1;
                            rv += 1;
                        }
                    }
                    dst.write_uint(state, ti.bits, U128(rv));
                }
                // - CounT Lrailing Zeros
                else if( te->name == "ctlz" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    MIR_ASSERT(state, ty.data().is_Primitive(), "`ctlz` with non-primitive " << ty);
                    auto ti = TypeInfo::for_type(ty);
                    MIR_ASSERT(state, ti.ty == TypeInfo::Unsigned, "`ctlz` with non-unsigned " << ty);
                    auto val = local_state.read_param_uint(ti.bits, e.args.at(0));
                    unsigned rv = 0;
                    // Count how many shifts needed to remove the MSB
                    while( val != U128(0) ) {
                        val >>= 1;
                        rv += 1;
                    }
                    // Then subtract from the total bit count (no shift needed = max bits)
                    dst.write_uint(state, ti.bits, U128(ti.bits - rv));
                }
                else if( te->name == "bswap" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
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
                        static U128 bswap128(U128 v) {
                            return U128( bswap64((v >> 64).truncate_u64()), bswap64(v.truncate_u64()) );
                        }
                    };
                    U128 rv;
                    switch(ty.data().as_Primitive())
                    {
                    case ::HIR::CoreType::I8:
                    case ::HIR::CoreType::U8:
                        rv = val;
                        break;
                    case ::HIR::CoreType::I16:
                    case ::HIR::CoreType::U16:
                        rv = U128(H::bswap16(val.truncate_u64()));
                        break;
                    case ::HIR::CoreType::I32:
                    case ::HIR::CoreType::U32:
                        rv = U128(H::bswap32(val.truncate_u64()));
                        break;
                    case ::HIR::CoreType::I64:
                    case ::HIR::CoreType::U64:
                        rv = U128(H::bswap64(val.truncate_u64()));
                        break;
                    case ::HIR::CoreType::I128:
                    case ::HIR::CoreType::U128:
                        rv = H::bswap128(val);
                        break;
                    default:
                        MIR_TODO(state, "Handle bswap with " << ty);
                    }
                    dst.write_uint(state, ti.bits, rv);
                }
                else if( te->name == "bitreverse" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    MIR_ASSERT(state, ty.data().is_Primitive(), "bswap with non-primitive " << ty);
                    auto ti = TypeInfo::for_type(ty);

                    auto val = local_state.read_param_uint(ti.bits, e.args.at(0));
                    U128    rv;
                    for(size_t i = 0; i < ti.bits; i ++) {
                        if( (val & 1) != 0 ) {
                            rv |= 1;
                        }
                        rv <<= 1;
                        val >>= 1;
                    }
                    dst.write_uint(state, ti.bits, rv);
                }
                else if( te->name == "rotate_left" || te->name == "rotate_right" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    MIR_ASSERT(state, ty.data().is_Primitive(), te->name << " with non-primitive " << ty);
                    auto ti = TypeInfo::for_type(ty);

                    auto val = local_state.read_param_uint(ti.bits, e.args.at(0));
                    auto count = local_state.read_param_uint(ti.bits, e.args.at(1));
                    MIR_ASSERT(state, count <= ti.bits, "Excessive rotation");
                    unsigned count_i = count.truncate_u64();

                    U128    rv;
                    if( count_i == 0 || ti.bits == 64 ) {
                        rv = val;
                    }
                    else if( te->name == "rotate_left" ) {
                        // NOTE: `read_param_uint` has zeroes in the high bits, so anything above `ti.bits` should be zero
                        auto a = val << count_i;
                        auto b = val >> (ti.bits - count_i);
                        rv = a | b;
                    }
                    else {
                        auto a = val >> count_i;
                        auto b = val << (ti.bits - count_i);
                        rv = a | b;
                    }
                    // Writing back will truncate away the higher bits
                    dst.write_uint(state, ti.bits, rv);
                }
                // ---
                else if( te->name == "add_with_overflow" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    MIR_ASSERT(state, ty.data().is_Primitive(), "`" << te->name << "` with non-primitive " << ty);
                    auto dst_tup = get_tuple_t_bool(local_state, dst, ty);
                    bool overflowed = do_arith_checked(local_state, ty, dst_tup.first, e.args.at(0), ::MIR::eBinOp::ADD, e.args.at(1));
                    dst_tup.second.write_uint(state, 8, U128(overflowed ? 1 : 0));
                }
                else if( te->name == "sub_with_overflow" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    MIR_ASSERT(state, ty.data().is_Primitive(), "`" << te->name << "` with non-primitive " << ty);
                    auto dst_tup = get_tuple_t_bool(local_state, dst, ty);
                    bool overflowed = do_arith_checked(local_state, ty, dst_tup.first, e.args.at(0), ::MIR::eBinOp::SUB, e.args.at(1));
                    dst_tup.second.write_uint(state, 8, U128(overflowed ? 1 : 0));
                }
                else if( te->name == "mul_with_overflow" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    MIR_ASSERT(state, ty.data().is_Primitive(), "`" << te->name << "` with non-primitive " << ty);
                    auto dst_tup = get_tuple_t_bool(local_state, dst, ty);
                    bool overflowed = do_arith_checked(local_state, ty, dst_tup.first, e.args.at(0), ::MIR::eBinOp::MUL, e.args.at(1));
                    dst_tup.second.write_uint(state, 8, U128(overflowed ? 1 : 0));
                }
                // Unchecked and wrapping are the same
                else if( te->name == "wrapping_add" ||  te->name == "unchecked_add" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    MIR_ASSERT(state, ty.data().is_Primitive(), "`" << te->name << "` with non-primitive " << ty);
                    do_arith_checked(local_state, ty, dst, e.args.at(0), ::MIR::eBinOp::ADD, e.args.at(1));
                }
                else if( te->name == "wrapping_sub" ||  te->name == "unchecked_sub" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    MIR_ASSERT(state, ty.data().is_Primitive(), "`" << te->name << "` with non-primitive " << ty);
                    do_arith_checked(local_state, ty, dst, e.args.at(0), ::MIR::eBinOp::SUB, e.args.at(1));
                }
                else if( te->name == "wrapping_mul" ||  te->name == "unchecked_mul" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    MIR_ASSERT(state, ty.data().is_Primitive(), "`" << te->name << "` with non-primitive " << ty);
                    do_arith_checked(local_state, ty, dst, e.args.at(0), ::MIR::eBinOp::MUL, e.args.at(1));
                }
                else if( te->name == "unchecked_shl" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    MIR_ASSERT(state, ty.data().is_Primitive(), "`" << te->name << "` with non-primitive " << ty);
                    do_arith_checked(local_state, ty, dst, e.args.at(0), ::MIR::eBinOp::BIT_SHL, e.args.at(1));
                }
                else if( te->name == "unchecked_shr" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    MIR_ASSERT(state, ty.data().is_Primitive(), "`" << te->name << "` with non-primitive " << ty);
                    do_arith_checked(local_state, ty, dst, e.args.at(0), ::MIR::eBinOp::BIT_SHR, e.args.at(1));
                }
                // - Except for div/rem, which add checking just in case
                else if( te->name == "unchecked_rem" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    MIR_ASSERT(state, ty.data().is_Primitive(), "`" << te->name << "` with non-primitive " << ty);
                    bool was_overflow = do_arith_checked(local_state, ty, dst, e.args.at(0), ::MIR::eBinOp::MOD, e.args.at(1));
                    MIR_ASSERT(state, !was_overflow, "`" << te->name << "` overflowed");
                }
                else if( te->name == "unchecked_div" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    MIR_ASSERT(state, ty.data().is_Primitive(), "`" << te->name << "` with non-primitive " << ty);
                    bool was_overflow = do_arith_checked(local_state, ty, dst, e.args.at(0), ::MIR::eBinOp::DIV, e.args.at(1));
                    MIR_ASSERT(state, !was_overflow, "`" << te->name << "` overflowed");
                }
                // `exact_div` is UB if the division results in a non-zero remainder (or if the division overflows)
                else if( te->name == "exact_div" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    MIR_ASSERT(state, ty.data().is_Primitive(), "`" << te->name << "` with non-primitive " << ty);
                    bool was_overflow = do_arith_checked(local_state, ty, dst, e.args.at(0), ::MIR::eBinOp::DIV, e.args.at(1));
                    MIR_ASSERT(state, !was_overflow, "`" << te->name << "` overflowed");
                }
                // Saturating operations
                else if( te->name == "saturating_add" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    MIR_ASSERT(state, ty.data().is_Primitive(), "`" << te->name << "` with non-primitive " << ty);
                    do_arith_checked(local_state, ty, dst, e.args.at(0), ::MIR::eBinOp::ADD, e.args.at(1), true);
                }
                else if( te->name == "saturating_sub" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    MIR_ASSERT(state, ty.data().is_Primitive(), "`" << te->name << "` with non-primitive " << ty);
                    do_arith_checked(local_state, ty, dst, e.args.at(0), ::MIR::eBinOp::SUB, e.args.at(1), true);
                }
                // ---
                else if( te->name == "transmute" ) {
                    local_state.write_param(dst, e.args.at(0));
                }
                else if( te->name == "unlikely" ) {
                    local_state.write_param(dst, e.args.at(0));
                }
                else if( te->name == "assume" ) {
                    auto val = local_state.read_param_uint(8, e.args.at(0));
                    MIR_ASSERT(state, val != 0, "`assume` failed");
                }
                else if( te->name == "assert_inhabited" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    // TODO: Determine if the type is inhabited (i.e. isn't diverge)
                    bool is_uninhabited = resolve.type_is_impossible(state.sp, ty);
                    MIR_ASSERT(state, !is_uninhabited, "assert_inhabited " << ty << " failed");
                }
                // ---
                else if( te->name == "const_eval_select" ) {
                    // "Selects which function to call depending on the context."
                    // `fn const_eval_select<ARG, F, G, RET>(arg: ARG, called_in_const: F, called_at_rt: G ) -> RET`
                    auto arg_ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    MIR_ASSERT(state, arg_ty.data().is_Tuple(), "`" << te->name << "` requires a tuple for ARG, got " << arg_ty);
                    auto* repr = Target_GetTypeRepr(state.sp, resolve, arg_ty);
                    if(!repr) {
                        throw Defer();
                    }
                    auto arg_val = local_state.get_lval(e.args.at(0).as_LValue());
                    const auto& fcn_arg = e.args.at(1);
                    std::shared_ptr<HIR::Path> fcn_path;

                    TU_MATCH_HDRA( (fcn_arg), {)
                    TU_ARMA(LValue, e) {
                        auto fcn_val = local_state.get_lval(e).read_ptr(state);
                        MIR_ASSERT(state, fcn_val.first == EncodedLiteral::PTR_BASE, "");

                        const auto* fcn_sr = fcn_val.second.as_staticref();
                        MIR_ASSERT(state, fcn_sr, "");
                        fcn_path = std::make_shared<HIR::Path>( fcn_sr->path().clone() );
                        }
                    TU_ARMA(Borrow, e) {
                        MIR_BUG(state, "Invalid argument for function pointer to `const_eval_select`: " << fcn_arg);
                        }
                    TU_ARMA(Constant, e) {
                        if( const auto* ce = e.opt_Function() ) {
                            fcn_path = std::make_shared<HIR::Path>( ms.monomorph_path(state.sp, *ce->p) );
                        }
                        else if( const auto* ce = e.opt_ItemAddr() ) {
                            fcn_path = std::make_shared<HIR::Path>( ms.monomorph_path(state.sp, **ce) );
                        }
                        else {
                            MIR_BUG(state, "Invalid argument for function pointer to `const_eval_select`: " << fcn_arg);
                        }
                        }
                    }

                    // Argument values
                    ::std::vector<AllocationPtr>  call_args;
                    call_args.reserve( repr->fields.size() );
                    for(const auto& f : repr->fields) {
                        auto size = local_state.size_of_or_bug(f.ty);
                        call_args.push_back(AllocationPtr::allocate(resolve, state, f.ty));
                        auto vr = ValueRef(call_args.back());
                        vr.copy_from(state, arg_val.slice(f.offset, size));
                    }

                    MonomorphState  fcn_ms;
                    const ::HIR::GenericParams* impl_params_def = nullptr;
                    auto& fcn = get_function(this->root_span, this->resolve, *fcn_path, fcn_ms, impl_params_def);

                    // Monomorphised argument types
                    ::HIR::Function::args_t arg_defs;
                    for(const auto& a : fcn.m_args) {
                        arg_defs.push_back( ::std::make_pair(::HIR::Pattern(), this->resolve.monomorph_expand(this->root_span, a.second, fcn_ms)) );
                    }
                    auto ret_ty = this->resolve.monomorph_expand(this->root_span, fcn.m_return, fcn_ms);

                    const auto* mir = this->resolve.m_crate.get_or_gen_mir( ::HIR::ItemPath(*fcn_path), fcn );
                    MIR_ASSERT(state, mir != nullptr, "No MIR for function " << *fcn_path);

                    push_stack_entry(::FmtLambda([=](std::ostream& os){ os << *fcn_path; }), *mir,
                        std::move(fcn_ms), std::move(ret_ty), ::std::move(arg_defs), std::move(call_args),
                        &fcn.m_params, impl_params_def
                        );
                    return TERM_RET_PUSHED;
                }
                // ---
                else if( te->name == "copy_nonoverlapping" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    size_t element_size;
                    if( !Target_GetSizeOf(state.sp, resolve, ty, element_size) )
                        throw Defer();
                    auto ptr_src = local_state.get_lval(e.args.at(0).as_LValue()).read_ptr(state);
                    auto ptr_dst = local_state.get_lval(e.args.at(1).as_LValue()).read_ptr(state);
                    U128 count = local_state.read_param_uint(Target_GetPointerBits(), e.args.at(2));
                    MIR_ASSERT(state, count.is_u64(), "Excessive count in `" << te->name << "`");
                    MIR_ASSERT(state, count * element_size < U128(SIZE_MAX), "Excessive size in `" << te->name << "`");
                    size_t nbytes = element_size * count.truncate_u64();
                    MIR_ASSERT(state, ptr_src.first >= EncodedLiteral::PTR_BASE, "");
                    MIR_ASSERT(state, ptr_dst.first >= EncodedLiteral::PTR_BASE, "");
                    auto vr_src = ValueRef(ptr_src.second, ptr_src.first - EncodedLiteral::PTR_BASE).slice(0, nbytes);
                    auto vr_dst = ValueRef(ptr_dst.second, ptr_dst.first - EncodedLiteral::PTR_BASE).slice(0, nbytes);
                    vr_dst.copy_from(state, vr_src);
                }
                else if( te->name == "offset" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0).data().as_Pointer().inner);
                    size_t element_size;
                    if( !Target_GetSizeOf(state.sp, resolve, ty, element_size) )
                        throw Defer();
                    auto ptr_pair = local_state.read_param_ptr(e.args.at(0));
                    auto ofs = local_state.read_param_uint(Target_GetPointerBits(), e.args.at(1));
                    dst.write_ptr(state, ptr_pair.first + ofs.truncate_u64() * element_size, ptr_pair.second);
                }
                else if( te->name == "write_bytes" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    size_t element_size;
                    if( !Target_GetSizeOf(state.sp, resolve, ty, element_size) )
                        throw Defer();
                    auto ptr_dst = local_state.get_lval(e.args.at(0).as_LValue()).read_ptr(state);
                    auto val = local_state.read_param_uint(8, e.args.at(1));
                    U128 count = local_state.read_param_uint(Target_GetPointerBits(), e.args.at(2));
                    MIR_ASSERT(state, count.is_u64(), "Excessive count in `" << te->name << "`");
                    MIR_ASSERT(state, count * element_size < U128(SIZE_MAX), "Excessive size in `" << te->name << "`");
                    size_t nbytes = element_size * count.truncate_u64();
                    MIR_ASSERT(state, ptr_dst.first >= EncodedLiteral::PTR_BASE, "");
                    ValueRef vr_dst = ValueRef(ptr_dst.second, ptr_dst.first - EncodedLiteral::PTR_BASE).slice(0, nbytes);
                    memset(vr_dst.ext_write_bytes(state, nbytes), val.truncate_u64(), nbytes);
                }
                else if( te->name == "variant_count" ) {
                    auto ty = local_state.monomorph_expand(te->params.m_types.at(0));
                    MIR_ASSERT(state, ty.data().is_Path(), "`variant_count` on non-enum - " << ty);
                    MIR_ASSERT(state, ty.data().as_Path().binding.is_Enum(), "`variant_count` on non-enum - " << ty);
                    const auto* enm = ty.data().as_Path().binding.as_Enum();
                    dst.write_uint(state, Target_GetPointerBits(), enm->num_variants());
                }
                else {
                    MIR_TODO(state, "Call intrinsic \"" << te->name << "\" - " << terminator);
                }
                DEBUG("> E" << this->eval_index << " F" << local_state.frame_index << " " << e.ret_val << " := " << dst);
                return e.ret_block;
            }
            else if( const auto* te = e.fcn.opt_Path() )
            {
                const auto& fcnp_raw = *te;
                auto fcnp = std::make_shared<HIR::Path>(ms.monomorph_path(state.sp, fcnp_raw));

                MonomorphState  fcn_ms;
                const ::HIR::GenericParams* impl_params_def = nullptr;
                auto& fcn = get_function(this->root_span, this->resolve, *fcnp, fcn_ms, impl_params_def);

                // Argument values
                ::std::vector<AllocationPtr>  call_args;
                call_args.reserve( e.args.size() );
                for(const auto& a : e.args)
                {
                    ::HIR::TypeRef  tmp;
                    const auto& ty = state.get_param_type(tmp, a);
                    call_args.push_back(AllocationPtr::allocate(resolve, state, ty));
                    auto vr = ValueRef(call_args.back());
                    local_state.write_param( vr, a );
                }

                // Monomorphised argument types
                ::HIR::Function::args_t arg_defs;
                for(const auto& a : fcn.m_args) {
                    arg_defs.push_back( ::std::make_pair(::HIR::Pattern(), this->resolve.monomorph_expand(this->root_span, a.second, fcn_ms)) );
                }
                auto ret_ty = this->resolve.monomorph_expand(this->root_span, fcn.m_return, fcn_ms);

                // TODO: Set m_const during parse and check here

                if( !fcn.m_code && !fcn.m_code.m_mir ) {
                    if( fcn.m_linkage.name == "" ) {
                    }
                    else if( fcn.m_linkage.name == "panic_impl" ) {
                        MIR_TODO(state, "panic in constant evaluation");
                    }
                    else {
                        MIR_TODO(state, "Call extern function `" << fcn.m_linkage.name << "`");
                    }
                }

                // Call by invoking evaluate_constant on the function
                const auto* mir = this->resolve.m_crate.get_or_gen_mir( ::HIR::ItemPath(*fcnp), fcn );
                MIR_ASSERT(state, mir, "No MIR for function " << fcnp);

                push_stack_entry(
                    ::FmtLambda([=](std::ostream& os){ os << *fcnp; }),
                    *mir, std::move(fcn_ms), std::move(ret_ty), ::std::move(arg_defs), std::move(call_args),
                    &fcn.m_params, impl_params_def
                    );
                return TERM_RET_PUSHED;
            }
            else
            {
                MIR_BUG(state, "Unexpected terminator - " << terminator);
            }
            }
        }
        throw std::runtime_error("Unreachable?");
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
        DEBUG("ms = " << ms);
        const auto* mir = this->resolve.m_crate.get_or_gen_mir(ip, expr, exp);

        if( mir ) {
            ASSERT_BUG(Span(), expr.m_state, "");
            if( !resolve.m_item_generics && !resolve.m_impl_generics ) {
                resolve.set_both_generics_raw(expr.m_state->m_impl_generics, expr.m_state->m_item_generics);
            }
        }

        // If `ms` is empty, but `resolve` has impl/item generics, then re-make `ms` as a nop set of params
        // - This is a lazy hack, isntead of doing this creation in the caller
        ::HIR::PathParams   nop_params_impl;
        ::HIR::PathParams   nop_params_method;
        if( !ms.pp_impl && !ms.pp_method ) {
            if( resolve.m_item_generics ) {
                ms.pp_method = &(nop_params_method = resolve.m_item_generics->make_nop_params(1));
            }
            if( resolve.m_impl_generics ) {
                ms.pp_impl = &(nop_params_impl = resolve.m_impl_generics->make_nop_params(0));
            }
        }

        if( mir ) {
            // Might want to have a fully-populated MonomorphState for expanding inside impl blocks
            // HACK: Generate a roughly-correct one
            const auto& top_ip = ip.get_top_ip();
            if( top_ip.trait && !top_ip.ty ) {
                ms.self_ty = ty_Self.clone();
            }

            assert( this->call_stack.empty() );
            this->num_frames = 0;
            // Note: Since this is the entrypoint, `this->resolve` has the correct GenericParams
            this->push_stack_entry(FMT_CB(os, os << ip), *mir, std::move(ms), std::move(exp), {}, {}, resolve.m_item_generics, resolve.m_impl_generics);
            auto rv_raw = this->run_until_stack_empty();

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
    struct Expander:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;
        const ::HIR::Module*  m_mod;
        const ::HIR::ItemPath*  m_mod_path;
        MonomorphState  m_monomorph_state;
        bool m_recurse_types;

        const ::HIR::GenericParams* m_impl_params;
        const ::HIR::GenericParams* m_item_params;

        std::function<const ::HIR::GenericParams&(const Span& sp)>    m_get_params;

        enum class Pass {
            OuterOnly,
            Values,
        } m_pass;

        Expander(const ::HIR::Crate& crate)
            : m_crate(crate)
            , m_mod(nullptr)
            , m_mod_path(nullptr)
            , m_recurse_types(false)
            , m_impl_params(nullptr)
            , m_item_params(nullptr)
            , m_pass(Pass::OuterOnly)
        {}

        ::HIR::Evaluator get_eval(const Span& sp, NewvalState& nvs) const
        {
            auto eval = ::HIR::Evaluator { sp, m_crate, nvs };
            eval.resolve.set_both_generics_raw(m_impl_params, m_item_params);
            return eval;
        }

        ::HIR::PathParams get_params_for_def(const ::HIR::GenericParams& tpl, bool is_function_level=false) const {
            return tpl.make_nop_params(is_function_level ? 1 : 0);
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

            auto pp_fcn = get_params_for_def(f.m_params, true);
            m_monomorph_state.pp_method = &pp_fcn;
            m_item_params = &f.m_params;
            ::HIR::Visitor::visit_function(p, f);
            m_item_params = nullptr;
            m_monomorph_state.pp_method = nullptr;
        }

        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            static Span sp;
            TRACE_FUNCTION_F("impl" << impl.m_params.fmt_args() << " " << trait_path << impl.m_trait_args << " for " << impl.m_type);

            auto mp = ::HIR::ItemPath(impl.m_src_module);
            m_mod_path = &mp;
            m_mod = &m_crate.get_mod_by_path(sp, impl.m_src_module);

            auto pp_impl = get_params_for_def(impl.m_params);
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

            auto pp_impl = get_params_for_def(impl.m_params);
            m_monomorph_state.pp_impl = &pp_impl;
            m_impl_params = &impl.m_params;

            ::HIR::Visitor::visit_type_impl(impl);

            assert(m_impl_params);
            m_impl_params = nullptr;
            m_monomorph_state.pp_impl = nullptr;

            m_mod = nullptr;
            m_mod_path = nullptr;
        }

        void visit_trait(::HIR::ItemPath ip, ::HIR::Trait& trait) override
        {
            auto pp_impl = get_params_for_def(trait.m_params);
            m_monomorph_state.self_ty = ::HIR::TypeRef::new_self();
            m_monomorph_state.pp_impl = &pp_impl;
            m_impl_params = &trait.m_params;

            ::HIR::Visitor::visit_trait(ip, trait);

            assert(m_impl_params);
            m_impl_params = nullptr;
            m_monomorph_state.pp_impl = nullptr;
        }

        void visit_path_params(::HIR::PathParams& p) override
        {
            static Span sp;
            ::HIR::Visitor::visit_path_params(p);

            for( auto& v : p.m_values )
            {
                if(v.is_Unevaluated())
                {
                    const auto& e = *v.as_Unevaluated()->expr;
                    auto name = FMT("param_" << &v << "#");
                    auto nvs = NewvalState { *m_mod, *m_mod_path, name };
                    TRACE_FUNCTION_FR(name, name);
                    auto eval = get_eval(e->span(), nvs);

                    // Need to look up the required type - to do that requires knowing the item it's for
                    // - Which, might not be known at this point - might be a UfcsInherent
                    try
                    {
                        const auto& params_def = m_get_params(sp);
                        auto idx = static_cast<size_t>(&v - &p.m_values.front());
                        ASSERT_BUG(sp, idx < params_def.m_values.size(), "");
                        const auto& ty = params_def.m_values[idx].m_type;
                        ASSERT_BUG(sp, !monomorphise_type_needed(ty), "" << ty);

                        auto val = eval.evaluate_constant( ::HIR::ItemPath(*m_mod_path, name.c_str()), e, ty.clone() );
                        v = ::HIR::ConstGeneric::make_Evaluated(std::move(val));
                    }
                    catch(const Defer& )
                    {
                        // Deferred - no update
                    }
                }
            }
        }
        void visit_generic_path(::HIR::GenericPath& p, ::HIR::Visitor::PathContext pc) override
        {
            TRACE_FUNCTION_FR(p, p);
            auto saved = m_get_params;
            m_get_params = [&](const Span& sp)->const ::HIR::GenericParams& {
                DEBUG("visit_generic_path[m_get_params] " << p);
                switch(pc)
                {
                case ::HIR::Visitor::PathContext::VALUE: {
                    auto& vi = m_crate.get_valitem_by_path(sp, p.m_path);
                    TU_MATCH_HDRA( (vi), { )
                    TU_ARMA(Import, e)  BUG(sp, "Module Import");
                    TU_ARMA(Static, e)  BUG(sp, "Getting params definition for Static - " << p);
                    TU_ARMA(Constant, e)    return e.m_params;
                    TU_ARMA(Function, e)    return e.m_params;
                    TU_ARMA(StructConstant, e)   return m_crate.get_struct_by_path(sp, e.ty).m_params;
                    TU_ARMA(StructConstructor, e)   return m_crate.get_struct_by_path(sp, e.ty).m_params;
                    }
                    break; }
                case ::HIR::Visitor::PathContext::TYPE:
                case ::HIR::Visitor::PathContext::TRAIT: {
                    auto& vi = m_crate.get_typeitem_by_path(sp, p.m_path);
                    TU_MATCH_HDRA( (vi), { )
                    TU_ARMA(Import, e)  BUG(sp, "Module Import");
                    TU_ARMA(Module, e)  BUG(sp, "mod - " << p);
                    TU_ARMA(TypeAlias, e)  BUG(sp, "type - " << p);
                    TU_ARMA(TraitAlias, e)  BUG(sp, "trait= - " << p);
                    TU_ARMA(Struct, e)  return e.m_params;
                    TU_ARMA(Enum , e)   return e.m_params;
                    TU_ARMA(Union, e)   return e.m_params;
                    TU_ARMA(Trait, e)   return e.m_params;
                    TU_ARMA(ExternType, e)   BUG(sp, "extern type - " << p);
                    }
                    break; }
                }
                TODO(sp, "visit_generic_path[m_get_params] - " << p);
            };
            ::HIR::Visitor::visit_generic_path(p, pc);
            m_get_params = saved;
        }
        void visit_path(::HIR::Path& p, ::HIR::Visitor::PathContext pc) override
        {
            auto saved = m_get_params;
            m_get_params = [&](const Span& sp)->const ::HIR::GenericParams& {
                DEBUG("visit_path[m_get_params] " << p);
                StaticTraitResolve  resolve(m_crate);
                resolve.set_both_generics_raw(m_impl_params, m_item_params);
                switch(pc)
                {
                case ::HIR::Visitor::PathContext::VALUE: {
                    MonomorphState  unused;
                    auto vi = resolve.get_value(sp, p, unused, true);
                    TU_MATCH_HDRA( (vi), {)
                    TU_ARMA(NotFound, e)
                        BUG(sp, "NotFound");
                    TU_ARMA(NotYetKnown, e)
                        TODO(sp, "NotYetKnown");
                    TU_ARMA(Static, e)  return e->m_params;
                    TU_ARMA(Constant, e)    return e->m_params;
                    TU_ARMA(Function, e)    return e->m_params;
                    TU_ARMA(EnumConstructor, e)
                        TODO(sp, "Handle EnumConstructor - " << p);
                    TU_ARMA(EnumValue, e)
                        TODO(sp, "Handle EnumValue - " << p);
                    TU_ARMA(StructConstructor, e)
                        TODO(sp, "Handle StructConstructor - " << p);
                    TU_ARMA(StructConstant, e)
                        TODO(sp, "Handle StructConstant - " << p);
                    }
                    break; }
                case ::HIR::Visitor::PathContext::TYPE:
                case ::HIR::Visitor::PathContext::TRAIT: {
                    //const auto& vi = tr.m_types.at(pe->item);
                    BUG(sp, "type - " << p);
                    break; }
                }
                TODO(sp, "visit_path[m_get_params] - " << p);
                };
            ::HIR::Visitor::visit_path(p, pc);
            m_get_params = saved;
        }

        void visit_arraysize(::HIR::ArraySize& as, std::string name)
        {
            if( as.is_Unevaluated() && as.as_Unevaluated().is_Unevaluated() )
            {
                TRACE_FUNCTION_FR(as, as);
                const auto& expr_ptr = *as.as_Unevaluated().as_Unevaluated()->expr;

                auto nvs = NewvalState { *m_mod, *m_mod_path, name };
                auto eval = get_eval(expr_ptr->span(), nvs);
                try
                {
                    auto val = eval.evaluate_constant(*m_mod_path + name, expr_ptr, ::HIR::CoreType::Usize, m_monomorph_state.clone());
                    as = val.read_usize(0);
                    //DEBUG("Array size = " << as);
                }
                catch(const Defer& )
                {
                    const auto* tn = dynamic_cast<const HIR::ExprNode_ConstParam*>(&*expr_ptr);
                    if(tn) {
                        as = HIR::ConstGeneric( HIR::GenericRef(tn->m_name, tn->m_binding) );
                    }
                    else {
                        //TODO(expr_ptr->span(), "Handle defer for array sizes");
                    }
                }
            }
            else
            {
                DEBUG("Array size (known) = " << as);
            }
        }

        void visit_type(::HIR::TypeRef& ty) override
        {
            ::HIR::Visitor::visit_type(ty);

            if(auto* e = ty.data_mut().opt_Array())
            {
                TRACE_FUNCTION_FR(ty, ty);
                visit_arraysize(e->size, FMT("ty_" << e << "#"));
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
            if( m_pass != Pass::Values )
            {
            }
            else if( item.m_value || item.m_value.m_mir )
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
                DEBUG("constant?");// " << *item.m_value);
            }

            m_item_params = nullptr;
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override
        {
            TRACE_FUNCTION_F(p);
            m_item_params = &item.m_params;

            m_recurse_types = true;
            ::HIR::Visitor::visit_static(p, item);
            m_recurse_types = false;

            if( m_pass != Pass::Values )
            {
            }
            else if( item.m_value )
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

            m_item_params = nullptr;
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
                void visit_path(::HIR::Visitor::PathContext pc, ::HIR::Path& p) override {
                    m_exp.visit_path(p, pc);
                }

                void visit(::HIR::ExprNode_CallMethod& node) override {
                    auto saved = m_exp.m_get_params;
                    m_exp.m_get_params = [&](const Span& sp)->const ::HIR::GenericParams& {
                        DEBUG("visit(ExprNode_CallMethod)[m_get_params] Defer until after main typecheck");
                        throw Defer();
                        };
                    ::HIR::ExprVisitorDef::visit(node);
                    m_exp.m_get_params = std::move(saved);
                }
                void visit(::HIR::ExprNode_ArraySized& node) override {
                    ::HIR::ExprVisitorDef::visit(node);
                    m_exp.visit_arraysize(node.m_size, FMT("array_" << &node << "#"));
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
                bool is_signed = false;
                switch(ty.data().as_Primitive())
                {
                case ::HIR::CoreType::I8:
                case ::HIR::CoreType::I16:
                case ::HIR::CoreType::I32:
                case ::HIR::CoreType::I64:
                case ::HIR::CoreType::Isize:
                case ::HIR::CoreType::I128: // TODO: Emulation
                    is_signed = true;
                    break;
                case ::HIR::CoreType::Bool:
                case ::HIR::CoreType::U8:
                case ::HIR::CoreType::U16:
                case ::HIR::CoreType::U32:
                case ::HIR::CoreType::U64:
                case ::HIR::CoreType::Usize:
                case ::HIR::CoreType::Char:
                case ::HIR::CoreType::U128: // TODO: Emulation
                    is_signed = false;
                    break;
                case ::HIR::CoreType::F32:
                case ::HIR::CoreType::F64:
                    TODO(Span(), "Floating point enum tag.");
                    break;
                case ::HIR::CoreType::Str:
                    BUG(Span(), "Unsized tag?!");
                }
                uint64_t i = 0;
                for(auto& var : e->variants)
                {

                    if( var.expr )
                    {
                        auto nvs = NewvalState { mod, mod_path, FMT(name << "_" << var.name << "#") };
                        auto eval = ::HIR::Evaluator { var.expr->span(), crate, nvs };
                        eval.resolve.set_impl_generics_raw(MetadataType::None, item.m_params);
                        try
                        {
                            auto val = eval.evaluate_constant(p, var.expr, ty.clone());
                            DEBUG("enum variant: " << p << "::" << var.name << " = " << val);
                            if( is_signed ) {
                                i = EncodedLiteralSlice(val).read_sint().truncate_i64();
                            }
                            else {
                                i = EncodedLiteralSlice(val).read_uint().truncate_u64();
                            }
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

    void ConvertHIR_ConstantEvaluate_Static(const ::HIR::Crate& crate, const ::HIR::GenericParams* impl_params, const ::HIR::ItemPath& ip, ::HIR::Static& e)
    {
        Expander    exp { crate };
        exp.m_impl_params = impl_params;
        exp.visit_static(ip, e);
    }
    void ConvertHIR_ConstantEvaluate_FcnSig(const ::HIR::Crate& crate, const ::HIR::GenericParams* impl_params, const ::HIR::ItemPath& ip, ::HIR::Function& fcn)
    {
        Expander    exp { crate };
        exp.m_impl_params = impl_params;
        exp.visit_function(ip, fcn);
    }
}   // namespace

void ConvertHIR_ConstantEvaluate(::HIR::Crate& crate)
{
    Expander    exp { crate };
    exp.visit_crate( crate );
    exp.m_pass = Expander::Pass::Values;
    exp.visit_crate( crate );

    ExpanderApply().visit_crate(crate);
    for(auto& new_ty_pair : crate.m_new_types)
    {
        auto res = crate.m_root_module.m_mod_items.insert( mv$(new_ty_pair) );
        ASSERT_BUG(Span(), res.second, "Duplicate type in consteval?");
    }
    crate.m_new_types.clear();
    for(auto& new_val_pair : crate.m_new_values)
    {
        auto res = crate.m_root_module.m_value_items.insert( mv$(new_val_pair) );
        ASSERT_BUG(Span(), res.second, "Duplicate value in consteval?");
    }
    crate.m_new_values.clear();
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
    auto item_name = mod_path.pop_component();
    const auto& mod = crate.get_mod_by_path(Span(), mod_path);

    auto& item = const_cast<::HIR::Enum&>(enm);

    Expander::visit_enum_inner(crate, ip, mod, mod_path, item_name.c_str(), item);
}
void ConvertHIR_ConstantEvaluate_ConstGeneric( const Span& sp, const ::HIR::Crate& crate, const HIR::TypeRef& ty, ::HIR::ConstGeneric& cg )
{
    if( auto* cge_p = cg.opt_Unevaluated() )
    {
        const auto& cge = *cge_p;
        const auto& e = *cge->expr;
        ASSERT_BUG(sp, e.m_state, "TODO: Should the expression state be set already?");
        const auto& s = *e.m_state;
        auto name = FMT("const_" << &e << "#");
        struct NewvalState_Nop
            : public HIR::Evaluator::Newval
        {
            const Span& sp;
            ::HIR::Path new_static(::HIR::TypeRef type, EncodedLiteral value) override
            {
                TODO(this->sp, "new_static - in ConvertHIR_ConstantEvaluate_ConstGeneric");
            }
            NewvalState_Nop(const Span& sp): sp(sp) {}
        } nvs { sp };
        //auto nvs = NewvalState { crate.get_mod_by_path(Span(), mod_path), mod_path, name };
        auto eval = ::HIR::Evaluator { sp, crate, nvs };
        eval.resolve.set_both_generics_raw(s.m_impl_generics, s.m_item_generics);

        // Need to look up the required type - to do that requires knowing the item it's for
        // - Which, might not be known at this point - might be a UfcsInherent
        try
        {
            MonomorphState  ms;
            ms.pp_impl   = &cge->params_impl;
            ms.pp_method = &cge->params_item;
            auto val = eval.evaluate_constant( ::HIR::ItemPath(s.m_mod_path, name.c_str()), e, ty.clone(), std::move(ms) );
            cg = HIR::EncodedLiteralPtr(std::move(val));
        }
        catch(const Defer& )
        {
            // Deferred - no update
        }
    }
}

void ConvertHIR_ConstantEvaluate_ArraySize(const Span& sp, const ::HIR::Crate& crate, const ::HIR::SimplePath& path, ::HIR::ArraySize& size)
{
    if( auto* se = size.opt_Unevaluated() ) {
        if(se->is_Unevaluated()) {
            ConvertHIR_ConstantEvaluate_ConstGeneric(sp, crate, HIR::CoreType::Usize, *se);
        }
        if( const auto* e = se->opt_Evaluated() ) {
            size = (*e)->read_usize(0);
        }
    }
}

namespace {
    bool params_contain_ivars(const ::HIR::PathParams& params) {
        for(const auto& t : params.m_types) {
            if( visit_ty_with(t, [](const HIR::TypeRef& t){ return t.data().is_Infer(); })) {
                return true;
            }
        }
        for(const auto& v : params.m_values) {
            if( v.is_Infer() ) {
                return true;
            }
        }
        return false;
    }
}

void ConvertHIR_ConstantEvaluate_MethodParams(
    const Span& sp,
    const ::HIR::Crate& crate, const HIR::SimplePath& mod_path, const ::HIR::GenericParams* impl_generics, const ::HIR::GenericParams* item_generics,
    const ::HIR::GenericParams& params_def,
    ::HIR::PathParams& params
    )
{
    for(auto& v : params.m_values)
    {
        if(v.is_Unevaluated())
        {
            const auto& ue = *v.as_Unevaluated();
            const auto& e = *ue.expr;
            auto name = FMT("param_" << &v << "#");
            TRACE_FUNCTION_FR(name, name);
            auto nvs = NewvalState { crate.get_mod_by_path(Span(), mod_path), mod_path, name };
            auto eval = ::HIR::Evaluator { sp, crate, nvs };
            eval.resolve.set_both_generics_raw(impl_generics, item_generics);

            // Need to look up the required type - to do that requires knowing the item it's for
            // - Which, might not be known at this point - might be a UfcsInherent
            try
            {
                // TODO: if there's an ivar in the param list, then throw defer
                // - Caller should ensure that known ivars are expanded.
                if( params_contain_ivars(ue.params_impl) || params_contain_ivars(ue.params_item) ) {
                    throw Defer();
                }

                auto idx = static_cast<size_t>(&v - &params.m_values.front());
                ASSERT_BUG(sp, idx < params_def.m_values.size(), "");
                const auto& ty = params_def.m_values[idx].m_type;
                ASSERT_BUG(sp, !monomorphise_type_needed(ty), "" << ty);
                MonomorphState  ms;
                ms.pp_impl = &ue.params_impl;
                ms.pp_method = &ue.params_item;

                auto val = eval.evaluate_constant( ::HIR::ItemPath(mod_path, name.c_str()), e, ty.clone(), std::move(ms) );
                v = ::HIR::ConstGeneric::make_Evaluated(std::move(val));
            }
            catch(const Defer& )
            {
                // Deferred - no update
            }
        }
    }
}
