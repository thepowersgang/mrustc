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

::AST::Path Resolve_Use_AbsolutisePath(const ::AST::Path& base_path, ::AST::Path path);
void Resolve_Use_Mod(const ::AST::Crate& crate, ::AST::Module& mod, ::AST::Path path, ::std::span< const ::AST::Module* > parent_modules={});
::AST::PathBinding Resolve_Use_GetBinding(const Span& span, const ::AST::Crate& crate, const ::AST::Path& path, ::std::span< const ::AST::Module* > parent_modules, Lookup allow=Lookup::Any);
::AST::PathBinding Resolve_Use_GetBinding__ext(const Span& span, const ::AST::Crate& crate, const ::AST::Path& path,  const ::HIR::Module& hmodr, unsigned int start,  Lookup allow);


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
        if( base_path.nodes().size() > 0 && base_path.nodes().back().name()[0] == '#' ) {
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
        if( base_path.nodes().size() > 0 && base_path.nodes().back().name()[0] == '#' ) {
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
        while( base_path.nodes().size() > n_anon && base_path.nodes()[ base_path.nodes().size()-1-n_anon ].name()[0] == '#' )
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
        use_stmt_data.path = Resolve_Use_AbsolutisePath(span, path, mv$(use_stmt_data.path));
        if( !use_stmt_data.path.m_class.is_Absolute() )
            BUG(span, "Use path is not absolute after absolutisation");

        // TODO: Have Resolve_Use_GetBinding return the actual path
        use_stmt_data.path.bind( Resolve_Use_GetBinding(span, crate, use_stmt_data.path, parent_modules) );
        DEBUG("'" << use_stmt.name << "' = " << use_stmt_data.path);

        // - If doing a glob, ensure the item type is valid
        if( use_stmt.name == "" )
        {
            TU_MATCH_DEF(::AST::PathBinding, (use_stmt_data.path.binding()), (e),
            (
                ERROR(span, E0000, "Wildcard import of invalid item type - " << use_stmt_data.path);
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
            // TODO: Handle case where a use can resolve to two different items (one value, one type/namespace)
            // - Easiest way is with an extra binding slot
            Lookup  allow = Lookup::Any;
            switch( use_stmt_data.path.binding().tag() )
            {
            case ::AST::PathBinding::TAG_Crate:
            case ::AST::PathBinding::TAG_Module:
            case ::AST::PathBinding::TAG_Trait:
            case ::AST::PathBinding::TAG_TypeAlias:
            case ::AST::PathBinding::TAG_Enum:
                allow = Lookup::Value;
                break;
            case ::AST::PathBinding::TAG_Struct:
            case ::AST::PathBinding::TAG_Union:
                allow = Lookup::Value;
                break;
            case ::AST::PathBinding::TAG_EnumVar:
                allow = Lookup::Value;
                break;
            case ::AST::PathBinding::TAG_Static:
            case ::AST::PathBinding::TAG_Function:
                allow = Lookup::Type;
                break;
            // DEAD, Unbound, ...
            default:    break;
            }
            ASSERT_BUG(span, allow != Lookup::Any, "Invalid path binding type in use statement - " << use_stmt_data.path);
            use_stmt_data.alt_binding = Resolve_Use_GetBinding(span, crate, use_stmt_data.path, parent_modules, allow);
            DEBUG("- Alt Binding: " << use_stmt_data.alt_binding);
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

::AST::PathBinding Resolve_Use_GetBinding_Mod(
        const Span& span,
        const ::AST::Crate& crate, const ::AST::Module& mod,
        const ::std::string& des_item_name,
        ::std::span< const ::AST::Module* > parent_modules,
        Lookup allow
    )
{
    // If the desired item is an anon module (starts with #) then parse and index
    if( des_item_name.size() > 0 && des_item_name[0] == '#' ) {
        unsigned int idx = 0;
        if( ::std::sscanf(des_item_name.c_str(), "#%u", &idx) != 1 ) {
            BUG(span, "Invalid anon path segment '" << des_item_name << "'");
        }
        if( idx >= mod.anon_mods().size() ) {
            BUG(span, "Invalid anon path segment '" << des_item_name << "'");
        }
        assert( mod.anon_mods()[idx] );
        return ::AST::PathBinding::make_Module({&*mod.anon_mods()[idx], nullptr});
    }

    // Seach for the name defined in the module.
    for( const auto& item : mod.items() )
    {
        if( item.data.is_None() )
            continue ;

        if( item.name == des_item_name ) {
            //if( allow != Lookup::Any )
            //    DEBUG(mod.path() << " " << des_item_name << " " << item.data.tag_str());
            TU_MATCH(::AST::Item, (item.data), (e),
            (None,
                // IMPOSSIBLE - Handled above
                ),
            (MacroInv,
                BUG(span, "Hit MacroInv in use resolution");
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
                if( allow != Lookup::Value )
                    ASSERT_BUG(span, crate.m_extern_crates.count(e.name), "Crate '" << e.name << "' not loaded");
                    return ::AST::PathBinding::make_Crate({ &crate.m_extern_crates.at(e.name) });
                ),
            (Type,
                if( allow != Lookup::Value )
                    return ::AST::PathBinding::make_TypeAlias({&e});
                ),
            (Trait,
                if( allow != Lookup::Value )
                    return ::AST::PathBinding::make_Trait({&e});
                ),

            (Function,
                if( allow != Lookup::Type )
                    return ::AST::PathBinding::make_Function({&e});
                ),
            (Static,
                if( allow != Lookup::Type )
                    return ::AST::PathBinding::make_Static({&e});
                ),
            (Struct,
                if( allow != Lookup::Value )
                    return ::AST::PathBinding::make_Struct({&e});
                if( e.m_data.is_Tuple() && allow != Lookup::Type )
                    return ::AST::PathBinding::make_Struct({&e});
                ),
            (Enum,
                if( allow != Lookup::Value )
                    return ::AST::PathBinding::make_Enum({&e});
                ),
            (Union,
                if( allow != Lookup::Value )
                    return ::AST::PathBinding::make_Union({&e});
                ),
            (Module,
                if( allow != Lookup::Value )
                    return ::AST::PathBinding::make_Module({&e});
                )
            )
        }
    }

    // Imports
    for( const auto& imp : mod.items() )
    {
        if( ! imp.data.is_Use() )
            continue ;
        const auto& imp_data = imp.data.as_Use();
        const Span& sp2 = imp_data.sp;
        if( imp.name == des_item_name ) {
            DEBUG("- Named import " << imp.name << " = " << imp_data);
            if( imp_data.path.binding().is_Unbound() ) {
                DEBUG(" > Needs resolve");
                // TODO: Handle possibility of recursion
                //out_path = imp_data.path;
                return Resolve_Use_GetBinding(sp2, crate, Resolve_Use_AbsolutisePath(sp2, mod.path(), imp_data.path), parent_modules, allow);
            }
            else {
                if( allow != Lookup::Any && allow != Lookup::AnyOpt )
                {
                    switch( imp_data.path.binding().tag() )
                    {
                    case ::AST::PathBinding::TAG_Crate:
                    case ::AST::PathBinding::TAG_Module:
                    case ::AST::PathBinding::TAG_Trait:
                    case ::AST::PathBinding::TAG_TypeAlias:
                    case ::AST::PathBinding::TAG_Enum:
                        if( allow != Lookup::Type )
                            continue;
                        break;
                    case ::AST::PathBinding::TAG_Struct:
                        break;
                    case ::AST::PathBinding::TAG_EnumVar:
                        break;
                    case ::AST::PathBinding::TAG_Static:
                    case ::AST::PathBinding::TAG_Function:
                        if( allow != Lookup::Value )
                            continue;
                        break;
                    default:
                        break;
                    }
                }
                //out_path = imp_data.path;
                return imp_data.path.binding().clone();
            }
        }
        if( imp.is_pub && imp.name == "" ) {
            DEBUG("- Search glob of " << imp_data.path);
            // INEFFICIENT! Resolves and throws away the result (because we can't/shouldn't mutate here)
            ::AST::PathBinding  binding_;
            const auto* binding = &imp_data.path.binding();
            if( binding->is_Unbound() ) {
                DEBUG("Temp resolving wildcard " << imp_data);
                // Handle possibility of recursion
                static ::std::vector<const ::AST::UseStmt*>    resolve_stack_ptrs;
                if( ::std::find(resolve_stack_ptrs.begin(), resolve_stack_ptrs.end(), &imp_data) == resolve_stack_ptrs.end() )
                {
                    resolve_stack_ptrs.push_back( &imp_data );
                    binding_ = Resolve_Use_GetBinding(sp2, crate, Resolve_Use_AbsolutisePath(sp2, mod.path(), imp_data.path), parent_modules);
                    // *waves hand* I'm not evil.
                    const_cast< ::AST::PathBinding&>( imp_data.path.binding() ) = binding_.clone();
                    binding = &binding_;
                    resolve_stack_ptrs.pop_back();
                }
                else {
                    continue ;
                }
            }
            else {
                //out_path = imp_data.path;
            }

            TU_MATCH_DEF(::AST::PathBinding, (*binding), (e),
            (
                BUG(sp2, "Wildcard import expanded to an invalid item class - " << binding->tag_str());
                ),
            (Crate,
                assert(e.crate_);
                const ::HIR::Module& hmod = e.crate_->m_hir->m_root_module;
                auto rv = Resolve_Use_GetBinding__ext(sp2, crate, AST::Path("", { AST::PathNode(des_item_name,{}) }), hmod, 0, allow);
                if( ! rv.is_Unbound() ) {
                    return mv$(rv);
                }
                ),
            (Module,
                auto allow_inner = (allow == Lookup::Any ? Lookup::AnyOpt : allow);
                assert(e.module_);
                // TODO: Prevent infinite recursion?
                auto rv = Resolve_Use_GetBinding_Mod(span, crate, *e.module_, des_item_name, {}, allow_inner);
                if( ! rv.is_Unbound() ) {
                    return mv$(rv);
                }
                ),
            (Enum,
                assert(e.enum_ || e.hir);
                if( e.enum_ ) {
                    const auto& enm = *e.enum_;
                    unsigned int i = 0;
                    for(const auto& var : enm.variants())
                    {
                        if( var.m_name == des_item_name ) {
                            return ::AST::PathBinding::make_EnumVar({ &enm, i });
                        }
                        i ++;
                    }
                }
                else {
                    const auto& enm = *e.hir;
                    auto idx = enm.find_variant(des_item_name);
                    if( idx != SIZE_MAX )
                    {
                        return ::AST::PathBinding::make_EnumVar({ nullptr, static_cast<unsigned>(idx), &enm });
                    }
                }
                )
            )
        }
    }

    if( mod.path().nodes().size() > 0 && mod.path().nodes().back().name()[0] == '#' ) {
        assert( parent_modules.size() > 0 );
        return Resolve_Use_GetBinding_Mod(span, crate, *parent_modules.back(), des_item_name, parent_modules.subspan(0, parent_modules.size()-1), allow);
    }
    else {
        if( allow == Lookup::Any )
            ERROR(span, E0000, "Could not find node '" << des_item_name << "' in module " << mod.path());
        else
            return ::AST::PathBinding::make_Unbound({});
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

::AST::PathBinding Resolve_Use_GetBinding__ext(const Span& span, const ::AST::Crate& crate, const ::AST::Path& path,  const ::HIR::Module& hmodr, unsigned int start,  Lookup allow)
{
    TRACE_FUNCTION_F(path);
    const auto& nodes = path.nodes();
    const ::HIR::Module* hmod = &hmodr;
    for(unsigned int i = start; i < nodes.size() - 1; i ++)
    {
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
                return ::AST::PathBinding::make_EnumVar({ nullptr, static_cast<unsigned int>(idx), &enm });
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
            return ::AST::PathBinding::make_EnumVar({ nullptr, static_cast<unsigned int>(idx), &e });
            )
        )
    }
    if( allow != Lookup::Value )
    {
        auto it = hmod->m_mod_items.find(nodes.back().name());
        if( it != hmod->m_mod_items.end() ) {
            const auto* item_ptr = &it->second->ent;
            DEBUG("E : " << nodes.back().name() << " = " << item_ptr->tag_str());
            if( item_ptr->is_Import() ) {
                const auto& e = item_ptr->as_Import();
                const auto& ec = crate.m_extern_crates.at( e.path.m_crate_name );
                // This doesn't need to recurse - it can just do a single layer (as no Import should refer to another)
                if( e.is_variant ) {
                    auto p = e.path;
                    p.m_components.pop_back();
                    const auto& enm = ec.m_hir->get_typeitem_by_path(span, p, true).as_Enum();
                    assert(e.idx < enm.num_variants());
                    return ::AST::PathBinding::make_EnumVar({ nullptr, e.idx, &enm });
                }
                if( e.path.m_components.empty() )
                    return ::AST::PathBinding::make_Module({nullptr, &ec.m_hir->m_root_module});
                item_ptr = &ec.m_hir->get_typeitem_by_path(span, e.path, true);    // ignore_crate_name=true
            }
            TU_MATCHA( (*item_ptr), (e),
            (Import,
                BUG(span, "Recursive import in " << path << " - " << it->second->ent.as_Import().path << " -> " << e.path);
                ),
            (Module,
                return ::AST::PathBinding::make_Module({nullptr, &e});
                ),
            (TypeAlias,
                return ::AST::PathBinding::make_TypeAlias({nullptr});
                ),
            (Enum,
                return ::AST::PathBinding::make_Enum({nullptr, &e});
                ),
            (Struct,
                return ::AST::PathBinding::make_Struct({nullptr, &e});
                ),
            (Union,
                return ::AST::PathBinding::make_Union({nullptr, &e});
                ),
            (Trait,
                return ::AST::PathBinding::make_Trait({nullptr, &e});
                )
            )
        }
        DEBUG("Types = " << FMT_CB(ss, for(const auto& e : hmod->m_mod_items){ ss << e.first << ":" << e.second->ent.tag_str() << ","; }));
    }
    if( allow != Lookup::Type )
    {
        auto it2 = hmod->m_value_items.find(nodes.back().name());
        if( it2 != hmod->m_value_items.end() ) {
            const auto* item_ptr = &it2->second->ent;
            DEBUG("E : " << nodes.back().name() << " = " << item_ptr->tag_str());
            if( item_ptr->is_Import() ) {
                const auto& e = item_ptr->as_Import();
                // This doesn't need to recurse - it can just do a single layer (as no Import should refer to another)
                const auto& ec = crate.m_extern_crates.at( e.path.m_crate_name );
                if( e.is_variant ) {
                    auto p = e.path;
                    p.m_components.pop_back();
                    const auto& enm = ec.m_hir->get_typeitem_by_path(span, p, true).as_Enum();
                    assert(e.idx < enm.num_variants());
                    return ::AST::PathBinding::make_EnumVar({ nullptr, e.idx, &enm });
                }
                item_ptr = &ec.m_hir->get_valitem_by_path(span, e.path, true);    // ignore_crate_name=true
            }
            TU_MATCHA( (*item_ptr), (e),
            (Import,
                BUG(span, "Recursive import in " << path << " - " << it2->second->ent.as_Import().path << " -> " << e.path);
                ),
            (Constant,
                return ::AST::PathBinding::make_Static({ nullptr });
                ),
            (Static,
                return ::AST::PathBinding::make_Static({ nullptr });
                ),
            // TODO: What happens if these two refer to an enum constructor?
            (StructConstant,
                ASSERT_BUG(span, crate.m_extern_crates.count(e.ty.m_crate_name), "Crate '" << e.ty.m_crate_name << "' not loaded for " << e.ty);
                return ::AST::PathBinding::make_Struct({ nullptr, &crate.m_extern_crates.at(e.ty.m_crate_name).m_hir->get_typeitem_by_path(span, e.ty, true).as_Struct() });
                ),
            (StructConstructor,
                ASSERT_BUG(span, crate.m_extern_crates.count(e.ty.m_crate_name), "Crate '" << e.ty.m_crate_name << "' not loaded for " << e.ty);
                return ::AST::PathBinding::make_Struct({ nullptr, &crate.m_extern_crates.at(e.ty.m_crate_name).m_hir->get_typeitem_by_path(span, e.ty, true).as_Struct() });
                ),
            (Function,
                return ::AST::PathBinding::make_Function({ nullptr });
                )
            )
        }

        DEBUG("Values = " << FMT_CB(ss, for(const auto& e : hmod->m_value_items){ ss << e.first << ":" << e.second->ent.tag_str() << ","; }));
    }

    DEBUG("E : None");
    return ::AST::PathBinding::make_Unbound({});
}
::AST::PathBinding Resolve_Use_GetBinding__ext(const Span& span, const ::AST::Crate& crate, const ::AST::Path& path,  const AST::ExternCrate& ec, unsigned int start,  Lookup allow)
{
    return Resolve_Use_GetBinding__ext(span, crate, path, ec.m_hir->m_root_module, start, allow);
}

::AST::PathBinding Resolve_Use_GetBinding(const Span& span, const ::AST::Crate& crate, const ::AST::Path& path, ::std::span< const ::AST::Module* > parent_modules, Lookup allow)
{
    TRACE_FUNCTION_F(path);
    //::AST::Path rv;

    // If the path is directly referring to an external crate - call __ext
    if( path.m_class.is_Absolute() && path.m_class.as_Absolute().crate != "" ) {
        const auto& path_abs = path.m_class.as_Absolute();

        ASSERT_BUG(span, crate.m_extern_crates.count(path_abs.crate), "Crate '" << path_abs.crate << "' not loaded");
        return Resolve_Use_GetBinding__ext(span, crate, path,  crate.m_extern_crates.at( path_abs.crate ), 0, allow);
    }

    const AST::Module* mod = &crate.m_root_module;
    const auto& nodes = path.nodes();
    if( nodes.size() == 0 ) {
        // An import of the root.
        return ::AST::PathBinding::make_Module({ mod, nullptr });
    }
    for( unsigned int i = 0; i < nodes.size()-1; i ++ )
    {
        // TODO: If this came from an import, return the real path?

        //rv = Resolve_Use_CanoniseAndBind_Mod(span, crate, *mod, mv$(rv), nodes[i].name(), parent_modules, Lookup::Type);
        //const auto& b = rv.binding();
        assert(mod);
        auto b = Resolve_Use_GetBinding_Mod(span, crate, *mod, nodes.at(i).name(), parent_modules, Lookup::Type);
        TU_MATCH_DEF(::AST::PathBinding, (b), (e),
        (
            ERROR(span, E0000, "Unexpected item type " << b.tag_str() << " in import of " << path);
            ),
        (Unbound,
            ERROR(span, E0000, "Cannot find component " << i << " of " << path);
            ),
        (Crate,
            // TODO: Mangle the original path (or return a new path somehow)
            return Resolve_Use_GetBinding__ext(span, crate, path,  *e.crate_, i+1, allow);
            ),
        (Enum,
            const auto& enum_ = *e.enum_;
            i += 1;
            if( i != nodes.size() - 1 ) {
                ERROR(span, E0000, "Encountered enum at unexpected location in import");
            }

            const auto& node2 = nodes[i];
             int    variant_index = -1;
            for( unsigned int j = 0; j < enum_.variants().size(); j ++ )
            {
                if( enum_.variants()[j].m_name == node2.name() ) {
                    variant_index = j;
                    break ;
                }
            }
            if( variant_index < 0 ) {
                ERROR(span, E0000, "Unknown enum variant '" << node2.name() << "'");
            }

            return ::AST::PathBinding::make_EnumVar({&enum_, static_cast<unsigned int>(variant_index)});
            ),
        (Module,
            ASSERT_BUG(span, e.module_ || e.hir, "nullptr module pointer in node " << i << " of " << path);
            if( !e.module_ )
            {
                assert(e.hir);
                // TODO: Mangle the original path (or return a new path somehow)
                return Resolve_Use_GetBinding__ext(span, crate, path,  *e.hir, i+1, allow);
            }
            mod = e.module_;
            )
        )
    }

    assert(mod);
    return Resolve_Use_GetBinding_Mod(span, crate, *mod, nodes.back().name(), parent_modules, allow);
}

