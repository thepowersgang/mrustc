/*
 * Absolutise and check all 'use' statements
 */
#include <main_bindings.hpp>
#include <ast/crate.hpp>
#include <ast/ast.hpp>

void Resolve_Use_Mod(const ::AST::Crate& crate, ::AST::Module& mod, ::AST::Path path);
::AST::Path Resolve_Use_AbsolutisePath(const ::AST::Path& base_path, ::AST::Path path);
::AST::PathBinding Resolve_Use_GetBinding(const Span& span, const ::AST::Crate& crate, const ::AST::Path& path);

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
        return base_path + path;
        ),
    (Super,
        assert(e.count >= 1);
        AST::Path   np(base_path.crate(), {});
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

void Resolve_Use_Mod(const ::AST::Crate& crate, ::AST::Module& mod, ::AST::Path path)
{
    TRACE_FUNCTION_F("path = " << path << ", mod.path() = " << mod.path());
    for(auto& use_stmt : mod.imports())
    {
        const Span& span = use_stmt.data.sp;
        use_stmt.data.path = Resolve_Use_AbsolutisePath(span, path, mv$(use_stmt.data.path));
        if( !use_stmt.data.path.m_class.is_Absolute() )
            BUG(span, "Use path is not absolute after absolutisation");
        
        // TODO: Is this a valid assertion?
        if( use_stmt.data.path.crate() != "" )
            BUG(span, "Use path crate was set before resolve");
        
        use_stmt.data.path.bind( Resolve_Use_GetBinding(span, crate, use_stmt.data.path) );
        
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
    }
    
    for(auto& i : mod.items())
    {
        if( i.data.is_Module() )
        {
            Resolve_Use_Mod(crate, i.data.as_Module(), path + i.name);
        }
    }
}

::AST::PathBinding Resolve_Use_GetBinding_Mod(const Span& span, const ::AST::Crate& crate, const ::AST::Module& mod, const ::std::string& des_item_name)
{
    for( const auto& item : mod.items() )
    {
        if( item.data.is_None() )
            continue ;
        
        if( item.name == des_item_name ) {
            TU_MATCH(::AST::Item, (item.data), (e),
            (None,
                // IMPOSSIBLe - Handled above
                ),
            (Crate,
                //return ::AST::PathBinding::make_Crate({&e});
                TODO(span, "Handle importing from a crate");
                ),
            (Type,
                return ::AST::PathBinding::make_TypeAlias({&e});
                ),
            (Trait,
                return ::AST::PathBinding::make_Trait({&e});
                ),
            
            (Function,
                return ::AST::PathBinding::make_Function({&e});
                ),
            (Static,
                return ::AST::PathBinding::make_Static({&e});
                ),
            (Struct,
                return ::AST::PathBinding::make_Struct({&e});
                ),
            (Enum,
                return ::AST::PathBinding::make_Enum({&e});
                ),
            (Module,
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
                return Resolve_Use_GetBinding(sp2, crate, Resolve_Use_AbsolutisePath(sp2, mod.path(), imp.data.path));
            }
            else {
                return imp.data.path.binding().clone();
            }
        }
        if( imp.is_pub && imp.name == "" ) {
            // INEFFICIENT! Resolves and throws away the result (because we can't/shouldn't mutate here)
            ::AST::PathBinding  binding_;
            const auto* binding = &imp.data.path.binding();
            if( binding->is_Unbound() ) {
                DEBUG("Temp resolving wildcard " << imp.data);
                // TODO: Handle possibility of recursion
                binding_ = Resolve_Use_GetBinding(sp2, crate, Resolve_Use_AbsolutisePath(sp2, mod.path(), imp.data.path));
                binding = &binding_;
            }
            
            TU_MATCH_DEF(::AST::PathBinding, ((*binding)), (e),
            (
                BUG(sp2, "Wildcard import expanded to an invalid item class");
                ),
            (Module,
                TODO(span, "Look up wildcard in module");
                ),
            (Enum,
                TODO(span, "Look up wildcard in enum");
                )
            )
        }
    }

    ERROR(span, E0000, "Could not find node '" << des_item_name << "' in module " << mod.path());
}

::AST::PathBinding Resolve_Use_GetBinding(const Span& span, const ::AST::Crate& crate, const ::AST::Path& path)
{
    const AST::Module* mod = &crate.m_root_module;
    const auto& nodes = path.nodes();
    for( unsigned int i = 0; i < nodes.size()-1; i ++ )
    {
        auto b = Resolve_Use_GetBinding_Mod(span, crate, *mod, nodes[i].name());
        TU_MATCH_DEF(::AST::PathBinding, (b), (e),
        (
            ERROR(span, E0000, "Unexpected item type in import");
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
            mod = e.module_;
            )
        )
    }
    
    return Resolve_Use_GetBinding_Mod(span, crate, *mod, nodes.back().name());
}

