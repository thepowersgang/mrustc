//
//
//
#include <iostream>
#include "module_tree.hpp"
#include "value.hpp"
#include <algorithm>
#include <iomanip>
#include "debug.hpp"
#ifdef _WIN32
# define NOMINMAX
# include <Windows.h>
#endif

struct ProgramOptions
{
    ::std::string   infile;

    int parse(int argc, const char* argv[]);
};

Value MIRI_Invoke(ModuleTree& modtree, ::HIR::Path path, ::std::vector<Value> args);
Value MIRI_Invoke_Extern(const ::std::string& link_name, const ::std::string& abi, ::std::vector<Value> args);
Value MIRI_Invoke_Intrinsic(ModuleTree& modtree, const ::std::string& name, const ::HIR::PathParams& ty_params, ::std::vector<Value> args);

int main(int argc, const char* argv[])
{
    ProgramOptions  opts;

    if( opts.parse(argc, argv) )
    {
        return 1;
    }

    auto tree = ModuleTree {};

    tree.load_file(opts.infile);

    auto val_argc = Value( ::HIR::TypeRef{RawType::I32} );
    ::HIR::TypeRef  argv_ty { RawType::I8 };
    argv_ty.wrappers.push_back(TypeWrapper { TypeWrapper::Ty::Pointer, 0 });
    argv_ty.wrappers.push_back(TypeWrapper { TypeWrapper::Ty::Pointer, 0 });
    auto val_argv = Value(argv_ty);
    val_argc.write_bytes(0, "\0\0\0", 4);
    val_argv.write_bytes(0, "\0\0\0\0\0\0\0", argv_ty.get_size());

    try
    {
        ::std::vector<Value>    args;
        args.push_back(::std::move(val_argc));
        args.push_back(::std::move(val_argv));
        auto rv = MIRI_Invoke( tree, tree.find_lang_item("start"), ::std::move(args) );
        ::std::cout << rv << ::std::endl;
    }
    catch(const DebugExceptionTodo& /*e*/)
    {
        ::std::cerr << "TODO Hit" << ::std::endl;
        return 1;
    }
    catch(const DebugExceptionError& /*e*/)
    {
        ::std::cerr << "Error encountered" << ::std::endl;
        return 1;
    }

    return 0;
}
class PrimitiveValue
{
public:
    virtual ~PrimitiveValue() {}

    virtual bool add(const PrimitiveValue& v) = 0;
    virtual bool subtract(const PrimitiveValue& v) = 0;
    virtual bool multiply(const PrimitiveValue& v) = 0;
    virtual bool divide(const PrimitiveValue& v) = 0;
    virtual bool modulo(const PrimitiveValue& v) = 0;
    virtual void write_to_value(Value& tgt, size_t ofs) const = 0;

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
};
struct PrimitiveU64: public PrimitiveUInt<uint64_t>
{
    PrimitiveU64(uint64_t v): PrimitiveUInt(v) {}
    void write_to_value(Value& tgt, size_t ofs) const override {
        tgt.write_u64(ofs, this->v);
    }
};
struct PrimitiveU32: public PrimitiveUInt<uint32_t>
{
    PrimitiveU32(uint32_t v): PrimitiveUInt(v) {}
    void write_to_value(Value& tgt, size_t ofs) const override {
        tgt.write_u32(ofs, this->v);
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
};
struct PrimitiveI64: public PrimitiveSInt<int64_t>
{
    PrimitiveI64(int64_t v): PrimitiveSInt(v) {}
    void write_to_value(Value& tgt, size_t ofs) const override {
        tgt.write_i64(ofs, this->v);
    }
};
struct PrimitiveI32: public PrimitiveSInt<int32_t>
{
    PrimitiveI32(int32_t v): PrimitiveSInt(v) {}
    void write_to_value(Value& tgt, size_t ofs) const override {
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
        LOG_ASSERT(t.wrappers.empty(), "PrimitiveValueVirt::from_value: " << t);
        switch(t.inner_type)
        {
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

namespace
{

    void drop_value(ModuleTree& modtree, Value ptr, const ::HIR::TypeRef& ty)
    {
        if( ty.wrappers.empty() )
        {
            if( ty.inner_type == RawType::Composite )
            {
                if( ty.composite_type->drop_glue != ::HIR::Path() )
                {
                    LOG_DEBUG("Drop - " << ty);

                    MIRI_Invoke(modtree, ty.composite_type->drop_glue, { ptr });
                }
                else
                {
                    // No drop glue
                }
            }
            else if( ty.inner_type == RawType::TraitObject )
            {
                LOG_TODO("Drop - " << ty << " - trait object");
            }
            else
            {
                // No destructor
            }
        }
        else if( ty.wrappers[0].type == TypeWrapper::Ty::Borrow )
        {
            if( ty.wrappers[0].size == static_cast<size_t>(::HIR::BorrowType::Move) )
            {
                LOG_TODO("Drop - " << ty << " - dereference and go to inner");
                // TODO: Clear validity on the entire inner value.
            }
            else
            {
                // No destructor
            }
        }
        // TODO: Arrays
        else
        {
            LOG_TODO("Drop - " << ty << " - array?");
        }
    }

}

Value MIRI_Invoke(ModuleTree& modtree, ::HIR::Path path, ::std::vector<Value> args)
{
    Value   ret;

    const auto& fcn = modtree.get_function(path);


    // TODO: Support overriding certain functions
    {
        if( path == ::HIR::SimplePath { "std", { "sys", "imp", "c", "SetThreadStackGuarantee" } } )
        {
            ret = Value(::HIR::TypeRef{RawType::I32});
            ret.write_i32(0, 120);  // ERROR_CALL_NOT_IMPLEMENTED
            return ret;
        }

        // - No guard page needed
        if( path == ::HIR::SimplePath { "std",  {"sys", "imp", "thread", "guard", "init" } } )
        {
            ret = Value::with_size(16, false);
            ret.write_u64(0, 0);
            ret.write_u64(8, 0);
            return ret;
        }
        
        // - No stack overflow handling needed
        if( path == ::HIR::SimplePath { "std", { "sys", "imp", "stack_overflow", "imp", "init" } } )
        {
            return ret;
        }
    }

    if( fcn.external.link_name != "" )
    {
        // External function!
        ret = MIRI_Invoke_Extern(fcn.external.link_name, fcn.external.link_abi, ::std::move(args));
        LOG_DEBUG(path << " = " << ret);
        return ret;
    }

    TRACE_FUNCTION_R(path, path << " = " << ret);
    for(size_t i = 0; i < args.size(); i ++)
    {
        LOG_DEBUG("- Argument(" << i << ") = " << args[i]);
    }

    ret = Value(fcn.ret_ty == RawType::Unreachable ? ::HIR::TypeRef() : fcn.ret_ty);

    struct State
    {
        ModuleTree& modtree;
        const Function& fcn;
        Value&  ret;
        ::std::vector<Value>    args;
        ::std::vector<Value>    locals;
        ::std::vector<bool>     drop_flags;

        State(ModuleTree& modtree, const Function& fcn, Value& ret, ::std::vector<Value> args):
            modtree(modtree),
            fcn(fcn),
            ret(ret),
            args(::std::move(args)),
            drop_flags(fcn.m_mir.drop_flags)
        {
            locals.reserve(fcn.m_mir.locals.size());
            for(const auto& ty : fcn.m_mir.locals)
            {
                if( ty == RawType::Unreachable ) {
                    // HACK: Locals can be !, but they can NEVER be accessed
                    locals.push_back(Value());
                }
                else {
                    locals.push_back(Value(ty));
                }
            }
        }

        ValueRef get_value_and_type(const ::MIR::LValue& lv, ::HIR::TypeRef& ty)
        {
            switch(lv.tag())
            {
            case ::MIR::LValue::TAGDEAD:    throw "";
            TU_ARM(lv, Return, _e) {
                ty = fcn.ret_ty;
                return ValueRef(ret, 0, ret.size());
                } break;
            TU_ARM(lv, Local, e) {
                ty = fcn.m_mir.locals.at(e);
                return ValueRef(locals.at(e), 0, locals.at(e).size());
                } break;
            TU_ARM(lv, Argument, e) {
                ty = fcn.args.at(e.idx);
                return ValueRef(args.at(e.idx), 0, args.at(e.idx).size());
                } break;
            TU_ARM(lv, Static, e) {
                // TODO: Type!
                return ValueRef(modtree.get_static(e), 0, modtree.get_static(e).size());
                } break;
            TU_ARM(lv, Index, e) {
                auto idx = get_value_ref(*e.idx).read_usize(0);
                ::HIR::TypeRef  array_ty;
                auto base_val = get_value_and_type(*e.val, array_ty);
                if( array_ty.wrappers.empty() )
                    throw "ERROR";
                if( array_ty.wrappers.front().type == TypeWrapper::Ty::Array )
                {
                    ty = array_ty.get_inner();
                    base_val.m_offset += ty.get_size() * idx;
                    return base_val;
                }
                else if( array_ty.wrappers.front().type == TypeWrapper::Ty::Slice )
                {
                    throw "TODO";
                }
                else
                {
                    throw "ERROR";
                }
                } break;
            TU_ARM(lv, Field, e) {
                ::HIR::TypeRef  composite_ty;
                auto base_val = get_value_and_type(*e.val, composite_ty);
                // TODO: if there's metadata present in the base, but the inner doesn't have metadata, clear the metadata
                size_t inner_ofs;
                ty = composite_ty.get_field(e.field_index, inner_ofs);
                LOG_DEBUG("Field - " << composite_ty << "#" << e.field_index << " = @" << inner_ofs << " " << ty);
                base_val.m_offset += inner_ofs;
                if( !ty.get_meta_type() )
                {
                    LOG_ASSERT(base_val.m_size >= ty.get_size(), "Field didn't fit in the value - " << ty.get_size() << " required, but " << base_val.m_size << " avail");
                    base_val.m_size = ty.get_size();
                }
                return base_val;
                }
            TU_ARM(lv, Downcast, e) {
                ::HIR::TypeRef  composite_ty;
                auto base_val = get_value_and_type(*e.val, composite_ty);
                LOG_DEBUG("Downcast - " << composite_ty);

                size_t inner_ofs;
                ty = composite_ty.get_field(e.variant_index, inner_ofs);
                base_val.m_offset += inner_ofs;
                return base_val;
                }
            TU_ARM(lv, Deref, e) {
                ::HIR::TypeRef  ptr_ty;
                auto val = get_value_and_type(*e.val, ptr_ty);
                ty = ptr_ty.get_inner();
                LOG_DEBUG("val = " << val);

                LOG_ASSERT(val.m_size >= POINTER_SIZE, "Deref of a value that doesn't fit a pointer - " << ty);
                size_t ofs = val.read_usize(0);

                // There MUST be a relocation at this point with a valid allocation.
                auto& val_alloc = val.m_alloc ? val.m_alloc : val.m_value->allocation;
                LOG_ASSERT(val_alloc, "Deref of a value with no allocation (hence no relocations)");
                LOG_ASSERT(val_alloc.is_alloc(), "Deref of a value with a non-data allocation");
                LOG_TRACE("Deref " << val_alloc.alloc() << " + " << ofs << " to give value of type " << ty);
                auto alloc = val_alloc.alloc().get_relocation(val.m_offset);
                LOG_ASSERT(alloc, "Deref of a value with no relocation");
                if( alloc.is_alloc() )
                {
                    LOG_DEBUG("> " << lv << " alloc=" << alloc.alloc());
                }
                size_t size;

                const auto* meta_ty = ty.get_meta_type();
                ::std::shared_ptr<Value>    meta_val;
                // If the type has metadata, store it.
                if( meta_ty )
                {
                    auto meta_size = meta_ty->get_size();
                    LOG_ASSERT(val.m_size == POINTER_SIZE + meta_size, "Deref of " << ty << ", but pointer isn't correct size");
                    meta_val = ::std::make_shared<Value>( val.read_value(POINTER_SIZE, meta_size) );

                    // TODO: Get a more sane size from the metadata
                    LOG_DEBUG("> Meta " << *meta_val << ", size = " << alloc.get_size() << " - " << ofs);
                    size = alloc.get_size() - ofs;
                }
                else
                {
                    LOG_ASSERT(val.m_size == POINTER_SIZE, "Deref of a value that isn't a pointer-sized value (size=" << val.m_size << ") - " << val << ": " << ptr_ty);
                    size = ty.get_size();
                }

                auto rv = ValueRef(::std::move(alloc), ofs, size);
                rv.m_metadata = ::std::move(meta_val);
                return rv;
                } break;
            }
            throw "";
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
            //LOG_DEBUG(lv << " = " << val);
            ::HIR::TypeRef  ty;
            auto base_value = get_value_and_type(lv, ty);

            if(base_value.m_alloc) {
                base_value.m_alloc.alloc().write_value(base_value.m_offset, ::std::move(val));
            }
            else {
                base_value.m_value->write_value(base_value.m_offset, ::std::move(val));
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
                return val;
                } break;
            TU_ARM(c, Uint, ce) {
                ty = ::HIR::TypeRef(ce.t);
                Value val = Value(ty);
                val.write_bytes(0, &ce.v, ::std::min(ty.get_size(), sizeof(ce.v)));  // TODO: Endian
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
            TU_ARM(c, Bytes, ce) {
                LOG_TODO("Constant::Bytes");
                } break;
            TU_ARM(c, StaticString, ce) {
                ty = ::HIR::TypeRef(RawType::Str);
                ty.wrappers.push_back(TypeWrapper { TypeWrapper::Ty::Borrow, 0 });
                Value val = Value(ty);
                val.write_usize(0, 0);
                val.write_usize(POINTER_SIZE, ce.size());
                val.allocation.alloc().relocations.push_back(Relocation { 0, AllocationPtr::new_string(&ce) });
                LOG_DEBUG(c << " = " << val);
                //return Value::new_dataptr(ce.data());
                return val;
                } break;
            TU_ARM(c, ItemAddr, ce) {
                // Create a value with a special backing allocation of zero size that references the specified item.
                if( const auto* fn = modtree.get_function_opt(ce) ) {
                    return Value::new_fnptr(ce);
                }
                LOG_TODO("Constant::ItemAddr - statics?");
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
    } state { modtree, fcn, ret, ::std::move(args) };

    size_t bb_idx = 0;
    for(;;)
    {
        const auto& bb = fcn.m_mir.blocks.at(bb_idx);

        for(const auto& stmt : bb.statements)
        {
            LOG_DEBUG("BB" << bb_idx << "/" << (&stmt - bb.statements.data()) << ": " << stmt);
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
                    if( !alloc )
                    {
                        if( !src_base_value.m_value->allocation )
                        {
                            src_base_value.m_value->create_allocation();
                        }
                        alloc = AllocationPtr(src_base_value.m_value->allocation);
                    }
                    if( alloc.is_alloc() )
                        LOG_DEBUG("- alloc=" << alloc << " (" << alloc.alloc() << ")");
                    else
                        LOG_DEBUG("- alloc=" << alloc);
                    size_t ofs = src_base_value.m_offset;
                    const auto* meta = src_ty.get_meta_type();
                    bool is_slice_like = src_ty.has_slice_meta();
                    src_ty.wrappers.insert(src_ty.wrappers.begin(), TypeWrapper { TypeWrapper::Ty::Borrow, static_cast<size_t>(re.type) });

                    new_val = Value(src_ty);
                    // ^ Pointer value
                    new_val.write_usize(0, ofs);
                    if( meta )
                    {
                        LOG_ASSERT(src_base_value.m_metadata, "Borrow of an unsized value, but no metadata avaliable");
                        new_val.write_value(POINTER_SIZE, *src_base_value.m_metadata);
                    }
                    // - Add the relocation after writing the value (writing clears the relocations)
                    new_val.allocation.alloc().relocations.push_back(Relocation { 0, ::std::move(alloc) });
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
                    else if( !re.type.wrappers.empty() )
                    {
                        // Destination can only be a raw pointer
                        if( re.type.wrappers.at(0).type != TypeWrapper::Ty::Pointer ) {
                            throw "ERROR";
                        }
                        if( !src_ty.wrappers.empty() )
                        {
                            // Source can be either
                            if( src_ty.wrappers.at(0).type != TypeWrapper::Ty::Pointer
                                && src_ty.wrappers.at(0).type != TypeWrapper::Ty::Borrow ) {
                                throw "ERROR";
                            }

                            if( src_ty.get_size() > re.type.get_size() ) {
                                // TODO: How to casting fat to thin?
                                //LOG_TODO("Handle casting fat to thin, " << src_ty << " -> " << re.type);
                                new_val = src_value.read_value(0, re.type.get_size());
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
                                ::std::cerr << "ERROR: Trying to pointer (" << re.type <<" ) from invalid type (" << src_ty << ")\n";
                                throw "ERROR";
                            }
                            new_val = src_value.read_value(0, re.type.get_size());
                        }
                    }
                    else if( !src_ty.wrappers.empty() )
                    {
                        // TODO: top wrapper MUST be a pointer
                        if( src_ty.wrappers.at(0).type != TypeWrapper::Ty::Pointer
                            && src_ty.wrappers.at(0).type != TypeWrapper::Ty::Borrow ) {
                            throw "ERROR";
                        }
                        // TODO: MUST be a thin pointer?

                        // TODO: MUST be an integer (usize only?)
                        if( re.type != RawType::USize && re.type != RawType::ISize ) {
                            LOG_ERROR("Casting from a pointer to non-usize - " << re.type << " to " << src_ty);
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
                        case RawType::Composite:    throw "ERROR";
                        case RawType::TraitObject:    throw "ERROR";
                        case RawType::Function:    throw "ERROR";
                        case RawType::Str:    throw "ERROR";
                        case RawType::Unit:   throw "ERROR";
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
                            case RawType::USize:    throw "TODO";// /*dst_val = src_value.read_usize();*/   break;
                            case RawType::ISize:    throw "TODO";// /*dst_val = src_value.read_isize();*/   break;
                            case RawType::U8:   dst_val = static_cast<float>( src_value.read_u8 (0) );  break;
                            case RawType::I8:   dst_val = static_cast<float>( src_value.read_i8 (0) );  break;
                            case RawType::U16:  dst_val = static_cast<float>( src_value.read_u16(0) );  break;
                            case RawType::I16:  dst_val = static_cast<float>( src_value.read_i16(0) );  break;
                            case RawType::U32:  dst_val = static_cast<float>( src_value.read_u32(0) );  break;
                            case RawType::I32:  dst_val = static_cast<float>( src_value.read_i32(0) );  break;
                            case RawType::U64:  dst_val = static_cast<float>( src_value.read_u64(0) );  break;
                            case RawType::I64:  dst_val = static_cast<float>( src_value.read_i64(0) );  break;
                            case RawType::U128: throw "TODO";// /*dst_val = src_value.read_u128();*/ break;
                            case RawType::I128: throw "TODO";// /*dst_val = src_value.read_i128();*/ break;
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
                            LOG_TODO("Cast to " << re.type);
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
                            case RawType::Char:
                                LOG_ASSERT(re.type.inner_type == RawType::U32, "Char can only be casted to u32, instead " << re.type);
                                new_val = src_value.read_value(0, 4);
                                break;
                            case RawType::Unit:
                                LOG_FATAL("Cast of unit");
                            case RawType::Composite: {
                                const auto& dt = *src_ty.composite_type;
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
                                LOG_ASSERT(tag_ty.wrappers.empty(), "");
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
                            case RawType::U128: throw "TODO"; /*dst_val = src_value.read_u128();*/ break;
                            case RawType::I128: throw "TODO"; /*dst_val = src_value.read_i128();*/ break;
                            }
                            } break;
                        case RawType::U128:
                        case RawType::I128:
                            LOG_TODO("Cast to " << re.type);
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
                        // TODO: Handle comparison of the relocations too

                        const auto& alloc_l = v_l.m_value ? v_l.m_value->allocation : v_l.m_alloc;
                        const auto& alloc_r = v_r.m_value ? v_r.m_value->allocation : v_r.m_alloc;
                        auto reloc_l = alloc_l ? v_l.get_relocation(v_l.m_offset) : AllocationPtr();
                        auto reloc_r = alloc_r ? v_r.get_relocation(v_r.m_offset) : AllocationPtr();

                        if( reloc_l != reloc_r )
                        {
                            res = (reloc_l < reloc_r ? -1 : 1);
                        }
                        LOG_DEBUG("res=" << res << ", " << reloc_l << " ? " << reloc_r);

                        if( ty_l.wrappers.empty() )
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
                            default:
                                LOG_TODO("BinOp comparisons - " << se.src << " w/ " << ty_l);
                            }
                        }
                        else if( ty_l.wrappers.front().type == TypeWrapper::Ty::Pointer )
                        {
                            // TODO: Technically only EQ/NE are valid.

                            res = res != 0 ? res : Ops::do_compare(v_l.read_usize(0), v_r.read_usize(0));

                            // Compare fat metadata.
                            if( res == 0 && v_l.m_size > POINTER_SIZE )
                            {
                                reloc_l = alloc_l ? alloc_l.alloc().get_relocation(POINTER_SIZE) : AllocationPtr();
                                reloc_r = alloc_r ? alloc_r.alloc().get_relocation(POINTER_SIZE) : AllocationPtr();

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
                        LOG_ASSERT(ty_l.wrappers.empty(), "Bitwise operator on non-primitive - " << ty_l);
                        LOG_ASSERT(ty_r.wrappers.empty(), "Bitwise operator with non-primitive - " << ty_r);
                        size_t max_bits = ty_r.get_size() * 8;
                        uint8_t shift;
                        auto check_cast = [&](auto v){ LOG_ASSERT(0 <= v && v <= max_bits, "Shift out of range - " << v); return static_cast<uint8_t>(v); };
                        switch(ty_r.inner_type)
                        {
                        case RawType::U64:  shift = check_cast(v_r.read_u64(0));    break;
                        case RawType::U32:  shift = check_cast(v_r.read_u32(0));    break;
                        case RawType::U16:  shift = check_cast(v_r.read_u16(0));    break;
                        case RawType::U8 :  shift = check_cast(v_r.read_u8 (0));    break;
                        case RawType::I64:  shift = check_cast(v_r.read_i64(0));    break;
                        case RawType::I32:  shift = check_cast(v_r.read_i32(0));    break;
                        case RawType::I16:  shift = check_cast(v_r.read_i16(0));    break;
                        case RawType::I8 :  shift = check_cast(v_r.read_i8 (0));    break;
                        case RawType::USize:  shift = check_cast(v_r.read_usize(0));    break;
                        case RawType::ISize:  shift = check_cast(v_r.read_isize(0));    break;
                        default:
                            LOG_TODO("BinOp shift rhs unknown type - " << se.src << " w/ " << ty_r);
                        }
                        new_val = Value(ty_l);
                        switch(ty_l.inner_type)
                        {
                        case RawType::U64:  new_val.write_u64(0, Ops::do_bitwise(v_l.read_u64(0), static_cast<uint64_t>(shift), re.op));   break;
                        case RawType::U32:  new_val.write_u32(0, Ops::do_bitwise(v_l.read_u32(0), static_cast<uint32_t>(shift), re.op));   break;
                        case RawType::U16:  new_val.write_u16(0, Ops::do_bitwise(v_l.read_u16(0), static_cast<uint16_t>(shift), re.op));   break;
                        case RawType::U8 :  new_val.write_u8 (0, Ops::do_bitwise(v_l.read_u8 (0), static_cast<uint8_t >(shift), re.op));   break;
                        case RawType::USize: new_val.write_usize(0, Ops::do_bitwise(v_l.read_usize(0), static_cast<uint64_t>(shift), re.op));   break;
                        default:
                            LOG_TODO("BinOp shift rhs unknown type - " << se.src << " w/ " << ty_r);
                        }
                        } break;
                    case ::MIR::eBinOp::BIT_AND:
                    case ::MIR::eBinOp::BIT_OR:
                    case ::MIR::eBinOp::BIT_XOR:
                        LOG_ASSERT(ty_l == ty_r, "BinOp type mismatch - " << ty_l << " != " << ty_r);
                        LOG_ASSERT(ty_l.wrappers.empty(), "Bitwise operator on non-primitive - " << ty_l);
                        new_val = Value(ty_l);
                        switch(ty_l.inner_type)
                        {
                        case RawType::U64:
                            new_val.write_u64( 0, Ops::do_bitwise(v_l.read_u64(0), v_r.read_u64(0), re.op) );
                            break;
                        case RawType::U32:
                            new_val.write_u32( 0, static_cast<uint32_t>(Ops::do_bitwise(v_l.read_u32(0), v_r.read_u32(0), re.op)) );
                            break;
                        case RawType::U16:
                            new_val.write_u16( 0, static_cast<uint16_t>(Ops::do_bitwise(v_l.read_u16(0), v_r.read_u16(0), re.op)) );
                            break;
                        case RawType::U8:
                            new_val.write_u8 ( 0, static_cast<uint8_t >(Ops::do_bitwise(v_l.read_u8 (0), v_r.read_u8 (0), re.op)) );
                            break;
                        case RawType::USize:
                            new_val.write_usize( 0, Ops::do_bitwise(v_l.read_usize(0), v_r.read_usize(0), re.op) );
                            break;
                        case RawType::I32:
                            new_val.write_i32( 0, static_cast<int32_t>(Ops::do_bitwise(v_l.read_i32(0), v_r.read_i32(0), re.op)) );
                            break;
                        default:
                            LOG_TODO("BinOp bitwise - " << se.src << " w/ " << ty_l);
                        }

                        break;
                    default:
                        LOG_ASSERT(ty_l == ty_r, "BinOp type mismatch - " << ty_l << " != " << ty_r);
                        auto val_l = PrimitiveValueVirt::from_value(ty_l, v_l);
                        auto val_r = PrimitiveValueVirt::from_value(ty_r, v_r);
                        switch(re.op)
                        {
                        case ::MIR::eBinOp::ADD:    val_l.get().add( val_r.get() ); break;
                        case ::MIR::eBinOp::SUB:    val_l.get().subtract( val_r.get() ); break;
                        case ::MIR::eBinOp::MUL:    val_l.get().multiply( val_r.get() ); break;
                        case ::MIR::eBinOp::DIV:    val_l.get().divide( val_r.get() ); break;
                        case ::MIR::eBinOp::MOD:    val_l.get().modulo( val_r.get() ); break;

                        default:
                            LOG_TODO("Unsupported binary operator?");
                        }
                        new_val = Value(ty_l);
                        val_l.get().write_to_value(new_val, 0);
                        break;
                    }
                    } break;
                TU_ARM(se.src, UniOp, re) {
                    ::HIR::TypeRef  ty;
                    auto v = state.get_value_and_type(re.val, ty);
                    LOG_ASSERT(ty.wrappers.empty(), "UniOp on wrapped type - " << ty);
                    new_val = Value(ty);
                    switch(re.op)
                    {
                    case ::MIR::eUniOp::INV:
                        switch(ty.inner_type)
                        {
                        case RawType::U128:
                            LOG_TODO("UniOp::INV U128");
                        case RawType::U64:
                            new_val.write_u64( 0, ~v.read_u64(0) );
                            break;
                        case RawType::U32:
                            new_val.write_u32( 0, ~v.read_u32(0) );
                            break;
                        case RawType::U16:
                            new_val.write_u16( 0, ~v.read_u16(0) );
                            break;
                        case RawType::U8:
                            new_val.write_u8 ( 0, ~v.read_u8 (0) );
                            break;
                        case RawType::USize:
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
                            LOG_TODO("UniOp::INV - w/ type " << ty);
                        }
                        break;
                    }
                    } break;
                TU_ARM(se.src, DstMeta, re) {
                    LOG_TODO(stmt);
                    } break;
                TU_ARM(se.src, DstPtr, re) {
                    LOG_TODO(stmt);
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

                    for(size_t i = 0; i < re.vals.size(); i++)
                    {
                        auto fld_ofs = dst_ty.composite_type->fields.at(i).first;
                        new_val.write_value(fld_ofs, state.param_to_value(re.vals[i]));
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
                    const auto& data_ty = state.modtree.get_composite(re.path);
                    auto dst_ty = ::HIR::TypeRef(&data_ty);
                    new_val = Value(dst_ty);
                    LOG_DEBUG("Variant " << new_val);
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
                    LOG_DEBUG("Variant " << new_val);
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
                    const auto& data_ty = state.modtree.get_composite(re.path);

                    ::HIR::TypeRef  dst_ty;
                    state.get_value_and_type(se.dst, dst_ty);
                    new_val = Value(dst_ty);
                    LOG_ASSERT(dst_ty.composite_type == &data_ty, "Destination type of RValue::Struct isn't the same as the input");

                    for(size_t i = 0; i < re.vals.size(); i++)
                    {
                        auto fld_ofs = data_ty.fields.at(i).first;
                        new_val.write_value(fld_ofs, state.param_to_value(re.vals[i]));
                    }
                    } break;
                }
                LOG_DEBUG("- " << new_val);
                state.write_lvalue(se.dst, ::std::move(new_val));
                } break;
            case ::MIR::Statement::TAG_Asm:
                LOG_TODO(stmt);
                break;
            TU_ARM(stmt, Drop, se) {
                if( se.flag_idx == ~0u || state.drop_flags.at(se.flag_idx) )
                {
                    ::HIR::TypeRef  ty;
                    auto v = state.get_value_and_type(se.slot, ty);

                    // - Take a pointer to the inner
                    auto alloc = v.m_alloc;
                    if( !alloc )
                    {
                        if( !v.m_value->allocation )
                        {
                            v.m_value->create_allocation();
                        }
                        alloc = AllocationPtr(v.m_value->allocation);
                    }
                    size_t ofs = v.m_offset;
                    assert(!ty.get_meta_type());

                    auto ptr_ty = ty.wrap(TypeWrapper::Ty::Borrow, 2);

                    auto ptr_val = Value(ptr_ty);
                    ptr_val.write_usize(0, ofs);
                    ptr_val.allocation.alloc().relocations.push_back(Relocation { 0, ::std::move(alloc) });

                    drop_value(modtree, ptr_val, ty);
                    // TODO: Clear validity on the entire inner value.
                    //alloc.mark_as_freed();
                }
                } break;
            TU_ARM(stmt, SetDropFlag, se) {
                bool val = (se.other == ~0 ? false : state.drop_flags.at(se.other)) != se.new_val;
                LOG_DEBUG("- " << val);
                state.drop_flags.at(se.idx) = val;
                } break;
            case ::MIR::Statement::TAG_ScopeEnd:
                LOG_TODO(stmt);
                break;
            }
        }

        LOG_DEBUG("BB" << bb_idx << "/TERM: " << bb.terminator);
        switch(bb.terminator.tag())
        {
        case ::MIR::Terminator::TAGDEAD:    throw "";
        TU_ARM(bb.terminator, Incomplete, _te)
            LOG_TODO("Terminator::Incomplete hit");
        TU_ARM(bb.terminator, Diverge, _te)
            LOG_TODO("Terminator::Diverge hit");
        TU_ARM(bb.terminator, Panic, _te)
            LOG_TODO("Terminator::Panic");
        TU_ARM(bb.terminator, Goto, te)
            bb_idx = te;
            continue;
        TU_ARM(bb.terminator, Return, _te)
            LOG_DEBUG("RETURN " << state.ret);
            return state.ret;
        TU_ARM(bb.terminator, If, te) {
            uint8_t v = state.get_value_ref(te.cond).read_u8(0);
            LOG_ASSERT(v == 0 || v == 1, "");
            bb_idx = v ? te.bb0 : te.bb1;
            } continue;
        TU_ARM(bb.terminator, Switch, te) {
            ::HIR::TypeRef ty;
            auto v = state.get_value_and_type(te.val, ty);
            LOG_ASSERT(ty.wrappers.size() == 0, "" << ty);
            LOG_ASSERT(ty.inner_type == RawType::Composite, "" << ty);

            // TODO: Convert the variant list into something that makes it easier to switch on.
            size_t found_target = SIZE_MAX;
            size_t default_target = SIZE_MAX;
            for(size_t i = 0; i < ty.composite_type->variants.size(); i ++)
            {
                const auto& var = ty.composite_type->variants[i];
                if( var.tag_data.size() == 0 )
                {
                    // Save as the default, error for multiple defaults
                    if( default_target != SIZE_MAX )
                    {
                        LOG_FATAL("Two variants with no tag in Switch");
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
                        found_target = i;
                        break ;
                    }
                }
            }

            if( found_target == SIZE_MAX )
            {
                found_target = default_target;
            }
            if( found_target == SIZE_MAX )
            {
                LOG_FATAL("Terminator::Switch on " << ty << " didn't find a variant");
            }
            bb_idx = te.targets.at(found_target);
            } continue;
        TU_ARM(bb.terminator, SwitchValue, _te)
            LOG_TODO("Terminator::SwitchValue");
        TU_ARM(bb.terminator, Call, te) {
            ::std::vector<Value>    sub_args; sub_args.reserve(te.args.size());
            for(const auto& a : te.args)
            {
                sub_args.push_back( state.param_to_value(a) );
            }
            if( te.fcn.is_Intrinsic() )
            {
                const auto& fe = te.fcn.as_Intrinsic();
                state.write_lvalue(te.ret_val, MIRI_Invoke_Intrinsic(modtree, fe.name, fe.params, ::std::move(sub_args)));
            }
            else
            {
                const ::HIR::Path* fcn_p;
                if( te.fcn.is_Path() ) {
                    fcn_p = &te.fcn.as_Path();
                }
                else {
                    ::HIR::TypeRef ty;
                    auto v = state.get_value_and_type(te.fcn.as_Value(), ty);
                    // TODO: Assert type
                    // TODO: Assert offset/content.
                    assert(v.read_usize(v.m_offset) == 0);
                    auto& alloc_ptr = v.m_alloc ? v.m_alloc : v.m_value->allocation;
                    LOG_ASSERT(alloc_ptr, "Calling value that can't be a pointer (no allocation)");
                    const auto& fcn_alloc_ptr = alloc_ptr.alloc().get_relocation(v.m_offset);
                    LOG_ASSERT(fcn_alloc_ptr, "Calling value with no relocation");
                    LOG_ASSERT(fcn_alloc_ptr.get_ty() == AllocationPtr::Ty::Function, "Calling value that isn't a function pointer");
                    fcn_p = &fcn_alloc_ptr.fcn();
                }

                LOG_DEBUG("Call " << *fcn_p);
                auto v = MIRI_Invoke(modtree, *fcn_p, ::std::move(sub_args));
                LOG_DEBUG(te.ret_val << " = " << v << " (resume " << path << ")");
                state.write_lvalue(te.ret_val, ::std::move(v));
            }
            bb_idx = te.ret_block;
            } continue;
        }
        throw "";
    }

    throw "";
}

extern "C" {
    long sysconf(int);
}

Value MIRI_Invoke_Extern(const ::std::string& link_name, const ::std::string& abi, ::std::vector<Value> args)
{
    if( link_name == "__rust_allocate" )
    {
        auto size = args.at(0).read_usize(0);
        auto align = args.at(1).read_usize(0);
        LOG_DEBUG("__rust_allocate(size=" << size << ", align=" << align << ")");
        ::HIR::TypeRef  rty { RawType::Unit };
        rty.wrappers.push_back({ TypeWrapper::Ty::Pointer, 0 });
        Value rv = Value(rty);
        rv.write_usize(0, 0);
        // TODO: Use the alignment when making an allocation?
        rv.allocation.alloc().relocations.push_back({ 0,  Allocation::new_alloc(size) });
        return rv;
    }
    else if( link_name == "__rust_reallocate" )
    {
        LOG_ASSERT(args.at(0).allocation, "__rust_reallocate first argument doesn't have an allocation");
        auto alloc_ptr = args.at(0).allocation.alloc().get_relocation(0);
        auto ptr_ofs = args.at(0).read_usize(0);
        LOG_ASSERT(ptr_ofs == 0, "__rust_reallocate with offset pointer");
        auto oldsize = args.at(1).read_usize(0);
        auto newsize = args.at(2).read_usize(0);
        auto align = args.at(3).read_usize(0);
        LOG_DEBUG("__rust_reallocate(ptr=" << alloc_ptr << ", oldsize=" << oldsize << ", newsize=" << newsize << ", align=" << align << ")");

        LOG_ASSERT(alloc_ptr, "__rust_reallocate with no backing allocation attached to pointer");
        LOG_ASSERT(alloc_ptr.is_alloc(), "__rust_reallocate with no backing allocation attached to pointer");
        auto& alloc = alloc_ptr.alloc();
        // TODO: Check old size and alignment against allocation.
        alloc.data.resize( (newsize + 8-1) / 8 );
        alloc.mask.resize( (newsize + 8-1) / 8 );
        // TODO: Should this instead make a new allocation to catch use-after-free?
        return ::std::move(args.at(0));
    }
    else if( link_name == "__rust_deallocate" )
    {
        LOG_ASSERT(args.at(0).allocation, "__rust_deallocate first argument doesn't have an allocation");
        auto alloc_ptr = args.at(0).allocation.alloc().get_relocation(0);
        auto ptr_ofs = args.at(0).read_usize(0);
        LOG_ASSERT(ptr_ofs == 0, "__rust_deallocate with offset pointer");

        LOG_ASSERT(alloc_ptr, "__rust_deallocate with no backing allocation attached to pointer");
        LOG_ASSERT(alloc_ptr.is_alloc(), "__rust_deallocate with no backing allocation attached to pointer");
        auto& alloc = alloc_ptr.alloc();
        // TODO: Figure out how to prevent this ever being written again.
        //alloc.mark_as_freed();
        for(auto& v : alloc.mask)
            v = 0;
        // Just let it drop.
        return Value();
    }
#ifdef _WIN32
    // WinAPI functions used by libstd
    else if( link_name == "AddVectoredExceptionHandler" )
    {
        LOG_DEBUG("Call `AddVectoredExceptionHandler` - Ignoring and returning non-null");
        auto rv = Value(::HIR::TypeRef(RawType::USize));
        rv.write_usize(0, 1);
        return rv;
    }
    else if( link_name == "GetModuleHandleW" )
    {
        LOG_ASSERT(args.at(0).allocation.is_alloc(), "");
        const auto& tgt_alloc = args.at(0).allocation.alloc().get_relocation(0);
        const void* arg0 = (tgt_alloc ? tgt_alloc.alloc().data_ptr() : nullptr);
        //extern void* GetModuleHandleW(const void* s);
        if(arg0) {
            LOG_DEBUG("GetModuleHandleW(" << tgt_alloc.alloc() << ")");
        }
        else {
            LOG_DEBUG("GetModuleHandleW(NULL)");
        }

        auto rv = GetModuleHandleW(static_cast<LPCWSTR>(arg0));
        if(rv)
        {
            return Value::new_ffiptr(FFIPointer { "GetModuleHandleW", rv });
        }
        else
        {
            auto rv = Value(::HIR::TypeRef(RawType::USize));
            rv.create_allocation();
            rv.write_usize(0,0);
            return rv;
        }
    }
    else if( link_name == "GetProcAddress" )
    {
        LOG_ASSERT(args.at(0).allocation.is_alloc(), "");
        const auto& handle_alloc = args.at(0).allocation.alloc().get_relocation(0);
        LOG_ASSERT(args.at(1).allocation.is_alloc(), "");
        const auto& sym_alloc = args.at(1).allocation.alloc().get_relocation(0);

        // TODO: Ensure that first arg is a FFI pointer with offset+size of zero
        void* handle = handle_alloc.ffi().ptr_value;
        // TODO: Get either a FFI data pointer, or a inner data pointer
        const void* symname = sym_alloc.alloc().data_ptr();
        // TODO: Sanity check that it's a valid c string within its allocation
        LOG_DEBUG("FFI GetProcAddress(" << handle << ", \"" << static_cast<const char*>(symname) << "\")");

        auto rv = GetProcAddress(static_cast<HMODULE>(handle), static_cast<LPCSTR>(symname));

        if( rv )
        {
            return Value::new_ffiptr(FFIPointer { "GetProcAddress", rv });
        }
        else
        {
            auto rv = Value(::HIR::TypeRef(RawType::USize));
            rv.create_allocation();
            rv.write_usize(0,0);
            return rv;
        }
    }
#else
    // std C
    else if( link_name == "signal" )
    {
        LOG_DEBUG("Call `signal` - Ignoring and returning SIG_IGN");
        auto rv = Value(::HIR::TypeRef(RawType::USize));
        rv.write_usize(0, 1);
        return rv;
    }
    // POSIX
    else if( link_name == "sysconf" )
    {
        auto name = args.at(0).read_i32(0);
        LOG_DEBUG("FFI sysconf(" << name << ")");
        long val = sysconf(name);
        auto rv = Value(::HIR::TypeRef(RawType::USize));
        rv.write_usize(0, val);
        return rv;
    }
#endif
    // Allocators!
    else
    {
        LOG_TODO("Call external function " << link_name);
    }
    throw "";
}
Value MIRI_Invoke_Intrinsic(ModuleTree& modtree, const ::std::string& name, const ::HIR::PathParams& ty_params, ::std::vector<Value> args)
{
    Value rv;
    TRACE_FUNCTION_R(name, rv);
    for(const auto& a : args)
        LOG_DEBUG("#" << (&a - args.data()) << ": " << a);
    if( name == "atomic_store" )
    {
        auto& ptr_val = args.at(0);
        auto& data_val = args.at(1);

        LOG_ASSERT(ptr_val.size() == POINTER_SIZE, "atomic_store of a value that isn't a pointer-sized value");

        // There MUST be a relocation at this point with a valid allocation.
        LOG_ASSERT(ptr_val.allocation, "Deref of a value with no allocation (hence no relocations)");
        LOG_TRACE("Deref " << ptr_val.allocation.alloc());
        auto alloc = ptr_val.allocation.alloc().get_relocation(0);
        LOG_ASSERT(alloc, "Deref of a value with no relocation");

        // TODO: Atomic side of this?
        size_t ofs = ptr_val.read_usize(0);
        const auto& ty = ty_params.tys.at(0);
        alloc.alloc().write_value(ofs, ::std::move(data_val));
    }
    else if( name == "atomic_load" )
    {
        auto& ptr_val = args.at(0);
        LOG_ASSERT(ptr_val.size() == POINTER_SIZE, "atomic_store of a value that isn't a pointer-sized value");

        // There MUST be a relocation at this point with a valid allocation.
        LOG_ASSERT(ptr_val.allocation, "Deref of a value with no allocation (hence no relocations)");
        LOG_TRACE("Deref " << ptr_val.allocation.alloc());
        auto alloc = ptr_val.allocation.alloc().get_relocation(0);
        LOG_ASSERT(alloc, "Deref of a value with no relocation");

        // TODO: Atomic side of this?
        size_t ofs = ptr_val.read_usize(0);
        const auto& ty = ty_params.tys.at(0);
        rv = alloc.alloc().read_value(ofs, ty.get_size());
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
        auto ptr_val = ::std::move(args.at(0));
        auto& ofs_val = args.at(1);

        auto r = ptr_val.allocation.alloc().get_relocation(0);
        auto orig_ofs = ptr_val.read_usize(0);
        auto delta_counts = ofs_val.read_usize(0);
        auto new_ofs = orig_ofs + delta_counts * ty_params.tys.at(0).get_size();
        if(POINTER_SIZE != 8) {
            new_ofs &= 0xFFFFFFFF;
        }

        ptr_val.write_usize(0, new_ofs);
        ptr_val.allocation.alloc().relocations.push_back({ 0, r });
        rv = ::std::move(ptr_val);
    }
    // effectively ptr::write
    else if( name == "move_val_init" )
    {
        auto& ptr_val = args.at(0);
        auto& data_val = args.at(1);

        LOG_ASSERT(ptr_val.size() == POINTER_SIZE, "move_val_init of an address that isn't a pointer-sized value");

        // There MUST be a relocation at this point with a valid allocation.
        LOG_ASSERT(ptr_val.allocation, "Deref of a value with no allocation (hence no relocations)");
        LOG_TRACE("Deref " << ptr_val << " and store " << data_val);
        auto alloc = ptr_val.allocation.alloc().get_relocation(0);
        LOG_ASSERT(alloc, "Deref of a value with no relocation");

        size_t ofs = ptr_val.read_usize(0);
        const auto& ty = ty_params.tys.at(0);
        alloc.alloc().write_value(ofs, ::std::move(data_val));
        LOG_DEBUG(alloc.alloc());
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
    // - Unsized stuff
    else if( name == "size_of_val" )
    {
        auto& val = args.at(0);
        const auto& ty = ty_params.tys.at(0);
        rv = Value(::HIR::TypeRef(RawType::USize));
        // Get unsized type somehow.
        // - _HAS_ to be the last type, so that makes it easier
        size_t fixed_size = 0;
        if( const auto* ity = ty.get_usized_type(fixed_size) )
        {
            const auto& meta_ty = *ty.get_meta_type();
            LOG_DEBUG("size_of_val - " << ty << " ity=" << *ity << " meta_ty=" << meta_ty << " fixed_size=" << fixed_size);
            size_t flex_size = 0;
            if( !ity->wrappers.empty() )
            {
                LOG_ASSERT(ity->wrappers[0].type == TypeWrapper::Ty::Slice, "");
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
                LOG_TODO("size_of_val - Trait Object - " << ty);
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
    else if( name == "drop_in_place" )
    {
        auto& val = args.at(0);
        const auto& ty = ty_params.tys.at(0);
        if( !ty.wrappers.empty() )
        {
            size_t item_count = 0;
            switch(ty.wrappers[0].type)
            {
            case TypeWrapper::Ty::Slice:
            case TypeWrapper::Ty::Array:
                item_count = (ty.wrappers[0].type == TypeWrapper::Ty::Slice ? val.read_usize(POINTER_SIZE) : ty.wrappers[0].size);
                break;
            case TypeWrapper::Ty::Pointer:
                break;
            case TypeWrapper::Ty::Borrow:
                break;
            }
            LOG_ASSERT(ty.wrappers[0].type == TypeWrapper::Ty::Slice, "drop_in_place should only exist for slices - " << ty);
            const auto& ity = ty.get_inner();
            size_t item_size = ity.get_size();

            auto ptr = val.read_value(0, POINTER_SIZE);;
            for(size_t i = 0; i < item_count; i ++)
            {
                drop_value(modtree, ptr, ity);
                ptr.write_usize(0, ptr.read_usize(0) + item_size);
            }
        }
        else
        {
            LOG_TODO("drop_in_place - " << ty);
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
        const auto& dty = modtree.get_composite(gp);

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
        const auto& dty = modtree.get_composite(gp);

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
        const auto& dty = modtree.get_composite(gp);

        rv = Value(::HIR::TypeRef(&dty));
        lhs.get().write_to_value(rv, dty.fields[0].first);
        rv.write_u8( dty.fields[1].first, didnt_overflow ? 0 : 1 ); // Returns true if overflow happened
    }
    // Overflowing artithmatic
    else if( name == "overflowing_sub" )
    {
        const auto& ty = ty_params.tys.at(0);

        auto lhs = PrimitiveValueVirt::from_value(ty, args.at(0));
        auto rhs = PrimitiveValueVirt::from_value(ty, args.at(1));
        lhs.get().subtract( rhs.get() );

        rv = Value(ty);
        lhs.get().write_to_value(rv, 0);
    }
    // ----------------------------------------------------------------
    // memcpy
    else if( name == "copy_nonoverlapping" )
    {
        auto src_ofs = args.at(0).read_usize(0);
        auto src_alloc = args.at(0).allocation.alloc().get_relocation(0);
        auto dst_ofs = args.at(1).read_usize(0);
        auto dst_alloc = args.at(1).allocation.alloc().get_relocation(0);
        size_t ent_count = args.at(2).read_usize(0);
        size_t ent_size = ty_params.tys.at(0).get_size();
        auto byte_count = ent_count * ent_size;

        LOG_ASSERT(src_alloc, "Source of copy* must have an allocation");
        LOG_ASSERT(dst_alloc, "Destination of copy* must be a memory allocation");
        LOG_ASSERT(dst_alloc.is_alloc(), "Destination of copy* must be a memory allocation");

        switch(src_alloc.get_ty())
        {
        case AllocationPtr::Ty::Allocation: {
            auto v = src_alloc.alloc().read_value(src_ofs, byte_count);
            dst_alloc.alloc().write_value(dst_ofs, ::std::move(v));
            } break;
        case AllocationPtr::Ty::StdString:
            LOG_ASSERT(src_ofs <= src_alloc.str().size(), "");
            LOG_ASSERT(byte_count <= src_alloc.str().size(), "");
            LOG_ASSERT(src_ofs + byte_count <= src_alloc.str().size(), "");
            dst_alloc.alloc().write_bytes(dst_ofs, src_alloc.str().data() + src_ofs, byte_count);
            break;
        case AllocationPtr::Ty::Function:
            LOG_FATAL("Attempt to copy* a function");
            break;
        case AllocationPtr::Ty::FfiPointer:
            LOG_BUG("Trying to copy from a FFI pointer");
            break;
        }
    }
    else
    {
        LOG_TODO("Call intrinsic \"" << name << "\"");
    }
    return rv;
}

int ProgramOptions::parse(int argc, const char* argv[])
{
    bool all_free = false;
    for(int argidx = 1; argidx < argc; argidx ++)
    {
        const char* arg = argv[argidx]; 
        if( arg[0] != '-' || all_free )
        {
            // Free
            if( this->infile == "" )
            {
                this->infile = arg;
            }
            else
            {
                // TODO: Too many free arguments
            }
        }
        else if( arg[1] != '-' )
        {
            // Short
        }
        else if( arg[2] != '\0' )
        {
            // Long
        }
        else
        {
            all_free = true;
        }
    }
    return 0;
}
