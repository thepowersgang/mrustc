/*
 * Build up a name index in all modules (optimising lookups in later stages)
 */
#include <ast/ast.hpp>
#include <ast/crate.hpp>
#include <main_bindings.hpp>

void _add_item(AST::Module& mod, bool is_type, const ::std::string& name, bool is_pub, ::AST::PathBinding ir)
{
    DEBUG("Add " << (is_type ? "type" : "value") << " item '" << name << "': " << ir);
    auto& list = (is_type ? mod.m_type_items : mod.m_value_items);
    
    if( false == list.insert( ::std::make_pair(name, ::std::make_pair(is_pub, mv$(ir))) ).second )
    {
        ERROR(Span(), E0000, "Duplicate definition of name '" << name << "' in " << (is_type ? "type" : "value") << " scope (" << mod.path() << ")");
    }
}
void _add_item_type(AST::Module& mod, const ::std::string& name, bool is_pub, ::AST::PathBinding ir)
{
    _add_item(mod, true, name, is_pub, mv$(ir));
}
void _add_item_value(AST::Module& mod, const ::std::string& name, bool is_pub, ::AST::PathBinding ir)
{
    _add_item(mod, false, name, is_pub, mv$(ir));
}

void Resolve_Index_Module(AST::Module& mod)
{
    TRACE_FUNCTION_F("mod = " << mod.path());
    for( const auto& i : mod.items() )
    {
        TU_MATCH(AST::Item, (i.data), (e),
        (None,
            ),
        // - Types/modules only
        (Module,
            _add_item_type(mod, i.name, i.is_pub,  ::AST::PathBinding::make_Module({&e}));
            ),
        (Crate,
            TODO(Span(), "Crate in Resolve_Index_Module");
            //_add_item_type(mod, i.name, i.is_pub,  ::AST::PathBinding::make_Crate(e));
            ),
        (Enum,
            _add_item_type(mod, i.name, i.is_pub,  ::AST::PathBinding::make_Enum({&e}));
            ),
        (Trait,
            _add_item_type(mod, i.name, i.is_pub,  ::AST::PathBinding::make_Trait({&e}));
            ),
        (Type,
            _add_item_type(mod, i.name, i.is_pub,  ::AST::PathBinding::make_TypeAlias({&e}));
            ),
        // - Mixed
        (Struct,
            _add_item_type(mod, i.name, i.is_pub,  ::AST::PathBinding::make_Struct({&e}));
            if( e.m_data.is_Struct() ) {
                _add_item_value(mod, i.name, i.is_pub,  ::AST::PathBinding::make_Struct({&e}));
            }
            ),
        // - Values only
        (Function,
            _add_item_value(mod, i.name, i.is_pub,  ::AST::PathBinding::make_Function({&e}));
            ),
        (Static,
            _add_item_value(mod, i.name, i.is_pub,  ::AST::PathBinding::make_Static({&e}));
            )
        )
    }
    
    for( const auto& i : mod.imports() )
    {
        if( i.name != "" )
        {
            const auto& sp = i.data.sp;
            const auto& b = i.data.path.binding();
            TU_MATCH(::AST::PathBinding, (b), (e),
            (Unbound,
                BUG(sp, "Import left unbound");
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
            
            (Module,  _add_item_type(mod, i.name, i.is_pub,  b.clone()); ),
            //(Crate,   _add_item_type(mod, i.name, i.is_pub,  b.clone()); ),
            (Enum,    _add_item_type(mod, i.name, i.is_pub,  b.clone()); ),
            (Trait,   _add_item_type(mod, i.name, i.is_pub,  b.clone()); ),
            (TypeAlias, _add_item_type(mod, i.name, i.is_pub,  b.clone()); ),
            
            (Struct,
                _add_item_type(mod, i.name, i.is_pub,  b.clone());
                ),
            (Static  , _add_item_value(mod, i.name, i.is_pub,  b.clone()); ),
            (Function, _add_item_value(mod, i.name, i.is_pub,  b.clone()); ),
            (EnumVar , _add_item_value(mod, i.name, i.is_pub,  b.clone()); )
            )
        }
    }
    
    for( auto& i : mod.items() )
    {
        TU_MATCH_DEF(AST::Item, (i.data), (e),
        (
            ),
        (Module,
            Resolve_Index_Module(e);
            )
        )
    }
}

void Resolve_Index(AST::Crate& crate)
{
    Resolve_Index_Module(crate.m_root_module);
}
