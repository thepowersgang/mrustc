/*
 * Absolutise and check all 'use' statements
 */
#include <main_bindings.hpp>
#include <ast/crate.hpp>
#include <ast/ast.hpp>
#include <ast/expr.hpp>

::AST::Path Resolve_Use_AbsolutisePath(const ::AST::Path& base_path, ::AST::Path path);
void Resolve_Use_Mod(const ::AST::Crate& crate, ::AST::Module& mod, ::AST::Path path, slice< const ::AST::Module* > parent_modules={});
::AST::PathBinding Resolve_Use_GetBinding(const Span& span, const ::AST::Crate& crate, const ::AST::Path& path, slice< const ::AST::Module* > parent_modules);


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
        
        // TODO: Is this a valid assertion?
        //if( use_stmt.data.path.crate() != "" )
        //    BUG(span, "Use path crate was set before resolve");
        
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
    }
    
    for(auto& i : mod.items())
    {
        if( i.data.is_Module() )
        {
            Resolve_Use_Mod(crate, i.data.as_Module(), path + i.name);
        }
    }
    /*
    unsigned int idx = 0;
    for(auto& mp : mod.anon_mods())
    {
        Resolve_Use_Mod(crate, *mp, path + FMT("#" << idx));
        idx ++;
    }
    */
    // TODO: Handle anon modules by iterating code (allowing correct item mappings)

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
    }
}

::AST::PathBinding Resolve_Use_GetBinding_Mod(const Span& span, const ::AST::Crate& crate, const ::AST::Module& mod, const ::std::string& des_item_name, slice< const ::AST::Module* > parent_modules)
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
    
    if( des_item_name[0] == '#' ) {
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
                return Resolve_Use_GetBinding(sp2, crate, Resolve_Use_AbsolutisePath(sp2, mod.path(), imp.data.path), parent_modules);
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
                binding_ = Resolve_Use_GetBinding(sp2, crate, Resolve_Use_AbsolutisePath(sp2, mod.path(), imp.data.path), parent_modules);
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

    if( mod.path().nodes().back().name()[0] == '#' ) {
        assert( parent_modules.size() > 0 );
        return Resolve_Use_GetBinding_Mod(span, crate, *parent_modules.back(), des_item_name, parent_modules.subslice(0, parent_modules.size()-1));
    }
    else {
        ERROR(span, E0000, "Could not find node '" << des_item_name << "' in module " << mod.path());
    }
}

::AST::PathBinding Resolve_Use_GetBinding(const Span& span, const ::AST::Crate& crate, const ::AST::Path& path, slice< const ::AST::Module* > parent_modules)
{
    const AST::Module* mod = &crate.m_root_module;
    const auto& nodes = path.nodes();
    for( unsigned int i = 0; i < nodes.size()-1; i ++ )
    {
        auto b = Resolve_Use_GetBinding_Mod(span, crate, *mod, nodes[i].name(), parent_modules);
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
    
    return Resolve_Use_GetBinding_Mod(span, crate, *mod, nodes.back().name(), parent_modules);
}

