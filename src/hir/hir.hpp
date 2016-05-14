/*
 * High-level intermediate representation
 *
 * Contains the expanded and desugared AST
 */
#pragma once

#include <cassert>
#include <unordered_map>
#include <vector>
#include <memory>

#include <tagged_union.hpp>

#include <macros.hpp>   // DAMNIT - Why can't I have it be incomplete

#include <hir/type.hpp>
#include <hir/path.hpp>

namespace HIR {

class Expr {
};
class Pattern {
};

class Crate;
class Module;

class Function;
class Static;

class ValueItem;
class TypeItem;

template<typename Ent>
struct VisEnt
{
    bool is_public;
    Ent ent;
};


struct GenericParams
{
};

// --------------------------------------------------------------------
// Type structures
// --------------------------------------------------------------------
struct Static
{
    //GenericParams   m_params;
    
    bool    m_is_mut;
    TypeRef m_type;
    Expr    m_value;
};
struct Constant
{
    GenericParams   m_params;
    
    TypeRef m_type;
    Expr    m_value;
};
struct Function
{
    GenericParams   m_params;
    
    ::std::vector< ::std::pair< Pattern, TypeRef > >    m_args;
    TypeRef m_return;
    
    Expr    m_code;
};

// --------------------------------------------------------------------
// Type structures
// --------------------------------------------------------------------
struct TypeAlias
{
    GenericParams   m_params;
    ::HIR::TypeRef  m_type;
};
struct Enum
{
    TAGGED_UNION(Variant, Unit,
        (Unit, struct{}),
        (Value, ::HIR::Expr),
        (Tuple, ::std::vector<::HIR::TypeRef>),
        (Struct, ::std::pair< ::std::string, ::HIR::TypeRef>)
        );
    GenericParams   m_params;
    ::std::vector< Variant >    m_variants;
};
struct Struct
{
    TAGGED_UNION(Data, Unit,
        (Unit, struct {}),
        (Tuple, ::std::vector< VisEnt<::HIR::TypeRef> >),
        (Named, ::std::vector< ::std::pair< ::std::string, VisEnt<::HIR::TypeRef> > >)
        );
    GenericParams   m_params;
    Data  m_data;
};

struct AssociatedType
{
    GenericParams   m_params;   // For bounds, and maybe HKT?
    ::HIR::TypeRef  m_default;
};
TAGGED_UNION(TraitValueItem, None,
    (None,      struct {}),
    (Constant,  Constant),
    (Static,    Static),
    (Function,  Function)
    );
struct Trait
{
    GenericParams   m_params;
    ::std::vector< ::HIR::GenericPath >  m_parent_traits;
    
    ::std::unordered_map< ::std::string, AssociatedType >   m_types;
    ::std::unordered_map< ::std::string, TraitValueItem >   m_values;
};

class Module
{
public:
    // Contains all values and functions (including type constructors)
    ::std::unordered_map< ::std::string, ::std::unique_ptr<VisEnt<ValueItem>> > m_value_items;
    // Contains types, traits, and modules
    ::std::unordered_map< ::std::string, ::std::unique_ptr<VisEnt<TypeItem>> > m_mod_items;
};

// --------------------------------------------------------------------

TAGGED_UNION(TypeItem, Import,
    (Import, ::HIR::SimplePath),  // `pub use` statements (no globs)
    (Module, Module),
    (TypeAlias, TypeAlias), // NOTE: These don't introduce new values
    (Enum,      Enum),
    (Struct,    Struct),
    (Trait,     Trait)
    );
TAGGED_UNION(ValueItem, Import,
    (Import,    ::HIR::SimplePath),
    (Constant,  Constant),
    (Static,    Static),
    (StructConstant,    struct { ::HIR::TypeRef ty; }),
    (Function,  Function),
    (StructConstructor, struct { ::HIR::TypeRef ty; })
    );

class Crate
{
public:
    Module  m_root_module;
    
    ::std::unordered_map< ::std::string, ::MacroRules >   m_exported_macros;
};

}   // namespace HIR
