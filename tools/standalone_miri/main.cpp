//
//
//
#include <iostream>
#include "module_tree.hpp"
#include "value.hpp"
#include <algorithm>
#include <iomanip>

struct ProgramOptions
{
    ::std::string   infile;

    int parse(int argc, const char* argv[]);
};

Value MIRI_Invoke(ModuleTree& modtree, ::HIR::Path path, ::std::vector<Value> args);

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
    //TRACE_FUNCTION_FR(path,path)
    const auto& fcn = modtree.get_function(path);
    for(size_t i = 0; i < args.size(); i ++)
    {
        //DEBUG(a);
        ::std::cout << "Argument(" << i << ") = " << args[i] << ::std::endl;
    }

    ::std::vector<bool> drop_flags = fcn.m_mir.drop_flags;
    ::std::vector<Value>    locals; locals.reserve( fcn.m_mir.locals.size() );

    Value   ret_val = Value(fcn.ret_ty);
    for(const auto& ty : fcn.m_mir.locals)
    {
        locals.push_back(Value(ty));
    }

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
                locals.push_back(Value(ty));
            }
        }

        Value& get_value_type_and_ofs(const ::MIR::LValue& lv, size_t& ofs, ::HIR::TypeRef& ty)
        {
            switch(lv.tag())
            {
            case ::MIR::LValue::TAGDEAD:    throw "";
            TU_ARM(lv, Return, _e) {
                ofs = 0;
                ty = fcn.ret_ty;
                return ret;
                } break;
            TU_ARM(lv, Local, e) {
                ofs = 0;
                ty = fcn.m_mir.locals.at(e);
                return locals.at(e);
                } break;
            TU_ARM(lv, Argument, e) {
                ofs = 0;
                ty = fcn.args.at(e.idx);
                return args.at(e.idx);
                } break;
            TU_ARM(lv, Static, e) {
                return modtree.get_static(e);
                } break;
            TU_ARM(lv, Index, e) {
                auto idx = read_lvalue(*e.idx).as_usize();
                ::HIR::TypeRef  array_ty;
                auto& base_val = get_value_type_and_ofs(*e.val, ofs, array_ty);
                if( array_ty.wrappers.empty() )
                    throw "ERROR";
                if( array_ty.wrappers.front().type == TypeWrapper::Ty::Array )
                {
                    ty = array_ty.get_inner();
                    ofs += ty.get_size() * idx;
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
                auto& base_val = get_value_type_and_ofs(*e.val, ofs, composite_ty);
                ::std::cout << "get_type_and_ofs: " << composite_ty << ::std::endl;
                size_t inner_ofs;
                ty = composite_ty.get_field(e.field_index, inner_ofs);
                ofs += inner_ofs;
                return base_val;
                }
            TU_ARM(lv, Downcast, e) {
                ::HIR::TypeRef  composite_ty;
                auto& base_val = get_value_type_and_ofs(*e.val, ofs, composite_ty);

                size_t inner_ofs;
                ty = composite_ty.get_field(e.variant_index, inner_ofs);
                ::std::cerr << "TODO: Read from Downcast - " << lv << ::std::endl;
                throw "TODO";
                ofs += inner_ofs;
                return base_val;
                }
            TU_ARM(lv, Deref, e) {
                //auto addr = read_lvalue(*e.val);

                ::std::cerr << "TODO: Read from deref - " << lv << ::std::endl;
                throw "TODO";
                } break;
            }
            throw "";
        }

        ::HIR::TypeRef get_lvalue_ty(const ::MIR::LValue& lv)
        {
            ::HIR::TypeRef  ty;
            size_t ofs = 0;
            get_value_type_and_ofs(lv, ofs, ty);
            return ty;
        }

        Value read_lvalue_with_ty(const ::MIR::LValue& lv, ::HIR::TypeRef& ty)
        {
            ::std::cout << "read_lvalue_with_ty: " << lv << ::std::endl;
            size_t ofs = 0;
            Value&  base_value = get_value_type_and_ofs(lv, ofs, ty);

            ::std::cout << "> read_lvalue_with_ty: " << ty << ::std::endl;

            return base_value.read_value(ofs, ty.get_size());
        }
        Value read_lvalue(const ::MIR::LValue& lv)
        {
            ::std::cout << "read_lvalue: " << lv << ::std::endl;
            ::HIR::TypeRef  ty;
            return read_lvalue_with_ty(lv, ty);
        }
        void write_lvalue(const ::MIR::LValue& lv, Value val)
        {
            ::std::cout << "write_lvaue: " << lv << ::std::endl;
            //::std::cout << "write_lvaue: " << lv << " = " << val << ::std::endl;
            ::HIR::TypeRef  ty;
            size_t ofs = 0;
            Value&  base_value = get_value_type_and_ofs(lv, ofs, ty);

            base_value.write_value(ofs, val);
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
            ::std::cout << "BB" << bb_idx << "/" << (&stmt - bb.statements.data()) << ": " << stmt << ::std::endl;
            switch(stmt.tag())
            {
            case ::MIR::Statement::TAGDEAD: throw "";
            TU_ARM(stmt, Assign, se) {
                Value   val;
                switch(se.src.tag())
                {
                case ::MIR::RValue::TAGDEAD: throw "";
                TU_ARM(se.src, Use, re) {
                    state.write_lvalue(se.dst, state.read_lvalue(re));
                    } break;
                TU_ARM(se.src, Constant, re) {
                    state.write_lvalue(se.dst, state.const_to_value(re));
                    } break;
                TU_ARM(se.src, Borrow, re) {
                    ::HIR::TypeRef  src_ty;
                    size_t ofs = 0;
                    Value&  base_value = state.get_value_type_and_ofs(re.val, ofs, src_ty);
                    if( !base_value.allocation )
                    {
                        // TODO: Need to convert this value into an allocation version
                        ::std::cerr << "TODO: RValue::Borrow - " << se.src << " - convert to non-inline" << ::std::endl;
                        throw "TODO";
                        //base_value.to_allocation();
                    }
                    ofs += base_value.meta.indirect_meta.offset;
                    src_ty.wrappers.insert(src_ty.wrappers.begin(), TypeWrapper { TypeWrapper::Ty::Borrow, static_cast<size_t>(re.type) });
                    Value new_val = Value(src_ty);
                    // ^ Pointer value
                    new_val.allocation.alloc().relocations.push_back(Relocation { 0, base_value.allocation });
                    new_val.write_bytes(0, &ofs, src_ty.get_size());
                    ::std::cerr << "TODO: RValue::Borrow - " << se.src << ::std::endl;
                    throw "TODO";
                    } break;
                TU_ARM(se.src, SizedArray, re) {
                    throw "TODO";
                    } break;
                TU_ARM(se.src, Cast, re) {
                    throw "TODO";
                    } break;
                TU_ARM(se.src, BinOp, re) {
                    throw "TODO";
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
                    size_t ofs = 0;
                    Value&  base_value = state.get_value_type_and_ofs(se.dst, ofs, dst_ty);

                    for(size_t i = 0; i < re.vals.size(); i++)
                    {
                        auto fld_ofs = dst_ty.composite_type->fields.at(i).first;
                        base_value.write_value(ofs + fld_ofs, state.param_to_value(re.vals[i]));
                    }
                    } break;
                TU_ARM(se.src, Array, re) {
                    throw "TODO";
                    } break;
                TU_ARM(se.src, Variant, re) {
                    throw "TODO";
                    } break;
                TU_ARM(se.src, Struct, re) {
                    throw "TODO";
                    } break;
                }
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

        ::std::cout << "BB" << bb_idx << "/TERM: " << bb.terminator << ::std::endl;
        switch(bb.terminator.tag())
        {
        case ::MIR::Terminator::TAGDEAD:    throw "";
        TU_ARM(bb.terminator, Incomplete, _te)
            throw ::std::runtime_error("BUG: Terminator::Incomplete hit");
        TU_ARM(bb.terminator, Diverge, _te)
            throw ::std::runtime_error("BUG: Terminator::Diverge hit");
        TU_ARM(bb.terminator, Panic, _te)
            throw ::std::runtime_error("TODO: Terminator::Panic");
        TU_ARM(bb.terminator, Goto, te)
            bb_idx = te;
            continue;
        TU_ARM(bb.terminator, Return, _te)
            return state.ret;
        TU_ARM(bb.terminator, If, _te)
            throw ::std::runtime_error("TODO: Terminator::If");
        TU_ARM(bb.terminator, Switch, _te)
            throw ::std::runtime_error("TODO: Terminator::Switch");
        TU_ARM(bb.terminator, SwitchValue, _te)
            throw ::std::runtime_error("TODO: Terminator::SwitchValue");
        TU_ARM(bb.terminator, Call, te) {
            if( te.fcn.is_Intrinsic() ) {
                throw ::std::runtime_error("TODO: Terminator::Call - intrinsic");
            }
            else {
                const ::HIR::Path* fcn_p;
                if( te.fcn.is_Path() ) {
                    fcn_p = &te.fcn.as_Path();
                }
                else {
                    ::HIR::TypeRef ty;
                    auto v = state.read_lvalue_with_ty(te.fcn.as_Value(), ty);
                    // TODO: Assert type
                    // TODO: Assert offset/content.
                    assert(v.as_usize() == 0);
                    fcn_p = &v.allocation.alloc().relocations.at(0).backing_alloc.fcn();
                }

                ::std::vector<Value>    sub_args; sub_args.reserve(te.args.size());
                for(const auto& a : te.args)
                {
                    sub_args.push_back( state.param_to_value(a) );
                }
                ::std::cout << "TODO: Call " << *fcn_p << ::std::endl;
                MIRI_Invoke(modtree, *fcn_p, ::std::move(sub_args));
            }
            throw ::std::runtime_error("TODO: Terminator::Call");
            } break;
        }
        throw "";
    }

    throw "";
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
