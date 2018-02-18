//
//
//
#include <iostream>
#include "module_tree.hpp"
#include "value.hpp"
#include <algorithm>
#include <iomanip>
#include "debug.hpp"

struct ProgramOptions
{
    ::std::string   infile;

    int parse(int argc, const char* argv[]);
};

Value MIRI_Invoke(ModuleTree& modtree, ::HIR::Path path, ::std::vector<Value> args);
Value MIRI_Invoke_Extern(const ::std::string& link_name, const ::std::string& abi, ::std::vector<Value> args);

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

    ::std::vector<Value>    args;
    args.push_back(::std::move(val_argc));
    args.push_back(::std::move(val_argv));
    auto rv = MIRI_Invoke( tree, tree.find_lang_item("start"), ::std::move(args) );
    ::std::cout << rv << ::std::endl;

    return 0;
}

Value MIRI_Invoke(ModuleTree& modtree, ::HIR::Path path, ::std::vector<Value> args)
{
    TRACE_FUNCTION_R(path, "");

    LOG_DEBUG(path);
    const auto& fcn = modtree.get_function(path);
    for(size_t i = 0; i < args.size(); i ++)
    {
        LOG_DEBUG("- Argument(" << i << ") = " << args[i]);
    }

    if( fcn.external.link_name != "" )
    {
        // External function!
        return MIRI_Invoke_Extern(fcn.external.link_name, fcn.external.link_abi, ::std::move(args));
    }

    ::std::vector<bool> drop_flags = fcn.m_mir.drop_flags;

    struct State
    {
        ModuleTree& modtree;
        const Function& fcn;
        Value   ret;
        ::std::vector<Value>    args;
        ::std::vector<Value>    locals;

        State(ModuleTree& modtree, const Function& fcn, ::std::vector<Value> args):
            modtree(modtree),
            fcn(fcn),
            ret(fcn.ret_ty),
            args(::std::move(args))
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
                LOG_DEBUG("Field - " << composite_ty);
                size_t inner_ofs;
                ty = composite_ty.get_field(e.field_index, inner_ofs);
                base_val.m_offset += inner_ofs;
                return base_val;
                }
            TU_ARM(lv, Downcast, e) {
                ::HIR::TypeRef  composite_ty;
                auto base_val = get_value_and_type(*e.val, composite_ty);

                size_t inner_ofs;
                ty = composite_ty.get_field(e.variant_index, inner_ofs);
                LOG_TODO("Read from Downcast - " << lv);
                base_val.m_offset += inner_ofs;
                return base_val;
                }
            TU_ARM(lv, Deref, e) {
                ::HIR::TypeRef  ptr_ty;
                auto val = get_value_and_type(*e.val, ptr_ty);
                LOG_ASSERT(val.m_size == POINTER_SIZE, "Deref of a value that isn't a pointer-sized value");
                // There MUST be a relocation at this point with a valid allocation.
                auto& val_alloc = val.m_alloc ? val.m_alloc : val.m_value->allocation;
                LOG_ASSERT(val_alloc, "Deref of a value with no allocation (hence no relocations)");
                LOG_TRACE("Deref " << val_alloc.alloc());
                auto alloc = val_alloc.alloc().get_relocation(val.m_offset);
                LOG_ASSERT(alloc, "Deref of a value with no relocation");
                size_t ofs = val.read_usize(0);
                ty = ptr_ty.get_inner();
                return ValueRef(::std::move(alloc), ofs, ty.get_size());
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
                throw ::std::runtime_error("BUG: Constant::Const in mmir");
                } break;
            TU_ARM(c, Bytes, ce) {
                throw ::std::runtime_error("TODO: Constant::Bytes");
                } break;
            TU_ARM(c, StaticString, ce) {
                throw ::std::runtime_error("TODO: Constant::StaticString");
                } break;
            TU_ARM(c, ItemAddr, ce) {
                // Create a value with a special backing allocation of zero size that references the specified item.
                if( const auto* fn = modtree.get_function_opt(ce) ) {
                    return Value::new_fnptr(ce);
                }
                throw ::std::runtime_error("TODO: Constant::ItemAddr");
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
    } state { modtree, fcn, ::std::move(args) };

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
                    LOG_DEBUG("- alloc=" << alloc << " (" << alloc.alloc() << ")");
                    size_t ofs = src_base_value.m_offset;
                    src_ty.wrappers.insert(src_ty.wrappers.begin(), TypeWrapper { TypeWrapper::Ty::Borrow, static_cast<size_t>(re.type) });

                    new_val = Value(src_ty);
                    // ^ Pointer value
                    new_val.write_usize(0, ofs);
                    // - Add the relocation after writing the value (writing clears the relocations)
                    new_val.allocation.alloc().relocations.push_back(Relocation { 0, ::std::move(alloc) });
                    } break;
                TU_ARM(se.src, SizedArray, re) {
                    throw "TODO";
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
                                throw "TODO";
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
                        // TODO: MUST be a thin pointer

                        // TODO: MUST be an integer (usize only?)
                        if( re.type != RawType::USize ) {
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
                            case RawType::USize:    throw "TODO"; /*dst_val = src_value.read_usize();*/   break;
                            case RawType::ISize:    throw "TODO"; /*dst_val = src_value.read_isize();*/   break;
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
                            throw "TODO";
                        case RawType::Char:
                            throw "TODO";
                        case RawType::USize:
                        case RawType::ISize:
                        case RawType::U8:
                        case RawType::I8:
                        case RawType::U16:
                        case RawType::I16:
                        case RawType::U32:
                        case RawType::I32:
                        case RawType::U64:
                        case RawType::I64:
                        case RawType::U128:
                        case RawType::I128:
                            throw "TODO";
                        }
                    }
                    } break;
                TU_ARM(se.src, BinOp, re) {
                    LOG_TODO("Handle BinOp - " << se.src);
                    } break;
                TU_ARM(se.src, UniOp, re) {
                    throw "TODO";
                    } break;
                TU_ARM(se.src, DstMeta, re) {
                    throw "TODO";
                    } break;
                TU_ARM(se.src, DstPtr, re) {
                    throw "TODO";
                    } break;
                TU_ARM(se.src, MakeDst, re) {
                    throw "TODO";
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
                    throw "TODO";
                    } break;
                TU_ARM(se.src, Variant, re) {
                    // 1. Get the composite by path.
                    const auto& data_ty = state.modtree.get_composite(re.path);
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
                    } break;
                TU_ARM(se.src, Struct, re) {
                    throw "TODO";
                    } break;
                }
                LOG_DEBUG("- " << new_val);
                state.write_lvalue(se.dst, ::std::move(new_val));
                } break;
            case ::MIR::Statement::TAG_Asm:
                throw "TODO";
                break;
            case ::MIR::Statement::TAG_Drop:
                throw "TODO";
                break;
            case ::MIR::Statement::TAG_SetDropFlag:
                throw "TODO";
                break;
            case ::MIR::Statement::TAG_ScopeEnd:
                throw "TODO";
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
            return state.ret;
        TU_ARM(bb.terminator, If, _te)
            LOG_TODO("Terminator::If");
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
            if( te.fcn.is_Intrinsic() )
            {
                const auto& fe = te.fcn.as_Intrinsic();
                if( fe.name == "atomic_store" )
                {
                    const auto& ptr_param = te.args.at(0);
                    const auto& val_param = te.args.at(1);

                    ::HIR::TypeRef  ptr_ty;
                    auto val = state.param_to_value(ptr_param, ptr_ty);
                    LOG_ASSERT(val.size() == POINTER_SIZE, "atomic_store of a value that isn't a pointer-sized value");

                    // There MUST be a relocation at this point with a valid allocation.
                    LOG_ASSERT(val.allocation, "Deref of a value with no allocation (hence no relocations)");
                    LOG_TRACE("Deref " << val.allocation.alloc());
                    auto alloc = val.allocation.alloc().get_relocation(0);
                    LOG_ASSERT(alloc, "Deref of a value with no relocation");

                    // TODO: Atomic side of this?
                    size_t ofs = val.read_usize(0);
                    auto ty = ptr_ty.get_inner();
                    alloc.alloc().write_value(ofs, state.param_to_value(val_param));
                }
                else 
                {
                    LOG_TODO("Terminator::Call - intrinsic \"" << fe.name << "\"");
                }
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
                    LOG_ASSERT(alloc_ptr, "");
                    auto& fcn_alloc_ptr = alloc_ptr.alloc().get_relocation(v.m_offset);
                    LOG_ASSERT(fcn_alloc_ptr, "");
                    fcn_p = &fcn_alloc_ptr.fcn();
                }

                ::std::vector<Value>    sub_args; sub_args.reserve(te.args.size());
                for(const auto& a : te.args)
                {
                    sub_args.push_back( state.param_to_value(a) );
                }
                ::std::cout << "Call " << *fcn_p << ::std::endl;
                MIRI_Invoke(modtree, *fcn_p, ::std::move(sub_args));
            }
            bb_idx = te.ret_block;
            } continue;
        }
        throw "";
    }

    throw "";
}
Value MIRI_Invoke_Extern(const ::std::string& link_name, const ::std::string& abi, ::std::vector<Value> args)
{
    if( link_name == "AddVectoredExceptionHandler" )
    {
        LOG_DEBUG("Call `AddVectoredExceptionHandler` - Ignoring and returning non-null");
        auto rv = Value(::HIR::TypeRef(RawType::USize));
        rv.write_usize(0, 1);
        return rv;
    }
    else
    {
        LOG_TODO("Call external function " << link_name);
    }
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
