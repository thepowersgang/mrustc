/*
 * Build up a name index in all modules (optimising lookups in later stages)
 */
#include <ast/ast.hpp>
#include <ast/crate.hpp>
#include <main_bindings.hpp>

enum class IndexName
{
    Namespace,
    Type,
    Value,
};
::std::ostream& operator<<(::std::ostream& os, const IndexName& loc)
{
    switch(loc)
    {
    case IndexName::Namespace:
        return os << "namespace";
    case IndexName::Type:
        return os << "type";
    case IndexName::Value:
        return os << "value";
    }
    throw "";
}
::std::unordered_map< ::std::string, ::AST::Module::IndexEnt >& get_mod_index(::AST::Module& mod, IndexName location) {
    switch(location)
    {
    case IndexName::Namespace:
        return mod.m_namespace_items;
    case IndexName::Type:
        return mod.m_type_items;
    case IndexName::Value:
        return mod.m_value_items;
    }
    throw "";
}

void _add_item(AST::Module& mod, IndexName location, const ::std::string& name, bool is_pub, ::AST::Path ir, bool error_on_collision=true)
{
    DEBUG("Add " << location << " item '" << name << "': " << ir);
    auto& list = get_mod_index(mod, location);
    
    if( false == list.insert(::std::make_pair(name, ::AST::Module::IndexEnt { is_pub, mv$(ir) } )).second )
    {
        if( error_on_collision ) 
        {
            ERROR(Span(), E0000, "Duplicate definition of name '" << name << "' in " << location << " scope (" << mod.path() << ")");
        }
        else
        {
            DEBUG("Name collision " << mod.path() << ", ignored");
        }
    }
}
void _add_item_type(AST::Module& mod, const ::std::string& name, bool is_pub, ::AST::Path ir, bool error_on_collision=true)
{
    _add_item(mod, IndexName::Namespace, name, is_pub, ::AST::Path(ir), error_on_collision);
    _add_item(mod, IndexName::Type, name, is_pub, mv$(ir), error_on_collision);
}
void _add_item_value(AST::Module& mod, const ::std::string& name, bool is_pub, ::AST::Path ir, bool error_on_collision=true)
{
    _add_item(mod, IndexName::Value, name, is_pub, mv$(ir), error_on_collision);
}

void Resolve_Index_Module_Base(AST::Module& mod)
{
    TRACE_FUNCTION_F("mod = " << mod.path());
    for( const auto& i : mod.items() )
    {
        ::AST::Path p = mod.path() + i.name;
        
        TU_MATCH(AST::Item, (i.data), (e),
        (None,
            ),
        // - Types/modules only
        (Module,
            p.bind( ::AST::PathBinding::make_Module({&e}) );
            _add_item(mod, IndexName::Namespace, i.name, i.is_pub,  mv$(p));
            ),
        (Crate,
            TODO(Span(), "Crate in Resolve_Index_Module");
            //p.bind( ::AST::PathBinding::make_Crate(e) );
            //_add_item(mod, IndexName::Namespace, i.name, i.is_pub,  mv$(p));
            ),
        (Enum,
            p.bind( ::AST::PathBinding::make_Enum({&e}) );
            _add_item_type(mod, i.name, i.is_pub,  mv$(p));
            ),
        (Trait,
            p.bind( ::AST::PathBinding::make_Trait({&e}) );
            _add_item_type(mod, i.name, i.is_pub,  mv$(p));
            ),
        (Type,
            p.bind( ::AST::PathBinding::make_TypeAlias({&e}) );
            _add_item_type(mod, i.name, i.is_pub,  mv$(p));
            ),
        // - Mixed
        (Struct,
            p.bind( ::AST::PathBinding::make_Struct({&e}) );
            // - If the struct is a tuple-like struct (or unit-like), it presents in the value namespace
            if( e.m_data.is_Tuple() ) {
                _add_item_value(mod, i.name, i.is_pub,  p);
            }
            _add_item_type(mod, i.name, i.is_pub,  mv$(p));
            ),
        // - Values only
        (Function,
            p.bind( ::AST::PathBinding::make_Function({&e}) );
            _add_item_value(mod, i.name, i.is_pub,  mv$(p));
            ),
        (Static,
            p.bind( ::AST::PathBinding::make_Static({&e}) );
            _add_item_value(mod, i.name, i.is_pub,  mv$(p));
            )
        )
    }
    
    bool has_pub_wildcard = false;
    // Named imports
    for( const auto& i : mod.imports() )
    {
        if( i.name != "" )
        {
            const auto& sp = i.data.sp;
            const auto& b = i.data.path.binding();
            TU_MATCH(::AST::PathBinding, (b), (e),
            (Unbound,
                BUG(sp, "Import left unbound ("<<i.data.path<<")");
                ),
            (Variable,
                BUG(sp, "Import was bound to variable");
                ),
            (TypeParameter,
                BUG(sp, "Import was bound to type parameter");
                ),
            (TraitMethod,
                BUG(sp, "Import was bound to trait method");
                ),
            (StructMethod,
                BUG(sp, "Import was bound to struct method");
                ),
            
            (Module,  _add_item(mod, IndexName::Namespace, i.name, i.is_pub,  i.data.path); ),
            //(Crate,   _add_item_type(mod, IndexName::Namespace, i.name, i.is_pub,  i.data.path); ),
            (Enum,    _add_item_type(mod, i.name, i.is_pub,  i.data.path); ),
            (Trait,   _add_item_type(mod, i.name, i.is_pub,  i.data.path); ),
            (TypeAlias, _add_item_type(mod, i.name, i.is_pub,  i.data.path); ),
            
            (Struct,
                _add_item_type(mod, i.name, i.is_pub,  i.data.path);
                // - If the struct is a tuple-like struct, it presents in the value namespace
                if( e.struct_->m_data.is_Tuple() ) {
                    _add_item_value(mod, i.name, i.is_pub,  i.data.path);
                }
                ),
            (Static  , _add_item_value(mod, i.name, i.is_pub,  i.data.path); ),
            (Function, _add_item_value(mod, i.name, i.is_pub,  i.data.path); ),
            (EnumVar , _add_item_value(mod, i.name, i.is_pub,  i.data.path); )
            )
        }
        else
        {
            if( i.is_pub )
            {
                has_pub_wildcard = true;
            }
        }
    }
    
    mod.m_index_populated = (has_pub_wildcard ? 1 : 2);
    
    // Handle child modules
    for( auto& i : mod.items() )
    {
        TU_MATCH_DEF(AST::Item, (i.data), (e),
        (
            ),
        (Module,
            Resolve_Index_Module_Base(e);
            )
        )
    }
    for(auto& mp : mod.anon_mods())
    {
        Resolve_Index_Module_Base(*mp);
    }
}

void Resolve_Index_Module_Wildcard(AST::Module& mod, bool handle_pub)
{
    TRACE_FUNCTION_F("mod = " << mod.path());
    // Glob/wildcard imports
    for( const auto& i : mod.imports() )
    {
        if( i.name == "" && i.is_pub == handle_pub )
        {
            const auto& sp = i.data.sp;
            const auto& b = i.data.path.binding();
            TU_MATCH_DEF(::AST::PathBinding, (b), (e),
            (
                BUG(sp, "Glob import of invalid type encountered");
                ),
            (Unbound,
                BUG(sp, "Import left unbound ("<<i.data.path<<")");
                ),
            (Variable,
                BUG(sp, "Import was bound to variable");
                ),
            (TypeParameter,
                BUG(sp, "Import was bound to type parameter");
                ),
            (TraitMethod,
                BUG(sp, "Import was bound to trait method");
                ),
            (StructMethod,
                BUG(sp, "Import was bound to struct method");
                ),
            
            (Module,
                if( e.module_ == &mod ) {
                    ERROR(sp, E0000, "Glob import of self");
                }
                // 1. Begin indexing of this module if it is not already indexed
                if( e.module_->m_index_populated == 1 )
                {
                    // TODO: Handle wildcard import of a module with a public wildcard import
                    TODO(sp, "Handle wildcard import of module with a wildcard (" << e.module_->path() << " by " << mod.path() << ")");
                    //Resolve_Index_Module( *e.module_ );
                }
                for(const auto& vi : e.module_->m_type_items) {
                    if( vi.second.is_pub ) {
                        _add_item_type( mod, vi.first, i.is_pub, vi.second.path, false );
                    }
                }
                for(const auto& vi : e.module_->m_value_items) {
                    if( vi.second.is_pub ) {
                        _add_item_value( mod, vi.first, i.is_pub, vi.second.path, false );
                    }
                }
                ),
            (Enum,
                unsigned int idx = 0;
                for( const auto& ev : e.enum_->variants() ) {
                    ::AST::Path p = mod.path() + ev.m_name;
                    p.bind( ::AST::PathBinding::make_EnumVar({e.enum_, idx}) );
                    if( ev.m_data.is_Struct() ) {
                        _add_item_type ( mod, ev.m_name, i.is_pub, mv$(p), false );
                    }
                    else {
                        _add_item_value( mod, ev.m_name, i.is_pub, mv$(p), false );
                    }
                    
                    idx += 1;
                }
                )
            )
        }
    }
    
    // handle_pub == true first, leading to full resoltion no matter what
    mod.m_index_populated = 2;
    
    // Handle child modules
    for( auto& i : mod.items() )
    {
        TU_MATCH_DEF(AST::Item, (i.data), (e),
        (
            ),
        (Module,
            Resolve_Index_Module_Wildcard(e, handle_pub);
            )
        )
    }
    for(auto& mp : mod.anon_mods())
    {
        Resolve_Index_Module_Wildcard(*mp, handle_pub);
    }
}

void Resolve_Index(AST::Crate& crate)
{
    Resolve_Index_Module_Base(crate.m_root_module);
    Resolve_Index_Module_Wildcard(crate.m_root_module, true);
    Resolve_Index_Module_Wildcard(crate.m_root_module, false);
}
