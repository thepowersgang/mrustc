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

namespace {
    struct ResolveState
    {
        const Span& sp;
        const AST::Crate& crate;
        std::vector<const AST::Module*> antirecurse_stack;

        ResolveState(const Span& span, const AST::Crate& crate):
            sp(span),
            crate(crate)
        {
        }

        ResolveModuleRef get_module(const ::AST::Path& base_path, const AST::Path& path, bool ignore_last, ::AST::Path* out_path)
        {
            const auto& base_nodes = base_path.nodes();
            int node_offset = 0;
            const AST::Module*  mod = nullptr;
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
                const auto& name = e.nodes.front().name();
                // Look up in stack of anon modules
                size_t i = 0;
                do {
                    // Get a reference to the module, given the current path
                    const auto& start_mod = this->get_mod_by_true_path(base_nodes, base_nodes.size() - i);

                    // Find the top of the path in that namespace
                    auto real_mod = this->get_source_module_for_name(start_mod, name, ResolveNamespace::Namespace);
                    TU_MATCH_HDRA( (real_mod), {)
                    TU_ARMA(Ast, mod_ptr) {
                        if( e.nodes.size() > 1 )
                        {
                            for(const auto& i : mod_ptr->m_items)
                            {
                                // What about `cfg()`?
                                if( i->name == name )
                                {
                                    ASSERT_BUG(sp, i->data.is_Module(), "Front of " << path << " not a module");
                                    return get_module_ast(i->data.as_Module(), path, 1, ignore_last, out_path);
                                }
                            }
                            BUG(sp, "get_source_module_for_name returned true (AST) but not found");
                        }
                        else
                        {
                            return ResolveModuleRef::make_Ast(mod_ptr);
                        }
                        }
                    TU_ARMA(Hir, mod_ptr) {
                        for(const auto& i : mod_ptr->m_mod_items)
                        {
                            if( i.first == name )
                            {
                                return get_module_hir(i.second->ent.as_Module(), path, 1, ignore_last, out_path);
                            }
                        }
                        BUG(sp, "get_source_module_for_name returned true (HIR) but not found");
                        }
                    TU_ARMA(None, e) {
                        // Not found in this module, keep searching
                        }
                    }

                    i += 1;
                } while( i < base_nodes.size() && base_nodes[base_nodes.size() - i].name().c_str()[0] == '#' );

                // If not found, and 2018 - look for a crate
                if( crate.m_edition >= AST::Edition::Rust2018 )
                {
                    DEBUG("Trying implicit externs for " << name);
                    DEBUG(FmtLambda([&](std::ostream& os) { for(const auto& v : crate.m_extern_crates) os << " " << v.first;})); 
                    auto ec_it = AST::g_implicit_crates.find(name);
                    if(ec_it != AST::g_implicit_crates.end())
                    {
                        const auto& ec = crate.m_extern_crates.at(ec_it->second);
                        DEBUG("Implicitly imported crate");
                        return get_module_hir(ec.m_hir->m_root_module, path, 1, ignore_last, out_path);
                    }
                }
                DEBUG("Not found");
                return ResolveModuleRef();
                }

            // Simple logic
            TU_ARMA(Self, e) {
                DEBUG("Self " << path);
                ASSERT_BUG(sp, !e.nodes.empty(), "");
                // Look up within the non-anon module
                TODO(sp, "");
                }
            TU_ARMA(Super, e) {
                DEBUG("Super " << path);
                ASSERT_BUG(sp, !e.nodes.empty(), "");
                // Pop current non-anon module, then look up in anon modules
                TODO(sp, "");
                }
            TU_ARMA(Absolute, e) {
                DEBUG("Absolute " << path);
                if( e.crate == "" || e.crate == crate.m_crate_name ) {
                    return get_module_ast(crate.m_root_module, path, 0, ignore_last, out_path);
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

        ResolveModuleRef get_module_ast(const AST::Module& start_mod, const AST::Path& path, size_t start_offset, bool ignore_last, ::AST::Path* out_path)
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

                        if(i->data.is_Module())
                        {
                            mod = &i->data.as_Module();
                            found = true;
                            break;
                        }

                        TODO(sp, "Unexpected item type for " << name << " in module " << mod->path() << " for " << path);
                    }
                }
                if(found)
                {
                    break;
                }
                BUG(sp, "Unable to find " << name << " in module " << mod->path() << " for " << path);
            }
            if(out_path)
            {
                *out_path = AST::Path(mod->path());
            }
            return ResolveModuleRef(mod);
        }
        ResolveModuleRef get_module_hir(const HIR::Module& start_mod, const AST::Path& path, size_t start_offset, bool ignore_last, ::AST::Path* out_path)
        {
            TRACE_FUNCTION_F("path=" << path << ", start_offset=" << start_offset << ", ignore_last=" << ignore_last);
            const HIR::Module* mod = &start_mod;
            for(size_t i = start_offset; i < path.nodes().size() - (ignore_last ? 1 : 0); i ++)
            {
                const auto& name = path.nodes()[i].name();
                // Find the module for this node
                auto it = mod->m_mod_items.find(name);
                if( it == mod->m_mod_items.end() ) {
                    DEBUG(name << " Not Found");
                    return ResolveModuleRef();
                }
                if( !it->second->ent.is_Module() ) {
                    DEBUG(name << " Not Module");
                    return ResolveModuleRef();
                }
                mod = &it->second->ent.as_Module();
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
                    BUG(sp, "Unable to find component " << tgt_name << " of " << base_nodes << " in module " << mod->path());
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

        //ResolveItemRef find_item(const AST::Module& mod, const RcString& name, ResolveNamespace ns, ::AST::Path* out_path=nullptr)
        ResolveModuleRef get_source_module_for_name(const AST::Module& mod, const RcString& name, ResolveNamespace ns, ::AST::Path* out_path=nullptr)
        {
            TRACE_FUNCTION_F("Looking for " << name << " in " << mod.path());
            if( mod.m_index_populated )
            {
                TODO(sp, "Look up in index");
            }
            
            if(ns == ResolveNamespace::Macro )
            {
                for(const auto& mac : mod.macro_imports_res())
                {
                    if(mac.name == name) {
                        // TODO: What about macro re-exports a builtin?
                        return ResolveModuleRef(&mod);
                    }
                }
            }

            bool pushed = false;
            for(const auto& i : mod.m_items)
            {
                //DEBUG(i.name << " " << i.data.tag_str());
                // What about `cfg()`?
                if( matching_namespace(i->data, ns) && i->name == name )
                {
                    return ResolveModuleRef(&mod);
                }

                if(const auto* use_stmt = i->data.opt_Use())
                {
                    for(const auto& e : use_stmt->entries)
                    {
                        if( e.name == name )
                        {
                            // TODO:
                            // - Push module to a stack
                            // - Outer recurse
                            // NOTE: Should update `out_path`
                            TODO(sp, "Look through use statements - " << e.path);
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
                            // TODO:
                            // - Push module to a stack
                            if( std::find(antirecurse_stack.begin(), antirecurse_stack.end(), &mod) != antirecurse_stack.end() ) {
                                DEBUG("Recursion detected, not looking at `use` statements in " << mod.path());
                                continue ;
                            }
                            struct Guard {
                                std::vector<const AST::Module*>* s;
                                Guard(std::vector<const AST::Module*>* s, const AST::Module* m):
                                    s(s)
                                {
                                    s->push_back(m);
                                }
                                ~Guard()
                                {
                                    s->pop_back();
                                }
                            } guard(&antirecurse_stack, &mod);

                            // - Outer recurse
                            //  > Get the module for this path
                            auto src_mod = this->get_module(mod.path(), e.path, false, nullptr);
                            TU_MATCH_HDRA( (src_mod), {)
                            TU_ARMA(None, _) {
                                //BUG(use_stmt->sp, "Unable to resolve use statement path " << e.path);
                                continue ;
                                }
                            TU_ARMA(Ast, sm) {
                                auto rv = get_source_module_for_name(*sm, name, ns, out_path);
                                if( !rv.is_None() )
                                {
                                    return rv;
                                }
                                // Fall through, keep searching
                                }
                            TU_ARMA(Hir, sm) {
                                switch(ns)
                                {
                                case ResolveNamespace::Macro: {
                                    auto it = sm->m_macro_items.find(name);
                                    if(it != sm->m_macro_items.end()) {
                                        return ResolveModuleRef(sm);
                                    }
                                    } break;
                                case ResolveNamespace::Namespace: {
                                    auto it = sm->m_mod_items.find(name);
                                    if(it != sm->m_mod_items.end()) {
                                        return ResolveModuleRef(sm);
                                    }
                                    } break;
                                case ResolveNamespace::Value: {
                                    auto it = sm->m_value_items.find(name);
                                    if(it != sm->m_value_items.end()) {
                                        return ResolveModuleRef(sm);
                                    }
                                    } break;
                                }
                                // Not found, fall through
                                }
                            }
                        }
                    }
                }
            }
            return ResolveModuleRef();
        }
    };
}

// TODO: Function that turns a relative path into a canonical absolute path to the containing module
// - This should check if the index has been populated, and use it if present.
// - NOTE: Can only go to the containing module, not to the item itself - `use` can end up importing disparate paths for all three namespaces.
ResolveModuleRef Resolve_Lookup_GetModule(const Span& sp, const AST::Crate& crate, const ::AST::Path& base_path, ::AST::Path path, bool ignore_last, ::AST::Path* out_path)
{
    ResolveState    rs(sp, crate);

    return rs.get_module(base_path, path, ignore_last, out_path);
}

/// Returns the source module for the specified name
// NOTE: Name resolution
ResolveModuleRef Resolve_Lookup_GetModuleForName(const Span& sp, const AST::Crate& crate, const ::AST::Path& base_path, const ::AST::Path& path, ResolveNamespace ns, ::AST::Path* out_path)
{
    TRACE_FUNCTION_F("path=" << path << " in " << base_path);
    ResolveState    rs(sp, crate);

    auto mod = rs.get_module(base_path, path, true, out_path);
    TU_MATCH_HDRA( (mod), {)
    TU_ARMA(Ast, mod_ptr) {
        auto rv = rs.get_source_module_for_name(*mod_ptr, path.nodes().back().name(), ns, out_path);
        return rv;
        }
    TU_ARMA(Hir, mod_ptr) {
        // If `get_module` provided a HIR module, then this is right?
        // - What if it's an alias? (not critical)
        return mod;
        }
    TU_ARMA(None, e) {
        BUG(sp, "Unable to find " << path << " (starting from " << base_path << ")");
        }
    }
    throw "";
}
