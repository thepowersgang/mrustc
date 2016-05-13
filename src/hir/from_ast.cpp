
#include "hir.hpp"
#include <main_bindings.hpp>
#include <ast/ast.hpp>
#include <ast/crate.hpp>

::HIR::Module LowerHIR_Module(::AST::Module module, ::HIR::SimplePath path);

/// \brief Converts the AST into HIR format
///
/// - Removes all possibility for unexpanded macros
/// - Performs desugaring of for/if-let/while-let/...
::HIR::CratePtr LowerHIR_FromAST(::AST::Crate crate)
{
    ::std::unordered_map< ::std::string, MacroRules >   macros;
    auto rootmod = LowerHIR_Module( mv$(crate.m_root_module), ::HIR::SimplePath("") );
    return ::HIR::CratePtr( ::HIR::Crate { mv$(rootmod), mv$(macros) } );
}


::HIR::Expr LowerHIR_Expr(const ::AST::Expr& e)
{
    throw ::std::runtime_error("TODO: LowerHIR_Expr");
}
::HIR::TypeRef LowerHIR_Type(const ::TypeRef& e)
{
    throw ::std::runtime_error("TODO: LowerHIR_Type");
}

::HIR::TypeAlias LowerHIR_TypeAlias(const ::AST::TypeAlias& ta)
{
    throw ::std::runtime_error("TODO: LowerHIR_TypeAlias");
}

::HIR::Struct LowerHIR_Struct(const ::AST::Struct& ta)
{
    ::HIR::Struct::Data data;

    throw ::std::runtime_error("TODO: LowerHIR_Struct");
    
    return ::HIR::Struct {
        LowerHIR_GenericParams(e.params()),
        mv$(data)
        };
}

::HIR::Enum LowerHIR_Enum(const ::AST::Enum& f)
{
    throw ::std::runtime_error("TODO: LowerHIR_Enum");
}
::HIR::Trait LowerHIR_Trait(const ::AST::Trait& f)
{
    throw ::std::runtime_error("TODO: LowerHIR_Trait");
}
::HIR::Function LowerHIR_Function(const ::AST::Function& f)
{
    throw ::std::runtime_error("TODO: LowerHIR_Function");
}

void _add_mod_ns_item(::HIR::Module& mod, ::std::string name, bool is_pub,  ::HIR::TypeItem ti) {
    mod.m_mod_items.insert( ::std::make_pair( mv$(name), ::HIR::VisEnt< ::HIR::TypeItem> { is_pub, mv$(ti) } ) );
}
void _add_mod_val_item(::HIR::Module& mod, ::std::string name, bool is_pub,  ::HIR::ValueItem ti) {
    mod.m_value_items.insert( ::std::make_pair( mv$(name), ::HIR::VisEnt< ::HIR::ValueItem> { is_pub, mv$(ti) } ) );
}

::HIR::Module LowerHIR_Module(::AST::Module module, ::HIR::SimplePath path)
{
    ::HIR::Module   mod { };

    for( const auto& item : module.items() )
    {
        auto item_path = path + item.name;
        TU_MATCH(::AST::Item, (item.data), (e),
        (None,
            ),
        (Module,
            _add_mod_ns_item( mod,  item.name, item.is_pub, LowerHIR_Module( mv$(e), mv$(item_path)) );
            ),
        (Crate,
            // TODO: All 'extern crate' items should be normalised into a list in the crate root
            ),
        (Type,
            _add_mod_ns_item( mod,  item.name, item.is_pub, ::HIR::TypeItem::make_TypeAlias( LowerHIR_TypeAlias(mv$(e)) ) );
            ),
        (Struct,
            TU_IFLET( ::AST::StructData, e.m_data, Struct, e2,
                ::HIR::TypeRef ty = ::HIR::TypeRef(mv$(item_path));
                if( e2.ents.size() == 0 )
                    _add_mod_val_item( mod,  item.name, item.is_pub, ::HIR::ValueItem::make_StructConstant({mv$(ty)}) );
                else
                    _add_mod_val_item( mod,  item.name, item.is_pub, ::HIR::ValueItem::make_StructConstructor({mv$(ty)}) );
            )
            _add_mod_ns_item( mod,  item.name, item.is_pub, LowerHIR_Struct(mv$(e)) );
            ),
        (Enum,
            _add_mod_ns_item( mod,  item.name, item.is_pub, LowerHIR_Enum(mv$(e)) );
            ),
        (Trait,
            _add_mod_ns_item( mod,  item.name, item.is_pub, LowerHIR_Trait(mv$(e)) );
            ),
        (Function,
            _add_mod_val_item(mod, item.name, item.is_pub,  LowerHIR_Function(mv$(e)));
            ),
        (Static,
            if( e.s_class() == ::AST::Static::CONST )
                _add_mod_val_item(mod, item.name, item.is_pub,  ::HIR::ValueItem::make_Constant(::HIR::Constant {
                    ::HIR::GenericParams {},
                    LowerHIR_Type( mv$(e.type()) ),
                    LowerHIR_Expr( mv$(e.value()) )
                    }));
            else {
                _add_mod_val_item(mod, item.name, item.is_pub,  ::HIR::ValueItem::make_Static(::HIR::Static {
                    (e.s_class() == ::AST::Static::MUT),
                    LowerHIR_Type( mv$(e.type()) ),
                    LowerHIR_Expr( mv$(e.value()) )
                    }));
            }
            )
        )
    }
    
    throw ::std::runtime_error("TODO: LowerHIR_Module");
}


