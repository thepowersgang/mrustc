/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * resolve/common.cpp
 * - Common core to the resolve phase
 */
#include "main_bindings.hpp"
#include "common.hpp"
#include <ast/crate.hpp>
#include <ast/ast.hpp>
#include <ast/expr.hpp>
#include <hir/hir.hpp>
#include <stdspan.hpp>  // std::span
#include <expand/cfg.hpp>
#include <synext_macro.hpp>

namespace {

    ResolveItemRef_Type as_Namespace(ResolveItemRef ir) {
        if(ir.is_None())
            return ResolveItemRef_Type::make_None({});
        return std::move(ir.as_Namespace());
    }

    struct ResolveState
    {
        const Span& sp;
        const AST::Crate& crate;

        typedef std::pair<const AST::Module*,RcString>  antirecurse_stack_ent_t;
        std::vector< antirecurse_stack_ent_t > antirecurse_stack;

        ResolveState(const Span& span, const AST::Crate& crate):
            sp(span),
            crate(crate)
        {
        }

        /// <summary>
        /// Obtain a reference to the specified module
        /// </summary>
        ResolveModuleRef get_module(const ::AST::Path& base_path, const AST::Path& path, bool ignore_last, ::AST::AbsolutePath* out_path, bool ignore_hygiene=false)
        {
            TRACE_FUNCTION_F(path << " in " << base_path << (ignore_last ? " (ignore last)" : ""));
            const auto& base_nodes = base_path.nodes();
            TU_MATCH_HDRA( (path.m_class), {)
            TU_ARMA(Invalid, e) {
                // Should never happen
                BUG(sp, "Invalid path class encountered");
                }
            TU_ARMA(Local, e) {
                // Wait, how is this already known?
                BUG(sp, "Local path class in use statement");
                }
            TU_ARMA(UFCS, e) {
                // Wait, how is this already known?
                BUG(sp, "UFCS path class in use statement");
                }

            TU_ARMA(Relative, e) {
                DEBUG("Relative " << path);
                ASSERT_BUG(sp, !e.nodes.empty(), "");
                if(!ignore_hygiene && e.hygiene.has_mod_path())
                {
                    const auto& mp = e.hygiene.mod_path();
                    if(mp.crate != "")
                    {
                        ASSERT_BUG(sp, this->crate.m_extern_crates.count(mp.crate), "Crate not loaded for " << mp);
                        const auto& crate = this->crate.m_extern_crates.at(mp.crate);
                        const HIR::Module*  mod = &crate.m_hir->m_root_module;
                        for(const auto& n : mp.ents)
                        {
                            ASSERT_BUG(sp, mod->m_mod_items.count(n), "Node `" << n << "` missing in path " << mp);
                            const auto& i = *mod->m_mod_items.at(n);
                            ASSERT_BUG(sp, i.ent.is_Module(), "Node `" << n << "` not a module in path " << mp);
                            mod = &i.ent.as_Module();
                        }
                        return get_module_hir(*mod, path, 1, ignore_last, out_path);
                    }
                    else
                    {
                        AST::Path   p("", {});
                        for(const auto& n : mp.ents)
                            p.nodes().push_back(n);
                        return get_module(p, path, ignore_last, out_path, /*ignore_hygiene=*/true);
                    }
                }
                if(e.nodes.size() == 1 && ignore_last )
                {
                    DEBUG("Ignore last");
                    return ResolveModuleRef(&this->get_mod_by_true_path(base_nodes, base_nodes.size()));
                }
                const auto& name = e.nodes.front().name();
                // Look up in stack of anon modules
                size_t i = 0;
                do {
                    // Get a reference to the module, given the current path
                    const auto& start_mod = this->get_mod_by_true_path(base_nodes, base_nodes.size() - i);

                    // Find the top of the path in that namespace
                    auto real_mod = as_Namespace(this->find_item(start_mod, name, ResolveNamespace::Namespace));
                    TU_MATCH_HDRA( (real_mod), {)
                    TU_ARMA(Ast, i_data) {
                        // TODO: What about an enum?
                        TU_MATCH_HDRA( (*i_data), {)
                        default: {
                            //ASSERT_BUG(sp, i->data.is_Module(), "Front of " << path << " not a module-alike (" << i->data.tag_str() << ")");
                            // Ignore, keep going
                            }
                        TU_ARMA(Crate, c) {
                            return get_module_hir(crate.m_extern_crates.at(c.name).m_hir->m_root_module, path, 1, ignore_last, out_path);
                            }
                        TU_ARMA(Module, m) {
                            return get_module_ast(m, path, 1, ignore_last, out_path);
                            }
                        }
                        }
                    TU_ARMA(HirRoot, hir_mod) {
                        return get_module_hir(*hir_mod, path, 1, ignore_last, out_path);
                        }
                    TU_ARMA(Hir, i_ent_ptr) {
                        ASSERT_BUG(sp, !i_ent_ptr->is_Import(), "");
                        if(i_ent_ptr->is_Enum()) {
                            DEBUG("Enum");
                            return ResolveModuleRef();
                        }

                        //if( const auto* imp = i.ent.opt_Import() ) {
                        //    ASSERT_BUG(sp, imp->path.m_components.empty(), "Expected crate path, got " << imp->path);
                        //    return get_module_hir(crate.m_extern_crates.at(imp->path.m_crate_name).m_hir->m_root_module, path, 1, ignore_last, out_path);
                        //}
                        //else {
                            ASSERT_BUG(sp, i_ent_ptr->is_Module(), "Expected Module, got " << i_ent_ptr->tag_str() << " for " << name << " in [" << base_nodes << "]");
                            return get_module_hir(i_ent_ptr->as_Module(), path, 1, ignore_last, out_path);
                        //}
                        }
                    TU_ARMA(None, e) {
                        // Not found in this module, keep searching
                        DEBUG("Keep searching (" << i << "/" << base_nodes.size() << ")");
                        }
                    }

                    i += 1;
                } while( i < base_nodes.size() && base_nodes[base_nodes.size() - i].name().c_str()[0] == '#' );

                // If not found, and 2018 - look for a crate
                if( crate.m_edition >= AST::Edition::Rust2018 )
                {
                    DEBUG("Trying implicit externs for " << name);
                    DEBUG(FmtLambda([&](std::ostream& os) { for(const auto& v : AST::g_implicit_crates) os << " " << v.first;}));
                    auto ec_it = AST::g_implicit_crates.find(name);
                    if(ec_it != AST::g_implicit_crates.end())
                    {
                        const auto& ec = crate.m_extern_crates.at(ec_it->second);
                        DEBUG("Implicitly imported crate");
                        //if( e.nodes.size() > 1 )
                        //{
                            return get_module_hir(ec.m_hir->m_root_module, path, 1, ignore_last, out_path);
                        //}
                        //else
                        //{
                        //    return ResolveModuleRef::make_ImplicitPrelude({});
                        //}
                    }
                }
                DEBUG("Not found");
                return ResolveModuleRef();
                }

            // Simple logic
            TU_ARMA(Self, e) {
                DEBUG("Self " << path);
                //ASSERT_BUG(sp, !base_nodes.empty(), "");
                // Look up within the non-anon module
                size_t i = 0;
                while( i < base_nodes.size() && base_nodes[base_nodes.size() - i - 1].name().c_str()[0] == '#' )
                {
                    i += 1;
                }
                const auto& start_mod = this->get_mod_by_true_path(base_nodes, base_nodes.size() - i);
                return get_module_ast(start_mod, path, 0, ignore_last, out_path);
                }
            TU_ARMA(Super, e) {
                DEBUG("Super " << path);
                //ASSERT_BUG(sp, !base_nodes.empty(), "Super in empty path");
                // Pop current non-anon module, then look up in anon modules
                size_t i = 0;
                while( i < base_nodes.size() && base_nodes[base_nodes.size() - i - 1].name().c_str()[0] == '#' )
                {
                    i += 1;
                }
                i += 1;
                ASSERT_BUG(sp, i <= base_nodes.size(), "");
                const auto& start_mod = this->get_mod_by_true_path(base_nodes, base_nodes.size() - i);
                return get_module_ast(start_mod, path, 0, ignore_last, out_path);
                }
            TU_ARMA(Absolute, e) {
                DEBUG("Absolute " << path);
                if( e.crate == "" || e.crate == crate.m_crate_name_real ) {
                    DEBUG("Current crate");
                    return get_module_ast(crate.m_root_module, path, 0, ignore_last, out_path);
                }
                // 2018 `::cratename::` paths
                else if( e.crate.c_str()[0] == '=' ) {
                    const char* n = e.crate.c_str()+1;
                    if( n == crate.m_crate_name_set) {
                        return get_module_ast(crate.m_root_module, path, 0, ignore_last, out_path);
                    }
                    auto ec_it = AST::g_implicit_crates.find(n);
                    if(ec_it == AST::g_implicit_crates.end())
                        return ResolveModuleRef();
                    auto ec_it2 = crate.m_extern_crates.find(ec_it->second);
                    if( ec_it2 == crate.m_extern_crates.end() ) {
                        DEBUG("Crate " << ec_it->second << " not found");
                        return ResolveModuleRef();
                    }
                    return get_module_hir(ec_it2->second.m_hir->m_root_module, path, 0, ignore_last, out_path);
                }
                else {
                    // HIR lookup (different)
                    auto ec_it = crate.m_extern_crates.find(e.crate);
                    if( ec_it == crate.m_extern_crates.end() ) {
                        DEBUG("Crate " << e.crate << " not found");
                        return ResolveModuleRef();
                    }
                    return get_module_hir(ec_it->second.m_hir->m_root_module, path, 0, ignore_last, out_path);
                }
                }
            }
            throw "";
        }

        ResolveModuleRef get_module_ast(const AST::Module& start_mod, const AST::Path& path, size_t start_offset, bool ignore_last, ::AST::AbsolutePath* out_path)
        {
            TRACE_FUNCTION_F("start_offset=" << start_offset << ", ignore_last=" << ignore_last);
            const AST::Module* mod = &start_mod;

            for(size_t i = start_offset; i < path.nodes().size() - (ignore_last ? 1 : 0); i ++)
            {
                const auto& name = path.nodes()[i].name();
                // Find the module for this node
                bool found = false;
                for(const auto& i : mod->m_items)
                {
                    if( i->name == name )
                    {
                        bool ignore = false;
                        for(const auto& a : i->attrs.m_items)
                        {
                            if( a.name() == "cfg" )
                            {
                                if( !check_cfg(sp, a) )
                                {
                                    ignore = true;
                                }
                            }
                        }
                        if(ignore)
                            continue;

                        TU_MATCH_HDRA( (i->data), { )
                        TU_ARMA(None, _e) {
                            }
                        TU_ARMA(Impl, _e) {
                            }
                        TU_ARMA(NegImpl, _e) {
                            }
                        TU_ARMA(ExternBlock, _e) {
                            }
                        TU_ARMA(MacroInv, _e) {
                            }
                        TU_ARMA(Use, _e) {
                            // Ignore for now
                            }
                        // Values - ignore
                        TU_ARMA(Static, _e) { }
                        TU_ARMA(Function, _e) { }
                        // Macro
                        TU_ARMA(Macro, _e) {
                            }
                        // Types - Return no module
                        TU_ARMA(Union, _e) {
                            return ResolveModuleRef();
                            }
                        TU_ARMA(Struct, _e) {
                            return ResolveModuleRef();
                            }
                        TU_ARMA(Trait, _e) {
                            return ResolveModuleRef();
                            }
                        TU_ARMA(TraitAlias, _e) {
                            return ResolveModuleRef();
                            }
                        TU_ARMA(Type, _e) {
                            return ResolveModuleRef();
                            }
                        TU_ARMA(Enum, _e) {
                            return ResolveModuleRef();
                            }
                        // Modules (and module-likes)
                        TU_ARMA(Module, e) {
                            mod = &e;
                            found = true;
                            }
                        TU_ARMA(Crate, e) {
                            if( e.name == "" )
                            {
                                if(out_path)
                                {
                                    *out_path = AST::AbsolutePath(e.name, {});
                                }
                                return ResolveModuleRef(&crate.m_root_module);
                            }
                            ASSERT_BUG(sp, crate.m_extern_crates.count(e.name) != 0, "Cannot find crate `" << e.name << "`");
                            if(out_path)
                            {
                                *out_path = AST::AbsolutePath(e.name, {});
                            }
                            return ResolveModuleRef(&crate.m_extern_crates.at(e.name).m_hir->m_root_module);
                            }
                        }
                        if(found) {
                            break;
                        }

                    }
                }
                if(found)
                {
                    break;
                }
                //BUG(sp, "Unable to find " << name << " in module " << mod->path() << " for " << path);
                return ResolveModuleRef();
            }
            if(out_path)
            {
                *out_path = mod->path();
            }
            return ResolveModuleRef(mod);
        }
        ResolveModuleRef get_module_hir(const HIR::Module& start_mod, const AST::Path& path, size_t start_offset, bool ignore_last, ::AST::AbsolutePath* out_path)
        {
            TRACE_FUNCTION_F("path=" << path << ", start_offset=" << start_offset << ", ignore_last=" << ignore_last);
            const HIR::Module* mod = &start_mod;
            for(size_t i = start_offset; i < path.nodes().size() - (ignore_last ? 1 : 0); i ++)
            {
                const auto& name = path.nodes()[i].name();
                // Find the module for this node
                auto it = mod->m_mod_items.find(name);
                if( it == mod->m_mod_items.end() || !it->second->publicity.is_global() ) {
                    DEBUG(name << " Not Found");
                    return ResolveModuleRef();
                }
                const auto* ti = &it->second->ent;
                if(const auto* imp = ti->opt_Import())
                {
                    ASSERT_BUG(sp, crate.m_extern_crates.count(imp->path.m_crate_name), "Crate " << imp->path.m_crate_name << " not loaded");
                    const auto& ext_crate = *crate.m_extern_crates.at(imp->path.m_crate_name).m_hir;
                    if( imp->path.m_components.empty() ) {
                        mod = &ext_crate.m_root_module;
                        continue;
                    }
                    ti = &ext_crate.get_typeitem_by_path(sp, imp->path, /*ignore_crate*/true, /*ignore_last*/false);
                }
                TU_MATCH_HDRA( (*ti), {)
                default:
                    DEBUG(name << " Not Module, instead " << ti->tag_str());
                    return ResolveModuleRef();
                TU_ARMA(Module, m) {
                    mod = &m;
                    }
                }
            }
            if(out_path)
            {
                TODO(sp, "Get path for HIR module");
            }
            return ResolveModuleRef(mod);
        }

        const AST::Module& get_mod_by_true_path(const std::vector<AST::PathNode>& base_nodes, size_t len)
        {
            const AST::Module*  mod = &crate.m_root_module;
            for(size_t i = 0; i < len; i++)
            {
                const auto& tgt_name = base_nodes[i].name();
                if( tgt_name.c_str()[0] == '#' ) {
                    auto idx = strtol(tgt_name.c_str()+1, nullptr, 10);
                    mod = &*mod->anon_mods()[idx];
                    continue ;
                }
                const AST::Module* next_mod = nullptr;
                for(const auto& i : mod->m_items)
                {
                    if( const auto* m = i->data.opt_Module() )
                    {
                        //DEBUG(i.name);
                        if( i->name == tgt_name )
                        {
                            next_mod = m;
                            break;
                        }
                    }
                }
                if( !next_mod )
                {
                    BUG(sp, "Unable to find component `" << tgt_name << "` of [" << base_nodes << "] in module " << mod->path());
                }
                mod = next_mod;
            }
            return *mod;
        }

        static bool matching_namespace(const AST::Item& i, ResolveNamespace ns)
        {
            switch(i.tag())
            {
            case AST::Item::TAGDEAD:
                return false;
            case AST::Item::TAG_Crate:
            case AST::Item::TAG_Module:
            case AST::Item::TAG_Type:
            case AST::Item::TAG_Enum:
            case AST::Item::TAG_Union:
            case AST::Item::TAG_Trait:
            case AST::Item::TAG_TraitAlias:
                return ns == ResolveNamespace::Namespace;
            case AST::Item::TAG_Struct:
                return ns == ResolveNamespace::Namespace || (ns == ResolveNamespace::Value && !i.as_Struct().m_data.is_Struct());
            case AST::Item::TAG_Function:
            case AST::Item::TAG_Static:
                return ns == ResolveNamespace::Value;
            case AST::Item::TAG_Macro:
                return ns == ResolveNamespace::Macro;
            case AST::Item::TAG_NegImpl:
            case AST::Item::TAG_Impl:
            case AST::Item::TAG_ExternBlock:
            case AST::Item::TAG_Use:
            case AST::Item::TAG_MacroInv:
            case AST::Item::TAG_None:
                return false;
            }
            throw "";
        }

        ResolveItemRef find_item(const AST::Module& mod, const RcString& name, ResolveNamespace ns, ::AST::AbsolutePath* out_path=nullptr)
        //ResolveModuleRef get_source_module_for_name(const AST::Module& mod, const RcString& name, ResolveNamespace ns, ::AST::AbsolutePath* out_path=nullptr)
        {
            TRACE_FUNCTION_F("Looking for " << name << " in " << mod.path());
            if( mod.m_index_populated )
            {
                TODO(sp, "Look up in index");
            }

            // Prevent infinite recursion
            // - Includes the target name to only catch on nested lookups of the same name
            auto guard_ent = ::std::make_pair(&mod, name);
            if( std::count(antirecurse_stack.begin(), antirecurse_stack.end(), guard_ent) > 0 ) {
                DEBUG("Recursion detected, not looking at `use` statements in " << mod.path());
                return ResolveItemRef();
            }
            struct Guard {
                std::vector<antirecurse_stack_ent_t>& s;
                Guard(std::vector<antirecurse_stack_ent_t>& s, antirecurse_stack_ent_t e):
                    s(s)
                {
                    s.push_back( std::move(e) );
                }
                ~Guard()
                {
                    s.pop_back();
                }
            } guard(antirecurse_stack, guard_ent);
            
            if(ns == ResolveNamespace::Macro )
            {
                for(const auto& i : mod.macros())
                {
                    if(i.name == name)
                    {
                        DEBUG("Found in ast (macro)");
                        return ResolveItemRef::make_Macro( &*i.data );
                    }
                }
                for(const auto& mac : reverse(mod.macro_imports_res()))
                {
                    if(mac.name == name) {
                        // TODO: What about macro re-exports a builtin?
                        DEBUG("Found in ast (macro import)");
                        TU_MATCH_HDRA( (mac.data), { )
                        TU_ARMA(None, me)
                            BUG(sp, "macro_imports_res had a None entry");
                        TU_ARMA(MacroRules, me)
                            return ResolveItemRef_Macro(me);
                        TU_ARMA(BuiltinProcMacro, me)
                            return ResolveItemRef_Macro(me);
                        TU_ARMA(ExternalProcMacro, me)
                            return ResolveItemRef_Macro(me);
                        }
                    }
                }
            }

            for(const auto& i : mod.m_items)
            {
                //DEBUG(i.name << " " << i.data.tag_str());
                // What about `cfg()`?
                if( matching_namespace(i->data, ns) && i->name == name )
                {
                    DEBUG("Found in ast");
                    switch(ns)
                    {
                    case ResolveNamespace::Macro:
                        if(const auto* mac = i->data.opt_Macro()) {
                            return ResolveItemRef_Macro(&**mac);
                        }
                        DEBUG("- Ignoring macro");
                        break;
                    case ResolveNamespace::Namespace:
                        return ResolveItemRef_Type(&i->data);
                    case ResolveNamespace::Value:
                        return ResolveItemRef_Value(&i->data);
                    }
                }

                if(const auto* use_stmt = i->data.opt_Use())
                {
                    for(const auto& e : use_stmt->entries)
                    {
                        if( e.name == name )
                        {
                            DEBUG("Use " << e.path);

                            if( e.path.m_class.is_Absolute() && e.path.m_class.as_Absolute().crate == CRATE_BUILTINS ) {
                                const auto& pe = e.path.m_class.as_Absolute();
                                if( ns == ResolveNamespace::Macro ) {
                                    if(out_path) {
                                        out_path->crate = pe.crate;
                                        out_path->nodes = make_vec1<RcString>( RcString(pe.nodes.front().name()) );
                                    }
                                    return ResolveItemRef_Macro(Expand_FindProcMacro(pe.nodes.front().name()));
                                }
                            }

                            const auto& item_name = e.path.nodes().back().name();
                            auto tgt_mod = this->get_module(mod.path(), e.path, true, out_path);

                            TU_MATCH_HDRA( (tgt_mod), {)
                            TU_ARMA(Ast, mod_ptr) {
                                // NOTE: Recursion
                                auto rv = this->find_item(*mod_ptr, item_name, ns, out_path);
                                if(!rv.is_None())
                                {
                                    DEBUG("Found in AST use");
                                    return rv;
                                }
                                }
                            TU_ARMA(Hir, mod_ptr) {
                                // If `get_module` provided a HIR module, then this is right?
                                // - What if it's an alias? (not critical)
                                auto rv = this->find_item_hir(*mod_ptr, item_name, ns, out_path);
                                if( !rv.is_None() )
                                {
                                    DEBUG("Found in HIR use");
                                    return rv;
                                }
                                }
                            TU_ARMA(ImplicitPrelude, _e) {
                                TODO(sp, "ImplicitPrelude?");
                                }
                            TU_ARMA(None, _e) {
                                //BUG(sp, "Unable to find " << e.path << " (starting from " << mod.path() << ")");
                                // Ignore for now?
                                }
                            }
                        }
                    }
                }
            }
            for(const auto& i : mod.m_items)
            {
                if(const auto* use_stmt = i->data.opt_Use())
                {
                    for(const auto& e : use_stmt->entries)
                    {
                        if( e.name == "" )
                        {
                            DEBUG("Glob use " << e.path);

                            // - Outer recurse
                            //  > Get the module for this path
                            auto src_mod = this->get_module(mod.path(), e.path, /*ignore_last=*/false, nullptr);
                            TU_MATCH_HDRA( (src_mod), {)
                            TU_ARMA(None, _) {
                                //BUG(use_stmt->sp, "Unable to resolve use statement path " << e.path);
                                continue ;
                                }
                            TU_ARMA(ImplicitPrelude, _e) {
                                TODO(sp, "ImplicitPrelude? " << e.path);
                                }
                            TU_ARMA(Ast, sm) {
                                auto rv = find_item(*sm, name, ns, out_path);
                                if( !rv.is_None() )
                                {
                                    DEBUG("Found AST glob");
                                    return rv;
                                }
                                // Fall through, keep searching
                                }
                            TU_ARMA(Hir, sm) {
                                auto rv = this->find_item_hir(*sm, name, ns, out_path);
                                if( !rv.is_None() )
                                {
                                    DEBUG("Found HIR glob");
                                    return rv;
                                }
                                // Not found, fall through
                                }
                            }
                        }
                    }
                }
            }
            if( mod.is_anon() )
            {
                DEBUG("Recurse to parent");
                const AST::Module* m = &crate.m_root_module;
                for(size_t i = 0; i < mod.path().nodes.size() - 1; i ++)
                {
                    auto& tgt_name = mod.path().nodes[i];
                    if( tgt_name.c_str()[0] == '#' ) {
                        auto idx = strtol(tgt_name.c_str()+1, nullptr, 10);
                        m = &*m->anon_mods()[idx];
                    }
                    else {
                        m = &as_Namespace(this->find_item(*m, tgt_name, ResolveNamespace::Namespace)).as_Ast()->as_Module();
                    }
                }
                return find_item(*m, name, ns, out_path);
            }
            DEBUG("Not found");
            return ResolveItemRef::make_None({});
        }

        /// Locate the named item in HIR (resolving `Import` references too)
        ResolveItemRef find_item_hir(const HIR::Module& mod, const RcString& item_name, ResolveNamespace ns, ::AST::AbsolutePath* out_path=nullptr)
        {
            struct H {
                static const HIR::Crate& get_crate(const Span& sp, const AST::Crate& crate, const HIR::SimplePath& p) {
                    return *crate.m_extern_crates.at(p.m_crate_name).m_hir;
                }
                static const HIR::Module& get_mod_for_hir_path(const Span& sp, const AST::Crate& crate, const HIR::SimplePath& p) {
                    const auto& hir_crate = *crate.m_extern_crates.at(p.m_crate_name).m_hir;
                    return hir_crate.get_mod_by_path(sp, p, /*ignore_last*/true, /*ingore_crate*/true);
                }
            };

            // Note, `out_path` should be populated to this module's path
            switch(ns)
            {
            case ResolveNamespace::Namespace: {
                auto it = mod.m_mod_items.find(item_name);
                if( it != mod.m_mod_items.end() && it->second->publicity.is_global() ) {
                    DEBUG("Found `" << item_name << "` in HIR namespace");
                    const HIR::TypeItem*    ti;
                    if(const auto* p = it->second->ent.opt_Import()) {
                        if(out_path) {
                            out_path->crate = p->path.m_crate_name;
                            out_path->nodes = p->path.m_components;
                        }
                        const auto& ext_crate = H::get_crate(sp, crate, p->path);
                        if( p->path.m_components.empty() ) {
                            return ResolveItemRef_Type(&ext_crate.m_root_module);
                        }
                        ti = &ext_crate.get_typeitem_by_path(sp, p->path, true);
                    }
                    else {
                        if(out_path)
                            out_path->nodes.push_back(item_name);
                        ti = &it->second->ent;
                    }
                    ASSERT_BUG(sp, !ti->is_Import(), "Recursive namespace import in HIR: " << it->second->ent.as_Import().path << " pointed to " << ti->as_Import().path);
                    return ResolveItemRef_Type(ti);
                }
                } break;
            case ResolveNamespace::Value: {
                auto it = mod.m_value_items.find(item_name);
                if( it != mod.m_value_items.end() && it->second->publicity.is_global() ) {
                    DEBUG("Found `" << item_name << "` in HIR value");
                    const HIR::ValueItem*    vi;
                    if(const auto* p = it->second->ent.opt_Import()) {
                        if(out_path) {
                            out_path->crate = p->path.m_crate_name;
                            out_path->nodes = p->path.m_components;
                        }
                        vi = &H::get_crate(sp, crate, p->path).get_valitem_by_path(sp, p->path, true);
                    }
                    else {
                        if(out_path)
                            out_path->nodes.push_back(item_name);
                        vi = &it->second->ent;
                    }
                    ASSERT_BUG(sp, !vi->is_Import(), "Recursive value import in HIR: " << it->second->ent.as_Import().path << " pointed to " << vi->as_Import().path);
                    return ResolveItemRef_Value(vi);
                }
                } break;
            case ResolveNamespace::Macro: {
                auto it = mod.m_macro_items.find(item_name);
                if( it != mod.m_macro_items.end() && it->second->publicity.is_global() ) {
                    DEBUG("Found `" << item_name << "` in HIR macro");
                    const HIR::MacroItem* mi;
                    if(const auto* p = it->second->ent.opt_Import()) {
                        if(out_path) {
                            out_path->crate = p->path.m_crate_name;
                            out_path->nodes = p->path.m_components;
                        }
                        if( p->path.m_crate_name == CRATE_BUILTINS ) {
                            auto* pm = Expand_FindProcMacro(p->path.m_components.at(0));
                            // TODO: What if it's a derive?
                            if( !pm )
                                break;
                            ASSERT_BUG(sp, pm, "Unable to find builtin macro " << p->path);
                            return ResolveItemRef_Macro(pm);
                        }
                        mi = &H::get_crate(sp, crate, p->path).get_macroitem_by_path(sp, p->path, true);
                        if(const auto* p = mi->opt_Import())
                        {
                            if( p->path.m_crate_name == CRATE_BUILTINS ) {
                                auto* pm = Expand_FindProcMacro(p->path.m_components.at(0));
                                // TODO: What if it's a derive?
                                if( !pm )
                                    break;
                                ASSERT_BUG(sp, pm, "Unable to find builtin macro " << p->path);
                                return ResolveItemRef_Macro(pm);
                            }
                            // Fall throught to fail
                        }
                    }
                    else {
                        mi = &it->second->ent;
                    }
                    TU_MATCH_HDRA( (*mi), {)
                    TU_ARMA(Import, me) {
                        BUG(sp, "Recursive macro import in HIR: " << it->second->ent.as_Import().path << " pointed to " << me.path);
                        }
                    TU_ARMA(MacroRules, me) {
                        return ResolveItemRef_Macro(&*me);
                        }
                    TU_ARMA(ProcMacro, me) {
                        return ResolveItemRef_Macro(&me);
                        }
                    }
                }
                } break;
            }

            return ResolveItemRef::make_None({});
        }
    };
}

// TODO: Function that turns a relative path into a canonical absolute path to the containing module
// - This should check if the index has been populated, and use it if present.
// - NOTE: Can only go to the containing module, not to the item itself - `use` can end up importing disparate paths for all three namespaces.
ResolveModuleRef Resolve_Lookup_GetModule(const Span& sp, const AST::Crate& crate, const ::AST::Path& base_path, ::AST::Path path, bool ignore_last, ::AST::AbsolutePath* out_path)
{
    ResolveState    rs(sp, crate);

    return rs.get_module(base_path, path, ignore_last, out_path);
}

ResolveItemRef_Macro Resolve_Lookup_Macro(const Span& span, const AST::Crate& crate, const ::AST::Path& base_path, ::AST::Path path, ::AST::AbsolutePath* out_path)
{
    TRACE_FUNCTION_F("path=" << path << " in " << base_path);
    ResolveState    rs(span, crate);

    const auto& item_name = path.nodes().back().name();
    auto mod = rs.get_module(base_path, path, true, out_path);
    TU_MATCH_HDRA( (mod), {)
    TU_ARMA(Ast, mod_ptr) {
        auto rv = rs.find_item(*mod_ptr, item_name, ResolveNamespace::Macro, out_path);
        if( rv.is_None() )
            return ResolveItemRef_Macro::make_None({});
        ASSERT_BUG(span, rv.is_Macro(), rv.tag_str());
        return std::move( rv.as_Macro() );
        }
    TU_ARMA(Hir, mod_ptr) {
        auto rv = rs.find_item_hir(*mod_ptr, item_name, ResolveNamespace::Macro, out_path);
        if( rv.is_None() )
            return ResolveItemRef_Macro::make_None({});
        ASSERT_BUG(span, rv.is_Macro(), rv.tag_str());
        return std::move( rv.as_Macro() );
        }
    TU_ARMA(ImplicitPrelude, _e)
        BUG(span, "Parent module of a macro is the implicit prelude?");
    TU_ARMA(None, e) {
        return ResolveItemRef_Macro::make_None({});
        }
    }
    // Technically a bug to reach this point.
    return ResolveItemRef_Macro::make_None({});
}

/// Returns the source module for the specified name
// NOTE: Name resolution
ResolveModuleRef Resolve_Lookup_GetModuleForName(const Span& sp, const AST::Crate& crate, const ::AST::Path& base_path, const ::AST::Path& path, ResolveNamespace ns, ::AST::AbsolutePath* out_path)
{
    TRACE_FUNCTION_F("path=" << path << " in " << base_path);
    ResolveState    rs(sp, crate);

    auto mod = rs.get_module(base_path, path, true, out_path);
    TU_MATCH_HDRA( (mod), {)
    TU_ARMA(Ast, mod_ptr) {
        AST::AbsolutePath   tmp;
        if(!out_path)
            out_path = &tmp;
        auto res = rs.find_item(*mod_ptr, path.nodes().back().name(), ns, out_path);
        if(res.is_None())
            BUG(sp, "Unable to find " << path << " (starting from " << base_path << ")");

        TODO(sp, "");
        //return rv;
        }
    TU_ARMA(Hir, mod_ptr) {
        // If `get_module` provided a HIR module, then this is right?
        // - What if it's an alias? (not critical)
        return mod;
        }
    TU_ARMA(ImplicitPrelude, _e) {
        return mod;
        }
    TU_ARMA(None, e) {
        BUG(sp, "Unable to find " << path << " (starting from " << base_path << ")");
        }
    }
    throw "";
}
