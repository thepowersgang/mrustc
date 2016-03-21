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

namespace HIR {

class TypeRef {
};

/// Simple path - Absolute with no generic parameters
class SimplePath {
};
/// Type path - Simple path with one lot of generic params
class TypePath {
public:
    SimplePath  m_path;
    ::std::vector<TypeRef>  m_params;
};
class Path {
};

class Expr {
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
    GenericParams   m_params;
};
struct Function
{
    GenericParams   m_params;
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
TAGGED_UNION(TraitValueItem, Static,
    (Static,    Static),
    (Function,  Function)
    );
struct Trait
{
    GenericParams   m_params;
    ::std::vector< ::HIR::TypePath >  m_parent_traits;
    
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
    // Glob imports
    ::std::vector< VisEnt<::HIR::Path> >   m_glob_imports;
};

// --------------------------------------------------------------------

TAGGED_UNION(TypeItem, Import,
    (Import, ::HIR::Path),
    (Module, Module),
    (TypeAlias, TypeAlias), // NOTE: These don't introduce new values
    (Enum,      Enum),
    (Struct,    Struct),
    (Trait,     Trait)
    );
TAGGED_UNION(ValueItem, Import,
    (Import,    ::HIR::Path),
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
