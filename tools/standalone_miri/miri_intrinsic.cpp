/*
* mrustc Standalone MIRI
* - by John Hodge (Mutabah)
*
* miri_intrinsic.cpp
* - Interpreter core - Intrinsics
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

bool InterpreterThread::call_intrinsic(Value& rv, const HIR::TypeRef& ret_ty, const RcString& name, const ::HIR::PathParams& ty_params, ::std::vector<Value> args)
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
        rv.write_ptr_ofs(0*POINTER_SIZE, 0, RelocationPtr::new_string(&it->second));
        rv.write_usize(1*POINTER_SIZE, it->second.size());
    }
    else if( name == "discriminant_value" )
    {
        const auto& ty = ty_params.tys.at(0);
        ValueRef val = args.at(0).deref(0, ty);

        size_t fallback = SIZE_MAX;
        size_t found_index = SIZE_MAX;
        LOG_ASSERT(ty.inner_type == RawType::Composite, "discriminant_value " << ty);
        const auto& dt = ty.composite_type();
        for(size_t i = 0; i < dt.variants.size(); i ++)
        {
            const auto& var = dt.variants[i];
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
                size_t tag_ofs = ty.get_field_ofs(dt.tag_path.base_field, dt.tag_path.other_indexes, tag_ty);
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
        const auto& ty_T = ty_params.tys.at(0);
        auto data_ref = args.at(0).read_pointer_valref_mut(0, ty_T.get_size());
        auto& data_val = args.at(1);
        LOG_ASSERT(data_ref.m_alloc.is_alloc(), "Atomic operation with non-allocation pointer - " << data_ref);

        // TODO: Atomic side of this?
        data_ref.m_alloc.alloc().write_value(data_ref.m_offset, ::std::move(data_val));
    }
    else if( name == "atomic_load" || name == "atomic_load_relaxed" || name == "atomic_load_acq" )
    {
        const auto& ty_T = ty_params.tys.at(0);
        auto data_ref = args.at(0).read_pointer_valref_mut(0, ty_T.get_size());
        LOG_ASSERT(data_ref.m_alloc.is_alloc(), "Atomic operation with non-allocation pointer - " << data_ref);

        // TODO: Atomic lock the allocation.
        rv = data_ref.m_alloc.alloc().read_value(data_ref.m_offset, ty_T.get_size());
    }
    else if( name == "atomic_xadd" || name == "atomic_xadd_relaxed" )
    {
        const auto& ty_T = ty_params.tys.at(0);
        auto data_ref = args.at(0).read_pointer_valref_mut(0, ty_T.get_size());
        auto v = args.at(1).read_value(0, ty_T.get_size());

        // TODO: Atomic lock the allocation.
        LOG_ASSERT(data_ref.m_alloc.is_alloc(), "Atomic operation with non-allocation pointer - " << data_ref);

        // - Result is the original value
        rv = data_ref.read_value(0, ty_T.get_size());

        auto val_l = PrimitiveValueVirt::from_value(ty_T, rv);
        const auto val_r = PrimitiveValueVirt::from_value(ty_T, v);
        val_l.get().add( val_r.get() );

        val_l.get().write_to_value( data_ref.m_alloc.alloc(), data_ref.m_offset );
    }
    else if( name == "atomic_xsub" || name == "atomic_xsub_relaxed" || name == "atomic_xsub_rel" )
    {
        const auto& ty_T = ty_params.tys.at(0);
        auto data_ref = args.at(0).read_pointer_valref_mut(0, ty_T.get_size());
        auto v = args.at(1).read_value(0, ty_T.get_size());

        // TODO: Atomic lock the allocation.
        LOG_ASSERT(data_ref.m_alloc.is_alloc(), "Atomic operation with non-allocation pointer - " << data_ref);

        // - Result is the original value
        rv = data_ref.read_value(0, ty_T.get_size());

        auto val_l = PrimitiveValueVirt::from_value(ty_T, rv);
        const auto val_r = PrimitiveValueVirt::from_value(ty_T, v);
        val_l.get().subtract( val_r.get() );

        val_l.get().write_to_value( data_ref.m_alloc.alloc(), data_ref.m_offset );
    }
    else if( name == "atomic_xchg" || name == "atomic_xchg_acqrel"  )
    {
        const auto& ty_T = ty_params.tys.at(0);
        auto data_ref = args.at(0).read_pointer_valref_mut(0, ty_T.get_size());
        const auto& new_v = args.at(1);

        rv = data_ref.read_value(0, new_v.size());
        LOG_ASSERT(data_ref.m_alloc.is_alloc(), "Atomic operation with non-allocation pointer - " << data_ref);
        data_ref.m_alloc.alloc().write_value( data_ref.m_offset, new_v );
    }
    else if( name == "atomic_cxchg" || name == "atomic_cxchg_acq" )
    {
        const auto& ty_T = ty_params.tys.at(0);
        const auto& ret_dt = ret_ty.composite_type();
        LOG_ASSERT(ret_dt.fields.size() == 2, "Return type of `atomic_cxchg` invalid");
        LOG_ASSERT(ret_dt.fields[0].second == ty_T, "Return type of `atomic_cxchg` invalid");
        //LOG_ASSERT(ret_dt.fields[1].second == HIR::CoreType::Bool);
        // TODO: Get a ValueRef to the target location
        auto data_ref = args.at(0).read_pointer_valref_mut(0, ty_T.get_size());
        const auto& old_v = args.at(1);
        const auto& new_v = args.at(2);
        rv = Value( ret_ty );
        rv.write_value(ret_dt.fields.at(0).first, data_ref.read_value(0, old_v.size()));
        LOG_DEBUG("> *ptr = " << data_ref);
        bool success = data_ref.compare(0, old_v.data_ptr(), old_v.size());
        if( success == true ) {
            data_ref.m_alloc.alloc().write_value( data_ref.m_offset, new_v );
        }
        rv.write_u8( ret_dt.fields.at(1).first, success ? 1 : 0 );
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
    else if( name == "offset" ) // Ensures pointer validity, pointer should never wrap around
    {
        auto data_ref = args.at(0).read_pointer_valref_mut(0, 0);

        auto& ofs_val = args.at(1);

        auto delta_counts = ofs_val.read_usize(0);
        auto ty_size = ty_params.tys.at(0).get_size();
        LOG_DEBUG("\"offset\": " << data_ref.m_alloc << " 0x" << ::std::hex << data_ref.m_offset << " + 0x" << delta_counts << " * 0x" << ty_size);
        auto new_ofs = data_ref.m_offset + delta_counts * ty_size;
        if(POINTER_SIZE != 8) {
            new_ofs &= 0xFFFFFFFF;
        }

        rv = ::std::move(args.at(0));
        rv.write_ptr_ofs(0, new_ofs, data_ref.m_alloc);
    }
    else if( name == "arith_offset" )   // Doesn't check validity, and allows wrapping
    {
        auto ptr_alloc = args.at(0).get_relocation(0);
        auto ptr_ofs = args.at(0).read_usize(0);
        auto& ofs_val = args.at(1);

        auto delta_counts = ofs_val.read_usize(0);
        auto new_ofs = ptr_ofs + delta_counts * ty_params.tys.at(0).get_size();
        if(POINTER_SIZE != 8) {
            new_ofs &= 0xFFFFFFFF;
        }

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
    else if( name == "ptr_guaranteed_eq" ) {
        bool is_eq = true;
        is_eq &= args.at(0).read_usize(0) == args.at(1).read_usize(0);
        is_eq &= args.at(0).get_relocation(0) == args.at(1).get_relocation(0);
        rv = Value( ret_ty );
        rv.write_u8(0, is_eq ? 1 : 0);
    }
    else if( name == "ptr_guaranteed_ne" ) {
        bool is_eq = true;
        is_eq &= args.at(0).read_usize(0) != args.at(1).read_usize(0);
        is_eq &= args.at(0).get_relocation(0) != args.at(1).get_relocation(0);
        rv = Value( ret_ty );
        rv.write_u8(0, is_eq ? 1 : 0);
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
    else if( name == "forget" )
    {
        // Nothing needs to be done, this just stops the destructor from running.
    }
    else if( name == "try" )
    {
        auto fcn_path = args.at(0).get_relocation(0).fcn();
        auto arg = args.at(1);
        auto panic_arg = args.at(2);

        ::std::vector<Value>    sub_args;
        sub_args.push_back( ::std::move(arg) );

        this->m_stack.push_back(StackFrame::make_wrapper([=](Value& out_rv, Value /*rv*/)mutable->bool{
            if( m_thread.panic_active )
            {
                assert(m_thread.panic_count > 0);
                m_thread.panic_active = false;
                m_thread.panic_count --;
                if( TARGETVER_MOST_1_39 ) {
                    auto out_panic_value = panic_arg.read_pointer_valref_mut(0, POINTER_SIZE);
                    LOG_ASSERT(m_thread.panic_value.size() == out_panic_value.m_size, "Panic value " << m_thread.panic_value << " doesn't fit in " << out_panic_value);
                    out_panic_value.m_alloc.alloc().write_value( out_panic_value.m_offset, ::std::move(m_thread.panic_value) );
                }
                else {
                    LOG_TODO("Handle panic value for 1.54+ (`try` takes a closure)");
                }
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
        bool didnt_overflow = lhs.get().add( rhs.get() ) == OverflowType::None;

        // Get return type - a tuple of `(T, bool,)`
        const auto& dty = ret_ty.composite_type();

        rv = Value(::HIR::TypeRef(&dty));
        lhs.get().write_to_value(rv, dty.fields[0].first);
        rv.write_u8( dty.fields[1].first, didnt_overflow ? 0 : 1 ); // Returns true if overflow happened
    }
    else if( name == "sub_with_overflow" )
    {
        const auto& ty = ty_params.tys.at(0);

        auto lhs = PrimitiveValueVirt::from_value(ty, args.at(0));
        auto rhs = PrimitiveValueVirt::from_value(ty, args.at(1));
        bool didnt_overflow = lhs.get().subtract( rhs.get() ) == OverflowType::None;

        // Get return type - a tuple of `(T, bool,)`
        const auto& dty = ret_ty.composite_type();

        rv = Value(::HIR::TypeRef(&dty));
        lhs.get().write_to_value(rv, dty.fields[0].first);
        rv.write_u8( dty.fields[1].first, didnt_overflow ? 0 : 1 ); // Returns true if overflow happened
    }
    else if( name == "mul_with_overflow" )
    {
        const auto& ty = ty_params.tys.at(0);

        auto lhs = PrimitiveValueVirt::from_value(ty, args.at(0));
        auto rhs = PrimitiveValueVirt::from_value(ty, args.at(1));
        bool didnt_overflow = lhs.get().multiply( rhs.get() ) == OverflowType::None;

        // Get return type - a tuple of `(T, bool,)`
        const auto& dty = ret_ty.composite_type();

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
    else if( name == "overflowing_sub" || name == "wrapping_sub" )
    {
        const auto& ty = ty_params.tys.at(0);

        auto lhs = PrimitiveValueVirt::from_value(ty, args.at(0));
        auto rhs = PrimitiveValueVirt::from_value(ty, args.at(1));
        lhs.get().subtract( rhs.get() );
        // TODO: Overflowing part

        rv = Value(ty);
        lhs.get().write_to_value(rv, 0);
    }
    else if( name == "overflowing_add" || name == "wrapping_add"  )
    {
        const auto& ty = ty_params.tys.at(0);

        auto lhs = PrimitiveValueVirt::from_value(ty, args.at(0));
        auto rhs = PrimitiveValueVirt::from_value(ty, args.at(1));
        lhs.get().add( rhs.get() );

        rv = Value(ty);
        lhs.get().write_to_value(rv, 0);
    }
    // Unchecked arithmatic
    else if( name == "unchecked_sub" )
    {
        const auto& ty = ty_params.tys.at(0);

        auto lhs = PrimitiveValueVirt::from_value(ty, args.at(0));
        auto rhs = PrimitiveValueVirt::from_value(ty, args.at(1));
        lhs.get().subtract( rhs.get() );

        rv = Value(ty);
        lhs.get().write_to_value(rv, 0);
    }
    // Saturating arithmatic
    else if( name == "saturating_sub" || name == "saturating_add" )
    {
        const auto* suf = ::std::strchr(name.c_str(), '_')+1;
        const auto& ty = ty_params.tys.at(0);

        auto lhs = PrimitiveValueVirt::from_value(ty, args.at(0));
        auto rhs = PrimitiveValueVirt::from_value(ty, args.at(1));

        OverflowType res;
        if(strcmp(suf, "sub") == 0) {
            res = lhs.get().subtract( rhs.get() );
        }
        else if(strcmp(suf, "add") == 0) {
            res = lhs.get().add( rhs.get() );
        }
        else if(strcmp(suf, "mul") == 0) {
            res = lhs.get().multiply( rhs.get() );
        }
        else {
            LOG_TODO("");
        }

        switch( res )
        {
        case OverflowType::None:
            break;
        //case OverflowType::Max: lhs = lhs.get_max();    break;
        //case OverflowType::Min: lhs = lhs.get_min();    break;
        case OverflowType::Max:
        case OverflowType::Min:
            LOG_TODO("Saturated operation");
        }

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
    // cttz = CounT POPulated
    else if( name == "ctpop" )
    {
        const auto& ty_T = ty_params.tys.at(0);
        auto v_inner = PrimitiveValueVirt::from_value(ty_T, args.at(0));
        auto v = v_inner.get().as_u128();
        unsigned n = 0;
        for(size_t i = ty_T.get_size()*8; i--;)
        {
            n += ((v & 1) == 0 ? 0 : 1);
            v = v >> static_cast<uint8_t>(1);
        }
        rv = Value( HIR::TypeRef(RawType::USize) );
        rv.write_usize(0, n);
    }
    // ----
    // Hints
    // ----
    else if( name == "unlikely" || name == "likely" )
    {
        rv = std::move(args.at(0));
    }
    else if( name == "panic_if_uninhabited" || name == "assert_inhabited" )
    {
        //LOG_ASSERT(ty_params.tys.at(0).get_size(0) != SIZE_MAX, "");
    }
    // ----
    // Track caller
    // ----
    else if( name == "caller_location" )
    {
        auto t = HIR::TypeRef(&m_global.m_modtree.get_composite("ZRG2cE9core0_0_05panic8Location0g"));
        auto v = Value(t);
        v.ensure_allocation();
        auto& a = v.m_inner.alloc.alloc;
        rv = Value(t.wrapped(TypeWrapper::Ty::Borrow, 0));
        rv.write_ptr(0, 0x1000, RelocationPtr::new_alloc(a));
    }
    else
    {
        LOG_TODO("Call intrinsic \"" << name << "\"" << ty_params);
    }
    return true;
}