/*
 * Absolutise and check all 'use' statements
 */
#include <main_bindings.hpp>
#include <ast/crate.hpp>
#include <ast/ast.hpp>
#include <ast/expr.hpp>
#include <hir/hir.hpp>

enum class Lookup
{
    Any,    // Allow binding to anything
    AnyOpt, // Allow binding to anything, but don't error on failure
    Type,   // Allow only type bindings
    Value,  // Allow only value bindings
};

::AST::Path Resolve_Use_AbsolutisePath(const ::AST::Path& base_path, ::AST::Path path);
void Resolve_Use_Mod(const ::AST::Crate& crate, ::AST::Module& mod, ::AST::Path path, slice< const ::AST::Module* > parent_modules={});
::AST::PathBinding Resolve_Use_GetBinding(const Span& span, const ::AST::Crate& crate, const ::AST::Path& path, slice< const ::AST::Module* > parent_modules, Lookup allow=Lookup::Any);


void Resolve_Use(::AST::Crate& crate)
{
    Resolve_Use_Mod(crate, crate.m_root_module, ::AST::Path("", {}));
}

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
        return base_path + path;
        ),
    (Self,
        // EVIL HACK: If the current module is an anon module, refer to the parent
        if( base_path.nodes().back().name()[0] == '#' ) {
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
        assert(e.count >= 1);
        AST::Path   np("", {});
        if( e.count > base_path.nodes().size() ) {
            ERROR(span, E0000, "Too many `super` components");
        }
        for( unsigned int i = 0; i < base_path.nodes().size() - e.count; i ++ )
            np.nodes().push_back( base_path.nodes()[i] );
        np += path;
        return np;
        ),
    (Absolute,
        // Leave as is
        return path;
        )
    )
    throw "BUG: Reached end of Resolve_Use_AbsolutisePath";
}

void Resolve_Use_Mod(const ::AST::Crate& crate, ::AST::Module& mod, ::AST::Path path, slice< const ::AST::Module* > parent_modules)
{
    TRACE_FUNCTION_F("path = " << path << ", mod.path() = " << mod.path());
    for(auto& use_stmt : mod.imports())
    {
        const Span& span = use_stmt.data.sp;
        use_stmt.data.path = Resolve_Use_AbsolutisePath(span, path, mv$(use_stmt.data.path));
        if( !use_stmt.data.path.m_class.is_Absolute() )
            BUG(span, "Use path is not absolute after absolutisation");
        
        use_stmt.data.path.bind( Resolve_Use_GetBinding(span, crate, use_stmt.data.path, parent_modules) );
        
        // - If doing a glob, ensure the item type is valid
        if( use_stmt.name == "" )
        {
            TU_MATCH_DEF(::AST::PathBinding, (use_stmt.data.path.binding()), (e),
            (
                ERROR(span, E0000, "Wildcard import of invalid item type");
                ),
            (Enum,
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
            switch( use_stmt.data.path.binding().tag() )
            {
            case ::AST::PathBinding::TAG_Crate:
            case ::AST::PathBinding::TAG_Module:
            case ::AST::PathBinding::TAG_Trait:
            case ::AST::PathBinding::TAG_TypeAlias:
            case ::AST::PathBinding::TAG_Enum:
                allow = Lookup::Value;
                break;
            case ::AST::PathBinding::TAG_Struct:
                allow = Lookup::Value;
                break;
            case ::AST::PathBinding::TAG_EnumVar:
                allow = Lookup::Value;
                break;
            case ::AST::PathBinding::TAG_Static:
            case ::AST::PathBinding::TAG_Function:
                allow = Lookup::Type;
                break;
            default:
                break;
            }
            ASSERT_BUG(span, allow != Lookup::Any, "");
            use_stmt.data.alt_binding = Resolve_Use_GetBinding(span, crate, use_stmt.data.path, parent_modules, allow);
        }
    }
    
    for(auto& i : mod.items())
    {
        if( i.data.is_Module() )
        {
            Resolve_Use_Mod(crate, i.data.as_Module(), path + i.name);
        }
    }
    // - Handle anon modules by iterating code (allowing correct item mappings)

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
    
    // TODO: Check that all code blocks are covered by these two
    for(auto& i : mod.items())
    {
        TU_MATCH_DEF( AST::Item, (i.data), (e),
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
    for(auto& im : mod.impls())
    {
        for(auto& i : im.items())
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
    }
}

::AST::PathBinding Resolve_Use_GetBinding_Mod(
        const Span& span,
        const ::AST::Crate& crate, const ::AST::Module& mod,
        const ::std::string& des_item_name,
        slice< const ::AST::Module* > parent_modules,
        Lookup allow
    )
{
    // HACK - Catch the possibiliy of a name clash (not sure if this is really an error)
    {
        bool found = false;
        for( const auto& item : mod.items() )
        {
            if( item.data.is_None() )
                continue ;
            if( item.name == des_item_name ) {
                if( found ) {
                    TODO(span, "Duplicate name found resolving use statement searching in module " << mod.path());
                }
                found = true;
            }
        }
    }
    
    if( des_item_name.size() > 0 && des_item_name[0] == '#' ) {
        unsigned int idx = 0;
        if( ::std::sscanf(des_item_name.c_str(), "#%u", &idx) != 1 ) {
            BUG(span, "Invalid anon path segment '" << des_item_name << "'");
        }
        if( idx >= mod.anon_mods().size() ) {
            BUG(span, "Invalid anon path segment '" << des_item_name << "'");
        }
        return ::AST::PathBinding::make_Module({mod.anon_mods()[idx]});
    }
    
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
            (Crate,
                if( allow != Lookup::Value )
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
            (Module,
                if( allow != Lookup::Value )
                    return ::AST::PathBinding::make_Module({&e});
                )
            )
            break ;
        }
    }
    
    // Imports
    for( const auto& imp : mod.imports() )
    {
        const Span& sp2 = imp.data.sp;
        if( imp.name == des_item_name ) {
            DEBUG("- Named import " << imp.name << " = " << imp.data);
            if( imp.data.path.binding().is_Unbound() ) {
                DEBUG(" > Needs resolve");
                // TODO: Handle possibility of recursion
                return Resolve_Use_GetBinding(sp2, crate, Resolve_Use_AbsolutisePath(sp2, mod.path(), imp.data.path), parent_modules, allow);
            }
            else {
                if( allow != Lookup::Any && allow != Lookup::AnyOpt )
                {
                    switch( imp.data.path.binding().tag() )
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
                return imp.data.path.binding().clone();
            }
        }
        if( imp.is_pub && imp.name == "" ) {
            DEBUG("- Search glob of " << imp.data.path);
            // INEFFICIENT! Resolves and throws away the result (because we can't/shouldn't mutate here)
            ::AST::PathBinding  binding_;
            const auto* binding = &imp.data.path.binding();
            if( binding->is_Unbound() ) {
                DEBUG("Temp resolving wildcard " << imp.data);
                // TODO: Handle possibility of recursion
                binding_ = Resolve_Use_GetBinding(sp2, crate, Resolve_Use_AbsolutisePath(sp2, mod.path(), imp.data.path), parent_modules);
                binding = &binding_;
            }
            
            TU_MATCH_DEF(::AST::PathBinding, (*binding), (e),
            (
                BUG(sp2, "Wildcard import expanded to an invalid item class");
                ),
            (Module,
                auto allow_inner = (allow == Lookup::Any ? Lookup::AnyOpt : allow);
                assert(e.module_);
                // TODO: Prevent infinite recursion
                auto rv = Resolve_Use_GetBinding_Mod(span, crate, *e.module_, des_item_name, {}, allow_inner);
                if( ! rv.is_Unbound() ) {
                    return mv$(rv);
                }
                ),
            (Enum,
                TODO(span, "Look up wildcard in enum");
                )
            )
        }
    }

    if( mod.path().nodes().size() > 0 && mod.path().nodes().back().name()[0] == '#' ) {
        assert( parent_modules.size() > 0 );
        return Resolve_Use_GetBinding_Mod(span, crate, *parent_modules.back(), des_item_name, parent_modules.subslice(0, parent_modules.size()-1), allow);
    }
    else {
        if( allow == Lookup::Any )
            ERROR(span, E0000, "Could not find node '" << des_item_name << "' in module " << mod.path());
        else
            return ::AST::PathBinding::make_Unbound({});
    }
}

::AST::PathBinding Resolve_Use_GetBinding__ext(const Span& span, const ::AST::Crate& crate, const ::AST::Path& path,  const ::HIR::Module& hmodr, unsigned int start,  Lookup allow)
{
    const auto& nodes = path.nodes();
    const ::HIR::Module* hmod = &hmodr;
    for(unsigned int i = start; i < nodes.size() - 1; i ++)
    {
        auto it = hmod->m_mod_items.find(nodes[i].name());
        if( it == hmod->m_mod_items.end() ) {
            // BZZT!
            ERROR(span, E0000, "Unable to find path component " << nodes[i].name() << " in " << path);
        }
        TU_MATCH_DEF( ::HIR::TypeItem, (it->second->ent), (e),
        (
            ERROR(span, E0000, "Unexpected item type in import");
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
            
            auto it2 = ::std::find_if( e.m_variants.begin(), e.m_variants.end(), [&](const auto& x){ return x.first == name; } );
            if( it2 == e.m_variants.end() ) {
                ERROR(span, E0000, "Unable to find variant " << path);
            }
            return ::AST::PathBinding::make_EnumVar({ nullptr, static_cast<unsigned int>(it2 - e.m_variants.begin()) });
            )
        )
    }
    if( allow != Lookup::Value )
    {
        auto it = hmod->m_mod_items.find(nodes.back().name());
        if( it != hmod->m_mod_items.end() ) {
            const auto* item_ptr = &it->second->ent;
            if( item_ptr->is_Import() ) {
                const auto& e = item_ptr->as_Import();
                // This doesn't need to recurse - it can just do a single layer (as no Import should refer to another)
                const auto& ec = crate.m_extern_crates.at( e.m_crate_name );
                item_ptr = &ec.m_hir->get_typeitem_by_path(span, e, true);    // ignore_crate_name=true
            }
            TU_MATCHA( (*item_ptr), (e),
            (Import,
                BUG(span, "Recursive import in " << path << " - " << it->second->ent.as_Import() << " -> " << e);
                ),
            (Module,
                return ::AST::PathBinding::make_Module({nullptr, &e});
                ),
            (TypeAlias,
                return ::AST::PathBinding::make_TypeAlias({nullptr});
                ),
            (Enum,
                return ::AST::PathBinding::make_Enum({nullptr});
                ),
            (Struct,
                return ::AST::PathBinding::make_Struct({nullptr});
                ),
            (Trait,
                return ::AST::PathBinding::make_Trait({nullptr});
                )
            )
        }
    }
    if( allow != Lookup::Type )
    {
        auto it2 = hmod->m_value_items.find(nodes.back().name());
        if( it2 != hmod->m_value_items.end() ) {
            TU_MATCHA( (it2->second->ent), (e),
            (Import,
                TODO(span, "Recurse to get binding for an import - " << path << " = " << e);
                ),
            (Constant,
                return ::AST::PathBinding::make_Static({ nullptr });
                ),
            (Static,
                return ::AST::PathBinding::make_Static({ nullptr });
                ),
            (StructConstant,
                return ::AST::PathBinding::make_Struct({ nullptr });
                ),
            (Function,
                return ::AST::PathBinding::make_Function({ nullptr });
                ),
            (StructConstructor,
                return ::AST::PathBinding::make_Struct({ nullptr });
                )
            )
        }
    }
    
    return ::AST::PathBinding::make_Unbound({});
}
::AST::PathBinding Resolve_Use_GetBinding__ext(const Span& span, const ::AST::Crate& crate, const ::AST::Path& path,  const AST::ExternCrate& ec, unsigned int start,  Lookup allow)
{
    return Resolve_Use_GetBinding__ext(span, crate, path, ec.m_hir->m_root_module, start, allow);
}

::AST::PathBinding Resolve_Use_GetBinding(const Span& span, const ::AST::Crate& crate, const ::AST::Path& path, slice< const ::AST::Module* > parent_modules, Lookup allow)
{
    if( path.m_class.is_Absolute() && path.m_class.as_Absolute().crate != "" ) {
        const auto& path_abs = path.m_class.as_Absolute();
        
        return Resolve_Use_GetBinding__ext(span, crate, path,  crate.m_extern_crates.at( path_abs.crate ), 0, allow);
    }
    const AST::Module* mod = &crate.m_root_module;
    const auto& nodes = path.nodes();
    for( unsigned int i = 0; i < nodes.size()-1; i ++ )
    {
        auto b = Resolve_Use_GetBinding_Mod(span, crate, *mod, nodes[i].name(), parent_modules, Lookup::Type);
        TU_MATCH_DEF(::AST::PathBinding, (b), (e),
        (
            ERROR(span, E0000, "Unexpected item type in import");
            ),
        (Crate,
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
                return Resolve_Use_GetBinding__ext(span, crate, path,  *e.hir, i+1, allow);
            }
            mod = e.module_;
            )
        )
    }
    
    assert(mod);
    return Resolve_Use_GetBinding_Mod(span, crate, *mod, nodes.back().name(), parent_modules, allow);
}

