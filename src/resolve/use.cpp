/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * resolve/use.cpp
 * - Absolutise and check all 'use' statements
 */
#include <main_bindings.hpp>
#include <ast/crate.hpp>
#include <ast/ast.hpp>
#include <ast/expr.hpp>
#include <hir/hir.hpp>
#include <stdspan.hpp>  // std::span

enum class Lookup
{
    Any,    // Allow binding to anything
    AnyOpt, // Allow binding to anything, but don't error on failure
    Type,   // Allow only type bindings
    Value,  // Allow only value bindings
};


void Resolve_Use_Mod(const ::AST::Crate& crate, ::AST::Module& mod, ::AST::Path path, ::std::span< const ::AST::Module* > parent_modules={});
::AST::Path::Bindings Resolve_Use_GetBinding(
    const Span& span, const ::AST::Crate& crate, const ::AST::Path& source_mod_path,
    const ::AST::Path& path, ::std::span< const ::AST::Module* > parent_modules,
    bool types_only=false
    );
::AST::Path::Bindings Resolve_Use_GetBinding__ext(const Span& span, const ::AST::Crate& crate, const ::AST::Path& path,  const ::HIR::Module& hmodr, unsigned int start);


void Resolve_Use(::AST::Crate& crate)
{
    Resolve_Use_Mod(crate, crate.m_root_module, ::AST::Path("", {}));
}

// - Convert self::/super:: paths into non-canonical absolute forms
::AST::Path Resolve_Use_AbsolutisePath(const Span& span, const ::AST::Path& base_path, ::AST::Path path)
{
    TU_MATCH(::AST::Path::Class, (path.m_class), (e),
    (Invalid,
        // Should never happen
        BUG(span, "Invalid path class encountered");
        ),
    (Local,
        // Wait, how is this already known?
        BUG(span, "Local path class in use statement");
        ),
    (UFCS,
        // Wait, how is this already known?
        BUG(span, "UFCS path class in use statement");
        ),
    (Relative,
        // How can this happen?
        DEBUG("Relative " << path);
        // EVIL HACK: If the current module is an anon module, refer to the parent
        if( base_path.nodes().size() > 0 && base_path.nodes().back().name().c_str()[0] == '#' ) {
            AST::Path   np("", {});
            for( unsigned int i = 0; i < base_path.nodes().size() - 1; i ++ )
                np.nodes().push_back( base_path.nodes()[i] );
            np += path;
            return np;
        }
        else {
            return base_path + path;
        }
        ),
    (Self,
        DEBUG("Self " << path);
        // EVIL HACK: If the current module is an anon module, refer to the parent
        if( base_path.nodes().size() > 0 && base_path.nodes().back().name().c_str()[0] == '#' ) {
            AST::Path   np("", {});
            for( unsigned int i = 0; i < base_path.nodes().size() - 1; i ++ )
                np.nodes().push_back( base_path.nodes()[i] );
            np += path;
            return np;
        }
        else {
            return base_path + path;
        }
        ),
    (Super,
        DEBUG("Super " << path);
        assert(e.count >= 1);
        AST::Path   np("", {});
        if( e.count > base_path.nodes().size() ) {
            ERROR(span, E0000, "Too many `super` components");
        }
        // TODO: Do this in a cleaner manner.
        unsigned int n_anon = 0;
        // Skip any anon modules in the way (i.e. if the current module is an anon, go to the parent)
        while( base_path.nodes().size() > n_anon && base_path.nodes()[ base_path.nodes().size()-1-n_anon ].name().c_str()[0] == '#' )
            n_anon ++;
        for( unsigned int i = 0; i < base_path.nodes().size() - e.count - n_anon; i ++ )
            np.nodes().push_back( base_path.nodes()[i] );
        np += path;
        return np;
        ),
    (Absolute,
        DEBUG("Absolute " << path);
        // Leave as is
        return path;
        )
    )
    throw "BUG: Reached end of Resolve_Use_AbsolutisePath";
}

void Resolve_Use_Mod(const ::AST::Crate& crate, ::AST::Module& mod, ::AST::Path path, ::std::span< const ::AST::Module* > parent_modules)
{
    TRACE_FUNCTION_F("path = " << path);

    for(auto& use_stmt : mod.items())
    {
        if( ! use_stmt.data.is_Use() )
            continue ;
        auto& use_stmt_data = use_stmt.data.as_Use();

        const Span& span = use_stmt_data.sp;
        for(auto& use_ent : use_stmt_data.entries)
        {
            use_ent.path = Resolve_Use_AbsolutisePath(span, path, mv$(use_ent.path));
            if( !use_ent.path.m_class.is_Absolute() )
                BUG(span, "Use path is not absolute after absolutisation");

            // NOTE: Use statements can refer to _three_ different items
            // - types/modules ("type namespace")
            // - values ("value namespace")
            // - macros ("macro namespace")
            // TODO: Have Resolve_Use_GetBinding return the actual path
            use_ent.path.m_bindings = Resolve_Use_GetBinding(span, crate, path, use_ent.path, parent_modules);
            if( !use_ent.path.m_bindings.has_binding() )
            {
                ERROR(span, E0000, "Unable to resolve `use` target " << use_ent.path);
            }
            DEBUG("'" << use_ent.name << "' = " << use_ent.path);

            // - If doing a glob, ensure the item type is valid
            if( use_ent.name == "" )
            {
                TU_MATCH_DEF(::AST::PathBinding_Type, (use_ent.path.m_bindings.type), (e),
                (
                    ERROR(span, E0000, "Wildcard import of invalid item type - " << use_ent.path);
                    ),
                (Enum,
                    ),
                (Crate,
                    ),
                (Module,
                    )
                )
            }
            else
            {
            }
        }
    }

    struct NV:
        public AST::NodeVisitorDef
    {
        const AST::Crate& crate;
        ::std::vector< const AST::Module* >   parent_modules;

        NV(const AST::Crate& crate, const AST::Module& cur_module):
            crate(crate),
            parent_modules()
        {
            parent_modules.push_back( &cur_module );
        }

        void visit(AST::ExprNode_Block& node) override {
            if( node.m_local_mod ) {
                Resolve_Use_Mod(this->crate, *node.m_local_mod, node.m_local_mod->path(), this->parent_modules);

                parent_modules.push_back(&*node.m_local_mod);
            }
            AST::NodeVisitorDef::visit(node);
            if( node.m_local_mod ) {
                parent_modules.pop_back();
            }
        }
    } expr_iter(crate, mod);

    // TODO: Check that all code blocks are covered by these
    // - NOTE: Handle anon modules by iterating code (allowing correct item mappings)
    for(auto& i : mod.items())
    {
        TU_MATCH_DEF( AST::Item, (i.data), (e),
        (
            ),
        (Module,
            Resolve_Use_Mod(crate, i.data.as_Module(), path + i.name);
            ),
        (Impl,
            for(auto& i : e.items())
            {
                TU_MATCH_DEF( AST::Item, (*i.data), (e),
                (
                    ),
                (Function,
                    if( e.code().is_valid() ) {
                        e.code().node().visit( expr_iter );
                    }
                    ),
                (Static,
                    if( e.value().is_valid() ) {
                        e.value().node().visit( expr_iter );
                    }
                    )
                )
            }
            ),
        (Trait,
            for(auto& ti : e.items())
            {
                TU_MATCH_DEF( AST::Item, (ti.data), (e),
                (
                    BUG(Span(), "Unexpected item in trait - " << ti.data.tag_str());
                    ),
                (None,
                    // Deleted, ignore
                    ),
                (MacroInv,
                    // TODO: Should this already be deleted?
                    ),
                (Type,
                    ),
                (Function,
                    if(e.code().is_valid()) {
                        e.code().node().visit( expr_iter );
                    }
                    ),
                (Static,
                    if( e.value().is_valid() ) {
                        e.value().node().visit( expr_iter );
                    }
                    )
                )
            }
            ),
        (Function,
            if( e.code().is_valid() ) {
                e.code().node().visit( expr_iter );
            }
            ),
        (Static,
            if( e.value().is_valid() ) {
                e.value().node().visit( expr_iter );
            }
            )
        )
    }
}

::AST::Path::Bindings Resolve_Use_GetBinding_Mod(
        const Span& span,
        const ::AST::Crate& crate, const ::AST::Path& source_mod_path, const ::AST::Module& mod,
        const RcString& des_item_name,
        ::std::span< const ::AST::Module* > parent_modules,
        bool types_only = false
    )
{
    ::AST::Path::Bindings   rv;
    // If the desired item is an anon module (starts with #) then parse and index
    if( des_item_name.size() > 0 && des_item_name.c_str()[0] == '#' ) {
        unsigned int idx = 0;
        if( ::std::sscanf(des_item_name.c_str(), "#%u", &idx) != 1 ) {
            BUG(span, "Invalid anon path segment '" << des_item_name << "'");
        }
        ASSERT_BUG(span, idx < mod.anon_mods().size(), "Invalid anon path segment '" << des_item_name << "'");
        assert( mod.anon_mods()[idx] );
        rv.type = ::AST::PathBinding_Type::make_Module({&*mod.anon_mods()[idx], nullptr});
        return rv;
    }

    // Seach for the name defined in the module.
    for( const auto& item : mod.items() )
    {
        if( item.data.is_None() )
            continue ;

        if( item.name == des_item_name ) {
            TU_MATCH(::AST::Item, (item.data), (e),
            (None,
                // IMPOSSIBLE - Handled above
                ),
            (MacroInv,
                BUG(span, "Hit MacroInv in use resolution");
                ),
            (Macro,
                //rv.macro = ::AST::PathBinding_Macro::make_MacroRules({nullptr, e.get()});
                ),
            (Use,
                continue; // Skip for now
                ),
            (Impl,
                BUG(span, "Hit Impl in use resolution");
                ),
            (NegImpl,
                BUG(span, "Hit NegImpl in use resolution");
                ),
            (ExternBlock,
                BUG(span, "Hit Extern in use resolution");
                ),
            (Crate,
                ASSERT_BUG(span, crate.m_extern_crates.count(e.name), "Crate '" << e.name << "' not loaded");
                rv.type = ::AST::PathBinding_Type::make_Crate({ &crate.m_extern_crates.at(e.name) });
                ),
            (Type,
                rv.type = ::AST::PathBinding_Type::make_TypeAlias({&e});
                ),
            (Trait,
                rv.type = ::AST::PathBinding_Type::make_Trait({&e});
                ),

            (Function,
                rv.value = ::AST::PathBinding_Value::make_Function({&e});
                ),
            (Static,
                rv.value = ::AST::PathBinding_Value::make_Static({&e});
                ),
            (Struct,
                // TODO: What happens with name collisions?
                if( !e.m_data.is_Struct() )
                    rv.value = ::AST::PathBinding_Value::make_Struct({&e});
                rv.type = ::AST::PathBinding_Type::make_Struct({&e});
                ),
            (Enum,
                rv.type = ::AST::PathBinding_Type::make_Enum({&e});
                ),
            (Union,
                rv.type = ::AST::PathBinding_Type::make_Union({&e});
                ),
            (Module,
                rv.type = ::AST::PathBinding_Type::make_Module({&e});
                )
            )
        }
    }
    // TODO: macros
    for(const auto& mac : mod.macros())
    {
        if( mac.name == des_item_name ) {
            rv.macro = ::AST::PathBinding_Macro::make_MacroRules({ nullptr, &*mac.data });
            break;
        }
    }
    // TODO: If target is the crate root AND the crate exports macros with `macro_export`
    if( rv.macro.is_Unbound() && &mod == &crate.m_root_module )
    {
        auto it = crate.m_exported_macros.find(des_item_name);
        if(it != crate.m_exported_macros.end())
        {
            rv.macro = ::AST::PathBinding_Macro::make_MacroRules({ nullptr, &*it->second });
        }
    }

    if( types_only && !rv.type.is_Unbound() ) {
        return rv;
    }

    // Imports
    for( const auto& imp : mod.items() )
    {
        if( ! imp.data.is_Use() )
            continue ;
        const auto& imp_data = imp.data.as_Use();
        for( const auto& imp_e : imp_data.entries )
        {
            const Span& sp2 = imp_e.sp;
            if( imp_e.name == des_item_name ) {
                DEBUG("- Named import " << imp_e.name << " = " << imp_e.path);
                if( !imp_e.path.m_bindings.has_binding() )
                {
                    DEBUG(" > Needs resolve p=" << &imp_e.path);
                    static ::std::vector<const ::AST::Path*> s_mods;
                    if( ::std::find(s_mods.begin(), s_mods.end(), &imp_e.path) == s_mods.end() )
                    {
                        s_mods.push_back(&imp_e.path);
                        rv.merge_from( Resolve_Use_GetBinding(sp2, crate, mod.path(), Resolve_Use_AbsolutisePath(sp2, mod.path(), imp_e.path), parent_modules) );
                        s_mods.pop_back();
                    }
                    else
                    {
                        DEBUG("Recursion on path " << &imp_e.path << " " << imp_e.path);
                    }
                }
                else {
                    //out_path = imp_e.path;
                    rv.merge_from( imp_e.path.m_bindings.clone() );
                }
                continue ;
            }

            // TODO: Correct privacy rules (if the origin of this lookup can see this item)
            if( (imp.is_pub || mod.path().is_parent_of(source_mod_path)) && imp_e.name == "" )
            {
                DEBUG("- Search glob of " << imp_e.path << " in " << mod.path());
                // INEFFICIENT! Resolves and throws away the result (because we can't/shouldn't mutate here)
                ::AST::Path::Bindings   bindings_;
                const auto* bindings = &imp_e.path.m_bindings;
                if( bindings->type.is_Unbound() ) {
                    DEBUG("Temp resolving wildcard " << imp_e.path);
                    // Handle possibility of recursion
                    static ::std::vector<const ::AST::UseItem*>    resolve_stack_ptrs;
                    if( ::std::find(resolve_stack_ptrs.begin(), resolve_stack_ptrs.end(), &imp_data) == resolve_stack_ptrs.end() )
                    {
                        resolve_stack_ptrs.push_back( &imp_data );
                        bindings_ = Resolve_Use_GetBinding(sp2, crate, mod.path(), Resolve_Use_AbsolutisePath(sp2, mod.path(), imp_e.path), parent_modules, /*type_only=*/true);
                        if( bindings_.type.is_Unbound() ) {
                            DEBUG("Recursion detected, skipping " << imp_e.path);
                            continue ;
                        }
                        // *waves hand* I'm not evil.
                        const_cast< ::AST::Path::Bindings&>( imp_e.path.m_bindings ) = bindings_.clone();
                        bindings = &bindings_;
                        resolve_stack_ptrs.pop_back();
                    }
                    else {
                        continue ;
                    }
                }
                else {
                    //out_path = imp_e.path;
                }

                TU_MATCH_HDRA( (bindings->type), {)
                TU_ARMA(Crate, e) {
                    assert(e.crate_);
                    const ::HIR::Module& hmod = e.crate_->m_hir->m_root_module;
                    auto imp_rv = Resolve_Use_GetBinding__ext(sp2, crate, AST::Path("", { AST::PathNode(des_item_name,{}) }), hmod, 0);
                    if( imp_rv.has_binding() ) {
                        rv.merge_from( imp_rv );
                    }
                    }
                TU_ARMA(Module, e) {
                    if( e.module_ ) {
                        // TODO: Prevent infinite recursion?
                        static ::std::vector<const AST::Module*>  s_use_glob_mod_stack;
                        if( ::std::find(s_use_glob_mod_stack.begin(), s_use_glob_mod_stack.end(), &*e.module_) == s_use_glob_mod_stack.end() )
                        {
                            s_use_glob_mod_stack.push_back( &*e.module_ );
                            rv.merge_from( Resolve_Use_GetBinding_Mod(span, crate, mod.path(), *e.module_, des_item_name, {}) );
                            s_use_glob_mod_stack.pop_back();
                        }
                        else
                        {
                            DEBUG("Recursion prevented of " << e.module_->path());
                        }
                    }
                    else if( e.hir ) {
                        const ::HIR::Module& hmod = *e.hir;
                        rv.merge_from( Resolve_Use_GetBinding__ext(sp2, crate, AST::Path("", { AST::PathNode(des_item_name,{}) }), hmod, 0) );
                    }
                    else {
                        BUG(span, "NULL module for binding on glob of " << imp_e.path);
                    }
                    }
                TU_ARMA(Enum, e) {
                    assert(e.enum_ || e.hir);
                    if( e.enum_ ) {
                        const auto& enm = *e.enum_;
                        unsigned int i = 0;
                        for(const auto& var : enm.variants())
                        {
                            if( var.m_name == des_item_name ) {
                                ::AST::Path::Bindings   tmp_rv;
                                if( var.m_data.is_Struct() )
                                    tmp_rv.type = ::AST::PathBinding_Type::make_EnumVar({ &enm, i });
                                else
                                    tmp_rv.value = ::AST::PathBinding_Value::make_EnumVar({ &enm, i });
                                rv.merge_from(tmp_rv);
                                break;
                            }
                            i ++;
                        }
                    }
                    else {
                        const auto& enm = *e.hir;
                        auto idx = enm.find_variant(des_item_name);
                        if( idx != SIZE_MAX )
                        {
                            ::AST::Path::Bindings   tmp_rv;
                            if( enm.m_data.is_Data() && enm.m_data.as_Data()[idx].is_struct ) {
                                tmp_rv.type = ::AST::PathBinding_Type::make_EnumVar({ nullptr, static_cast<unsigned>(idx), &enm });
                            }
                            else {
                                tmp_rv.value = ::AST::PathBinding_Value::make_EnumVar({ nullptr, static_cast<unsigned>(idx), &enm });
                            }
                            rv.merge_from(tmp_rv);
                            break;
                        }
                    }
                    } break;
                default:
                    BUG(sp2, "Wildcard import expanded to an invalid item class - " << bindings->type.tag_str());
                    break;
                }
            }
        }
    }
    if( rv.has_binding() )
    {
        return rv;
    }

    if( mod.path().nodes().size() > 0 && mod.path().nodes().back().name().c_str()[0] == '#' ) {
        assert( parent_modules.size() > 0 );
        return Resolve_Use_GetBinding_Mod(span, crate, source_mod_path, *parent_modules.back(), des_item_name, parent_modules.subspan(0, parent_modules.size()-1));
    }
    else {
        //if( allow == Lookup::Any )
        //    ERROR(span, E0000, "Could not find node '" << des_item_name << "' in module " << mod.path());
        return ::AST::Path::Bindings();
    }
}

namespace {
    const ::HIR::Module* get_hir_mod_by_path(const Span& sp, const ::AST::Crate& crate, const ::HIR::SimplePath& path);

    const void* get_hir_modenum_by_path(const Span& sp, const ::AST::Crate& crate, const ::HIR::SimplePath& path, bool& is_enum)
    {
        const auto* hmod = &crate.m_extern_crates.at( path.m_crate_name ).m_hir->m_root_module;
        for(const auto& node : path.m_components)
        {
            auto it = hmod->m_mod_items.find(node);
            if( it == hmod->m_mod_items.end() )
                BUG(sp, "");
            TU_IFLET( ::HIR::TypeItem, (it->second->ent), Module, mod,
                hmod = &mod;
            )
            else TU_IFLET( ::HIR::TypeItem, (it->second->ent), Import, import,
                hmod = get_hir_mod_by_path(sp, crate, import.path);
                if( !hmod )
                    BUG(sp, "Import in module position didn't resolve as a module - " << import.path);
            )
            else TU_IFLET( ::HIR::TypeItem, (it->second->ent), Enum, enm,
                if( &node == &path.m_components.back() ) {
                    is_enum = true;
                    return &enm;
                }
                BUG(sp, "");
            )
            else {
                if( &node == &path.m_components.back() )
                    return nullptr;
                BUG(sp, "");
            }
        }
        is_enum = false;
        return hmod;
    }
    const ::HIR::Module* get_hir_mod_by_path(const Span& sp, const ::AST::Crate& crate, const ::HIR::SimplePath& path)
    {
        bool is_enum = false;
        auto rv = get_hir_modenum_by_path(sp, crate, path, is_enum);
        if( !rv )   return nullptr;
        ASSERT_BUG(sp, !is_enum, "");
        return reinterpret_cast< const ::HIR::Module*>(rv);
    }
}

::AST::Path::Bindings Resolve_Use_GetBinding__ext(const Span& span, const ::AST::Crate& crate, const ::AST::Path& path,  const ::HIR::Module& hmodr, unsigned int start)
{
    ::AST::Path::Bindings   rv;
    TRACE_FUNCTION_F(path << " offset " << start);
    const auto& nodes = path.nodes();
    const ::HIR::Module* hmod = &hmodr;
    for(unsigned int i = start; i < nodes.size() - 1; i ++)
    {
        DEBUG("m_mod_items = {" << FMT_CB(ss, for(const auto& e : hmod->m_mod_items) ss << e.first << ", ";) << "}");
        auto it = hmod->m_mod_items.find(nodes[i].name());
        if( it == hmod->m_mod_items.end() ) {
            // BZZT!
            ERROR(span, E0000, "Unable to find path component " << nodes[i].name() << " in " << path);
        }
        DEBUG(i << " : " << nodes[i].name() << " = " << it->second->ent.tag_str());
        TU_MATCH_DEF( ::HIR::TypeItem, (it->second->ent), (e),
        (
            ERROR(span, E0000, "Unexpected item type in import " << path << " @ " << i << " - " << it->second->ent.tag_str());
            ),
        (Import,
            bool is_enum = false;
            auto ptr = get_hir_modenum_by_path(span, crate, e.path, is_enum);
            if( !ptr )
                BUG(span, "Path component " << nodes[i].name() << " pointed to non-module (" << path << ")");
            if( is_enum ) {
                const auto& enm = *reinterpret_cast<const ::HIR::Enum*>(ptr);
                i += 1;
                if( i != nodes.size() - 1 ) {
                    ERROR(span, E0000, "Encountered enum at unexpected location in import");
                }
                const auto& name = nodes[i].name();

                auto idx = enm.find_variant(name);
                if( idx == SIZE_MAX ) {
                    ERROR(span, E0000, "Unable to find variant " << path);
                }
                if( enm.m_data.is_Data() && enm.m_data.as_Data()[idx].is_struct ) {
                    rv.type = ::AST::PathBinding_Type::make_EnumVar({ nullptr, static_cast<unsigned int>(idx), &enm });
                }
                else {
                    rv.value = ::AST::PathBinding_Value::make_EnumVar({ nullptr, static_cast<unsigned int>(idx), &enm });
                }
                return rv;
            }
            else {
                hmod = reinterpret_cast<const ::HIR::Module*>(ptr);
            }
            ),
        (Module,
            hmod = &e;
            ),
        (Enum,
            i += 1;
            if( i != nodes.size() - 1 ) {
                ERROR(span, E0000, "Encountered enum at unexpected location in import");
            }
            const auto& name = nodes[i].name();

            auto idx = e.find_variant(name);
            if(idx == SIZE_MAX) {
                ERROR(span, E0000, "Unable to find variant " << path);
            }
            if( e.m_data.is_Data() && e.m_data.as_Data()[idx].is_struct ) {
                rv.type = ::AST::PathBinding_Type::make_EnumVar({ nullptr, static_cast<unsigned int>(idx), &e });
            }
            else {
                rv.value = ::AST::PathBinding_Value::make_EnumVar({ nullptr, static_cast<unsigned int>(idx), &e });
            }
            return rv;
            )
        )
    }
    // - namespace/type items
    {
        auto it = hmod->m_mod_items.find(nodes.back().name());
        if( it != hmod->m_mod_items.end() )
        {
            const auto* item_ptr = &it->second->ent;
            DEBUG("E : Mod " << nodes.back().name() << " = " << item_ptr->tag_str());
            if( item_ptr->is_Import() ) {
                const auto& e = item_ptr->as_Import();
                const auto& ec = crate.m_extern_crates.at( e.path.m_crate_name );
                // This doesn't need to recurse - it can just do a single layer (as no Import should refer to another)
                if( e.is_variant ) {
                    auto p = e.path;
                    p.m_components.pop_back();
                    const auto& enm = ec.m_hir->get_typeitem_by_path(span, p, true).as_Enum();
                    assert(e.idx < enm.num_variants());
                    rv.type = ::AST::PathBinding_Type::make_EnumVar({ nullptr, e.idx, &enm });
                }
                else if( e.path.m_components.empty() )
                {
                    rv.type = ::AST::PathBinding_Type::make_Module({nullptr, &ec.m_hir->m_root_module});
                }
                else
                {
                    item_ptr = &ec.m_hir->get_typeitem_by_path(span, e.path, true);    // ignore_crate_name=true
                }
            }
            if( rv.type.is_Unbound() )
            {
                TU_MATCHA( (*item_ptr), (e),
                (Import,
                    BUG(span, "Recursive import in " << path << " - " << it->second->ent.as_Import().path << " -> " << e.path);
                    ),
                (Module,
                    rv.type = ::AST::PathBinding_Type::make_Module({nullptr, &e});
                    ),
                (TypeAlias,
                    rv.type = ::AST::PathBinding_Type::make_TypeAlias({nullptr});
                    ),
                (ExternType,
                    rv.type = ::AST::PathBinding_Type::make_TypeAlias({nullptr});   // Lazy.
                    ),
                (Enum,
                    rv.type = ::AST::PathBinding_Type::make_Enum({nullptr, &e});
                    ),
                (Struct,
                    rv.type = ::AST::PathBinding_Type::make_Struct({nullptr, &e});
                    ),
                (Union,
                    rv.type = ::AST::PathBinding_Type::make_Union({nullptr, &e});
                    ),
                (Trait,
                    rv.type = ::AST::PathBinding_Type::make_Trait({nullptr, &e});
                    )
                )
            }
        }
        else
        {
            DEBUG("Types = " << FMT_CB(ss, for(const auto& e : hmod->m_mod_items){ ss << e.first << ":" << e.second->ent.tag_str() << ","; }));
        }
    }
    // - Values
    {
        auto it2 = hmod->m_value_items.find(nodes.back().name());
        if( it2 != hmod->m_value_items.end() ) {
            const auto* item_ptr = &it2->second->ent;
            DEBUG("E : Value " << nodes.back().name() << " = " << item_ptr->tag_str());
            if( item_ptr->is_Import() ) {
                const auto& e = item_ptr->as_Import();
                // This doesn't need to recurse - it can just do a single layer (as no Import should refer to another)
                const auto& ec = crate.m_extern_crates.at( e.path.m_crate_name );
                if( e.is_variant )
                {
                    auto p = e.path;
                    p.m_components.pop_back();
                    const auto& enm = ec.m_hir->get_typeitem_by_path(span, p, true).as_Enum();
                    assert(e.idx < enm.num_variants());
                    rv.value = ::AST::PathBinding_Value::make_EnumVar({ nullptr, e.idx, &enm });
                }
                else
                {
                    item_ptr = &ec.m_hir->get_valitem_by_path(span, e.path, true);    // ignore_crate_name=true
                }
            }
            if( rv.value.is_Unbound() )
            {
                TU_MATCHA( (*item_ptr), (e),
                (Import,
                    BUG(span, "Recursive import in " << path << " - " << it2->second->ent.as_Import().path << " -> " << e.path);
                    ),
                (Constant,
                    rv.value = ::AST::PathBinding_Value::make_Static({ nullptr });
                    ),
                (Static,
                    rv.value = ::AST::PathBinding_Value::make_Static({ nullptr });
                    ),
                // TODO: What happens if these two refer to an enum constructor?
                (StructConstant,
                    ASSERT_BUG(span, crate.m_extern_crates.count(e.ty.m_crate_name), "Crate '" << e.ty.m_crate_name << "' not loaded for " << e.ty);
                    rv.value = ::AST::PathBinding_Value::make_Struct({ nullptr, &crate.m_extern_crates.at(e.ty.m_crate_name).m_hir->get_typeitem_by_path(span, e.ty, true).as_Struct() });
                    ),
                (StructConstructor,
                    ASSERT_BUG(span, crate.m_extern_crates.count(e.ty.m_crate_name), "Crate '" << e.ty.m_crate_name << "' not loaded for " << e.ty);
                    rv.value = ::AST::PathBinding_Value::make_Struct({ nullptr, &crate.m_extern_crates.at(e.ty.m_crate_name).m_hir->get_typeitem_by_path(span, e.ty, true).as_Struct() });
                    ),
                (Function,
                    rv.value = ::AST::PathBinding_Value::make_Function({ nullptr });
                    )
                )
            }
        }
        else
        {
            DEBUG("Values = " << FMT_CB(ss, for(const auto& e : hmod->m_value_items){ ss << e.first << ":" << e.second->ent.tag_str() << ","; }));
        }
    }

    if( rv.type.is_Unbound() && rv.value.is_Unbound() )
    {
        DEBUG("E : None");
    }
    return rv;
}
::AST::Path::Bindings Resolve_Use_GetBinding__ext(const Span& span, const ::AST::Crate& crate, const ::AST::Path& path,  const AST::ExternCrate& ec, unsigned int start)
{
    DEBUG("Crate " << ec.m_name);
    auto rv = Resolve_Use_GetBinding__ext(span, crate, path, ec.m_hir->m_root_module, start);
    if( start + 1 == path.nodes().size() )
    {
        const auto& name = path.nodes().back().name();
        auto it = ec.m_hir->m_exported_macros.find( name );
        if( it != ec.m_hir->m_exported_macros.end() )
        {
            rv.macro = ::AST::PathBinding_Macro::make_MacroRules({ &ec, &*it->second });
        }

        {
            auto it = ::std::find_if( ec.m_hir->m_proc_macros.begin(), ec.m_hir->m_proc_macros.end(), [&](const auto& pm){ return pm.name == name;} );
            if( it != ec.m_hir->m_proc_macros.end() )
            {
                rv.macro = ::AST::PathBinding_Macro::make_ProcMacro({ &ec, name });
            }
        }
    }
    return rv;
}

::AST::Path::Bindings Resolve_Use_GetBinding(
    const Span& span, const ::AST::Crate& crate, const ::AST::Path& source_mod_path,
    const ::AST::Path& path, ::std::span< const ::AST::Module* > parent_modules,
    bool types_only/*=false*/
    )
{
    TRACE_FUNCTION_F(path);
    //::AST::Path rv;

    // If the path is directly referring to an external crate - call __ext
    if( path.m_class.is_Absolute() && path.m_class.as_Absolute().crate != "" ) {
        const auto& path_abs = path.m_class.as_Absolute();

        ASSERT_BUG(span, crate.m_extern_crates.count(path_abs.crate.c_str()), "Crate '" << path_abs.crate << "' not loaded");
        return Resolve_Use_GetBinding__ext(span, crate, path,  crate.m_extern_crates.at( path_abs.crate.c_str() ), 0);
    }

    ::AST::Path::Bindings   rv;

    const AST::Module* mod = &crate.m_root_module;
    const auto& nodes = path.nodes();
    if( nodes.size() == 0 ) {
        // An import of the root.
        rv.type = ::AST::PathBinding_Type::make_Module({ mod, nullptr });
        return rv;
    }
    for( unsigned int i = 0; i < nodes.size()-1; i ++ )
    {
        // TODO: If this came from an import, return the real path?

        //rv = Resolve_Use_CanoniseAndBind_Mod(span, crate, *mod, mv$(rv), nodes[i].name(), parent_modules, Lookup::Type);
        //const auto& b = rv.binding();
        assert(mod);
        auto b = Resolve_Use_GetBinding_Mod(span, crate, source_mod_path, *mod, nodes.at(i).name(), parent_modules, /*types_only=*/true);
        TU_MATCH_HDRA( (b.type), {)
        default:
            ERROR(span, E0000, "Unexpected item type " << b.type.tag_str() << " in import of " << path);
        TU_ARMA(Unbound, e) {
            ERROR(span, E0000, "Cannot find component " << i << " of " << path << " (" << b.type << ")");
            }
        TU_ARMA(Crate, e) {
            // TODO: Mangle the original path (or return a new path somehow)
            return Resolve_Use_GetBinding__ext(span, crate, path,  *e.crate_, i+1);
            }
        TU_ARMA(Enum, e) {
            ASSERT_BUG(span, e.enum_ || e.hir, "nullptr enum pointer in node " << i << " of " << path);
            ASSERT_BUG(span, e.enum_ == nullptr || e.hir == nullptr, "both AST and HIR pointers set in node " << i << " of " << path);
            i += 1;
            if( i != nodes.size() - 1 ) {
                ERROR(span, E0000, "Encountered enum at unexpected location in import");
            }
            ASSERT_BUG(span, i < nodes.size(), "Enum import position error, " << i << " >= " << nodes.size() << " - " << path);

            const auto& node2 = nodes[i];

            unsigned    variant_index = 0;
            bool    is_value = false;
            if(e.hir)
            {
                const auto& enum_ = *e.hir;
                size_t idx = enum_.find_variant(node2.name());
                if( idx == ~0u ) {
                    ERROR(span, E0000, "Unknown enum variant " << path);
                }
                TU_MATCH_HDRA( (enum_.m_data), {)
                TU_ARMA(Value, ve) {
                    is_value = true;
                    }
                TU_ARMA(Data, ve) {
                    is_value = !ve[idx].is_struct;
                    }
                }
                DEBUG("AST Enum variant - " << variant_index << ", is_value=" << is_value);
            }
            else
            {
                const auto& enum_ = *e.enum_;
                for( const auto& var : enum_.variants() )
                {
                    if( var.m_name == node2.name() ) {
                        is_value = !var.m_data.is_Struct();
                        break ;
                    }
                    variant_index ++;
                }
                if( variant_index == enum_.variants().size() ) {
                    ERROR(span, E0000, "Unknown enum variant '" << node2.name() << "'");
                }

                DEBUG("AST Enum variant - " << variant_index << ", is_value=" << is_value << " " << enum_.variants()[variant_index].m_data.tag_str());
            }
            if( is_value ) {
                rv.value = ::AST::PathBinding_Value::make_EnumVar({e.enum_, variant_index, e.hir});
            }
            else {
                rv.type = ::AST::PathBinding_Type::make_EnumVar({e.enum_, variant_index, e.hir});
            }
            return rv;
            }
        TU_ARMA(Module, e) {
            ASSERT_BUG(span, e.module_ || e.hir, "nullptr module pointer in node " << i << " of " << path);
            if( !e.module_ )
            {
                assert(e.hir);
                // TODO: Mangle the original path (or return a new path somehow)
                return Resolve_Use_GetBinding__ext(span, crate, path,  *e.hir, i+1);
            }
            mod = e.module_;
            }
        }
    }

    assert(mod);
    return Resolve_Use_GetBinding_Mod(span, crate, source_mod_path, *mod, nodes.back().name(), parent_modules, types_only);
}

//::AST::PathBinding_Macro Resolve_Use_GetBinding_Macro(const Span& span, const ::AST::Crate& crate, const ::AST::Path& path, ::std::span< const ::AST::Module* > parent_modules)
//{
//    throw "";
//}
