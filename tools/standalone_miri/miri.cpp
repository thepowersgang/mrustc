/*
 * mrustc Standalone MIRI
 * - by John Hodge (Mutabah)
 *
 * miri.cpp
 * - Interpreter core
 */
#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include "module_tree.hpp"
#include "value.hpp"
#include "string_view.hpp"
#include <algorithm>
#include <iomanip>
#include "debug.hpp"
#include "miri.hpp"
// VVV FFI
#include <cstring>  // memrchr
#include <sys/stat.h>
#include <fcntl.h>
#ifdef _WIN32
# define NOMINMAX
# include <Windows.h>
#else
# include <unistd.h>
#endif
#undef DEBUG

unsigned ThreadState::s_next_tls_key = 1;

class PrimitiveValue
{
public:
    virtual ~PrimitiveValue() {}

    virtual bool is_zero() const = 0;
    virtual bool add(const PrimitiveValue& v) = 0;
    virtual bool subtract(const PrimitiveValue& v) = 0;
    virtual bool multiply(const PrimitiveValue& v) = 0;
    virtual bool divide(const PrimitiveValue& v) = 0;
    virtual bool modulo(const PrimitiveValue& v) = 0;
    virtual void write_to_value(ValueCommonWrite& tgt, size_t ofs) const = 0;

    virtual U128 as_u128() const = 0;

    template<typename T>
    const T& check(const char* opname) const
    {
        const auto* xp = dynamic_cast<const T*>(this);
        LOG_ASSERT(xp, "Attempting to " << opname << " mismatched types, expected " << typeid(T).name() << " got " << typeid(*this).name());
        return *xp;
    }
};
template<typename T>
struct PrimitiveUInt:
    public PrimitiveValue
{
    typedef PrimitiveUInt<T>    Self;
    T   v;

    PrimitiveUInt(T v): v(v) {}
    ~PrimitiveUInt() override {}

    virtual bool is_zero() const {
        return this->v == 0;
    }
    bool add(const PrimitiveValue& x) override {
        const auto* xp = &x.check<Self>("add");
        T newv = this->v + xp->v;
        bool did_overflow = newv < this->v;
        this->v = newv;
        return !did_overflow;
    }
    bool subtract(const PrimitiveValue& x) override {
        const auto* xp = &x.check<Self>("subtract");
        T newv = this->v - xp->v;
        bool did_overflow = newv > this->v;
        this->v = newv;
        return !did_overflow;
    }
    bool multiply(const PrimitiveValue& x) override {
        const auto* xp = &x.check<Self>("multiply");
        T newv = this->v * xp->v;
        bool did_overflow = newv < this->v && newv < xp->v;
        this->v = newv;
        return !did_overflow;
    }
    bool divide(const PrimitiveValue& x) override {
        const auto* xp = &x.check<Self>("divide");
        if(xp->v == 0)  return false;
        T newv = this->v / xp->v;
        this->v = newv;
        return true;
    }
    bool modulo(const PrimitiveValue& x) override {
        const auto* xp = &x.check<Self>("modulo");
        if(xp->v == 0)  return false;
        T newv = this->v % xp->v;
        this->v = newv;
        return true;
    }

    U128 as_u128() const override {
        return U128(static_cast<uint64_t>(this->v));
    }
};
struct PrimitiveU64: public PrimitiveUInt<uint64_t>
{
    PrimitiveU64(uint64_t v): PrimitiveUInt(v) {}
    void write_to_value(ValueCommonWrite& tgt, size_t ofs) const override {
        tgt.write_u64(ofs, this->v);
    }
};
struct PrimitiveU32: public PrimitiveUInt<uint32_t>
{
    PrimitiveU32(uint32_t v): PrimitiveUInt(v) {}
    void write_to_value(ValueCommonWrite& tgt, size_t ofs) const override {
        tgt.write_u32(ofs, this->v);
    }
};
struct PrimitiveU16: public PrimitiveUInt<uint16_t>
{
    PrimitiveU16(uint16_t v): PrimitiveUInt(v) {}
    void write_to_value(ValueCommonWrite& tgt, size_t ofs) const override {
        tgt.write_u16(ofs, this->v);
    }
};
struct PrimitiveU8: public PrimitiveUInt<uint8_t>
{
    PrimitiveU8(uint8_t v): PrimitiveUInt(v) {}
    void write_to_value(ValueCommonWrite& tgt, size_t ofs) const override {
        tgt.write_u8(ofs, this->v);
    }
};
template<typename T>
struct PrimitiveSInt:
    public PrimitiveValue
{
    typedef PrimitiveSInt<T>    Self;
    T   v;

    PrimitiveSInt(T v): v(v) {}
    ~PrimitiveSInt() override {}

    virtual bool is_zero() const {
        return this->v == 0;
    }
    // TODO: Make this correct.
    bool add(const PrimitiveValue& x) override {
        const auto* xp = &x.check<Self>("add");
        T newv = this->v + xp->v;
        bool did_overflow = newv < this->v;
        this->v = newv;
        return !did_overflow;
    }
    bool subtract(const PrimitiveValue& x) override {
        const auto* xp = &x.check<Self>("subtract");
        T newv = this->v - xp->v;
        bool did_overflow = newv > this->v;
        this->v = newv;
        return !did_overflow;
    }
    bool multiply(const PrimitiveValue& x) override {
        const auto* xp = &x.check<Self>("multiply");
        T newv = this->v * xp->v;
        bool did_overflow = newv < this->v && newv < xp->v;
        this->v = newv;
        return !did_overflow;
    }
    bool divide(const PrimitiveValue& x) override {
        const auto* xp = &x.check<Self>("divide");
        if(xp->v == 0)  return false;
        T newv = this->v / xp->v;
        this->v = newv;
        return true;
    }
    bool modulo(const PrimitiveValue& x) override {
        const auto* xp = &x.check<Self>("modulo");
        if(xp->v == 0)  return false;
        T newv = this->v % xp->v;
        this->v = newv;
        return true;
    }

    U128 as_u128() const override {
        return U128(static_cast<uint64_t>(this->v));
    }
};
struct PrimitiveI64: public PrimitiveSInt<int64_t>
{
    PrimitiveI64(int64_t v): PrimitiveSInt(v) {}
    void write_to_value(ValueCommonWrite& tgt, size_t ofs) const override {
        tgt.write_i64(ofs, this->v);
    }
};
struct PrimitiveI32: public PrimitiveSInt<int32_t>
{
    PrimitiveI32(int32_t v): PrimitiveSInt(v) {}
    void write_to_value(ValueCommonWrite& tgt, size_t ofs) const override {
        tgt.write_i32(ofs, this->v);
    }
};

class PrimitiveValueVirt
{
    uint64_t    buf[3]; // Allows i128 plus a vtable pointer
    PrimitiveValueVirt() {}
public:
    // HACK: No copy/move constructors, assumes that contained data is always POD
    ~PrimitiveValueVirt() {
        reinterpret_cast<PrimitiveValue*>(&this->buf)->~PrimitiveValue();
    }
    PrimitiveValue& get() { return *reinterpret_cast<PrimitiveValue*>(&this->buf); }
    const PrimitiveValue& get() const { return *reinterpret_cast<const PrimitiveValue*>(&this->buf); }

    static PrimitiveValueVirt from_value(const ::HIR::TypeRef& t, const ValueRef& v) {
        PrimitiveValueVirt  rv;
        LOG_ASSERT(t.get_wrapper() == nullptr, "PrimitiveValueVirt::from_value: " << t);
        switch(t.inner_type)
        {
        case RawType::U8:
            new(&rv.buf) PrimitiveU8(v.read_u8(0));
            break;
        case RawType::U16:
            new(&rv.buf) PrimitiveU16(v.read_u16(0));
            break;
        case RawType::U32:
            new(&rv.buf) PrimitiveU32(v.read_u32(0));
            break;
        case RawType::U64:
            new(&rv.buf) PrimitiveU64(v.read_u64(0));
            break;
        case RawType::USize:
            if( POINTER_SIZE == 8 )
                new(&rv.buf) PrimitiveU64(v.read_u64(0));
            else
                new(&rv.buf) PrimitiveU32(v.read_u32(0));
            break;

        case RawType::I32:
            new(&rv.buf) PrimitiveI32(v.read_i32(0));
            break;
        case RawType::I64:
            new(&rv.buf) PrimitiveI64(v.read_i64(0));
            break;
        case RawType::ISize:
            if( POINTER_SIZE == 8 )
                new(&rv.buf) PrimitiveI64(v.read_i64(0));
            else
                new(&rv.buf) PrimitiveI32(v.read_i32(0));
            break;
        default:
            LOG_TODO("PrimitiveValueVirt::from_value: " << t);
        }
        return rv;
    }
};

struct Ops {
    template<typename T>
    static int do_compare(T l, T r) {
        if( l == r ) {
            return 0;
        }
        else if( !(l != r) ) {
            // Special return value for NaN w/ NaN
            return 2;
        }
        else if( l < r ) {
            return -1;
        }
        else {
            return 1;
        }
    }
    template<typename T>
    static T do_bitwise(T l, T r, ::MIR::eBinOp op) {
        switch(op)
        {
        case ::MIR::eBinOp::BIT_AND:    return l & r;
        case ::MIR::eBinOp::BIT_OR:     return l | r;
        case ::MIR::eBinOp::BIT_XOR:    return l ^ r;
        case ::MIR::eBinOp::BIT_SHL:    return l << r;
        case ::MIR::eBinOp::BIT_SHR:    return l >> r;
        default:
            LOG_BUG("Unexpected operation in Ops::do_bitwise");
        }
    }
};

struct MirHelpers
{
    InterpreterThread&  thread;
    InterpreterThread::StackFrame&  frame;

    MirHelpers(InterpreterThread& thread, InterpreterThread::StackFrame& frame):
        thread(thread),
        frame(frame)
    {
    }

    ValueRef get_value_and_type_root(const ::MIR::LValue::Storage& lv_root, ::HIR::TypeRef& ty)
    {
        switch(lv_root.tag())
        {
        case ::MIR::LValue::Storage::TAGDEAD:    throw "";
        // --> Slots
        TU_ARM(lv_root, Return, _e) {
            ty = this->frame.fcn->ret_ty;
            return ValueRef(this->frame.ret);
            } break;
        TU_ARM(lv_root, Local, e) {
            ty = this->frame.fcn->m_mir.locals.at(e);
            return ValueRef(this->frame.locals.at(e));
            } break;
        TU_ARM(lv_root, Argument, e) {
            ty = this->frame.fcn->args.at(e);
            return ValueRef(this->frame.args.at(e));
            } break;
        TU_ARM(lv_root, Static, e) {
            /*const*/ auto& s = this->thread.m_modtree.get_static(e);
            ty = s.ty;
            return ValueRef(s.val);
            } break;
        }
        throw "";
    }
    ValueRef get_value_and_type(const ::MIR::LValue& lv, ::HIR::TypeRef& ty)
    {
        auto vr = get_value_and_type_root(lv.m_root, ty);
        for(const auto& w : lv.m_wrappers)
        {
            switch(w.tag())
            {
            case ::MIR::LValue::Wrapper::TAGDEAD:    throw "";
            // --> Modifiers
            TU_ARM(w, Index, idx_var) {
                auto idx = this->frame.locals.at(idx_var).read_usize(0);
                const auto* wrapper = ty.get_wrapper();
                if( !wrapper )
                {
                    LOG_ERROR("Indexing non-array/slice - " << ty);
                    throw "ERROR";
                }
                else if( wrapper->type == TypeWrapper::Ty::Array )
                {
                    ty = ty.get_inner();
                    vr.m_offset += ty.get_size() * idx;
                }
                else if( wrapper->type == TypeWrapper::Ty::Slice )
                {
                    ty = ty.get_inner();
                    LOG_ASSERT(vr.m_metadata, "No slice metadata");
                    auto len = vr.m_metadata->read_usize(0);
                    LOG_ASSERT(idx < len, "Slice index out of range");
                    vr.m_offset += ty.get_size() * idx;
                    vr.m_metadata.reset();
                }
                else
                {
                    LOG_ERROR("Indexing non-array/slice - " << ty);
                    throw "ERROR";
                }
                } break;
            TU_ARM(w, Field, fld_idx) {
                // TODO: if there's metadata present in the base, but the inner doesn't have metadata, clear the metadata
                size_t inner_ofs;
                auto inner_ty = ty.get_field(fld_idx, inner_ofs);
                LOG_DEBUG("Field - " << ty << "#" << fld_idx << " = @" << inner_ofs << " " << inner_ty);
                vr.m_offset += inner_ofs;
                if( inner_ty.get_meta_type() == HIR::TypeRef(RawType::Unreachable) )
                {
                    LOG_ASSERT(vr.m_size >= inner_ty.get_size(), "Field didn't fit in the value - " << inner_ty.get_size() << " required, but " << vr.m_size << " available");
                    vr.m_size = inner_ty.get_size();
                }
                ty = ::std::move(inner_ty);
                }
            TU_ARM(w, Downcast, variant_index) {
                auto composite_ty = ::std::move(ty);
                LOG_DEBUG("Downcast - " << composite_ty);

                size_t inner_ofs;
                ty = composite_ty.get_field(variant_index, inner_ofs);
                vr.m_offset += inner_ofs;
                }
            TU_ARM(w, Deref, _) {
                auto ptr_ty = ::std::move(ty);
                ty = ptr_ty.get_inner();
                LOG_DEBUG("Deref - " << vr << " into " << ty);

                LOG_ASSERT(vr.m_size >= POINTER_SIZE, "Deref pointer isn't large enough to be a pointer");
                // TODO: Move the metadata machinery into `deref` (or at least the logic needed to get the value size)
                //auto inner_val = vr.deref(0, ty);
                size_t ofs = vr.read_usize(0);
                LOG_ASSERT(ofs != 0, "Dereferencing NULL pointer");
                auto alloc = vr.get_relocation(0);
                if( alloc )
                {
                    // TODO: It's valid to dereference (but not read) a non-null invalid pointer.
                    LOG_ASSERT(ofs >= Allocation::PTR_BASE, "Dereferencing invalid pointer - " << ofs << " into " << alloc);
                    ofs -= Allocation::PTR_BASE;
                }
                else
                {
                }

                // There MUST be a relocation at this point with a valid allocation.
                LOG_TRACE("Interpret " << alloc << " + " << ofs << " as value of type " << ty);
                // NOTE: No alloc can happen when dereferencing a zero-sized pointer
                if( alloc.is_alloc() )
                {
                    //LOG_DEBUG("Deref - lvr=" << ::MIR::LValue::CRef(lv, &w - &lv.m_wrappers.front()) << " alloc=" << alloc.alloc());
                }
                else
                {
                    LOG_ASSERT(ty.get_meta_type() != RawType::Unreachable || ty.get_size() >= 0, "Dereference (giving a non-ZST) with no allocation");
                }
                size_t size;

                const auto meta_ty = ty.get_meta_type();
                ::std::shared_ptr<Value>    meta_val;
                // If the type has metadata, store it.
                if( meta_ty != RawType::Unreachable )
                {
                    auto meta_size = meta_ty.get_size();
                    LOG_ASSERT(vr.m_size == POINTER_SIZE + meta_size, "Deref of " << ty << ", but pointer isn't correct size");
                    meta_val = ::std::make_shared<Value>( vr.read_value(POINTER_SIZE, meta_size) );

                    size_t    slice_inner_size;
                    if( ty.has_slice_meta(slice_inner_size) ) {
                        // Slice metadata, add the base size (if it's a struct) to the variable size
                        // - `get_wrapper` will return non-null for `[T]`, special-case `str`
                        size = (ty != RawType::Str && ty.get_wrapper() == nullptr ? ty.get_size() : 0) + meta_val->read_usize(0) * slice_inner_size;
                    }
                    //else if( ty == RawType::TraitObject) {
                    //    // NOTE: Getting the size from the allocation is semi-valid, as you can't sub-slice trait objects
                    //    size = alloc.get_size() - ofs;
                    //}
                    else {
                        LOG_DEBUG("> Meta " << *meta_val << ", size = " << alloc.get_size() << " - " << ofs);
                        // TODO: if the inner type is a trait object, then check that it has an allocation.
                        size = alloc.get_size() - ofs;
                    }
                }
                else
                {
                    LOG_DEBUG("sizeof(" << ty << ") = " << ty.get_size());
                    LOG_ASSERT(vr.m_size == POINTER_SIZE, "Deref of a value that isn't a pointer-sized value (size=" << vr << ") - " << vr << ": " << ptr_ty);
                    size = ty.get_size();
                    if( !alloc && size > 0 ) {
                        LOG_ERROR("Deref of a non-ZST pointer with no relocation - " << vr);
                    }
                }

                LOG_DEBUG("Deref - New VR: alloc=" << alloc << ", ofs=" << ofs << ", size=" << size);
                vr = ValueRef(::std::move(alloc), ofs, size);
                vr.m_metadata = ::std::move(meta_val);
                } break;
            }
        }
        return vr;
    }
    ValueRef get_value_ref(const ::MIR::LValue& lv)
    {
        ::HIR::TypeRef  tmp;
        return get_value_and_type(lv, tmp);
    }

    ::HIR::TypeRef get_lvalue_ty(const ::MIR::LValue& lv)
    {
        ::HIR::TypeRef  ty;
        get_value_and_type(lv, ty);
        return ty;
    }

    Value read_lvalue_with_ty(const ::MIR::LValue& lv, ::HIR::TypeRef& ty)
    {
        auto base_value = get_value_and_type(lv, ty);

        return base_value.read_value(0, ty.get_size());
    }
    Value read_lvalue(const ::MIR::LValue& lv)
    {
        ::HIR::TypeRef  ty;
        return read_lvalue_with_ty(lv, ty);
    }
    void write_lvalue(const ::MIR::LValue& lv, Value val)
    {
        // TODO: Ensure that target is writable? Or should write_value do that?
        //LOG_DEBUG(lv << " = " << val);
        ::HIR::TypeRef  ty;
        auto base_value = get_value_and_type(lv, ty);

        if( val.size() > 0 )
        {
            if(!base_value.m_value) {
                base_value.m_alloc.alloc().write_value(base_value.m_offset, ::std::move(val));
            }
            else {
                base_value.m_value->write_value(base_value.m_offset, ::std::move(val));
            }
        }
    }

    Value const_to_value(const ::MIR::Constant& c, ::HIR::TypeRef& ty)
    {
        switch(c.tag())
        {
        case ::MIR::Constant::TAGDEAD:  throw "";
        TU_ARM(c, Int, ce) {
            ty = ::HIR::TypeRef(ce.t);
            Value val = Value(ty);
            val.write_bytes(0, &ce.v, ::std::min(ty.get_size(), sizeof(ce.v)));  // TODO: Endian
            // TODO: If the write was clipped, sign-extend
            // TODO: i128/u128 need the upper bytes cleared+valid
            return val;
            } break;
        TU_ARM(c, Uint, ce) {
            ty = ::HIR::TypeRef(ce.t);
            Value val = Value(ty);
            val.write_bytes(0, &ce.v, ::std::min(ty.get_size(), sizeof(ce.v)));  // TODO: Endian
            // i128/u128 need the upper bytes cleared+valid
            if( ce.t.raw_type == RawType::U128 ) {
                uint64_t    zero = 0;
                val.write_bytes(8, &zero, 8);
            }
            return val;
            } break;
        TU_ARM(c, Bool, ce) {
            Value val = Value(::HIR::TypeRef { RawType::Bool });
            val.write_bytes(0, &ce.v, 1);
            return val;
            } break;
        TU_ARM(c, Float, ce) {
            ty = ::HIR::TypeRef(ce.t);
            Value val = Value(ty);
            if( ce.t.raw_type == RawType::F64 ) {
                val.write_bytes(0, &ce.v, ::std::min(ty.get_size(), sizeof(ce.v)));  // TODO: Endian/format?
            }
            else if( ce.t.raw_type == RawType::F32 ) {
                float v = static_cast<float>(ce.v);
                val.write_bytes(0, &v, ::std::min(ty.get_size(), sizeof(v)));  // TODO: Endian/format?
            }
            else {
                throw ::std::runtime_error("BUG: Invalid type in Constant::Float");
            }
            return val;
            } break;
        TU_ARM(c, Const, ce) {
            LOG_BUG("Constant::Const in mmir");
            } break;
        TU_ARM(c, Generic, ce) {
            LOG_BUG("Constant::Generic in mmir");
            } break;
        TU_ARM(c, Bytes, ce) {
            ty = ::HIR::TypeRef(RawType::U8).wrap(TypeWrapper::Ty::Slice, 0).wrap(TypeWrapper::Ty::Borrow, 0);
            Value val = Value(ty);
            val.write_ptr(0, Allocation::PTR_BASE + 0, RelocationPtr::new_ffi(FFIPointer::new_const_bytes("Constant::Bytes", ce.data(), ce.size())));
            val.write_usize(POINTER_SIZE, ce.size());
            LOG_DEBUG(c << " = " << val);
            return val;
            } break;
        TU_ARM(c, StaticString, ce) {
            ty = ::HIR::TypeRef(RawType::Str).wrap(TypeWrapper::Ty::Borrow, 0);
            Value val = Value(ty);
            val.write_ptr(0, Allocation::PTR_BASE + 0, RelocationPtr::new_string(&ce));
            val.write_usize(POINTER_SIZE, ce.size());
            LOG_DEBUG(c << " = " << val);
            return val;
            } break;
        // --> Accessor
        TU_ARM(c, ItemAddr, ce) {
            // Create a value with a special backing allocation of zero size that references the specified item.
            if( /*const auto* fn =*/ this->thread.m_modtree.get_function_opt(*ce) ) {
                ty = ::HIR::TypeRef(RawType::Function);
                return Value::new_fnptr(*ce);
            }
            if( const auto* s = this->thread.m_modtree.get_static_opt(*ce) ) {
                ty = s->ty.wrapped(TypeWrapper::Ty::Borrow, 0);
                LOG_ASSERT(s->val.m_inner.is_alloc, "Statics should already have an allocation assigned");
                return Value::new_pointer(ty, Allocation::PTR_BASE + 0, RelocationPtr::new_alloc(s->val.m_inner.alloc.alloc));
            }
            LOG_ERROR("Constant::ItemAddr - " << *ce << " - not found");
            } break;
        }
        throw "";
    }
    Value const_to_value(const ::MIR::Constant& c)
    {
        ::HIR::TypeRef  ty;
        return const_to_value(c, ty);
    }
    Value param_to_value(const ::MIR::Param& p, ::HIR::TypeRef& ty)
    {
        switch(p.tag())
        {
        case ::MIR::Param::TAGDEAD: throw "";
        TU_ARM(p, Constant, pe)
            return const_to_value(pe, ty);
        TU_ARM(p, LValue, pe)
            return read_lvalue_with_ty(pe, ty);
        }
        throw "";
    }
    Value param_to_value(const ::MIR::Param& p)
    {
        ::HIR::TypeRef  ty;
        return param_to_value(p, ty);
    }

    ValueRef get_value_ref_param(const ::MIR::Param& p, Value& tmp, ::HIR::TypeRef& ty)
    {
        switch(p.tag())
        {
        case ::MIR::Param::TAGDEAD: throw "";
        TU_ARM(p, Constant, pe)
            tmp = const_to_value(pe, ty);
            return ValueRef(tmp, 0, ty.get_size());
        TU_ARM(p, LValue, pe)
            return get_value_and_type(pe, ty);
        }
        throw "";
    }
};

// ====================================================================
//
// ====================================================================
InterpreterThread::~InterpreterThread()
{
    for(size_t i = 0; i < m_stack.size(); i++)
    {
        const auto& frame = m_stack[m_stack.size() - 1 - i];
        ::std::cout << "#" << i << ": F" << frame.frame_index << " ";
        if( frame.cb )
        {
            ::std::cout << "WRAPPER";
        }
        else
        {
            ::std::cout << frame.fcn->my_path << " BB" << frame.bb_idx << "/";
            if( frame.stmt_idx == frame.fcn->m_mir.blocks.at(frame.bb_idx).statements.size() )
                ::std::cout << "TERM";
            else
                ::std::cout << frame.stmt_idx;
        }
        ::std::cout << ::std::endl;
    }
}
void InterpreterThread::start(const ::HIR::Path& p, ::std::vector<Value> args)
{
    assert( this->m_stack.empty() );
    Value   v;
    if( this->call_path(v, p, ::std::move(args)) )
    {
        LOG_TODO("Handle immediate return thread entry");
    }
}
bool InterpreterThread::step_one(Value& out_thread_result)
{
    assert( !this->m_stack.empty() );
    assert( !this->m_stack.back().cb );
    auto& cur_frame = this->m_stack.back();
    auto instr_idx = this->m_instruction_count++;
    TRACE_FUNCTION_R("#" << instr_idx << " " << cur_frame.fcn->my_path << " BB" << cur_frame.bb_idx << "/" << cur_frame.stmt_idx, "#" << instr_idx);
    const auto& bb = cur_frame.fcn->m_mir.blocks.at( cur_frame.bb_idx );

    const size_t    MAX_STACK_DEPTH = 90;
    if( this->m_stack.size() > MAX_STACK_DEPTH )
    {
        LOG_ERROR("Maximum stack depth of " << MAX_STACK_DEPTH << " exceeded");
    }

    MirHelpers  state { *this, cur_frame };

    if( cur_frame.stmt_idx < bb.statements.size() )
    {
        const auto& stmt = bb.statements[cur_frame.stmt_idx];
        LOG_DEBUG("=== F" << cur_frame.frame_index << " BB" << cur_frame.bb_idx << "/" << cur_frame.stmt_idx << ": " << stmt);
        switch(stmt.tag())
        {
        case ::MIR::Statement::TAGDEAD: throw "";
        TU_ARM(stmt, Assign, se) {
            Value   new_val;
            switch(se.src.tag())
            {
            case ::MIR::RValue::TAGDEAD: throw "";
            TU_ARM(se.src, Use, re) {
                new_val = state.read_lvalue(re);
                } break;
            TU_ARM(se.src, Constant, re) {
                new_val = state.const_to_value(re);
                } break;
            TU_ARM(se.src, Borrow, re) {
                ::HIR::TypeRef  src_ty;
                ValueRef src_base_value = state.get_value_and_type(re.val, src_ty);
                auto alloc = src_base_value.m_alloc;
                // If the source doesn't yet have a relocation, give it a backing allocation so we can borrow
                if( !alloc && src_base_value.m_value )
                {
                    LOG_DEBUG("Borrow - Creating allocation for " << src_base_value);
                    alloc = RelocationPtr::new_alloc( src_base_value.m_value->borrow("Borrow") );
                }
                if( alloc.is_alloc() )
                    LOG_DEBUG("Borrow - alloc=" << alloc << " (" << alloc.alloc() << ")");
                else
                    LOG_DEBUG("Borrow - alloc=" << alloc);
                size_t ofs = src_base_value.m_offset;
                const auto meta = src_ty.get_meta_type();
                auto dst_ty = src_ty.wrapped(TypeWrapper::Ty::Borrow, static_cast<size_t>(re.type));
                LOG_DEBUG("Borrow - ofs=" << ofs << ", meta_ty=" << meta);

                // Create the pointer (can this just store into the target?)
                new_val = Value(dst_ty);
                new_val.write_ptr(0, Allocation::PTR_BASE + ofs, ::std::move(alloc));
                // - Add metadata if required
                if( meta != RawType::Unreachable )
                {
                    LOG_ASSERT(src_base_value.m_metadata, "Borrow of an unsized value, but no metadata avaliable");
                    new_val.write_value(POINTER_SIZE, *src_base_value.m_metadata);
                }
                } break;
            TU_ARM(se.src, Cast, re) {
                // Determine the type of cast, is it a reinterpret or is it a value transform?
                // - Float <-> integer is a transform, anything else should be a reinterpret.
                ::HIR::TypeRef  src_ty;
                auto src_value = state.get_value_and_type(re.val, src_ty);

                new_val = Value(re.type);
                if( re.type == src_ty )
                {
                    // No-op cast
                    new_val = src_value.read_value(0, re.type.get_size());
                }
                else if( const auto* dst_w = re.type.get_wrapper() )
                {
                    // Destination can only be a raw pointer
                    if( dst_w->type != TypeWrapper::Ty::Pointer ) {
                        LOG_ERROR("Attempting to cast to a type other than a raw pointer - " << re.type);
                    }
                    if( const auto* src_w = src_ty.get_wrapper() )
                    {
                        // Source can be either
                        if( src_w->type != TypeWrapper::Ty::Pointer && src_w->type != TypeWrapper::Ty::Borrow ) {
                            LOG_ERROR("Attempting to cast to a pointer from a non-pointer - " << src_ty);
                        }

                        if( src_ty.get_size() < re.type.get_size() )
                        {
                            LOG_ERROR("Casting to a fatter pointer, " << src_ty << " -> " << re.type);
                        }
                        else
                        {
                            new_val = src_value.read_value(0, re.type.get_size());
                        }
                    }
                    else
                    {
                        if( src_ty == RawType::Function )
                        {
                        }
                        else if( src_ty == RawType::USize )
                        {
                        }
                        else
                        {
                            LOG_ERROR("Trying to cast to pointer (" << re.type <<" ) from invalid type (" << src_ty << ")\n");
                        }
                        new_val = src_value.read_value(0, re.type.get_size());
                    }
                }
                else if( const auto* src_w = src_ty.get_wrapper() )
                {
                    if( src_w->type != TypeWrapper::Ty::Pointer && src_w->type != TypeWrapper::Ty::Borrow ) {
                        LOG_ERROR("Attempting to cast from a non-pointer - " << src_ty);
                    }
                    // TODO: MUST be a thin pointer?

                    // TODO: MUST be an integer (usize only?)
                    switch(re.type.wrappers.empty() ? re.type.inner_type : RawType::Unreachable)
                    {
                    case RawType::USize:
                    case RawType::ISize:
                        break;
                    case RawType::U64:
                    case RawType::I64:
                        // TODO: Only if 64-bit?
                        break;
                    default:
                        LOG_ERROR("Casting from a pointer to non-usize - " << src_ty << " to " << re.type);
                        throw "ERROR";
                    }
                    new_val = src_value.read_value(0, re.type.get_size());
                }
                else
                {
                    // TODO: What happens if there'a cast of something with a relocation?
                    switch(re.type.inner_type)
                    {
                    case RawType::Unreachable:  throw "BUG";
                    case RawType::Composite:
                    case RawType::TraitObject:
                    case RawType::Function:
                    case RawType::Str:
                    case RawType::Unit:
                        LOG_ERROR("Casting to " << re.type << " is invalid");
                        throw "ERROR";
                    case RawType::F32: {
                        float dst_val = 0.0;
                        // Can be an integer, or F64 (pointer is impossible atm)
                        switch(src_ty.inner_type)
                        {
                        case RawType::Unreachable:  throw "BUG";
                        case RawType::Composite:    throw "ERROR";
                        case RawType::TraitObject:  throw "ERROR";
                        case RawType::Function:     throw "ERROR";
                        case RawType::Char: throw "ERROR";
                        case RawType::Str:  throw "ERROR";
                        case RawType::Unit: throw "ERROR";
                        case RawType::Bool: throw "ERROR";
                        case RawType::F32:  throw "BUG";
                        case RawType::F64:  dst_val = static_cast<float>( src_value.read_f64(0) ); break;
                        case RawType::USize:    LOG_TODO("f32 from " << src_ty);// /*dst_val = src_value.read_usize();*/   break;
                        case RawType::ISize:    LOG_TODO("f32 from " << src_ty);// /*dst_val = src_value.read_isize();*/   break;
                        case RawType::U8:   dst_val = static_cast<float>( src_value.read_u8 (0) );  break;
                        case RawType::I8:   dst_val = static_cast<float>( src_value.read_i8 (0) );  break;
                        case RawType::U16:  dst_val = static_cast<float>( src_value.read_u16(0) );  break;
                        case RawType::I16:  dst_val = static_cast<float>( src_value.read_i16(0) );  break;
                        case RawType::U32:  dst_val = static_cast<float>( src_value.read_u32(0) );  break;
                        case RawType::I32:  dst_val = static_cast<float>( src_value.read_i32(0) );  break;
                        case RawType::U64:  dst_val = static_cast<float>( src_value.read_u64(0) );  break;
                        case RawType::I64:  dst_val = static_cast<float>( src_value.read_i64(0) );  break;
                        case RawType::U128: LOG_TODO("f32 from " << src_ty);// /*dst_val = src_value.read_u128();*/ break;
                        case RawType::I128: LOG_TODO("f32 from " << src_ty);// /*dst_val = src_value.read_i128();*/ break;
                        }
                        new_val.write_f32(0, dst_val);
                        } break;
                    case RawType::F64: {
                        double dst_val = 0.0;
                        // Can be an integer, or F32 (pointer is impossible atm)
                        switch(src_ty.inner_type)
                        {
                        case RawType::Unreachable:  throw "BUG";
                        case RawType::Composite:    throw "ERROR";
                        case RawType::TraitObject:  throw "ERROR";
                        case RawType::Function:     throw "ERROR";
                        case RawType::Char: throw "ERROR";
                        case RawType::Str:  throw "ERROR";
                        case RawType::Unit: throw "ERROR";
                        case RawType::Bool: throw "ERROR";
                        case RawType::F64:  throw "BUG";
                        case RawType::F32:  dst_val = static_cast<double>( src_value.read_f32(0) ); break;
                        case RawType::USize:    dst_val = static_cast<double>( src_value.read_usize(0) );   break;
                        case RawType::ISize:    dst_val = static_cast<double>( src_value.read_isize(0) );   break;
                        case RawType::U8:   dst_val = static_cast<double>( src_value.read_u8 (0) );  break;
                        case RawType::I8:   dst_val = static_cast<double>( src_value.read_i8 (0) );  break;
                        case RawType::U16:  dst_val = static_cast<double>( src_value.read_u16(0) );  break;
                        case RawType::I16:  dst_val = static_cast<double>( src_value.read_i16(0) );  break;
                        case RawType::U32:  dst_val = static_cast<double>( src_value.read_u32(0) );  break;
                        case RawType::I32:  dst_val = static_cast<double>( src_value.read_i32(0) );  break;
                        case RawType::U64:  dst_val = static_cast<double>( src_value.read_u64(0) );  break;
                        case RawType::I64:  dst_val = static_cast<double>( src_value.read_i64(0) );  break;
                        case RawType::U128: throw "TODO"; /*dst_val = src_value.read_u128();*/ break;
                        case RawType::I128: throw "TODO"; /*dst_val = src_value.read_i128();*/ break;
                        }
                        new_val.write_f64(0, dst_val);
                        } break;
                    case RawType::Bool:
                        LOG_TODO("Cast to " << re.type);
                    case RawType::Char:
                        switch(src_ty.inner_type)
                        {
                        case RawType::Char: new_val.write_u32(0, src_value.read_u32(0) );  break;
                        case RawType::U8:   new_val.write_u32(0, src_value.read_u8(0) );   break;
                        default:
                            LOG_ERROR("Cast from " << src_ty << " to char isn't valid");
                            break;
                        }
                        break;
                    case RawType::USize:
                    case RawType::U8:
                    case RawType::U16:
                    case RawType::U32:
                    case RawType::U64:
                    case RawType::ISize:
                    case RawType::I8:
                    case RawType::I16:
                    case RawType::I32:
                    case RawType::I64:
                        {
                        uint64_t dst_val = 0;
                        // Can be an integer, or F32 (pointer is impossible atm)
                        switch(src_ty.inner_type)
                        {
                        case RawType::Unreachable:
                            LOG_BUG("Casting unreachable");
                        case RawType::TraitObject:
                        case RawType::Str:
                            LOG_FATAL("Cast of unsized type - " << src_ty);
                        case RawType::Function:
                            LOG_ASSERT(re.type.inner_type == RawType::USize, "Function pointers can only be casted to usize, instead " << re.type);
                            new_val = src_value.read_value(0, re.type.get_size());
                            break;
                        case RawType::Char: {
                            uint32_t v = src_value.read_u32(0);
                            switch(re.type.inner_type)
                            {
                            case RawType::U8:
                                if( v > 0xFF ) {
                                    LOG_NOTICE("Casting to u8 from char above 255");
                                }
                                new_val.write_u8(0, v & 0xFF);
                                break;
                            case RawType::U32:
                                new_val = src_value.read_value(0, 4);
                                break;
                            case RawType::USize:
                                new_val.write_usize(0, v);
                                break;
                            default:
                                LOG_ERROR("Char can only be casted to u32/u8, instead " << re.type);
                            }
                            } break;
                        case RawType::Unit:
                            LOG_FATAL("Cast of unit");
                        case RawType::Composite: {
                            const auto& dt = src_ty.composite_type();
                            if( dt.variants.size() == 0 ) {
                                LOG_FATAL("Cast of composite - " << src_ty);
                            }
                            // TODO: Check that all variants have the same tag offset
                            LOG_ASSERT(dt.fields.size() == 1, "");
                            LOG_ASSERT(dt.fields[0].first == 0, "");
                            for(size_t i = 0; i < dt.variants.size(); i ++ ) {
                                LOG_ASSERT(dt.variants[i].base_field == 0, "");
                                LOG_ASSERT(dt.variants[i].field_path.empty(), "");
                            }
                            ::HIR::TypeRef  tag_ty = dt.fields[0].second;
                            LOG_ASSERT(tag_ty.get_wrapper() == nullptr, "");
                            switch(tag_ty.inner_type)
                            {
                            case RawType::USize:
                                dst_val = static_cast<uint64_t>( src_value.read_usize(0) );
                                if(0)
                            case RawType::ISize:
                                dst_val = static_cast<uint64_t>( src_value.read_isize(0) );
                                if(0)
                            case RawType::U8:
                                dst_val = static_cast<uint64_t>( src_value.read_u8 (0) );
                                if(0)
                            case RawType::I8:
                                dst_val = static_cast<uint64_t>( src_value.read_i8 (0) );
                                if(0)
                            case RawType::U16:
                                dst_val = static_cast<uint64_t>( src_value.read_u16(0) );
                                if(0)
                            case RawType::I16:
                                dst_val = static_cast<uint64_t>( src_value.read_i16(0) );
                                if(0)
                            case RawType::U32:
                                dst_val = static_cast<uint64_t>( src_value.read_u32(0) );
                                if(0)
                            case RawType::I32:
                                dst_val = static_cast<uint64_t>( src_value.read_i32(0) );
                                if(0)
                            case RawType::U64:
                                dst_val = static_cast<uint64_t>( src_value.read_u64(0) );
                                if(0)
                            case RawType::I64:
                                dst_val = static_cast<uint64_t>( src_value.read_i64(0) );
                                break;
                            default:
                                LOG_FATAL("Bad tag type in cast - " << tag_ty);
                            }
                            } if(0)
                        case RawType::Bool:
                            dst_val = static_cast<uint64_t>( src_value.read_u8 (0) );
                            if(0)
                        case RawType::F64:
                            dst_val = static_cast<uint64_t>( src_value.read_f64(0) );
                            if(0)
                        case RawType::F32:
                            dst_val = static_cast<uint64_t>( src_value.read_f32(0) );
                            if(0)
                        case RawType::USize:
                            dst_val = static_cast<uint64_t>( src_value.read_usize(0) );
                            if(0)
                        case RawType::ISize:
                            dst_val = static_cast<uint64_t>( src_value.read_isize(0) );
                            if(0)
                        case RawType::U8:
                            dst_val = static_cast<uint64_t>( src_value.read_u8 (0) );
                            if(0)
                        case RawType::I8:
                            dst_val = static_cast<uint64_t>( src_value.read_i8 (0) );
                            if(0)
                        case RawType::U16:
                            dst_val = static_cast<uint64_t>( src_value.read_u16(0) );
                            if(0)
                        case RawType::I16:
                            dst_val = static_cast<uint64_t>( src_value.read_i16(0) );
                            if(0)
                        case RawType::U32:
                            dst_val = static_cast<uint64_t>( src_value.read_u32(0) );
                            if(0)
                        case RawType::I32:
                            dst_val = static_cast<uint64_t>( src_value.read_i32(0) );
                            if(0)
                        case RawType::U64:
                            dst_val = static_cast<uint64_t>( src_value.read_u64(0) );
                            if(0)
                        case RawType::I64:
                            dst_val = static_cast<uint64_t>( src_value.read_i64(0) );
                            if(0)
                        case RawType::U128:
                            dst_val = static_cast<uint64_t>( src_value.read_u128(0) );
                            if(0)
                        case RawType::I128:
                            LOG_TODO("Cast i128 to " << re.type);
                            //dst_val = static_cast<uint64_t>( src_value.read_i128(0) );

                            switch(re.type.inner_type)
                            {
                            case RawType::USize:
                                new_val.write_usize(0, dst_val);
                                break;
                            case RawType::U8:
                                new_val.write_u8(0, static_cast<uint8_t>(dst_val));
                                break;
                            case RawType::U16:
                                new_val.write_u16(0, static_cast<uint16_t>(dst_val));
                                break;
                            case RawType::U32:
                                new_val.write_u32(0, static_cast<uint32_t>(dst_val));
                                break;
                            case RawType::U64:
                                new_val.write_u64(0, dst_val);
                                break;
                            case RawType::ISize:
                                new_val.write_usize(0, static_cast<int64_t>(dst_val));
                                break;
                            case RawType::I8:
                                new_val.write_i8(0, static_cast<int8_t>(dst_val));
                                break;
                            case RawType::I16:
                                new_val.write_i16(0, static_cast<int16_t>(dst_val));
                                break;
                            case RawType::I32:
                                new_val.write_i32(0, static_cast<int32_t>(dst_val));
                                break;
                            case RawType::I64:
                                new_val.write_i64(0, static_cast<int64_t>(dst_val));
                                break;
                            default:
                                throw "";
                            }
                            break;
                        }
                        } break;
                    case RawType::U128:
                    case RawType::I128: {
                        U128    dst_val;
                        switch(src_ty.inner_type)
                        {
                        case RawType::U8:   dst_val = src_value.read_u8 (0);    break;
                        case RawType::I8:   dst_val = src_value.read_i8 (0);    break;
                        default:
                            LOG_TODO("Cast " << src_ty << " to " << re.type);
                        }
                        new_val.write_u128(0, dst_val);
                        } break;
                    }
                }
                } break;
            TU_ARM(se.src, BinOp, re) {
                ::HIR::TypeRef  ty_l, ty_r;
                Value   tmp_l, tmp_r;
                auto v_l = state.get_value_ref_param(re.val_l, tmp_l, ty_l);
                auto v_r = state.get_value_ref_param(re.val_r, tmp_r, ty_r);
                LOG_DEBUG(v_l << " (" << ty_l <<") ? " << v_r << " (" << ty_r <<")");

                switch(re.op)
                {
                case ::MIR::eBinOp::EQ:
                case ::MIR::eBinOp::NE:
                case ::MIR::eBinOp::GT:
                case ::MIR::eBinOp::GE:
                case ::MIR::eBinOp::LT:
                case ::MIR::eBinOp::LE: {
                    LOG_ASSERT(ty_l == ty_r, "BinOp type mismatch - " << ty_l << " != " << ty_r);
                    int res = 0;

                    auto reloc_l = v_l.get_relocation(0);
                    auto reloc_r = v_r.get_relocation(0);


                    // TODO: Handle comparison of the relocations too
                    // - If both sides have a relocation:
                    //   > EQ/NE always valid
                    //   > others require the same relocation
                    // - If one side has a relocation:
                    //   > EQ/NE only allow zero on the non-reloc side
                    //   > others are invalid?
                    if( reloc_l && reloc_r )
                    {
                        // Both have relocations, check if they're equal
                        if( reloc_l != reloc_r )
                        {
                            switch(re.op)
                            {
                            case ::MIR::eBinOp::EQ:
                            case ::MIR::eBinOp::NE:
                                res = 1;
                                break;
                            default:
                                LOG_FATAL("Unable to compare " << v_l << " and " << v_r << " - different relocations (" << reloc_l << " != " << reloc_r << ")");
                            }
                            // - Equality will always fail
                            // - Ordering is a bug
                        }
                        else
                        {
                            // Equal: Allow all comparisons
                        }
                    }
                    else if( reloc_l || reloc_r )
                    {
                        // Only one side
                        // - Ordering is a bug
                        // - Equalities are allowed, but only for `0`?
                        //  > TODO: If the side with no reloation doesn't have value `0` then error?
                        switch(re.op)
                        {
                        case ::MIR::eBinOp::EQ:
                        case ::MIR::eBinOp::NE:
                            // - Allow success, as addresses can be masked down
                            break;
                        default:
                            if( reloc_l )
                                res = 1;
                            else// if( reloc_r )
                                res = -1;
                            //LOG_FATAL("Unable to order " << v_l << " and " << v_r << " - different relocations");
                            break;
                        }
                    }
                    else
                    {
                        // No relocations, no need to check more
                    }

                    if( const auto* w = ty_l.get_wrapper() )
                    {
                        if( w->type == TypeWrapper::Ty::Pointer )
                        {
                            // TODO: Technically only EQ/NE are valid.

                            res = res != 0 ? res : Ops::do_compare(v_l.read_usize(0), v_r.read_usize(0));

                            // Compare fat metadata.
                            if( res == 0 && v_l.m_size > POINTER_SIZE )
                            {
                                reloc_l = v_l.get_relocation(POINTER_SIZE);
                                reloc_r = v_r.get_relocation(POINTER_SIZE);

                                if( res == 0 && reloc_l != reloc_r )
                                {
                                    res = (reloc_l < reloc_r ? -1 : 1);
                                }
                                res = res != 0 ? res : Ops::do_compare(v_l.read_usize(POINTER_SIZE), v_r.read_usize(POINTER_SIZE));
                            }
                        }
                        else
                        {
                            LOG_TODO("BinOp comparisons - " << se.src << " w/ " << ty_l);
                        }
                    }
                    else
                    {
                        switch(ty_l.inner_type)
                        {
                        case RawType::U64:  res = res != 0 ? res : Ops::do_compare(v_l.read_u64(0), v_r.read_u64(0));   break;
                        case RawType::U32:  res = res != 0 ? res : Ops::do_compare(v_l.read_u32(0), v_r.read_u32(0));   break;
                        case RawType::U16:  res = res != 0 ? res : Ops::do_compare(v_l.read_u16(0), v_r.read_u16(0));   break;
                        case RawType::U8 :  res = res != 0 ? res : Ops::do_compare(v_l.read_u8 (0), v_r.read_u8 (0));   break;
                        case RawType::I64:  res = res != 0 ? res : Ops::do_compare(v_l.read_i64(0), v_r.read_i64(0));   break;
                        case RawType::I32:  res = res != 0 ? res : Ops::do_compare(v_l.read_i32(0), v_r.read_i32(0));   break;
                        case RawType::I16:  res = res != 0 ? res : Ops::do_compare(v_l.read_i16(0), v_r.read_i16(0));   break;
                        case RawType::I8 :  res = res != 0 ? res : Ops::do_compare(v_l.read_i8 (0), v_r.read_i8 (0));   break;
                        case RawType::USize: res = res != 0 ? res : Ops::do_compare(v_l.read_usize(0), v_r.read_usize(0)); break;
                        case RawType::ISize: res = res != 0 ? res : Ops::do_compare(v_l.read_isize(0), v_r.read_isize(0)); break;
                        case RawType::Char: res = res != 0 ? res : Ops::do_compare(v_l.read_u32(0), v_r.read_u32(0)); break;
                        case RawType::Bool: res = res != 0 ? res : Ops::do_compare(v_l.read_u8(0), v_r.read_u8(0)); break;  // TODO: `read_bool` that checks for bool values?
                        case RawType::U128: res = res != 0 ? res : Ops::do_compare(v_l.read_u128(0), v_r.read_u128(0));   break;
                        case RawType::I128: res = res != 0 ? res : Ops::do_compare(v_l.read_i128(0), v_r.read_i128(0));   break;
                        default:
                            LOG_TODO("BinOp comparisons - " << se.src << " w/ " << ty_l);
                        }
                    }
                    bool res_bool;
                    switch(re.op)
                    {
                    case ::MIR::eBinOp::EQ: res_bool = (res == 0);  break;
                    case ::MIR::eBinOp::NE: res_bool = (res != 0);  break;
                    case ::MIR::eBinOp::GT: res_bool = (res == 1);  break;
                    case ::MIR::eBinOp::GE: res_bool = (res == 1 || res == 0);  break;
                    case ::MIR::eBinOp::LT: res_bool = (res == -1); break;
                    case ::MIR::eBinOp::LE: res_bool = (res == -1 || res == 0); break;
                        break;
                    default:
                        LOG_BUG("Unknown comparison");
                    }
                    new_val = Value(::HIR::TypeRef(RawType::Bool));
                    new_val.write_u8(0, res_bool ? 1 : 0);
                    } break;
                case ::MIR::eBinOp::BIT_SHL:
                case ::MIR::eBinOp::BIT_SHR: {
                    LOG_ASSERT(ty_l.get_wrapper() == nullptr, "Bitwise operator on non-primitive - " << ty_l);
                    LOG_ASSERT(ty_r.get_wrapper() == nullptr, "Bitwise operator with non-primitive - " << ty_r);
                    size_t max_bits = ty_l.get_size() * 8;
                    uint8_t shift;
                    auto check_cast_u = [&](auto v){ LOG_ASSERT(0 <= v && v <= max_bits, "Shift out of range - " << v); return static_cast<uint8_t>(v); };
                    auto check_cast_s = [&](auto v){ LOG_ASSERT(v <= static_cast<int64_t>(max_bits), "Shift out of range - " << v); return static_cast<uint8_t>(v); };
                    switch(ty_r.inner_type)
                    {
                    case RawType::U64:  shift = check_cast_u(v_r.read_u64(0));    break;
                    case RawType::U32:  shift = check_cast_u(v_r.read_u32(0));    break;
                    case RawType::U16:  shift = check_cast_u(v_r.read_u16(0));    break;
                    case RawType::U8 :  shift = check_cast_u(v_r.read_u8 (0));    break;
                    case RawType::I64:  shift = check_cast_s(v_r.read_i64(0));    break;
                    case RawType::I32:  shift = check_cast_s(v_r.read_i32(0));    break;
                    case RawType::I16:  shift = check_cast_s(v_r.read_i16(0));    break;
                    case RawType::I8 :  shift = check_cast_s(v_r.read_i8 (0));    break;
                    case RawType::USize:  shift = check_cast_u(v_r.read_usize(0));    break;
                    case RawType::ISize:  shift = check_cast_s(v_r.read_isize(0));    break;
                    default:
                        LOG_TODO("BinOp shift RHS unknown type - " << se.src << " w/ " << ty_r);
                    }
                    new_val = Value(ty_l);
                    switch(ty_l.inner_type)
                    {
                    // TODO: U128
                    case RawType::U128: new_val.write_u128(0, Ops::do_bitwise(v_l.read_u128(0), U128(shift), re.op));   break;
                    case RawType::U64:  new_val.write_u64(0, Ops::do_bitwise(v_l.read_u64(0), static_cast<uint64_t>(shift), re.op));   break;
                    case RawType::U32:  new_val.write_u32(0, Ops::do_bitwise(v_l.read_u32(0), static_cast<uint32_t>(shift), re.op));   break;
                    case RawType::U16:  new_val.write_u16(0, Ops::do_bitwise(v_l.read_u16(0), static_cast<uint16_t>(shift), re.op));   break;
                    case RawType::U8 :  new_val.write_u8 (0, Ops::do_bitwise(v_l.read_u8 (0), static_cast<uint8_t >(shift), re.op));   break;
                    case RawType::USize: new_val.write_usize(0, Ops::do_bitwise(v_l.read_usize(0), static_cast<uint64_t>(shift), re.op));   break;
                    // Is signed allowed? (yes)
                    // - What's the exact semantics? For now assuming it's unsigned+reinterpret
                    case RawType::ISize: new_val.write_usize(0, Ops::do_bitwise(v_l.read_usize(0), static_cast<uint64_t>(shift), re.op));   break;
                    default:
                        LOG_TODO("BinOp shift LHS unknown type - " << se.src << " w/ " << ty_l);
                    }
                    } break;
                case ::MIR::eBinOp::BIT_AND:
                case ::MIR::eBinOp::BIT_OR:
                case ::MIR::eBinOp::BIT_XOR:
                    LOG_ASSERT(ty_l == ty_r, "BinOp type mismatch - " << ty_l << " != " << ty_r);
                    LOG_ASSERT(ty_l.get_wrapper() == nullptr, "Bitwise operator on non-primitive - " << ty_l);
                    new_val = Value(ty_l);
                    switch(ty_l.inner_type)
                    {
                    case RawType::U128:
                    case RawType::I128:
                        new_val.write_u128( 0, Ops::do_bitwise(v_l.read_u128(0), v_r.read_u128(0), re.op) );
                        break;
                    case RawType::U64:
                    case RawType::I64:
                        new_val.write_u64( 0, Ops::do_bitwise(v_l.read_u64(0), v_r.read_u64(0), re.op) );
                        break;
                    case RawType::U32:
                    case RawType::I32:
                        new_val.write_u32( 0, static_cast<uint32_t>(Ops::do_bitwise(v_l.read_u32(0), v_r.read_u32(0), re.op)) );
                        break;
                    case RawType::U16:
                    case RawType::I16:
                        new_val.write_u16( 0, static_cast<uint16_t>(Ops::do_bitwise(v_l.read_u16(0), v_r.read_u16(0), re.op)) );
                        break;
                    case RawType::U8:
                    case RawType::I8:
                    case RawType::Bool:
                        new_val.write_u8 ( 0, static_cast<uint8_t >(Ops::do_bitwise(v_l.read_u8 (0), v_r.read_u8 (0), re.op)) );
                        break;
                    case RawType::USize:
                    case RawType::ISize:
                        new_val.write_usize( 0, Ops::do_bitwise(v_l.read_usize(0), v_r.read_usize(0), re.op) );
                        break;
                    default:
                        LOG_TODO("BinOp bitwise - " << se.src << " w/ " << ty_l);
                    }
                    // If the LHS had a relocation, propagate it over
                    if( auto r = v_l.get_relocation(0) )
                    {
                        LOG_DEBUG("- Restore relocation " << r);
                        new_val.set_reloc(0, ::std::min(POINTER_SIZE, new_val.size()), r);
                    }

                    break;
                default:
                    LOG_ASSERT(ty_l == ty_r, "BinOp type mismatch - " << ty_l << " != " << ty_r);
                    auto val_l = PrimitiveValueVirt::from_value(ty_l, v_l);
                    auto val_r = PrimitiveValueVirt::from_value(ty_r, v_r);
                    RelocationPtr   new_val_reloc;
                    switch(re.op)
                    {
                    case ::MIR::eBinOp::ADD:
                        LOG_ASSERT(!v_r.get_relocation(0), "RHS of `+` has a relocation");
                        new_val_reloc = v_l.get_relocation(0);
                        val_l.get().add( val_r.get() );
                        break;
                    case ::MIR::eBinOp::SUB:
                        if( auto r = v_l.get_relocation(0) )
                        {
                            if( v_r.get_relocation(0) )
                            {
                                // Pointer difference, no relocation in output
                            }
                            else
                            {
                                new_val_reloc = ::std::move(r);
                            }
                        }
                        else
                        {
                            LOG_ASSERT(!v_r.get_relocation(0), "RHS of `-` has a relocation but LHS does not");
                        }
                        val_l.get().subtract( val_r.get() );
                        break;
                    case ::MIR::eBinOp::MUL:    val_l.get().multiply( val_r.get() ); break;
                    case ::MIR::eBinOp::DIV:    val_l.get().divide( val_r.get() ); break;
                    case ::MIR::eBinOp::MOD:    val_l.get().modulo( val_r.get() ); break;

                    default:
                        LOG_TODO("Unsupported binary operator?");
                    }
                    new_val = Value(ty_l);
                    val_l.get().write_to_value(new_val, 0);
                    if( new_val_reloc )
                    {
                        new_val.set_reloc(0, ::std::min(POINTER_SIZE, new_val.size()), ::std::move(new_val_reloc));
                    }
                    break;
                }
                } break;
            TU_ARM(se.src, UniOp, re) {
                ::HIR::TypeRef  ty;
                auto v = state.get_value_and_type(re.val, ty);
                LOG_ASSERT(ty.get_wrapper() == nullptr, "UniOp on wrapped type - " << ty);
                new_val = Value(ty);
                switch(re.op)
                {
                case ::MIR::eUniOp::INV:
                    switch(ty.inner_type)
                    {
                    case RawType::U128:
                    case RawType::I128:
                        LOG_TODO("UniOp::INV U128");
                    case RawType::U64:
                    case RawType::I64:
                        new_val.write_u64( 0, ~v.read_u64(0) );
                        break;
                    case RawType::U32:
                    case RawType::I32:
                        new_val.write_u32( 0, ~v.read_u32(0) );
                        break;
                    case RawType::U16:
                    case RawType::I16:
                        new_val.write_u16( 0, ~v.read_u16(0) );
                        break;
                    case RawType::U8:
                    case RawType::I8:
                        new_val.write_u8 ( 0, ~v.read_u8 (0) );
                        break;
                    case RawType::USize:
                    case RawType::ISize:
                        new_val.write_usize( 0, ~v.read_usize(0) );
                        break;
                    case RawType::Bool:
                        new_val.write_u8 ( 0, v.read_u8 (0) == 0 );
                        break;
                    default:
                        LOG_TODO("UniOp::INV - w/ type " << ty);
                    }
                    break;
                case ::MIR::eUniOp::NEG:
                    switch(ty.inner_type)
                    {
                    case RawType::I128:
                        LOG_TODO("UniOp::NEG I128");
                    case RawType::I64:
                        new_val.write_i64( 0, -v.read_i64(0) );
                        break;
                    case RawType::I32:
                        new_val.write_i32( 0, -v.read_i32(0) );
                        break;
                    case RawType::I16:
                        new_val.write_i16( 0, -v.read_i16(0) );
                        break;
                    case RawType::I8:
                        new_val.write_i8 ( 0, -v.read_i8 (0) );
                        break;
                    case RawType::ISize:
                        new_val.write_isize( 0, -v.read_isize(0) );
                        break;
                    default:
                        LOG_ERROR("UniOp::INV not valid on type " << ty);
                    }
                    break;
                }
                } break;
            TU_ARM(se.src, DstMeta, re) {
                auto ptr = state.get_value_ref(re.val);

                ::HIR::TypeRef  dst_ty;
                state.get_value_and_type(se.dst, dst_ty);
                new_val = ptr.read_value(POINTER_SIZE, dst_ty.get_size());
                } break;
            TU_ARM(se.src, DstPtr, re) {
                auto ptr = state.get_value_ref(re.val);

                new_val = ptr.read_value(0, POINTER_SIZE);
                } break;
            TU_ARM(se.src, MakeDst, re) {
                // - Get target type, just for some assertions
                ::HIR::TypeRef  dst_ty;
                state.get_value_and_type(se.dst, dst_ty);
                new_val = Value(dst_ty);

                auto ptr  = state.param_to_value(re.ptr_val );
                auto meta = state.param_to_value(re.meta_val);
                LOG_DEBUG("ty=" << dst_ty << ", ptr=" << ptr << ", meta=" << meta);

                new_val.write_value(0, ::std::move(ptr));
                new_val.write_value(POINTER_SIZE, ::std::move(meta));
                } break;
            TU_ARM(se.src, Tuple, re) {
                ::HIR::TypeRef  dst_ty;
                state.get_value_and_type(se.dst, dst_ty);
                new_val = Value(dst_ty);

                if( dst_ty.inner_type == RawType::Unit )
                {
                    LOG_ASSERT(re.vals.size() == 0 , "");
                }
                else
                {
                    LOG_ASSERT(dst_ty.inner_type == RawType::Composite, dst_ty);
                    for(size_t i = 0; i < re.vals.size(); i++)
                    {
                        auto fld_ofs = dst_ty.composite_type().fields.at(i).first;
                        new_val.write_value(fld_ofs, state.param_to_value(re.vals[i]));
                    }
                }
                } break;
            TU_ARM(se.src, Array, re) {
                ::HIR::TypeRef  dst_ty;
                state.get_value_and_type(se.dst, dst_ty);
                new_val = Value(dst_ty);
                // TODO: Assert that type is an array
                auto inner_ty = dst_ty.get_inner();
                size_t stride = inner_ty.get_size();

                size_t ofs = 0;
                for(const auto& v : re.vals)
                {
                    new_val.write_value(ofs, state.param_to_value(v));
                    ofs += stride;
                }
                } break;
            TU_ARM(se.src, SizedArray, re) {
                ::HIR::TypeRef  dst_ty;
                state.get_value_and_type(se.dst, dst_ty);
                new_val = Value(dst_ty);
                // TODO: Assert that type is an array
                auto inner_ty = dst_ty.get_inner();
                size_t stride = inner_ty.get_size();

                size_t ofs = 0;
                for(size_t i = 0; i < re.count; i++)
                {
                    new_val.write_value(ofs, state.param_to_value(re.val));
                    ofs += stride;
                }
                } break;
            TU_ARM(se.src, Variant, re) {
                // 1. Get the composite by path.
                const auto& data_ty = this->m_modtree.get_composite(re.path);
                auto dst_ty = ::HIR::TypeRef(&data_ty);
                new_val = Value(dst_ty);
                // Three cases:
                // - Unions (no tag)
                // - Data enums (tag and data)
                // - Value enums (no data)
                const auto& var = data_ty.variants.at(re.index);
                if( var.data_field != SIZE_MAX )
                {
                    const auto& fld = data_ty.fields.at(re.index);

                    new_val.write_value(fld.first, state.param_to_value(re.val));
                }
                if( var.base_field != SIZE_MAX )
                {
                    ::HIR::TypeRef  tag_ty;
                    size_t tag_ofs = dst_ty.get_field_ofs(var.base_field, var.field_path, tag_ty);
                    LOG_ASSERT(tag_ty.get_size() == var.tag_data.size(), "");
                    new_val.write_bytes(tag_ofs, var.tag_data.data(), var.tag_data.size());
                }
                else
                {
                    // Union, no tag
                }
                LOG_DEBUG("Variant " << new_val);
                } break;
            TU_ARM(se.src, Struct, re) {
                const auto& data_ty = m_modtree.get_composite(re.path);

                ::HIR::TypeRef  dst_ty;
                state.get_value_and_type(se.dst, dst_ty);
                new_val = Value(dst_ty);
                LOG_ASSERT(dst_ty.inner_type == RawType::Composite, dst_ty);
                LOG_ASSERT(dst_ty.ptr.composite_type == &data_ty, "Destination type of RValue::Struct isn't the same as the input");

                for(size_t i = 0; i < re.vals.size(); i++)
                {
                    auto fld_ofs = data_ty.fields.at(i).first;
                    auto v = state.param_to_value(re.vals[i]);
                    LOG_DEBUG("Struct - @" << fld_ofs << " = " << v);
                    new_val.write_value(fld_ofs, ::std::move(v));
                }
                } break;
            }
            LOG_DEBUG("- new_val=" << new_val);
            state.write_lvalue(se.dst, ::std::move(new_val));
            } break;
        case ::MIR::Statement::TAG_Asm:
            LOG_TODO(stmt);
            break;
        TU_ARM(stmt, Drop, se) {
            if( se.flag_idx == ~0u || cur_frame.drop_flags.at(se.flag_idx) )
            {
                ::HIR::TypeRef  ty;
                auto v = state.get_value_and_type(se.slot, ty);

                // - Take a pointer to the inner
                auto alloc = (v.m_value ? RelocationPtr::new_alloc(v.m_value->borrow("drop")) : v.m_alloc);
                size_t ofs = v.m_offset;
                //LOG_ASSERT(ty.get_meta_type() == RawType::Unreachable, "Dropping an unsized type with Statement::Drop - " << ty);

                auto ptr_ty = ty.wrapped(TypeWrapper::Ty::Borrow, /*BorrowTy::Unique*/2);

                auto ptr_val = Value::new_pointer(ptr_ty, Allocation::PTR_BASE + ofs, ::std::move(alloc));
                if( v.m_metadata )
                {
                    ptr_val.write_value(POINTER_SIZE, *v.m_metadata);
                }

                if( !drop_value(ptr_val, ty, /*shallow=*/se.kind == ::MIR::eDropKind::SHALLOW) )
                {
                    return false;
                }
            }
            } break;
        TU_ARM(stmt, SetDropFlag, se) {
            bool val = (se.other == ~0u ? false : cur_frame.drop_flags.at(se.other)) != se.new_val;
            LOG_DEBUG("- " << val);
            cur_frame.drop_flags.at(se.idx) = val;
            } break;
        case ::MIR::Statement::TAG_ScopeEnd:
            LOG_TODO(stmt);
            break;
        }

        cur_frame.stmt_idx += 1;
    }
    else
    {
        LOG_DEBUG("=== F" << cur_frame.frame_index << "  BB" << cur_frame.bb_idx << "/TERM: " << bb.terminator);
        switch(bb.terminator.tag())
        {
        case ::MIR::Terminator::TAGDEAD:    throw "";
        TU_ARM(bb.terminator, Incomplete, _te)
            LOG_TODO("Terminator::Incomplete hit");
        TU_ARM(bb.terminator, Diverge, _te)
            LOG_DEBUG("DIVERGE (continue panic)");
            assert(m_thread.panic_count > 0);
            m_thread.panic_active = true;
            return this->pop_stack(out_thread_result);
        TU_ARM(bb.terminator, Panic, _te)
            LOG_TODO("Terminator::Panic");
        TU_ARM(bb.terminator, Goto, te)
            cur_frame.bb_idx = te;
            break;
        TU_ARM(bb.terminator, Return, _te)
            LOG_DEBUG("RETURN " << cur_frame.ret);
            return this->pop_stack(out_thread_result);
        TU_ARM(bb.terminator, If, te) {
            uint8_t v = state.get_value_ref(te.cond).read_u8(0);
            LOG_ASSERT(v == 0 || v == 1, "");
            cur_frame.bb_idx = v ? te.bb0 : te.bb1;
            } break;
        TU_ARM(bb.terminator, Switch, te) {
            ::HIR::TypeRef ty;
            auto v = state.get_value_and_type(te.val, ty);
            LOG_ASSERT(ty.get_wrapper() == nullptr, "Matching on wrapped value - " << ty);
            LOG_ASSERT(ty.inner_type == RawType::Composite, "Matching on non-coposite - " << ty);
            LOG_DEBUG("Switch v = " << v);

            // TODO: Convert the variant list into something that makes it easier to switch on.
            size_t found_target = SIZE_MAX;
            size_t default_target = SIZE_MAX;
            for(size_t i = 0; i < ty.composite_type().variants.size(); i ++)
            {
                const auto& var = ty.composite_type().variants[i];
                if( var.tag_data.size() == 0 )
                {
                    // Save as the default, error for multiple defaults
                    if( default_target != SIZE_MAX )
                    {
                        LOG_FATAL("Two variants with no tag in Switch - " << ty);
                    }
                    default_target = i;
                }
                else
                {
                    // Get offset, read the value.
                    ::HIR::TypeRef  tag_ty;
                    size_t tag_ofs = ty.get_field_ofs(var.base_field, var.field_path, tag_ty);
                    // Read the value bytes
                    ::std::vector<char> tmp( var.tag_data.size() );
                    v.read_bytes(tag_ofs, const_cast<char*>(tmp.data()), tmp.size());
                    if( v.get_relocation(tag_ofs) )
                        continue ;
                    if( ::std::memcmp(tmp.data(), var.tag_data.data(), tmp.size()) == 0 )
                    {
                        LOG_DEBUG("Explicit match " << i);
                        found_target = i;
                        break ;
                    }
                }
            }

            if( found_target == SIZE_MAX )
            {
                LOG_DEBUG("Default match " << default_target);
                found_target = default_target;
            }
            if( found_target == SIZE_MAX )
            {
                LOG_FATAL("Terminator::Switch on " << ty << " didn't find a variant");
            }
            cur_frame.bb_idx = te.targets.at(found_target);
            } break;
        TU_ARM(bb.terminator, SwitchValue, te) {
            ::HIR::TypeRef ty;
            auto v = state.get_value_and_type(te.val, ty);
            TU_MATCH_HDRA( (te.values), {)
            TU_ARMA(Unsigned, vals) {
                LOG_ASSERT(vals.size() == te.targets.size(), "Mismatch in SwitchValue target/value list lengths");
                // Read an unsigned value 
                if( ty.get_wrapper() ) {
                    LOG_ERROR("Terminator::SwitchValue::Unsigned with wrapped type - " << ty);
                }
                uint64_t switch_val;
                switch(ty.inner_type)
                {
                case RawType::U8:   switch_val = v.read_u8(0); break;
                case RawType::U16:  switch_val = v.read_u16(0); break;
                case RawType::U32:  switch_val = v.read_u32(0); break;
                case RawType::U64:  switch_val = v.read_u64(0); break;
                case RawType::U128: LOG_TODO("Terminator::SwitchValue::Unsigned with u128");
                case RawType::USize:    switch_val = v.read_usize(0); break;
                case RawType::Char:  switch_val = v.read_u32(0); break;
                default:
                    LOG_ERROR("Terminator::SwitchValue::Unsigned with unexpected type - " << ty);
                }

                auto it = ::std::find(vals.begin(), vals.end(), switch_val);
                if( it != vals.end() )
                {
                    auto idx = it - vals.begin();
                    LOG_TRACE("- " << switch_val << " matched arm " << idx);
                    cur_frame.bb_idx = te.targets.at(idx);
                }
                else
                {
                    LOG_TRACE("- " << switch_val << " not matched, taking default arm");
                    cur_frame.bb_idx = te.def_target;
                }
                }
            TU_ARMA(Signed, vals) {
                if( ty.get_wrapper() ) {
                    LOG_ERROR("Terminator::SwitchValue::Signed with wrapped type - " << ty);
                }
                int64_t switch_val;
                switch(ty.inner_type)
                {
                case RawType::I8:   switch_val = v.read_i8(0); break;
                case RawType::I16:  switch_val = v.read_i16(0); break;
                case RawType::I32:  switch_val = v.read_i32(0); break;
                case RawType::I64:  switch_val = v.read_i64(0); break;
                case RawType::I128: LOG_TODO("Terminator::SwitchValue::Signed with i128");
                case RawType::ISize:    switch_val = v.read_isize(0); break;
                default:
                    LOG_ERROR("Terminator::SwitchValue::Signed with unexpected type - " << ty);
                }

                auto it = ::std::find(vals.begin(), vals.end(), switch_val);
                if( it != vals.end() )
                {
                    auto idx = it - vals.begin();
                    LOG_TRACE("- " << switch_val << " matched arm " << idx);
                    cur_frame.bb_idx = te.targets.at(idx);
                }
                else
                {
                    LOG_TRACE("- " << switch_val << " not matched, taking default arm");
                    cur_frame.bb_idx = te.def_target;
                }
                }
            TU_ARMA(String, vals) {
                auto size = v.read_usize(POINTER_SIZE);
                const char* sv_ptr = reinterpret_cast<const char*>(v.read_pointer_const(0, size));
                auto switch_val = ::stdx::string_view(sv_ptr, sv_ptr+size);

                auto it = ::std::find_if(vals.begin(), vals.end(), [&](const ::std::string& x){ return switch_val == x; });
                if( it != vals.end() )
                {
                    auto idx = it - vals.begin();
                    LOG_TRACE("- '" << switch_val << "' matched arm " << idx);
                    cur_frame.bb_idx = te.targets.at(idx);
                }
                else
                {
                    LOG_TRACE("- '" << switch_val << "' not matched, taking default arm");
                    cur_frame.bb_idx = te.def_target;
                }
                }
            }
            }
        TU_ARM(bb.terminator, Call, te) {
            ::std::vector<Value>    sub_args; sub_args.reserve(te.args.size());
            for(const auto& a : te.args)
            {
                sub_args.push_back( state.param_to_value(a) );
                LOG_DEBUG("#" << (sub_args.size() - 1) << " " << sub_args.back());
            }
            Value   rv;
            if( te.fcn.is_Intrinsic() )
            {
                const auto& fe = te.fcn.as_Intrinsic();
                if( !this->call_intrinsic(rv, fe.name, fe.params, ::std::move(sub_args)) )
                {
                    // Early return, don't want to update stmt_idx yet
                    return false;
                }
            }
            else
            {
                RelocationPtr   fcn_alloc_ptr;
                const ::HIR::Path* fcn_p;
                ::HIR::Path ffi_fcn_ptr;
                if( te.fcn.is_Path() ) {
                    fcn_p = &te.fcn.as_Path();
                }
                else {
                    ::HIR::TypeRef ty;
                    auto v = state.get_value_and_type(te.fcn.as_Value(), ty);
                    LOG_DEBUG("> Indirect call " << v);
                    // TODO: Assert type
                    // TODO: Assert offset/content.
                    LOG_ASSERT(v.read_usize(0) == Allocation::PTR_BASE, "Function pointer value invalid - " << v);
                    fcn_alloc_ptr = v.get_relocation(0);
                    LOG_ASSERT(fcn_alloc_ptr, "Calling value with no relocation - " << v);
                    switch(fcn_alloc_ptr.get_ty())
                    {
                    case RelocationPtr::Ty::Function:
                        fcn_p = &fcn_alloc_ptr.fcn();
                        break;
#if 0
                    case RelocationPtr::Ty::FfiPointer:
                        if( !fcn_alloc_ptr.ffi().layout )
                        {
                            // TODO: FFI function pointers
                            // - Call the function pointer using known argument rules
#ifdef _WIN32
                            if( fcn_alloc_ptr.ffi().ptr_value == AcquireSRWLockExclusive )
                            {
                                ffi_fcn_ptr = ::HIR::Path(::HIR::SimplePath { "#FFI", { "system", "AcquireSRWLockExclusive" } });
                                fcn_p = &ffi_fcn_ptr;
                                break;
                            }
                            else if( fcn_alloc_ptr.ffi().ptr_value == ReleaseSRWLockExclusive )
                            {
                                ffi_fcn_ptr = ::HIR::Path(::HIR::SimplePath { "#FFI", { "system", "ReleaseSRWLockExclusive" } });
                                fcn_p = &ffi_fcn_ptr;
                                break;
                            }
#endif
                        }
#endif
                    default:
                        LOG_ERROR("Calling value that isn't a function pointer - " << v);
                    }
                }

                LOG_DEBUG("Call " << *fcn_p);
                if( !this->call_path(rv, *fcn_p, ::std::move(sub_args)) )
                {
                    // Early return, don't want to update stmt_idx yet
                    LOG_DEBUG("- Non-immediate return, do not advance yet");
                    return false;
                }
            }
            // If a panic is in progress (in thread state), take the panic block instead
            if( m_thread.panic_active )
            {
                m_thread.panic_active = false;
                LOG_DEBUG("Panic into " << cur_frame.fcn->my_path);
                cur_frame.bb_idx = te.panic_block;
            }
            else
            {
                LOG_DEBUG(te.ret_val << " = " << rv << " (resume " << cur_frame.fcn->my_path << ")");
                state.write_lvalue(te.ret_val, rv);
                cur_frame.bb_idx = te.ret_block;
            }
            } break;
        }
        cur_frame.stmt_idx = 0;
    }

    return false;
}
bool InterpreterThread::pop_stack(Value& out_thread_result)
{
    assert( !this->m_stack.empty() );

    auto res_v = ::std::move(this->m_stack.back().ret);
    this->m_stack.pop_back();

    if( this->m_stack.empty() )
    {
        LOG_DEBUG("Thread complete, result " << res_v);
        out_thread_result = ::std::move(res_v);
        return true;
    }
    else
    {
        // Handle callback wrappers (e.g. for __rust_maybe_catch_panic, drop_value)
        if( this->m_stack.back().cb )
        {
            if( !this->m_stack.back().cb(res_v, ::std::move(res_v)) )
            {
                return false;
            }
            this->m_stack.pop_back();
            assert( !this->m_stack.empty() );
            assert( !this->m_stack.back().cb );
        }

        auto& cur_frame = this->m_stack.back();
        MirHelpers  state { *this, cur_frame };

        const auto& blk = cur_frame.fcn->m_mir.blocks.at( cur_frame.bb_idx );
        if( cur_frame.stmt_idx < blk.statements.size() )
        {
            assert( blk.statements[cur_frame.stmt_idx].is_Drop() );
            cur_frame.stmt_idx ++;
            LOG_DEBUG("DROP complete (resume " << cur_frame.fcn->my_path << ")");
        }
        else
        {
            assert( blk.terminator.is_Call() );
            const auto& te = blk.terminator.as_Call();

            LOG_DEBUG("Resume " << cur_frame.fcn->my_path);
            LOG_DEBUG("F" << cur_frame.frame_index << " " << te.ret_val << " = " << res_v);

            cur_frame.stmt_idx = 0;
            // If a panic is in progress (in thread state), take the panic block instead
            if( m_thread.panic_active )
            {
                m_thread.panic_active = false;
                LOG_DEBUG("Panic into " << cur_frame.fcn->my_path);
                cur_frame.bb_idx = te.panic_block;
            }
            else
            {
                state.write_lvalue(te.ret_val, res_v);
                cur_frame.bb_idx = te.ret_block;
            }
        }

        return false;
    }
}

unsigned InterpreterThread::StackFrame::s_next_frame_index = 0;
InterpreterThread::StackFrame::StackFrame(const Function& fcn, ::std::vector<Value> args):
    frame_index(s_next_frame_index++),
    fcn(&fcn),
    ret( fcn.ret_ty == RawType::Unreachable ? Value() : Value(fcn.ret_ty) ),
    args( ::std::move(args) ),
    locals( ),
    drop_flags( fcn.m_mir.drop_flags ),
    bb_idx(0),
    stmt_idx(0)
{
    LOG_DEBUG("F" << frame_index << " - Initializing " << fcn.m_mir.locals.size() << " locals");
    this->locals.reserve( fcn.m_mir.locals.size() );
    for(const auto& ty : fcn.m_mir.locals)
    {
        LOG_DEBUG("_" << (&ty - &fcn.m_mir.locals.front()) << ": " << ty);
        if( ty == RawType::Unreachable ) {
            // HACK: Locals can be !, but they can NEVER be accessed
            this->locals.push_back( Value() );
        }
        else {
            this->locals.push_back( Value(ty) );
        }
    }
}
bool InterpreterThread::call_path(Value& ret, const ::HIR::Path& path, ::std::vector<Value> args)
{
    // TODO: Support overriding certain functions
    {
        if( path == ::HIR::SimplePath { "std", { "sys", "imp", "c", "SetThreadStackGuarantee" } } 
         || path == ::HIR::SimplePath { "std", { "sys", "windows", "c", "SetThreadStackGuarantee" } }
         )
        {
            ret = Value::new_i32(120);  //ERROR_CALL_NOT_IMPLEMENTED
            return true;
        }
        // Win32 Shared RW locks (no-op)
        if( path == ::HIR::SimplePath { "std", { "sys", "windows", "c", "AcquireSRWLockExclusive" } }
         || path == ::HIR::SimplePath { "std", { "sys", "windows", "c", "ReleaseSRWLockExclusive" } }
            )
        {
            return true;
        }

        // - No guard page needed
        if( path == ::HIR::SimplePath { "std",  {"sys", "imp", "thread", "guard", "init" } }
         || path == ::HIR::SimplePath { "std",  {"sys", "unix", "thread", "guard", "init" } }
         )
        {
            ret = Value::with_size(16, false);
            ret.write_u64(0, 0);
            ret.write_u64(8, 0);
            return true;
        }

        // - No stack overflow handling needed
        if( path == ::HIR::SimplePath { "std", { "sys", "imp", "stack_overflow", "imp", "init" } } )
        {
            return true;
        }
    }

    if( path.m_name == "" && path.m_trait.m_simplepath.crate_name == "#FFI" )
    {
        const auto& link_abi  = path.m_trait.m_simplepath.ents.at(0);
        const auto& link_name = path.m_trait.m_simplepath.ents.at(1);
        return this->call_extern(ret, link_name, link_abi, ::std::move(args));
    }

    const auto& fcn = m_modtree.get_function(path);

    if( fcn.external.link_name != "" )
    {
        // TODO: Search for a function with both code and this link name
        if(const auto* ext_fcn = m_modtree.get_ext_function(fcn.external.link_name.c_str()))
        {
            this->m_stack.push_back(StackFrame(*ext_fcn, ::std::move(args)));
            return false;
        }
        else
        {
            // External function!
            return this->call_extern(ret, fcn.external.link_name, fcn.external.link_abi, ::std::move(args));
        }
    }

    this->m_stack.push_back(StackFrame(fcn, ::std::move(args)));
    return false;
}

#ifdef _WIN32
const char* memrchr(const void* p, int c, size_t s) {
    const char* p2 = reinterpret_cast<const char*>(p);
    while( s > 0 )
    {
        s -= 1;
        if( p2[s] == c )
            return &p2[s];
    }
    return nullptr;
}
#else
extern "C" {
    long sysconf(int);
    ssize_t write(int, const void*, size_t);
}
#endif
bool InterpreterThread::call_extern(Value& rv, const ::std::string& link_name, const ::std::string& abi, ::std::vector<Value> args)
{
    struct FfiHelpers {
        static const char* read_cstr(const Value& v, size_t ptr_ofs, size_t* out_strlen=nullptr)
        {
            bool _is_mut;
            size_t  size;
            // Get the base pointer and allocation size (checking for at least one valid byte to start with)
            const char* ptr = reinterpret_cast<const char*>( v.read_pointer_unsafe(0, 1, /*out->*/ size, _is_mut) );
            size_t len = 0;
            // Seek until either out of space, or a NUL is found
            while(size -- && *ptr)
            {
                ptr ++;
                len ++;
            }
            if( out_strlen )
            {
                *out_strlen = len;
            }
            return reinterpret_cast<const char*>(v.read_pointer_const(0, len + 1));  // Final read will trigger an error if the NUL isn't there
        }
    };
    if( link_name == "__rust_allocate" || link_name == "__rust_alloc" || link_name == "__rust_alloc_zeroed" )
    {
        static unsigned s_alloc_count = 0;

        auto alloc_idx = s_alloc_count ++;
        auto alloc_name = FMT_STRING("__rust_alloc#" << alloc_idx);
        auto size = args.at(0).read_usize(0);
        auto align = args.at(1).read_usize(0);
        LOG_DEBUG(link_name << "(size=" << size << ", align=" << align << "): name=" << alloc_name);

        // TODO: Use the alignment when making an allocation?
        auto alloc = Allocation::new_alloc(size, ::std::move(alloc_name));
        LOG_TRACE("- alloc=" << alloc << " (" << alloc->size() << " bytes)");
        auto rty = ::HIR::TypeRef(RawType::Unit).wrap( TypeWrapper::Ty::Pointer, 0 );

        if( link_name == "__rust_alloc_zeroed" )
        {
            alloc->mark_bytes_valid(0, size);
        }

        rv = Value::new_pointer(rty, Allocation::PTR_BASE, RelocationPtr::new_alloc(::std::move(alloc)));
    }
    else if( link_name == "__rust_reallocate" || link_name == "__rust_realloc" )
    {
        auto alloc_ptr = args.at(0).get_relocation(0);
        auto ptr_ofs = args.at(0).read_usize(0);
        auto oldsize = args.at(1).read_usize(0);
        // NOTE: The ordering here depends on the rust version (1.19 has: old, new, align - 1.29 has: old, align, new)
        auto align = args.at(true /*1.29*/ ? 2 : 3).read_usize(0);
        auto newsize = args.at(true /*1.29*/ ? 3 : 2).read_usize(0);
        LOG_DEBUG("__rust_reallocate(ptr=" << alloc_ptr << ", oldsize=" << oldsize << ", newsize=" << newsize << ", align=" << align << ")");
        LOG_ASSERT(ptr_ofs == Allocation::PTR_BASE, "__rust_reallocate with offset pointer");

        LOG_ASSERT(alloc_ptr, "__rust_reallocate with no backing allocation attached to pointer");
        LOG_ASSERT(alloc_ptr.is_alloc(), "__rust_reallocate with no backing allocation attached to pointer");
        auto& alloc = alloc_ptr.alloc();
        // TODO: Check old size and alignment against allocation.
        alloc.resize(newsize);
        // TODO: Should this instead make a new allocation to catch use-after-free?
        rv = ::std::move(args.at(0));
    }
    else if( link_name == "__rust_deallocate" || link_name == "__rust_dealloc" )
    {
        auto alloc_ptr = args.at(0).get_relocation(0);
        auto ptr_ofs = args.at(0).read_usize(0);
        LOG_ASSERT(ptr_ofs == Allocation::PTR_BASE, "__rust_deallocate with offset pointer");
        LOG_DEBUG("__rust_deallocate(ptr=" << alloc_ptr << ")");

        LOG_ASSERT(alloc_ptr, "__rust_deallocate with no backing allocation attached to pointer");
        LOG_ASSERT(alloc_ptr.is_alloc(), "__rust_deallocate with no backing allocation attached to pointer");
        auto& alloc = alloc_ptr.alloc();
        alloc.mark_as_freed();
        // Just let it drop.
        rv = Value();
    }
    else if( link_name == "__rust_maybe_catch_panic" )
    {
        auto fcn_path = args.at(0).get_relocation(0).fcn();
        auto arg = args.at(1);
        auto data_ptr = args.at(2).read_pointer_valref_mut(0, POINTER_SIZE);
        auto vtable_ptr = args.at(3).read_pointer_valref_mut(0, POINTER_SIZE);

        ::std::vector<Value>    sub_args;
        sub_args.push_back( ::std::move(arg) );

        this->m_stack.push_back(StackFrame::make_wrapper([=](Value& out_rv, Value /*rv*/)->bool{
            out_rv = Value::new_u32(0);
            return true;
            }));

        // TODO: Catch the panic out of this.
        if( this->call_path(rv, fcn_path, ::std::move(sub_args)) )
        {
            bool v = this->pop_stack(rv);
            assert( v == false );
            return true;
        }
        else
        {
            return false;
        }
    }
    else if( link_name == "panic_impl" )
    {
        LOG_TODO("panic_impl");
    }
    else if( link_name == "__rust_start_panic" )
    {
        LOG_TODO("__rust_start_panic");
    }
    else if( link_name == "rust_begin_unwind" )
    {
        LOG_TODO("rust_begin_unwind");
    }
    // libunwind
    else if( link_name == "_Unwind_RaiseException" )
    {
        LOG_DEBUG("_Unwind_RaiseException(" << args.at(0) << ")");
        // Save the first argument in TLS, then return a status that indicates unwinding should commence.
        m_thread.panic_active = true;
        m_thread.panic_count += 1;
        m_thread.panic_value = ::std::move(args.at(0));
    }
    else if( link_name == "_Unwind_DeleteException" )
    {
        LOG_DEBUG("_Unwind_DeleteException(" << args.at(0) << ")");
    }
#ifdef _WIN32
    // WinAPI functions used by libstd
    else if( link_name == "AddVectoredExceptionHandler" )
    {
        LOG_DEBUG("Call `AddVectoredExceptionHandler` - Ignoring and returning non-null");
        rv = Value::new_usize(1);
    }
    else if( link_name == "GetModuleHandleW" )
    {
        const auto& tgt_alloc = args.at(0).get_relocation(0);
        const void* arg0 = (tgt_alloc ? tgt_alloc.alloc().data_ptr() : nullptr);
        //extern void* GetModuleHandleW(const void* s);
        if(arg0) {
            LOG_DEBUG("GetModuleHandleW(" << tgt_alloc.alloc() << ")");
        }
        else {
            LOG_DEBUG("GetModuleHandleW(NULL)");
        }

        auto ret = GetModuleHandleW(static_cast<LPCWSTR>(arg0));
        if(ret)
        {
            rv = Value::new_ffiptr(FFIPointer::new_void("GetModuleHandleW", ret));
        }
        else
        {
            rv = Value(::HIR::TypeRef(RawType::USize));
            rv.create_allocation();
            rv.write_usize(0,0);
        }
    }
    else if( link_name == "GetProcAddress" )
    {
        const auto& handle_alloc = args.at(0).get_relocation(0);
        const auto& sym_alloc = args.at(1).get_relocation(0);

        // TODO: Ensure that first arg is a FFI pointer with offset+size of zero
        void* handle = handle_alloc.ffi().ptr_value;
        // TODO: Get either a FFI data pointer, or a inner data pointer
        const void* symname = sym_alloc.alloc().data_ptr();
        // TODO: Sanity check that it's a valid c string within its allocation
        LOG_DEBUG("FFI GetProcAddress(" << handle << ", \"" << static_cast<const char*>(symname) << "\")");

        auto ret = GetProcAddress(static_cast<HMODULE>(handle), static_cast<LPCSTR>(symname));

        if( ret )
        {
            // TODO: Get the functon name (and source library) and store in the result
            // - Maybe return a FFI function pointer (::"#FFI"::DllName+ProcName)
            rv = Value::new_ffiptr(FFIPointer::new_void("GetProcAddress", ret));
        }
        else
        {
            rv = Value(::HIR::TypeRef(RawType::USize));
            rv.create_allocation();
            rv.write_usize(0,0);
        }
    }
    // --- Thread-local storage
    else if( link_name == "TlsAlloc" )
    {
        auto key = ThreadState::s_next_tls_key ++;

        rv = Value::new_u32(key);
    }
    else if( link_name == "TlsGetValue" )
    {
        // LPVOID TlsGetValue( DWORD dwTlsIndex );
        auto key = args.at(0).read_u32(0);

        // Get a pointer-sized value from storage
        if( key < m_thread.tls_values.size() )
        {
            const auto& e = m_thread.tls_values[key];
            rv = Value::new_usize(e.first);
            if( e.second )
            {
                rv.set_reloc(0, POINTER_SIZE, e.second);
            }
        }
        else
        {
            // Return zero until populated
            rv = Value::new_usize(0);
        }
    }
    else if( link_name == "TlsSetValue" )
    {
        // BOOL TlsSetValue( DWORD  dwTlsIndex, LPVOID lpTlsValue );
        auto key = args.at(0).read_u32(0);
        auto v = args.at(1).read_usize(0);
        auto v_reloc = args.at(1).get_relocation(0);

        // Store a pointer-sized value in storage
        if( key >= m_thread.tls_values.size() ) {
            m_thread.tls_values.resize(key+1);
        }
        m_thread.tls_values[key] = ::std::make_pair(v, v_reloc);

        rv = Value::new_i32(1);
    }
    // ---
    else if( link_name == "InitializeCriticalSection" )
    {
        // HACK: Just ignore, no locks
    }
    else if( link_name == "EnterCriticalSection" )
    {
        // HACK: Just ignore, no locks
    }
    else if( link_name == "TryEnterCriticalSection" )
    {
        // HACK: Just ignore, no locks
        rv = Value::new_i32(1);
    }
    else if( link_name == "LeaveCriticalSection" )
    {
        // HACK: Just ignore, no locks
    }
    else if( link_name == "DeleteCriticalSection" )
    {
        // HACK: Just ignore, no locks
    }
    // ---
    else if( link_name == "GetStdHandle" )
    {
        // HANDLE WINAPI GetStdHandle( _In_ DWORD nStdHandle );
        auto val = args.at(0).read_u32(0);
        rv = Value::new_ffiptr(FFIPointer::new_void("HANDLE", GetStdHandle(val)));
    }
    else if( link_name == "GetConsoleMode" )
    {
        // BOOL WINAPI GetConsoleMode( _In_  HANDLE  hConsoleHandle, _Out_ LPDWORD lpMode );
        auto hConsoleHandle = args.at(0).read_pointer_tagged_nonnull(0, "HANDLE");
        auto lpMode_vr = args.at(1).read_pointer_valref_mut(0, sizeof(DWORD));
        LOG_DEBUG("GetConsoleMode(" << hConsoleHandle << ", " << lpMode_vr);
        auto lpMode = reinterpret_cast<LPDWORD>(lpMode_vr.data_ptr_mut());
        auto rv_bool = GetConsoleMode(hConsoleHandle, lpMode);
        if( rv_bool )
        {
            LOG_DEBUG("= TRUE (" << *lpMode << ")");
            lpMode_vr.mark_bytes_valid(0, sizeof(DWORD));
        }
        else
        {
            LOG_DEBUG("= FALSE");
        }
        rv = Value::new_i32(rv_bool ? 1 : 0);
    }
    else if( link_name == "WriteConsoleW" )
    {
        //BOOL WINAPI WriteConsole( _In_ HANDLE  hConsoleOutput, _In_ const VOID    *lpBuffer, _In_ DWORD   nNumberOfCharsToWrite,  _Out_ LPDWORD lpNumberOfCharsWritten, _Reserved_ LPVOID  lpReserved );
        auto hConsoleOutput = args.at(0).read_pointer_tagged_nonnull(0, "HANDLE");
        auto nNumberOfCharsToWrite = args.at(2).read_u32(0);
        auto lpBuffer = args.at(1).read_pointer_const(0, nNumberOfCharsToWrite * 2);
        auto lpNumberOfCharsWritten_vr = args.at(3).read_pointer_valref_mut(0, sizeof(DWORD));
        auto lpReserved = args.at(4).read_usize(0);
        LOG_DEBUG("WriteConsoleW(" << hConsoleOutput << ", " << lpBuffer << ", " << nNumberOfCharsToWrite << ", " << lpNumberOfCharsWritten_vr << ")");

        auto lpNumberOfCharsWritten = reinterpret_cast<LPDWORD>(lpNumberOfCharsWritten_vr.data_ptr_mut());

        LOG_ASSERT(lpReserved == 0, "");
        auto rv_bool = WriteConsoleW(hConsoleOutput, lpBuffer, nNumberOfCharsToWrite, lpNumberOfCharsWritten, nullptr);
        if( rv_bool )
        {
            LOG_DEBUG("= TRUE (" << *lpNumberOfCharsWritten << ")");
        }
        else
        {
            LOG_DEBUG("= FALSE");
        }
        rv = Value::new_i32(rv_bool ? 1 : 0);
    }
#else
    // POSIX
    else if( link_name == "write" )
    {
        auto fd = args.at(0).read_i32(0);
        auto count = args.at(2).read_isize(0);
        const auto* buf = args.at(1).read_pointer_const(0, count);

        ssize_t val = write(fd, buf, count);

        rv = Value::new_isize(val);
    }
    else if( link_name == "read" )
    {
        auto fd = args.at(0).read_i32(0);
        auto count = args.at(2).read_isize(0);
        auto buf_vr = args.at(1).read_pointer_valref_mut(0, count);

        LOG_DEBUG("read(" << fd << ", " << buf_vr.data_ptr_mut() << ", " << count << ")");
        ssize_t val = read(fd, buf_vr.data_ptr_mut(), count);
        LOG_DEBUG("= " << val);

        if( val > 0 )
        {
            buf_vr.mark_bytes_valid(0, val);
        }

        rv = Value::new_isize(val);
    }
    else if( link_name == "close" )
    {
        auto fd = args.at(0).read_i32(0);
        LOG_DEBUG("close(" << fd << ")");
        // TODO: Ensure that this FD is from the set known by the FFI layer
        close(fd);
    }
    else if( link_name == "isatty" )
    {
        auto fd = args.at(0).read_i32(0);
        LOG_DEBUG("isatty(" << fd << ")");
        int rv_i = isatty(fd);
        LOG_DEBUG("= " << rv_i);
        rv = Value::new_i32(rv_i);
    }
    else if( link_name == "fcntl" )
    {
        // `fcntl` has custom handling for the third argument, as some are pointers
        int fd = args.at(0).read_i32(0);
        int command = args.at(1).read_i32(0);

        int rv_i;
        const char* name;
        switch(command)
        {
        // - No argument
        case F_GETFD: name = "F_GETFD"; if(0)
            ;
            {
                LOG_DEBUG("fcntl(" << fd << ", " << name << ")");
                rv_i = fcntl(fd, command);
            } break;
        // - Integer arguments
        case F_DUPFD: name = "F_DUPFD"; if(0)
        case F_DUPFD_CLOEXEC: name = "F_DUPFD_CLOEXEC"; if(0)
        case F_SETFD: name = "F_SETFD"; if(0)
            ;
            {
                int arg = args.at(2).read_i32(0);
                LOG_DEBUG("fcntl(" << fd << ", " << name << ", " << arg << ")");
                rv_i = fcntl(fd, command, arg);
            } break;
        default:
            if( args.size() > 2 )
            {
                LOG_TODO("fnctl(..., " << command << ", " << args[2] << ")");
            }
            else
            {
                LOG_TODO("fnctl(..., " << command << ")");
            }
        }

        LOG_DEBUG("= " << rv_i);
        rv = Value(::HIR::TypeRef(RawType::I32));
        rv.write_i32(0, rv_i);
    }
    else if( link_name == "prctl" )
    {
        auto option = args.at(0).read_i32(0);
        int rv_i;
        switch(option)
        {
        case 15: {   // PR_SET_NAME - set thread name
            auto name = FfiHelpers::read_cstr(args.at(1), 0);
            LOG_DEBUG("prctl(PR_SET_NAME, \"" << name << "\"");
            rv_i = 0;
            } break;
        default:
            LOG_TODO("prctl(" << option << ", ...");
        }
        rv = Value::new_i32(rv_i);
    }
    else if( link_name == "sysconf" )
    {
        auto name = args.at(0).read_i32(0);
        LOG_DEBUG("FFI sysconf(" << name << ")");

        long val = sysconf(name);

        rv = Value::new_usize(val);
    }
    else if( link_name == "pthread_self" )
    {
        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_mutex_init" || link_name == "pthread_mutex_lock" || link_name == "pthread_mutex_unlock" || link_name == "pthread_mutex_destroy" )
    {
        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_rwlock_rdlock" )
    {
        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_rwlock_unlock" )
    {
        // TODO: Check that this thread holds the lock?
        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_mutexattr_init" || link_name == "pthread_mutexattr_settype" || link_name == "pthread_mutexattr_destroy" )
    {
        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_condattr_init" || link_name == "pthread_condattr_destroy" || link_name == "pthread_condattr_setclock" )
    {
        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_attr_init" || link_name == "pthread_attr_destroy" || link_name == "pthread_getattr_np" )
    {
        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_attr_setstacksize" )
    {
        // Lie and return succeess
        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_attr_getguardsize" )
    {
        const auto attr_p = args.at(0).read_pointer_const(0, 1);
        auto out_size = args.at(1).deref(0, HIR::TypeRef(RawType::USize));

        out_size.m_alloc.alloc().write_usize(out_size.m_offset, 0x1000);

        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_attr_getstack" )
    {
        const auto attr_p = args.at(0).read_pointer_const(0, 1);
        auto out_ptr = args.at(2).deref(0, HIR::TypeRef(RawType::USize));
        auto out_size = args.at(2).deref(0, HIR::TypeRef(RawType::USize));

        out_size.m_alloc.alloc().write_usize(out_size.m_offset, 0x4000);

        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_create" )
    {
        auto thread_handle_out = args.at(0).read_pointer_valref_mut(0, sizeof(pthread_t));
        auto attrs = args.at(1).read_pointer_const(0, sizeof(pthread_attr_t));
        auto fcn_path = args.at(2).get_relocation(0).fcn();
        LOG_ASSERT(args.at(2).read_usize(0) == Allocation::PTR_BASE, "");
        auto arg = args.at(3);
        LOG_NOTICE("TODO: pthread_create(" << thread_handle_out << ", " << attrs << ", " << fcn_path << ", " << arg << ")");
        // TODO: Create a new interpreter context with this thread, use co-operative scheduling
        // HACK: Just run inline
        if( true )
        {
            auto tls = ::std::move(m_thread.tls_values);
            this->m_stack.push_back(StackFrame::make_wrapper([=](Value& out_rv, Value /*rv*/)mutable ->bool {
                out_rv = Value::new_i32(0);
                m_thread.tls_values = ::std::move(tls);
                return true;
                }));

            // TODO: Catch the panic out of this.
            if( this->call_path(rv, fcn_path, { ::std::move(arg) }) )
            {
                bool v = this->pop_stack(rv);
                assert( v == false );
                return true;
            }
            else
            {
                return false;
            }
        }
        else {
            //this->m_parent.create_thread(fcn_path, arg);
            rv = Value::new_i32(EPERM);
        }
    }
    else if( link_name == "pthread_detach" )
    {
        // "detach" - Prevent the need to explitly join a thread
        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_cond_init" || link_name == "pthread_cond_destroy" )
    {
        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_key_create" )
    {
        auto key_ref = args.at(0).read_pointer_valref_mut(0, 4);

        auto key = ThreadState::s_next_tls_key ++;
        key_ref.m_alloc.alloc().write_u32( key_ref.m_offset, key );

        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_getspecific" )
    {
        auto key = args.at(0).read_u32(0);

        // Get a pointer-sized value from storage
        if( key < m_thread.tls_values.size() )
        {
            const auto& e = m_thread.tls_values[key];
            rv = Value::new_usize(e.first);
            if( e.second )
            {
                rv.set_reloc(0, POINTER_SIZE, e.second);
            }
        }
        else
        {
            // Return zero until populated
            rv = Value::new_usize(0);
        }
    }
    else if( link_name == "pthread_setspecific" )
    {
        auto key = args.at(0).read_u32(0);
        auto v = args.at(1).read_u64(0);
        auto v_reloc = args.at(1).get_relocation(0);

        // Store a pointer-sized value in storage
        if( key >= m_thread.tls_values.size() ) {
            m_thread.tls_values.resize(key+1);
        }
        m_thread.tls_values[key] = ::std::make_pair(v, v_reloc);

        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_key_delete" )
    {
        rv = Value::new_i32(0);
    }
    // - Time
    else if( link_name == "clock_gettime" )
    {
        // int clock_gettime(clockid_t clk_id, struct timespec *tp);
        auto clk_id = args.at(0).read_u32(0);
        auto tp_vr = args.at(1).read_pointer_valref_mut(0, sizeof(struct timespec));

        LOG_DEBUG("clock_gettime(" << clk_id << ", " << tp_vr);
        int rv_i = clock_gettime(clk_id, reinterpret_cast<struct timespec*>(tp_vr.data_ptr_mut()));
        if(rv_i == 0)
            tp_vr.mark_bytes_valid(0, tp_vr.m_size);
        LOG_DEBUG("= " << rv_i << " (" << tp_vr << ")");
        rv = Value::new_i32(rv_i);
    }
    // - Linux extensions
    else if( link_name == "open64" )
    {
        const auto* path = FfiHelpers::read_cstr(args.at(0), 0);
        auto flags = args.at(1).read_i32(0);
        auto mode = (args.size() > 2 ? args.at(2).read_i32(0) : 0);

        LOG_DEBUG("open64(\"" << path << "\", " << flags << ")");
        int rv_i = open(path, flags, mode);
        LOG_DEBUG("= " << rv_i);

        rv = Value(::HIR::TypeRef(RawType::I32));
        rv.write_i32(0, rv_i);
    }
    else if( link_name == "stat64" )
    {
        const auto* path = FfiHelpers::read_cstr(args.at(0), 0);
        auto outbuf_vr = args.at(1).read_pointer_valref_mut(0, sizeof(struct stat));

        LOG_DEBUG("stat64(\"" << path << "\", " << outbuf_vr << ")");
        int rv_i = stat(path, reinterpret_cast<struct stat*>(outbuf_vr.data_ptr_mut()));
        LOG_DEBUG("= " << rv_i);

        if( rv_i == 0 )
        {
            // TODO: Mark the buffer as valid?
        }

        rv = Value(::HIR::TypeRef(RawType::I32));
        rv.write_i32(0, rv_i);
    }
    else if( link_name == "__errno_location" )
    {
        rv = Value::new_ffiptr(FFIPointer::new_const_bytes("errno", &errno, sizeof(errno)));
    }
    else if( link_name == "syscall" )
    {
        auto num = args.at(0).read_u32(0);

        LOG_DEBUG("syscall(" << num << ", ...) - hack return ENOSYS");
        errno = ENOSYS;
        rv = Value::new_i64(-1);
    }
    else if( link_name == "dlsym" )
    {
        auto handle = args.at(0).read_usize(0);
        const char* name = FfiHelpers::read_cstr(args.at(1), 0);

        LOG_DEBUG("dlsym(0x" << ::std::hex << handle << ", '" << name << "')");
        LOG_NOTICE("dlsym stubbed to zero");
        rv = Value::new_usize(0);
    }
#endif
    // std C
    else if( link_name == "signal" )
    {
        LOG_DEBUG("Call `signal` - Ignoring and returning SIG_IGN");
        rv = Value(::HIR::TypeRef(RawType::USize));
        rv.write_usize(0, 1);
    }
    else if( link_name == "sigaction" )
    {
        rv = Value::new_i32(-1);
    }
    else if( link_name == "sigaltstack" )   // POSIX: Set alternate signal stack
    {
        rv = Value::new_i32(-1);
    }
    else if( link_name == "memcmp" )
    {
        auto n = args.at(2).read_usize(0);
        int rv_i;
        if( n > 0 )
        {
            const void* ptr_b = args.at(1).read_pointer_const(0, n);
            const void* ptr_a = args.at(0).read_pointer_const(0, n);

            rv_i = memcmp(ptr_a, ptr_b, n);
        }
        else
        {
            rv_i = 0;
        }
        rv = Value::new_i32(rv_i);
    }
    // - `void *memchr(const void *s, int c, size_t n);`
    else if( link_name == "memchr" )
    {
        auto ptr_alloc = args.at(0).get_relocation(0);
        auto c = args.at(1).read_i32(0);
        auto n = args.at(2).read_usize(0);
        const void* ptr = args.at(0).read_pointer_const(0, n);

        const void* ret = memchr(ptr, c, n);

        rv = Value(::HIR::TypeRef(RawType::USize));
        if( ret )
        {
            auto rv_ofs = args.at(0).read_usize(0) + ( static_cast<const uint8_t*>(ret) - static_cast<const uint8_t*>(ptr) );
            rv.write_ptr(0, rv_ofs, ptr_alloc);
        }
        else
        {
            rv.write_usize(0, 0);
        }
    }
    else if( link_name == "memrchr" )
    {
        auto ptr_alloc = args.at(0).get_relocation(0);
        auto c = args.at(1).read_i32(0);
        auto n = args.at(2).read_usize(0);
        const void* ptr = args.at(0).read_pointer_const(0, n);

        const void* ret = memrchr(ptr, c, n);

        rv = Value(::HIR::TypeRef(RawType::USize));
        if( ret )
        {
            auto rv_ofs = args.at(0).read_usize(0) + ( static_cast<const uint8_t*>(ret) - static_cast<const uint8_t*>(ptr) );
            rv.write_ptr(0, rv_ofs, ptr_alloc);
        }
        else
        {
            rv.write_usize(0, 0);
        }
    }
    else if( link_name == "strlen" )
    {
        // strlen - custom implementation to ensure validity
        size_t len = 0;
        FfiHelpers::read_cstr(args.at(0), 0, &len);

        //rv = Value::new_usize(len);
        rv = Value(::HIR::TypeRef(RawType::USize));
        rv.write_usize(0, len);
    }
    else if( link_name == "getenv" )
    {
        const auto* name = FfiHelpers::read_cstr(args.at(0), 0);
        LOG_DEBUG("getenv(\"" << name << "\")");
        const auto* ret_ptr = getenv(name);
        if( ret_ptr )
        {
            LOG_DEBUG("= \"" << ret_ptr << "\"");
            rv = Value::new_ffiptr(FFIPointer::new_const_bytes("getenv", ret_ptr, strlen(ret_ptr)+1));
        }
        else
        {
            LOG_DEBUG("= NULL");
            rv = Value(::HIR::TypeRef(RawType::USize));
            rv.create_allocation();
            rv.write_usize(0,0);
        }
    }
    else if( link_name == "setenv" )
    {
        LOG_TODO("Allow `setenv` without incurring thread unsafety");
    }
    // Allocators!
    else
    {
        LOG_TODO("Call external function " << link_name);
    }
    return true;
}

bool InterpreterThread::call_intrinsic(Value& rv, const RcString& name, const ::HIR::PathParams& ty_params, ::std::vector<Value> args)
{
    TRACE_FUNCTION_R(name, rv);
    for(const auto& a : args)
        LOG_DEBUG("#" << (&a - args.data()) << ": " << a);
    if( name == "type_id" )
    {
        const auto& ty_T = ty_params.tys.at(0);
        static ::std::vector<HIR::TypeRef>  type_ids;
        auto it = ::std::find(type_ids.begin(), type_ids.end(), ty_T);
        if( it == type_ids.end() )
        {
            it = type_ids.insert(it, ty_T);
        }

        rv = Value::with_size(POINTER_SIZE, false);
        rv.write_usize(0, it - type_ids.begin());
    }
    else if( name == "type_name" )
    {
        const auto& ty_T = ty_params.tys.at(0);

        static ::std::map<HIR::TypeRef, ::std::string>  s_type_names;
        auto it = s_type_names.find(ty_T);
        if( it == s_type_names.end() )
        {
            it = s_type_names.insert( ::std::make_pair(ty_T, FMT_STRING(ty_T)) ).first;
        }

        rv = Value::with_size(2*POINTER_SIZE, /*needs_alloc=*/true);
        rv.write_ptr(0*POINTER_SIZE, Allocation::PTR_BASE, RelocationPtr::new_string(&it->second));
        rv.write_usize(1*POINTER_SIZE, 0);
    }
    else if( name == "discriminant_value" )
    {
        const auto& ty = ty_params.tys.at(0);
        ValueRef val = args.at(0).deref(0, ty);

        size_t fallback = SIZE_MAX;
        size_t found_index = SIZE_MAX;
        LOG_ASSERT(ty.inner_type == RawType::Composite, "discriminant_value " << ty);
        for(size_t i = 0; i < ty.composite_type().variants.size(); i ++)
        {
            const auto& var = ty.composite_type().variants[i];
            if( var.tag_data.size() == 0 )
            {
                // Only seen in Option<NonNull>
                assert(fallback == SIZE_MAX);
                fallback = i;
            }
            else
            {
                // Get offset to the tag
                ::HIR::TypeRef  tag_ty;
                size_t tag_ofs = ty.get_field_ofs(var.base_field, var.field_path, tag_ty);
                // Compare
                if( val.compare(tag_ofs, var.tag_data.data(), var.tag_data.size()) == 0 )
                {
                    found_index = i;
                    break ;
                }
            }
        }

        if( found_index == SIZE_MAX )
        {
            LOG_ASSERT(fallback != SIZE_MAX, "Can't find variant of " << ty << " for " << val);
            found_index = fallback;
        }

        rv = Value::new_usize(found_index);
    }
    else if( name == "atomic_fence" || name == "atomic_fence_acq" )
    {
        rv = Value();
    }
    else if( name == "atomic_store" || name == "atomic_store_relaxed" || name == "atomic_store_rel" )
    {
        auto& ptr_val = args.at(0);
        auto& data_val = args.at(1);

        LOG_ASSERT(ptr_val.size() == POINTER_SIZE, "atomic_store of a value that isn't a pointer-sized value");

        // There MUST be a relocation at this point with a valid allocation.
        auto alloc = ptr_val.get_relocation(0);
        LOG_ASSERT(alloc, "Deref of a value with no relocation");

        // TODO: Atomic side of this?
        size_t ofs = ptr_val.read_usize(0) - Allocation::PTR_BASE;
        alloc.alloc().write_value(ofs, ::std::move(data_val));
    }
    else if( name == "atomic_load" || name == "atomic_load_relaxed" || name == "atomic_load_acq" )
    {
        auto& ptr_val = args.at(0);
        LOG_ASSERT(ptr_val.size() == POINTER_SIZE, "atomic_load of a value that isn't a pointer-sized value");

        // There MUST be a relocation at this point with a valid allocation.
        auto alloc = ptr_val.get_relocation(0);
        LOG_ASSERT(alloc, "Deref of a value with no relocation");
        // TODO: Atomic lock the allocation.

        size_t ofs = ptr_val.read_usize(0) - Allocation::PTR_BASE;
        const auto& ty = ty_params.tys.at(0);

        rv = alloc.alloc().read_value(ofs, ty.get_size());
    }
    else if( name == "atomic_xadd" || name == "atomic_xadd_relaxed" )
    {
        const auto& ty_T = ty_params.tys.at(0);
        auto ptr_ofs = args.at(0).read_usize(0) - Allocation::PTR_BASE;
        auto ptr_alloc = args.at(0).get_relocation(0);
        auto v = args.at(1).read_value(0, ty_T.get_size());

        // TODO: Atomic lock the allocation.
        if( !ptr_alloc || !ptr_alloc.is_alloc() ) {
            LOG_ERROR("atomic pointer has no allocation");
        }

        // - Result is the original value
        rv = ptr_alloc.alloc().read_value(ptr_ofs, ty_T.get_size());

        auto val_l = PrimitiveValueVirt::from_value(ty_T, rv);
        const auto val_r = PrimitiveValueVirt::from_value(ty_T, v);
        val_l.get().add( val_r.get() );

        val_l.get().write_to_value( ptr_alloc.alloc(), ptr_ofs );
    }
    else if( name == "atomic_xsub" || name == "atomic_xsub_relaxed" || name == "atomic_xsub_rel" )
    {
        const auto& ty_T = ty_params.tys.at(0);
        auto ptr_ofs = args.at(0).read_usize(0) - Allocation::PTR_BASE;
        auto ptr_alloc = args.at(0).get_relocation(0);
        auto v = args.at(1).read_value(0, ty_T.get_size());

        // TODO: Atomic lock the allocation.
        if( !ptr_alloc || !ptr_alloc.is_alloc() ) {
            LOG_ERROR("atomic pointer has no allocation");
        }

        // - Result is the original value
        rv = ptr_alloc.alloc().read_value(ptr_ofs, ty_T.get_size());

        auto val_l = PrimitiveValueVirt::from_value(ty_T, rv);
        const auto val_r = PrimitiveValueVirt::from_value(ty_T, v);
        val_l.get().subtract( val_r.get() );

        val_l.get().write_to_value( ptr_alloc.alloc(), ptr_ofs );
    }
    else if( name == "atomic_xchg" || name == "atomic_xchg_acqrel" )
    {
        const auto& ty_T = ty_params.tys.at(0);
        auto data_ref = args.at(0).read_pointer_valref_mut(0, ty_T.get_size());
        const auto& new_v = args.at(1);

        rv = data_ref.read_value(0, new_v.size());
        data_ref.m_alloc.alloc().write_value( data_ref.m_offset, new_v );
    }
    else if( name == "atomic_cxchg" )
    {
        const auto& ty_T = ty_params.tys.at(0);
        // TODO: Get a ValueRef to the target location
        auto data_ref = args.at(0).read_pointer_valref_mut(0, ty_T.get_size());
        const auto& old_v = args.at(1);
        const auto& new_v = args.at(2);
        rv = Value::with_size( ty_T.get_size() + 1, false );
        rv.write_value(0, data_ref.read_value(0, old_v.size()));
        LOG_DEBUG("> *ptr = " << data_ref);
        if( data_ref.compare(0, old_v.data_ptr(), old_v.size()) == true ) {
            data_ref.m_alloc.alloc().write_value( data_ref.m_offset, new_v );
            rv.write_u8( old_v.size(), 1 );
        }
        else {
            rv.write_u8( old_v.size(), 0 );
        }
    }
    else if( name == "transmute" )
    {
        // Transmute requires the same size, so just copying the value works
        rv = ::std::move(args.at(0));
    }
    else if( name == "assume" )
    {
        // Assume is a no-op which returns unit
    }
    else if( name == "offset" )
    {
        auto ptr_alloc = args.at(0).get_relocation(0);
        auto ptr_ofs = args.at(0).read_usize(0);
        LOG_ASSERT(ptr_ofs >= Allocation::PTR_BASE, "`offset` with invalid pointer - " << args.at(0));
        auto& ofs_val = args.at(1);

        auto delta_counts = ofs_val.read_usize(0);
        auto ty_size = ty_params.tys.at(0).get_size();
        LOG_DEBUG("\"offset\": 0x" << ::std::hex << ptr_ofs << " + 0x" << delta_counts << " * 0x" << ty_size);
        ptr_ofs -= Allocation::PTR_BASE;
        auto new_ofs = ptr_ofs + delta_counts * ty_size;
        if(POINTER_SIZE != 8) {
            new_ofs &= 0xFFFFFFFF;
        }

        rv = ::std::move(args.at(0));
        rv.write_ptr(0, Allocation::PTR_BASE + new_ofs, ptr_alloc);
    }
    else if( name == "arith_offset" )   // Doesn't check validity, and allows wrapping
    {
        auto ptr_alloc = args.at(0).get_relocation(0);
        auto ptr_ofs = args.at(0).read_usize(0);
        //LOG_ASSERT(ptr_ofs >= Allocation::PTR_BASE, "`offset` with invalid pointer - " << args.at(0));
        //ptr_ofs -= Allocation::PTR_BASE;
        auto& ofs_val = args.at(1);

        auto delta_counts = ofs_val.read_usize(0);
        auto new_ofs = ptr_ofs + delta_counts * ty_params.tys.at(0).get_size();
        if(POINTER_SIZE != 8) {
            new_ofs &= 0xFFFFFFFF;
        }
        //new_ofs += Allocation::PTR_BASE;

        rv = ::std::move(args.at(0));
        if( ptr_alloc )
        {
            rv.write_ptr(0, new_ofs, ptr_alloc);
        }
        else
        {
            rv.write_usize(0, new_ofs);
        }
    }
    // effectively ptr::write
    else if( name == "move_val_init" )
    {
        auto& ptr_val = args.at(0);
        auto& data_val = args.at(1);

        // There MUST be a relocation at this point with a valid allocation.
        // - TODO: What about FFI? (can't be a string or function though)
        auto dst_vr = ptr_val.deref(0, ty_params.tys.at(0));
        LOG_ASSERT(dst_vr.m_alloc, "Deref didn't yeild an allocation (error?)");
        LOG_ASSERT(dst_vr.m_alloc.is_alloc(), "Deref didn't yield an allocation");

        dst_vr.m_alloc.alloc().write_value(dst_vr.m_offset, ::std::move(data_val));
    }
    else if( name == "uninit" )
    {
        rv = Value(ty_params.tys.at(0));
    }
    else if( name == "init" )
    {
        rv = Value(ty_params.tys.at(0));
        rv.mark_bytes_valid(0, rv.size());
    }
    else if( name == "write_bytes" )
    {
        auto& dst_ptr_v = args.at(0);
        auto byte = args.at(1).read_u8(0);
        auto count = args.at(2).read_usize(0);
        auto bytes = count * ty_params.tys.at(0).get_size();

        LOG_DEBUG("'write_bytes'(" << dst_ptr_v << ", " << (int)byte << ", " << count << "): bytes=" << bytes);

        if( count > 0 )
        {
            auto dst_vr = dst_ptr_v.read_pointer_valref_mut(0, bytes);
            memset(dst_vr.data_ptr_mut(), byte, bytes);
            dst_vr.mark_bytes_valid(0, bytes);
        }
    }
    // - Unsized stuff
    else if( name == "size_of_val" )
    {
        auto& val = args.at(0);
        const auto& ty = ty_params.tys.at(0);
        rv = Value(::HIR::TypeRef(RawType::USize));
        // Get unsized type somehow.
        // - _HAS_ to be the last type, so that makes it easier
        size_t fixed_size = 0;
        if( const auto* ity = ty.get_unsized_type(fixed_size) )
        {
            const auto meta_ty = ty.get_meta_type();
            LOG_DEBUG("size_of_val - " << ty << " ity=" << *ity << " meta_ty=" << meta_ty << " fixed_size=" << fixed_size);
            size_t flex_size = 0;
            if( const auto* w = ity->get_wrapper() )
            {
                LOG_ASSERT(w->type == TypeWrapper::Ty::Slice, "size_of_val on wrapped type that isn't a slice - " << *ity);
                size_t item_size = ity->get_inner().get_size();
                size_t item_count = val.read_usize(POINTER_SIZE);
                flex_size = item_count * item_size;
                LOG_DEBUG("> item_size=" << item_size << " item_count=" << item_count << " flex_size=" << flex_size);
            }
            else if( ity->inner_type == RawType::Str )
            {
                flex_size = val.read_usize(POINTER_SIZE);
            }
            else if( ity->inner_type == RawType::TraitObject )
            {
                auto vtable_ty = meta_ty.get_inner();
                LOG_DEBUG("> vtable_ty = " << vtable_ty << " (size= " << vtable_ty.get_size() << ")");
                auto vtable = val.deref(POINTER_SIZE, vtable_ty);
                LOG_DEBUG("> vtable = " << vtable);
                auto size = vtable.read_usize(1*POINTER_SIZE);
                flex_size = size;
            }
            else
            {
                LOG_BUG("Inner unsized type unknown - " << *ity);
            }

            rv.write_usize(0, fixed_size + flex_size);
        }
        else
        {
            rv.write_usize(0, ty.get_size());
        }
    }
    else if( name == "min_align_of_val" )
    {
        /*const*/ auto& val = args.at(0);
        const auto& ty = ty_params.tys.at(0);
        rv = Value(::HIR::TypeRef(RawType::USize));
        size_t fixed_size = 0;  // unused
        size_t flex_align = 0;
        if( const auto* ity = ty.get_unsized_type(fixed_size) )
        {
            if( const auto* w = ity->get_wrapper() )
            {
                LOG_ASSERT(w->type == TypeWrapper::Ty::Slice, "align_of_val on wrapped type that isn't a slice - " << *ity);
                flex_align = ity->get_inner().get_align();
            }
            else if( ity->inner_type == RawType::Str )
            {
                flex_align = 1;
            }
            else if( ity->inner_type == RawType::TraitObject )
            {
                const auto meta_ty = ty.get_meta_type();
                auto vtable_ty = meta_ty.get_inner();
                LOG_DEBUG("> vtable_ty = " << vtable_ty << " (size= " << vtable_ty.get_size() << ")");
                auto vtable = val.deref(POINTER_SIZE, vtable_ty);
                LOG_DEBUG("> vtable = " << vtable);
                flex_align = vtable.read_usize(2*POINTER_SIZE);
            }
            else
            {
                LOG_BUG("Inner unsized type unknown - " << *ity);
            }
        }
        rv.write_usize(0, ::std::max( ty.get_align(), flex_align ));
    }
    else if( name == "drop_in_place" )
    {
        auto& val = args.at(0);
        const auto& ty = ty_params.tys.at(0);
        return drop_value(val, ty);
    }
    else if( name == "try" )
    {
        auto fcn_path = args.at(0).get_relocation(0).fcn();
        auto arg = args.at(1);
        auto out_panic_value = args.at(2).read_pointer_valref_mut(0, POINTER_SIZE);

        ::std::vector<Value>    sub_args;
        sub_args.push_back( ::std::move(arg) );

        this->m_stack.push_back(StackFrame::make_wrapper([=](Value& out_rv, Value /*rv*/)mutable->bool{
            if( m_thread.panic_active )
            {
                assert(m_thread.panic_count > 0);
                m_thread.panic_active = false;
                m_thread.panic_count --;
                LOG_ASSERT(m_thread.panic_value.size() == out_panic_value.m_size, "Panic value " << m_thread.panic_value << " doesn't fit in " << out_panic_value);
                out_panic_value.m_alloc.alloc().write_value( out_panic_value.m_offset, ::std::move(m_thread.panic_value) );
                out_rv = Value::new_u32(1);
                return true;
            }
            else
            {
                LOG_ASSERT(m_thread.panic_count == 0, "Panic count non-zero, but previous function returned non-panic");
                out_rv = Value::new_u32(0);
                return true;
            }
            }));

        // TODO: Catch the panic out of this.
        if( this->call_path(rv, fcn_path, ::std::move(sub_args)) )
        {
            bool v = this->pop_stack(rv);
            assert( v == false );
            return true;
        }
        else
        {
            return false;
        }
    }
    // ----------------------------------------------------------------
    // Checked arithmatic
    else if( name == "add_with_overflow" )
    {
        const auto& ty = ty_params.tys.at(0);

        auto lhs = PrimitiveValueVirt::from_value(ty, args.at(0));
        auto rhs = PrimitiveValueVirt::from_value(ty, args.at(1));
        bool didnt_overflow = lhs.get().add( rhs.get() );

        // Get return type - a tuple of `(T, bool,)`
        ::HIR::GenericPath  gp;
        gp.m_params.tys.push_back(ty);
        gp.m_params.tys.push_back(::HIR::TypeRef { RawType::Bool });
        const auto& dty = m_modtree.get_composite(gp);

        rv = Value(::HIR::TypeRef(&dty));
        lhs.get().write_to_value(rv, dty.fields[0].first);
        rv.write_u8( dty.fields[1].first, didnt_overflow ? 0 : 1 ); // Returns true if overflow happened
    }
    else if( name == "sub_with_overflow" )
    {
        const auto& ty = ty_params.tys.at(0);

        auto lhs = PrimitiveValueVirt::from_value(ty, args.at(0));
        auto rhs = PrimitiveValueVirt::from_value(ty, args.at(1));
        bool didnt_overflow = lhs.get().subtract( rhs.get() );

        // Get return type - a tuple of `(T, bool,)`
        ::HIR::GenericPath  gp;
        gp.m_params.tys.push_back(ty);
        gp.m_params.tys.push_back(::HIR::TypeRef { RawType::Bool });
        const auto& dty = m_modtree.get_composite(gp);

        rv = Value(::HIR::TypeRef(&dty));
        lhs.get().write_to_value(rv, dty.fields[0].first);
        rv.write_u8( dty.fields[1].first, didnt_overflow ? 0 : 1 ); // Returns true if overflow happened
    }
    else if( name == "mul_with_overflow" )
    {
        const auto& ty = ty_params.tys.at(0);

        auto lhs = PrimitiveValueVirt::from_value(ty, args.at(0));
        auto rhs = PrimitiveValueVirt::from_value(ty, args.at(1));
        bool didnt_overflow = lhs.get().multiply( rhs.get() );

        // Get return type - a tuple of `(T, bool,)`
        ::HIR::GenericPath  gp;
        gp.m_params.tys.push_back(ty);
        gp.m_params.tys.push_back(::HIR::TypeRef { RawType::Bool });
        const auto& dty = m_modtree.get_composite(gp);

        rv = Value(::HIR::TypeRef(&dty));
        lhs.get().write_to_value(rv, dty.fields[0].first);
        rv.write_u8( dty.fields[1].first, didnt_overflow ? 0 : 1 ); // Returns true if overflow happened
    }
    // - "exact_div" :: Normal divide, but UB if not an exact multiple
    else if( name == "exact_div" )
    {
        const auto& ty = ty_params.tys.at(0);

        auto lhs = PrimitiveValueVirt::from_value(ty, args.at(0));
        auto rhs = PrimitiveValueVirt::from_value(ty, args.at(1));

        LOG_ASSERT(!rhs.get().is_zero(), "`exact_div` with zero divisor: " << args.at(0) << " / " << args.at(1));
        auto rem = lhs;
        rem.get().modulo( rhs.get() );
        LOG_ASSERT(rem.get().is_zero(), "`exact_div` with yielded non-zero remainder: " << args.at(0) << " / " << args.at(1));
        bool didnt_overflow = lhs.get().divide( rhs.get() );
        LOG_ASSERT(didnt_overflow, "`exact_div` failed for unknown reason: " << args.at(0) << " /" << args.at(1));

        rv = Value(ty);
        lhs.get().write_to_value(rv, 0);
    }
    // Overflowing artithmatic
    else if( name == "overflowing_sub" )
    {
        const auto& ty = ty_params.tys.at(0);

        auto lhs = PrimitiveValueVirt::from_value(ty, args.at(0));
        auto rhs = PrimitiveValueVirt::from_value(ty, args.at(1));
        lhs.get().subtract( rhs.get() );
        // TODO: Overflowing part

        rv = Value(ty);
        lhs.get().write_to_value(rv, 0);
    }
    else if( name == "overflowing_add" )
    {
        const auto& ty = ty_params.tys.at(0);

        auto lhs = PrimitiveValueVirt::from_value(ty, args.at(0));
        auto rhs = PrimitiveValueVirt::from_value(ty, args.at(1));
        lhs.get().add( rhs.get() );

        rv = Value(ty);
        lhs.get().write_to_value(rv, 0);
    }
    // ----------------------------------------------------------------
    // memcpy
    else if( name == "copy_nonoverlapping" )
    {
        //auto src_ofs = args.at(0).read_usize(0);
        //auto src_alloc = args.at(0).get_relocation(0);
        //auto dst_ofs = args.at(1).read_usize(0);
        //auto dst_alloc = args.at(1).get_relocation(0);
        size_t ent_count = args.at(2).read_usize(0);
        size_t ent_size = ty_params.tys.at(0).get_size();
        auto byte_count = ent_count * ent_size;
        LOG_DEBUG("`copy_nonoverlapping`: byte_count=" << byte_count);

        // A count of zero doesn't need to do any of the checks (TODO: Validate this rule)
        if( byte_count > 0 )
        {
            auto src_vr = args.at(0).read_pointer_valref_mut(0, byte_count);
            auto dst_vr = args.at(1).read_pointer_valref_mut(0, byte_count);

            auto& dst_alloc = dst_vr.m_alloc;
            LOG_ASSERT(dst_alloc, "Destination of copy* must be a memory allocation");
            LOG_ASSERT(dst_alloc.is_alloc(), "Destination of copy* must be a memory allocation");

            // TODO: is this inefficient?
            auto src_val = src_vr.read_value(0, byte_count);
            LOG_DEBUG("src_val = " << src_val);
            dst_alloc.alloc().write_value(dst_vr.m_offset, ::std::move(src_val));
        }
    }
    // ----------------------------------------------------------------
    // Bit Twiddling
    // ---
    // cttz = CounT Trailing Zeroes
    else if( name == "cttz_nonzero" )
    {
        const auto& ty_T = ty_params.tys.at(0);
        auto v_inner = PrimitiveValueVirt::from_value(ty_T, args.at(0));
        auto v = v_inner.get().as_u128();
        unsigned n = 0;
        while( (v & 1) == 0 && n < ty_T.get_size()*8 )
        {
            v = v >> static_cast<uint8_t>(1);
            n ++;
        }
        rv = Value( HIR::TypeRef(RawType::USize) );
        rv.write_usize(0, n);
    }
    else
    {
        LOG_TODO("Call intrinsic \"" << name << "\"");
    }
    return true;
}

// TODO: Use a ValueRef instead?
bool InterpreterThread::drop_value(Value ptr, const ::HIR::TypeRef& ty, bool is_shallow/*=false*/)
{
    TRACE_FUNCTION_R(ptr << ": " << ty << (is_shallow ? " (shallow)" : ""), "");
    // TODO: After the drop is done, flag the backing allocation for `ptr` as freed
    if( is_shallow )
    {
        // HACK: Only works for Box<T> where the first pointer is the data pointer
        auto box_ptr_vr = ptr.read_pointer_valref_mut(0, POINTER_SIZE);
        auto ofs = box_ptr_vr.read_usize(0);
        auto alloc = box_ptr_vr.get_relocation(0);
        if( ofs != Allocation::PTR_BASE || !alloc || !alloc.is_alloc() ) {
            LOG_ERROR("Attempting to shallow drop with invalid pointer (no relocation or non-zero offset) - " << box_ptr_vr);
        }

        LOG_DEBUG("drop_value SHALLOW deallocate " << alloc);
        alloc.alloc().mark_as_freed();
        return true;
    }
    if( const auto* w = ty.get_wrapper() )
    {
        switch( w->type )
        {
        case TypeWrapper::Ty::Borrow:
            if( w->size == static_cast<size_t>(::HIR::BorrowType::Move) )
            {
                LOG_TODO("Drop - " << ty << " - dereference and go to inner");
                // TODO: Clear validity on the entire inner value.
                //auto iptr = ptr.read_value(0, ty.get_size());
                //drop_value(iptr, ty.get_inner());
            }
            else
            {
                // No destructor
            }
            break;
        case TypeWrapper::Ty::Pointer:
            // No destructor
            break;
        case TypeWrapper::Ty::Slice: {
            // - Get thin pointer and count
            auto ofs = ptr.read_usize(0);
            LOG_ASSERT(ofs >= Allocation::PTR_BASE, "");
            auto ptr_reloc = ptr.get_relocation(0);
            auto count = ptr.read_usize(POINTER_SIZE);

            auto ity = ty.get_inner();
            auto pty = ity.wrapped(TypeWrapper::Ty::Borrow, static_cast<size_t>(::HIR::BorrowType::Move));
            for(uint64_t i = 0; i < count; i ++)
            {
                auto ptr = Value::new_pointer(pty, ofs, ptr_reloc);
                if( !drop_value(ptr, ity) )
                {
                    // - This is trying to invoke custom drop glue, need to suspend this operation and come back later

                    // > insert a new frame shim BEFORE the current top (which would be the frame created by
                    // `drop_value` calling a function)
                    m_stack.insert( m_stack.end() - 1, StackFrame::make_wrapper([this,pty,ity,ptr_reloc,count, i,ofs](Value& rv, Value drop_rv) mutable {
                        assert(i < count);
                        i ++;
                        ofs += ity.get_size();
                        if( i < count )
                        {
                            auto ptr = Value::new_pointer(pty, ofs, ptr_reloc);
                            assert(!drop_value(ptr, ity));
                            return false;
                        }
                        else
                        {
                            return true;
                        }
                        }) );
                    return false;
                }
                ofs += ity.get_size();
            }
            } break;
        // TODO: Arrays?
        default:
            LOG_TODO("Drop - " << ty << " - array?");
            break;
        }
    }
    else
    {
        if( ty.inner_type == RawType::Composite )
        {
            if( ty.composite_type().drop_glue != ::HIR::Path() )
            {
                LOG_DEBUG("Drop - " << ty);

                Value   tmp;
                return this->call_path(tmp, ty.composite_type().drop_glue, { ptr });
            }
            else
            {
                // No drop glue
            }
        }
        else if( ty.inner_type == RawType::TraitObject )
        {
            // Get the drop glue from the vtable (first entry)
            auto inner_ptr = ptr.read_value(0, POINTER_SIZE);
            auto vtable = ptr.deref(POINTER_SIZE, ty.get_meta_type().get_inner());
            auto drop_r = vtable.get_relocation(0);
            if( drop_r )
            {
                LOG_ASSERT(drop_r.get_ty() == RelocationPtr::Ty::Function, "");
                auto fcn = drop_r.fcn();
                static Value    tmp;
                return this->call_path(tmp, fcn, { ::std::move(inner_ptr) });
            }
            else
            {
                // None
            }
        }
        else
        {
            // No destructor
        }
    }
    return true;
}
