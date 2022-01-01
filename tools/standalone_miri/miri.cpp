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
#include <target_version.hpp>
#include "primitive_value.h"
#undef DEBUG

unsigned ThreadState::s_next_tls_key = 1;


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
            /*const*/ auto& s = this->thread.m_global.m_modtree.get_static(e);
            ty = s.ty;
            return ValueRef( this->thread.m_global.m_statics.at(&s) );
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
                    LOG_ASSERT(ofs >= alloc.get_base(), "Dereferencing invalid pointer - " << ofs << " into " << alloc);
                    ofs -= alloc.get_base();
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

    Value borrow_value(const ::MIR::LValue& lv, ::HIR::BorrowType bt, ::HIR::TypeRef& dst_ty)
    {
        ::HIR::TypeRef  src_ty;
        ValueRef src_base_value = this->get_value_and_type(lv, src_ty);
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
        dst_ty = src_ty.wrapped(TypeWrapper::Ty::Borrow, static_cast<size_t>(bt));
        LOG_DEBUG("Borrow - ofs=" << ofs << ", meta_ty=" << meta);

        // Create the pointer (can this just store into the target?)
        auto new_val = Value(dst_ty);
        new_val.write_ptr_ofs(0, ofs, ::std::move(alloc));
        // - Add metadata if required
        if( meta != RawType::Unreachable )
        {
            LOG_ASSERT(src_base_value.m_metadata, "Borrow of an unsized value, but no metadata avaliable");
            new_val.write_value(POINTER_SIZE, *src_base_value.m_metadata);
        }
        return new_val;
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
            val.write_ptr_ofs(0, 0, RelocationPtr::new_ffi(FFIPointer::new_const_bytes("Constant::Bytes", ce.data(), ce.size())));
            val.write_usize(POINTER_SIZE, ce.size());
            LOG_DEBUG(c << " = " << val);
            return val;
            } break;
        TU_ARM(c, StaticString, ce) {
            ty = ::HIR::TypeRef(RawType::Str).wrap(TypeWrapper::Ty::Borrow, 0);
            Value val = Value(ty);
            val.write_ptr_ofs(0, 0, RelocationPtr::new_string(&ce));
            val.write_usize(POINTER_SIZE, ce.size());
            LOG_DEBUG(c << " = " << val);
            return val;
            } break;
        // --> Accessor
        TU_ARM(c, ItemAddr, ce) {
            // Create a value with a special backing allocation of zero size that references the specified item.
            if( /*const auto* fn =*/ this->thread.m_global.m_modtree.get_function_opt(*ce) ) {
                ty = ::HIR::TypeRef(RawType::Function);
                return Value::new_fnptr(*ce);
            }
            if( const auto* s = this->thread.m_global.m_modtree.get_static_opt(*ce) ) {
                ty = s->ty.wrapped(TypeWrapper::Ty::Borrow, 0);
                auto& val = this->thread.m_global.m_statics.at(s);
                LOG_ASSERT(val.m_inner.is_alloc, "Statics should already have an allocation assigned");
                return Value::new_pointer_ofs(ty, 0, RelocationPtr::new_alloc(val.m_inner.alloc.alloc));
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
        TU_ARM(p, Borrow, pe)
            return borrow_value(pe.val, pe.type, ty);
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
        TU_ARM(p, Borrow, pe)
            LOG_TODO("");
        TU_ARM(p, LValue, pe)
            return get_value_and_type(pe, ty);
        }
        throw "";
    }
};

GlobalState::GlobalState(const ModuleTree& modtree):
    m_modtree(modtree)
{
    // Generate statics
    m_modtree.iterate_statics([this](RcString name, const Static& s) {
        auto val = Value(s.ty);
        // - Statics need to always have an allocation (for references)
        val.ensure_allocation();
        val.write_bytes(0, s.init.bytes.data(), s.init.bytes.size());
        for(const auto& r : s.init.relocs)
        {
            RelocationPtr   ptr;
            if( r.fcn_path.n == "" )
            {
                auto a = Allocation::new_alloc( r.string.size(), FMT_STRING("static " << name) );
                a->write_bytes(0, r.string.data(), r.string.size());
                ptr = RelocationPtr::new_alloc(::std::move(a));
            }
            else
            {
                ptr = RelocationPtr::new_fcn(r.fcn_path);
            }
            val.set_reloc( r.ofs, r.len, std::move(ptr) );
        }

        this->m_statics.insert(std::make_pair(&s, std::move(val)));
        });

    //
    // Register overrides for functions that are hard to emulate
    //

    // Hacky implementation of the mangling rules (doesn't support generics)
    auto fmt_ident = [](::std::ostream& os, const char* i) {
        if( const auto* hash = strchr(i, '#') ) {
            auto hofs = hash - i;
            assert(hofs < 26);
            os << char('A' + hofs);
            os << strlen(i) - 1;
            os << ::std::string(i, hash-i);
            os << hash+1;
        }
        else {
            os << strlen(i) << i;
        }
        };
    auto make_simplepath = [fmt_ident](const char* crate, std::initializer_list<const char*> il) -> RcString {
        std::stringstream   ss;
        ss << "ZRG" << (end(il)-begin(il)) << "c"; fmt_ident(ss, crate);
        for(const auto* e : il) {
            ss << strlen(e);
            ss << e;
        }
        ss << "0g";
        //std::cerr << ss.str() << std::endl;
        return RcString::new_interned(ss.str().c_str());
        };
    auto push_override_std = [&](std::initializer_list<const char*> il, override_handler_t* cb) {
        m_fcn_overrides.insert(::std::make_pair( make_simplepath("std"      , il), cb));
        m_fcn_overrides.insert(::std::make_pair( make_simplepath("std#0_0_0_H2", il), cb)); // 1.19
        m_fcn_overrides.insert(::std::make_pair( make_simplepath("std#0_0_0", il), cb));
        m_fcn_overrides.insert(::std::make_pair( make_simplepath("std#0_0_0_H300", il), cb));   // 1.39
        };

    override_handler_t* cb_nop = [](auto& state, auto& ret, const auto& path, auto args){
        return true;
    };
    // SetThreadStackGuarantee
    // - Calls GetProcAddress
    override_handler_t* cb_SetThreadStackGuarantee = [](auto& state, auto& ret, const auto& path, auto args){
        ret = Value::new_i32(120);  //ERROR_CALL_NOT_IMPLEMENTED
        return true;
    };
    push_override_std( {"sys", "imp", "c", "SetThreadStackGuarantee"}, cb_SetThreadStackGuarantee );
    push_override_std( {"sys", "windows", "c", "SetThreadStackGuarantee"}, cb_SetThreadStackGuarantee );

    // Win32 Shared RW locks (no-op)
    // TODO: Emulate fully for inter-thread locks
    push_override_std( { "sys", "windows", "c", "AcquireSRWLockExclusive" }, cb_nop );
    push_override_std( { "sys", "windows", "c", "ReleaseSRWLockExclusive" }, cb_nop );

    // - No guard page needed
    override_handler_t* cb_guardpage_init = [](auto& state, auto& ret, const auto& path, auto args){
        ret = Value::with_size(16, false);
        ret.write_u64(0, 0);
        ret.write_u64(8, 0);
        return true;
    };
    push_override_std( {"sys", "imp", "thread", "guard", "init" }, cb_guardpage_init );
    push_override_std( {"sys", "unix", "thread", "guard", "init" }, cb_guardpage_init );

    // - No stack overflow handling needed
    push_override_std( {"sys", "imp", "stack_overflow", "imp", "init"}, cb_nop );
}

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
void InterpreterThread::start(const RcString& p, ::std::vector<Value> args)
{
    assert( this->m_stack.empty() );
    Value   v;
    if( this->call_path(v, HIR::Path { p }, ::std::move(args)) )
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
                HIR::TypeRef    dst_ty;
                new_val = state.borrow_value(re.val, re.type, dst_ty);
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
                            // NOTE: Ensure that this is a trivial enum (tag offset is zero, there are variants)
                            LOG_ASSERT(dt.fields.size() == 1, "");
                            LOG_ASSERT(dt.fields[0].first == 0, "");
                            LOG_ASSERT(dt.variants.size() > 0, "");
                            LOG_ASSERT(dt.tag_path.base_field == 0, "");
                            LOG_ASSERT(dt.tag_path.other_indexes.empty(), "");
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

                    // TODO: Stop treating the relocation as hidden information? Just use different pointers instead
                    // - Each allocation has its own address range (track the ranges when an allocation is created/released)

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
                            res = (reloc_l < reloc_r ? -1 : 1);
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
                        // TODO: Only propagate the allocation if the mask was of the high bits?
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
                        val_l.get().subtract( val_r.get() );
                        if( auto r_l = v_l.get_relocation(0) )
                        {
                            if( auto r_r = v_r.get_relocation(0) )
                            {
                                // Pointer difference, no relocation in output
                                if( r_l != r_r ) {
                                    LOG_DEBUG("Different relocations: " << r_l << " and " << r_r);
                                    if( r_l < r_r ) {
                                        // Subtraction should result in a negative value (a large negative?)
                                        // - Bias by `-r_r.size()`
                                        auto ofs = (r_l.get_size() + 1 + 0x1000-1) & ~(0x1000-1);
                                        val_l.get().add_imm(-static_cast<int64_t>(ofs));
                                    }
                                    else {
                                        // - Bias by `r_r.size()`
                                        auto ofs = (r_r.get_size() + 1 + 0x1000-1) & ~(0x1000-1);
                                        val_l.get().add_imm(static_cast<int64_t>(ofs));
                                    }
                                }
                                else {
                                    LOG_DEBUG("Equal relocations: " << r_l << " and " << r_r);
                                }
                            }
                            else
                            {
                                new_val_reloc = ::std::move(r_l);
                            }
                        }
                        else
                        {
                            LOG_ASSERT(!v_r.get_relocation(0), "RHS of `-` has a relocation but LHS does not");
                        }
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
                for(size_t i = 0; i < re.count.count; i++)
                {
                    new_val.write_value(ofs, state.param_to_value(re.val));
                    ofs += stride;
                }
                } break;
            TU_ARM(se.src, UnionVariant, re) {
                // 1. Get the composite by path.
                const auto& data_ty = this->m_global.m_modtree.get_composite(re.path.n);
                auto dst_ty = ::HIR::TypeRef(&data_ty);
                new_val = Value(dst_ty);
                LOG_ASSERT(data_ty.variants.size() == 0, "UnionVariant on non-union");

                // Union, no tag
                const auto& fld = data_ty.fields.at(re.index);

                new_val.write_value(fld.first, state.param_to_value(re.val));
                LOG_DEBUG("UnionVariant " << new_val);
                }
            TU_ARM(se.src, EnumVariant, re) {
                // 1. Get the composite by path.
                const auto& data_ty = this->m_global.m_modtree.get_composite(re.path.n);
                auto dst_ty = ::HIR::TypeRef(&data_ty);
                new_val = Value(dst_ty);
                LOG_ASSERT(data_ty.variants.size() > 0, "");
                // Two cases:
                // - Data enums (tag and data)
                // - Value enums (no data)
                const auto& var = data_ty.variants.at(re.index);
                if( var.data_field != SIZE_MAX )
                {
                    LOG_ASSERT(var.data_field < data_ty.fields.size(), "Data field (" << var.data_field << ") for " << re.path << " #" << re.index << " out of range");
                    const auto& fld = data_ty.fields.at(var.data_field);

                    for(size_t i = 0; i < re.vals.size(); i++)
                    {
                        auto fld_ofs = fld.first + fld.second.composite_type().fields.at(i).first;
                        auto v = state.param_to_value(re.vals[i]);
                        LOG_DEBUG("EnumVariant - @" << fld_ofs << " = " << v);
                        new_val.write_value(fld_ofs, ::std::move(v));
                    }
                }

                if( !var.tag_data.empty() )
                {
                    ::HIR::TypeRef  tag_ty;
                    size_t tag_ofs = dst_ty.get_field_ofs(data_ty.tag_path.base_field, data_ty.tag_path.other_indexes, tag_ty);
                    LOG_ASSERT(tag_ty.get_size() == var.tag_data.size(), "");
                    new_val.write_bytes(tag_ofs, var.tag_data.data(), var.tag_data.size());
                }
                LOG_DEBUG("EnumVariant " << new_val);
                } break;
            TU_ARM(se.src, Struct, re) {
                const auto& data_ty = m_global.m_modtree.get_composite(re.path.n);

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
            LOG_DEBUG("F" << cur_frame.frame_index << " " << se.dst << " = " << new_val);
            state.write_lvalue(se.dst, ::std::move(new_val));
            } break;
        TU_ARM(stmt, Asm, se) {
            // An empty output list and empty clobber list is just a `black_box` anti-optimisation trick
            if( se.tpl == "" && se.outputs.empty() ) {
                break;
            }
            LOG_TODO(stmt);
            } break;
        case ::MIR::Statement::TAG_Asm2:
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

                auto ptr_val = Value::new_pointer_ofs(ptr_ty, ofs, ::std::move(alloc));
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

            const auto& dt = ty.composite_type();

            // Get offset, read the value.
            ::HIR::TypeRef  tag_ty;
            size_t tag_ofs = ty.get_field_ofs(dt.tag_path.base_field, dt.tag_path.other_indexes, tag_ty);

            ::std::vector<char> tag_data( tag_ty.get_size() );
            v.read_bytes(tag_ofs, const_cast<char*>(tag_data.data()), tag_data.size());
            // If there's a relocation, force down the default route
            bool has_reloc = static_cast<bool>(v.get_relocation(tag_ofs));

            // TODO: Convert the variant list into something that makes it easier to switch on.
            size_t found_target = SIZE_MAX;
            size_t default_target = SIZE_MAX;
            for(size_t i = 0; i < dt.variants.size(); i ++)
            {
                const auto& var = dt.variants[i];
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
                    // Read the value bytes
                    LOG_ASSERT(var.tag_data.size() == tag_data.size(), "Mismatch in tag data size");
                    if( ! has_reloc && ::std::memcmp(tag_data.data(), var.tag_data.data(), tag_data.size()) == 0 )
                    {
                        LOG_DEBUG("Explicit match " << i);
                        found_target = i;
                        break ;
                    }
                }
            }

            if( found_target == SIZE_MAX && default_target != SIZE_MAX )
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
            TU_ARMA(ByteString, vals) {
                auto size = v.read_usize(POINTER_SIZE);
                const char* sv_ptr = reinterpret_cast<const char*>(v.read_pointer_const(0, size));
                //auto switch_val = ::stdx::array_view(sv_ptr, sv_ptr+size);
                auto switch_val = ::stdx::string_view(sv_ptr, sv_ptr+size);

                auto it = ::std::find_if(vals.begin(), vals.end(), [&](const ::std::vector<uint8_t>& x){ return x.size() == size && memcmp(x.data(), sv_ptr, size) == 0; });
                if( it != vals.end() )
                {
                    auto idx = it - vals.begin();
                    LOG_TRACE("- b\"" << switch_val << "\" matched arm " << idx);
                    cur_frame.bb_idx = te.targets.at(idx);
                }
                else
                {
                    LOG_TRACE("- b\"" << switch_val << "\" not matched, taking default arm");
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
                auto ret_ty = state.get_lvalue_ty(te.ret_val);
                if( !this->call_intrinsic(rv, ret_ty, fe.name, fe.params, ::std::move(sub_args)) )
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
                    fcn_alloc_ptr = v.get_relocation(0);
                    LOG_ASSERT(fcn_alloc_ptr, "Calling value with no relocation - " << v);
                    LOG_ASSERT(v.read_usize(0) == fcn_alloc_ptr.get_base(), "Function pointer value invalid - " << v);
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
    // Support overriding certain functions
    {
        auto it = m_global.m_fcn_overrides.find(path.n);
        if( it != m_global.m_fcn_overrides.end() )
        {
            return it->second(*this, ret, path, args);
        }
    }

    // TODO: Support paths that reference extern functions directly (instead of needing `link_name` set)
    //if( path.n.c_str()[0] == ':' m_name == "" && path.m_trait.m_simplepath.crate_name == "#FFI" )
    //{
    //    const auto& link_abi  = path.m_trait.m_simplepath.ents.at(0);
    //    const auto& link_name = path.m_trait.m_simplepath.ents.at(1);
    //    return this->call_extern(ret, link_name, link_abi, ::std::move(args));
    //}

    const auto& fcn = m_global.m_modtree.get_function(path);

    if( fcn.external.link_name != "" )
    {
        const auto& name = fcn.external.link_name;
        if(name == "__rust_allocate"
            || name == "__rust_reallocate"
            )
        {
            // Force using the `call_extern` version
        }
        else
        {
            // Search for a function with both code and this link name
            if(const auto* ext_fcn = m_global.m_modtree.get_ext_function(name.c_str()))
            {
                LOG_DEBUG("Matched extern - `" << name << "`");
                this->m_stack.push_back(StackFrame(*ext_fcn, ::std::move(args)));
                return false;
            }
        }
        // External function!
        return this->call_extern(ret, name, fcn.external.link_abi, ::std::move(args));
    }

    this->m_stack.push_back(StackFrame(fcn, ::std::move(args)));
    return false;
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
        if( ofs != alloc.get_base() || !alloc || !alloc.is_alloc() ) {
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
            auto ity = ty.get_inner();
            // - Get thin pointer and count
            auto count = ptr.read_usize(POINTER_SIZE);
            auto ptr_vr = ptr.read_pointer_valref_mut(0, ity.get_size() * count);
            auto ofs = ptr_vr.m_offset;
            auto ptr_reloc = ptr_vr.m_alloc;

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
            if( ty.composite_type().drop_glue.n != RcString() )
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
