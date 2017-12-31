/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/codegen.cpp
 * - Wrapper for translation
 */
#include "main_bindings.hpp"
#include "trans_list.hpp"
#include <hir/hir.hpp>
#include <mir/mir.hpp>
#include <mir/operations.hpp>
#include <algorithm>

#include "codegen.hpp"
#include "monomorphise.hpp"

void Trans_Codegen(const ::std::string& outfile, const TransOptions& opt, const ::HIR::Crate& crate, const TransList& list, bool is_executable)
{
    static Span sp;
    auto codegen = Trans_Codegen_GetGeneratorC(crate, outfile);

    // 1. Emit structure/type definitions.
    // - Emit in the order they're needed.
    for(const auto& ty : list.m_types)
    {
        if( ty.second )
        {
            codegen->emit_type_proto(ty.first);
        }
        else
        {
            TU_IFLET( ::HIR::TypeRef::Data, ty.first.m_data, Path, te,
                TU_MATCHA( (te.binding), (tpb),
                (Unbound,  throw ""; ),
                (Opaque,  throw ""; ),
                (Struct,
                    codegen->emit_struct(sp, te.path.m_data.as_Generic(), *tpb);
                    ),
                (Union,
                    codegen->emit_union(sp, te.path.m_data.as_Generic(), *tpb);
                    ),
                (Enum,
                    codegen->emit_enum(sp, te.path.m_data.as_Generic(), *tpb);
                    )
                )
            )
            codegen->emit_type(ty.first);
        }
    }
    for(const auto& ty : list.m_typeids)
    {
        codegen->emit_type_id(ty);
    }
    // Emit required constructor methods (and other wrappers)
    for(const auto& path : list.m_constructors)
    {
        // Get the item type
        // - Function (must be an intrinsic)
        // - Struct (must be a tuple struct)
        // - Enum variant (must be a tuple variant)
        const ::HIR::Module* mod_ptr = nullptr;
        if(path.m_path.m_components.size() > 1)
        {
            const auto& nse = crate.get_typeitem_by_path(sp, path.m_path, false, true);
            if(const auto* e = nse.opt_Enum())
            {
                auto var_idx = e->find_variant(path.m_path.m_components.back());
                codegen->emit_constructor_enum(sp, path, *e, var_idx);
                continue ;
            }
            mod_ptr = &nse.as_Module();
        }
        else
        {
            mod_ptr = &crate.get_mod_by_path(sp, path.m_path, true);
        }

        // Not an enum, currently must be a struct
        const auto& te = mod_ptr->m_mod_items.at(path.m_path.m_components.back())->ent;
        codegen->emit_constructor_struct(sp, path, te.as_Struct());
    }

    // 2. Emit function prototypes
    for(const auto& ent : list.m_functions)
    {
        DEBUG("FUNCTION " << ent.first);
        assert( ent.second->ptr );
        const auto& fcn = *ent.second->ptr;
        // Extern if there isn't any HIR
        bool is_extern = ! static_cast<bool>(fcn.m_code);
        if( fcn.m_code.m_mir ) {
            codegen->emit_function_proto(ent.first, fcn, ent.second->pp, is_extern);
        }
    }

    for(const auto& ent : list.m_functions)
    {
        //DEBUG("FUNCTION " << ent.first);
        assert( ent.second->ptr );
        const auto& fcn = *ent.second->ptr;
        if( fcn.m_code.m_mir ) {
        }
        else {
            // TODO: Why would an intrinsic be in the queue?
            // - If it's exported it does.
            if( fcn.m_abi == "rust-intrinsic" ) {
            }
            else {
                codegen->emit_function_ext(ent.first, fcn, ent.second->pp);
            }
        }
    }
    // VTables (may be needed by statics)
    for(const auto& ent : list.m_vtables)
    {
        const auto& trait = ent.first.m_data.as_UfcsKnown().trait;
        const auto& type = *ent.first.m_data.as_UfcsKnown().type;
        DEBUG("VTABLE " << trait << " for " << type);

        codegen->emit_vtable(ent.first, crate.get_trait_by_path(Span(), trait.m_path));
    }
    // 3. Emit statics
    for(const auto& ent : list.m_statics)
    {
        DEBUG("STATIC proto " << ent.first);
        assert(ent.second->ptr);
        const auto& stat = *ent.second->ptr;

        if( ! stat.m_value_res.is_Invalid() )
        {
            codegen->emit_static_proto(ent.first, stat, ent.second->pp);
        }
        else
        {
            codegen->emit_static_ext(ent.first, stat, ent.second->pp);
        }
    }
    for(const auto& ent : list.m_statics)
    {
        DEBUG("STATIC " << ent.first);
        assert(ent.second->ptr);
        const auto& stat = *ent.second->ptr;

        if( ! stat.m_value_res.is_Invalid() )
        {
            codegen->emit_static_local(ent.first, stat, ent.second->pp);
        }
    }


    // 4. Emit function code
    for(const auto& ent : list.m_functions)
    {
        if( ent.second->ptr && ent.second->ptr->m_code.m_mir )
        {
            const auto& path = ent.first;
            const auto& fcn = *ent.second->ptr;
            const auto& pp = ent.second->pp;
            TRACE_FUNCTION_F(path);
            DEBUG("FUNCTION CODE " << path);
            bool is_extern = ! static_cast<bool>(fcn.m_code);
            // If this is a provided trait method, it needs to be monomorphised too.
            bool is_method = ( fcn.m_args.size() > 0 && visit_ty_with(fcn.m_args[0].second, [&](const auto& x){return x == ::HIR::TypeRef("Self",0xFFFF);}) );
            if( pp.has_types() || is_method )
            {
                ::StaticTraitResolve    resolve { crate };
                auto ret_type = pp.monomorph(resolve, fcn.m_return);
                ::HIR::Function::args_t args;
                for(const auto& a : fcn.m_args)
                    args.push_back(::std::make_pair( ::HIR::Pattern{}, pp.monomorph(resolve, a.second) ));
                auto mir = Trans_Monomorphise(resolve, pp, fcn.m_code.m_mir);
                ::std::string s = FMT(path);
                ::HIR::ItemPath ip(s);
                MIR_Validate(resolve, ip, *mir, args, ret_type);
                MIR_Cleanup(resolve, ip, *mir, args, ret_type);
                MIR_Optimise(resolve, ip, *mir, args, ret_type);
                MIR_Validate(resolve, ip, *mir, args, ret_type);
                // TODO: Flag that this should be a weak (or weak-er) symbol?
                // - If it's from an external crate, it should be weak
                codegen->emit_function_code(path, fcn, ent.second->pp, is_extern,  mir);
            }
            // TODO: Detect if the function was a #[inline] function from another crate, and don't emit if that is the case?
            // - Emiting is nice, but it should be emitted as a weak symbol
            else {
                codegen->emit_function_code(path, fcn, pp, is_extern,  fcn.m_code.m_mir);
            }
        }
    }

    codegen->finalise(is_executable, opt);
}

