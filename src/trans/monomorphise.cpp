/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/monomorphise.hpp
 * - MIR monomorphisation
 */
#include "monomorphise.hpp"
#include "hir_typeck/static.hpp"
#include <mir/mir.hpp>
#include <hir/hir.hpp>
#include <mir/operations.hpp>   // Needed for post-monomorph checks and optimisations
#include <hir_conv/constant_evaluation.hpp>

namespace {
    class Cloner: public ::MIR::Cloner
    {
        const ::StaticTraitResolve& m_resolve;
        const Trans_Params& params;
    public:
        Cloner(const Span& sp, const ::StaticTraitResolve& resolve, const Trans_Params& params)
            : ::MIR::Cloner(sp)
            , m_resolve(resolve)
            , params(params)
        {}

        const HIR::TypeRef& value_generic_type(HIR::GenericRef g) const override {
            switch(g.group())
            {
            case 0:
                ASSERT_BUG(sp, g.idx() < m_resolve.impl_generics().m_values.size(),
                    "Value generic " << g << " out of bounds in impl: " << m_resolve.impl_generics().m_values.size());
                return m_resolve.impl_generics().m_values.at(g.idx()).m_type;
            case 1:
                ASSERT_BUG(sp, g.idx() < m_resolve.item_generics().m_values.size(),
                    "Value generic " << g << " out of bounds in fcn: " << m_resolve.item_generics().m_values.size());
                return m_resolve.item_generics().m_values.at(g.idx()).m_type;
            default:
                BUG(Span(), "");
            }
        }
        const Monomorphiser& monomorphiser() const override {
            return params;
        }
        const StaticTraitResolve* resolve() const override {
            return &m_resolve;
        }
    };
}

::MIR::FunctionPointer Trans_Monomorphise(const ::StaticTraitResolve& resolve, const Trans_Params& params, const ::MIR::FunctionPointer& tpl)
{
    static Span sp;
    TRACE_FUNCTION;
    assert(tpl);

    ::MIR::Function output;

    // 1. Monomorphise locals and temporaries
    output.locals.reserve( tpl->locals.size() );
    for(const auto& var : tpl->locals)
    {
        DEBUG("- _" << output.locals.size() << " (" << var << ")");
        output.locals.push_back( params.monomorph(resolve, var) );
        DEBUG(" = " << output.locals.back());
    }
    output.drop_flags = tpl->drop_flags;

    Cloner  c { sp, resolve, params };
    // 2. Monomorphise all paths
    output.blocks.reserve( tpl->blocks.size() );
    for(const auto& block : tpl->blocks)
    {
        ::std::vector< ::MIR::Statement>    statements;

        TRACE_FUNCTION_F("bb" << output.blocks.size());
        statements.reserve( block.statements.size() );
        for(const auto& stmt : block.statements)
        {
            switch( stmt.tag() )
            {
            // LAZY: These _should_ be in `clone_stmt`, but they're not needed in optimising and MIR cloning
            TU_ARM(stmt, SaveDropFlag, e) {
                statements.push_back(::MIR::Statement::make_SaveDropFlag({ e.slot.clone(), e.bit_index, e.idx }));
                } break;
            TU_ARM(stmt, LoadDropFlag, e) {
                statements.push_back(::MIR::Statement::make_LoadDropFlag({ e.idx, e.slot.clone(), e.bit_index}));
                } break;
            default:
                statements.push_back(c.clone_stmt(stmt));
                break;
            }
        }

        ::MIR::Terminator   terminator = c.clone_term(block.terminator);
        output.blocks.push_back( ::MIR::BasicBlock { mv$(statements), mv$(terminator) } );
    }

    return ::MIR::FunctionPointer( box$(output).release() );
}

/// Monomorphise all functions in a TransList
void Trans_Monomorphise_List(const ::HIR::Crate& crate, TransList& list)
{
    ::StaticTraitResolve    resolve { crate };
    
    struct Nvs: public ::HIR::Evaluator::Newval
    {
        TransList& out;
        const HIR::Crate& crate;
        unsigned count;
        ::std::vector<std::pair<HIR::SimplePath,HIR::Static*>>  added;

        Nvs(TransList& out, const HIR::Crate& crate)
            : out(out)
            , crate(crate)
            , count(0)
        {}

        ::HIR::Path new_static(::HIR::TypeRef type, EncodedLiteral value) override {
            // Ensure that the type is in enumeration (it should have been, but maybe not?)
            {
                bool found = false;
                for(const auto& v : out.m_types) {
                    if( v.first == type && !v.second ) {
                        found = true;
                        break;
                    }
                }
                if( !found ) {
                    out.m_types.push_back(::std::make_pair(type.clone(), false));
                }
            }
            auto name = RcString::new_interned(FMT("ConstEvalMonomorph#" << count));
            count ++;
            auto p = ::HIR::SimplePath(crate.m_crate_name, {name});
            auto ent = std::make_unique<HIR::VisEnt<HIR::ValueItem>>(HIR::VisEnt<HIR::ValueItem> {
                HIR::Publicity::new_global(),
                HIR::ValueItem(::HIR::Static(HIR::Linkage(), false, std::move(type), HIR::ExprPtr()))
                });
            
            {
                auto& s = ent->ent.as_Static();
                s.m_value_generated = true;
                s.m_value_res = std::move(value);
                s.m_save_literal = false;
                added.push_back(std::make_pair(p, &s));
            }
            const_cast<HIR::Module&>(crate.m_root_module).m_value_items.insert(std::make_pair(name, std::move(ent)));
            return p;
        }
    } nvs { list, crate };

    // Also do constants and statics (stored in where?)
    // - NOTE: Done in reverse order, because consteval needs used constants to be evaluated
    for(auto& ent : reverse(list.m_constants))
    {
        const auto& path = ent.first;
        const auto& pp = ent.second->pp;
        const auto& c = *ent.second->ptr;
        TRACE_FUNCTION_FR("CONSTANT " << path, "CONSTANT " << path);
        auto ty = pp.monomorph(resolve, c.m_type);
        // 1. Evaluate the constant
        auto eval = ::HIR::Evaluator { pp.sp, crate, nvs };
        eval.resolve.set_both_generics_raw(pp.gdef_impl, &c.m_params);
        MonomorphState   ms;
        ms.self_ty = pp.self_type.clone();
        ms.pp_impl = &pp.pp_impl;
        ms.pp_method = &pp.pp_method;
        DEBUG("ms = " << ms);
        try
        {
            auto new_lit = eval.evaluate_constant(path, c.m_value, ::std::move(ty), ::std::move(ms));
            // 2. Store evaluated HIR::Literal in c.m_monomorph_cache
            c.m_monomorph_cache.insert(::std::make_pair( path.clone(), ::std::move(new_lit) ));
        }
        catch(...)
        {
            // Deferred - no update
            BUG(Span(), "Exception thrown during evaluation of: " << path);
        }
    }

    for(auto& ent : list.m_statics)
    {
        const auto& path = ent.first;
        const auto& pp = ent.second->pp;
        const auto& s = *ent.second->ptr;

        if( !s.m_params.is_generic() )
        {
            continue ;
        }

        TRACE_FUNCTION_FR("STATIC " << path, "STATIC " << path);
        auto ty = pp.monomorph(resolve, s.m_type);
        // 1. Evaluate the constant
        auto eval = ::HIR::Evaluator { pp.sp, crate, nvs };
        eval.resolve.set_both_generics_raw(pp.gdef_impl, &s.m_params);
        MonomorphState   ms;
        ms.self_ty = pp.self_type.clone();
        ms.pp_impl = &pp.pp_impl;
        ms.pp_method = &pp.pp_method;
        DEBUG("ms = " << ms);
        try
        {
            auto new_lit = eval.evaluate_constant(path, s.m_value, ::std::move(ty), ::std::move(ms));
            // 2. Store evaluated HIR::Literal in s.m_monomorph_cache
            s.m_monomorph_cache.insert(::std::make_pair( path.clone(), ::std::move(new_lit) ));
        }
        catch(...)
        {
            // Deferred - no update
            BUG(Span(), "Exception thrown during evaluation of: " << path);
        }
    }

    for(auto& fcn_ent : list.m_functions)
    {
        const auto& fcn = *fcn_ent.second->ptr;
        // Trait methods (which are the only case where `Self` can exist in the argument list at this stage) always need to be monomorphised.
        bool is_method = ( fcn.m_args.size() > 0 && visit_ty_with(fcn.m_args[0].second, [&](const auto& x){return x == ::HIR::TypeRef::new_self();}) );
        bool monomorph_needed = fcn_ent.second->pp.has_types() || is_method;

        if( monomorph_needed )
        {
            const auto& path = fcn_ent.first;
            const auto& pp = fcn_ent.second->pp;
            TRACE_FUNCTION_FR("FUNCTION " << path, "FUNCTION " << path);
            ASSERT_BUG(Span(), fcn.m_code.m_mir, "No code for " << path);

            // TODO: Get the item params too
            if( fcn_ent.second->pp.pp_impl.has_params() ) {
                assert(pp.gdef_impl);
            }
            resolve.set_both_generics_raw(pp.gdef_impl, &fcn.m_params);

            auto mir = Trans_Monomorphise(resolve, fcn_ent.second->pp, fcn.m_code.m_mir);

            // TODO: Should these be moved to their own pass? Potentially not, the extra pass should just be an inlining optimise pass
            auto ret_type = pp.monomorph(resolve, fcn.m_return);
            ::HIR::Function::args_t args;
            for(const auto& a : fcn.m_args)
                args.push_back(::std::make_pair( ::HIR::Pattern{}, pp.monomorph(resolve, a.second) ));

            //::std::string s = FMT(path);
            ::HIR::ItemPath ip(path);
            MIR_Validate(resolve, ip, *mir, args, ret_type);
            MIR_Cleanup(resolve, ip, *mir, args, ret_type);
            MIR_Optimise(resolve, ip, *mir, args, ret_type, /*do_inline*/false);
            MIR_Validate(resolve, ip, *mir, args, ret_type);

            fcn_ent.second->monomorphised.ret_ty = ::std::move(ret_type);
            fcn_ent.second->monomorphised.arg_tys = ::std::move(args);
            fcn_ent.second->monomorphised.code = ::std::move(mir);
            resolve.clear_both_generics();
        }
        else
        {
            DEBUG("Non-generic: FUNCTION " << fcn_ent.first);
        }
    }

    for(auto& v : nvs.added) {
        auto* o = list.add_static(HIR::Path(v.first));
        ASSERT_BUG(Span(), o, "Generated static " << v.first << " already in TransList?");
        o->ptr = v.second;
    }
}

