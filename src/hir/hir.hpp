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

#include <macro_rules/macro_rules.hpp>   // DAMNIT - Why can't I have it be incomplete

#include <hir/type.hpp>
#include <hir/path.hpp>
#include <hir/pattern.hpp>
#include <hir/expr_ptr.hpp>
#include <hir/generic_params.hpp>

namespace HIR {

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

/// Literal type used for constant evaluation
/// NOTE: Intentionally minimal, just covers the values (not the types)
TAGGED_UNION(Literal, Invalid,
    (Invalid, struct {}),
    // List = Array, Tuple, struct literal
    (List, ::std::vector<Literal>), // TODO: Have a variant for repetition lists
    (Integer, uint64_t),
    (Float, double),
    // String = &'static str or &[u8; N]
    (String, ::std::string)
    );
extern ::std::ostream& operator<<(::std::ostream& os, const Literal& v);

// --------------------------------------------------------------------
// Type structures
// --------------------------------------------------------------------
class Static
{
public:
    bool    m_is_mut;
    TypeRef m_type;
    
    ExprPtr m_value;
    Literal   m_value_res;
};
struct Constant
{
    // NOTE: The generics can't influence the value of this `const`
    GenericParams   m_params;
    
    TypeRef m_type;
    ExprPtr m_value;
    Literal   m_value_res;
};
class Function
{
public:
    ::std::string   m_abi;
    bool    m_unsafe;
    bool    m_const;
    
    GenericParams   m_params;
    
    ::std::vector< ::std::pair< Pattern, TypeRef > >    m_args;
    TypeRef m_return;
    
    ExprPtr m_code;
};

// --------------------------------------------------------------------
// Type structures
// --------------------------------------------------------------------
struct TypeAlias
{
    GenericParams   m_params;
    ::HIR::TypeRef  m_type;
};
class Enum
{
public:
    TAGGED_UNION(Variant, Unit,
        (Unit, struct{}),
        (Value, ::HIR::ExprPtr),
        (Tuple, ::std::vector<::HIR::TypeRef>),
        (Struct, ::std::vector< ::std::pair< ::std::string, ::HIR::TypeRef> >)
        );
    enum class Repr
    {
        Rust,
        C,
        U8, U16, U32,
    };
    
    GenericParams   m_params;
    Repr    m_repr;
    ::std::vector< ::std::pair< ::std::string, Variant > >    m_variants;
};
class Struct
{
public:
    enum class Repr
    {
        Rust,
        C,
        Packed,
        //Union,
    };
    TAGGED_UNION(Data, Unit,
        (Unit, struct {}),
        (Tuple, ::std::vector< VisEnt<::HIR::TypeRef> >),
        (Named, ::std::vector< ::std::pair< ::std::string, VisEnt<::HIR::TypeRef> > >)
        );
    
    GenericParams   m_params;
    Repr    m_repr;
    Data    m_data;
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
    ::std::string   m_lifetime;
    ::std::vector< ::HIR::TraitPath >  m_parent_traits;
    
    bool    m_is_marker;
    
    ::std::unordered_map< ::std::string, AssociatedType >   m_types;
    ::std::unordered_map< ::std::string, TraitValueItem >   m_values;
    
    Trait( GenericParams gps, ::std::string lifetime, ::std::vector< ::HIR::TraitPath> parents):
        m_params( mv$(gps) ),
        m_lifetime( mv$(lifetime) ),
        m_parent_traits( mv$(parents) )
    {}
};

class Module
{
public:
    // List of in-scope traits in this module
    ::std::vector< ::HIR::SimplePath>   m_traits;
    
    // Contains all values and functions (including type constructors)
    ::std::unordered_map< ::std::string, ::std::unique_ptr<VisEnt<ValueItem>> > m_value_items;
    // Contains types, traits, and modules
    ::std::unordered_map< ::std::string, ::std::unique_ptr<VisEnt<TypeItem>> > m_mod_items;
    
    Module() {}
    Module(const Module&) = delete;
    Module(Module&& x) = default;
    Module& operator=(const Module&) = delete;
    Module& operator=(Module&&) = default;
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
    (StructConstant,    struct { ::HIR::SimplePath ty; }),
    (Function,  Function),
    (StructConstructor, struct { ::HIR::SimplePath ty; })
    );

// --------------------------------------------------------------------

class TypeImpl
{
public:
    ::HIR::GenericParams    m_params;
    ::HIR::TypeRef  m_type;
    
    ::std::map< ::std::string, ::HIR::Function> m_methods;

    ::HIR::SimplePath   m_src_module;
    
    bool matches_type(const ::HIR::TypeRef& tr, t_cb_resolve_type ty_res) const;
    bool matches_type(const ::HIR::TypeRef& tr) const {
        return matches_type(tr, [](const auto& x)->const auto&{ return x; });
    }
};

class TraitImpl
{
public:
    ::HIR::GenericParams    m_params;
    ::HIR::PathParams   m_trait_args;
    ::HIR::TypeRef  m_type;
    
    ::std::map< ::std::string, ::HIR::Function> m_methods;
    ::std::map< ::std::string, ::HIR::ExprPtr> m_constants;
    ::std::map< ::std::string, ::HIR::TypeRef> m_types;
    
    ::HIR::SimplePath   m_src_module;
    
    bool matches_type(const ::HIR::TypeRef& tr, t_cb_resolve_type ty_res) const;
    bool matches_type(const ::HIR::TypeRef& tr) const {
        return matches_type(tr, [](const auto& x)->const auto&{ return x; });
    }
};

class MarkerImpl
{
public:
    ::HIR::GenericParams    m_params;
    ::HIR::PathParams   m_trait_args;
    bool    is_positive;
    ::HIR::TypeRef  m_type;
    
    ::HIR::SimplePath   m_src_module;
    
    bool matches_type(const ::HIR::TypeRef& tr, t_cb_resolve_type ty_res) const;
    bool matches_type(const ::HIR::TypeRef& tr) const {
        return matches_type(tr, [](const auto& x)->const auto&{ return x; });
    }
};

class Crate
{
public:
    Module  m_root_module;
    
    /// Impl blocks on just a type
    ::std::vector< ::HIR::TypeImpl > m_type_impls;
    /// Impl blocks
    ::std::multimap< ::HIR::SimplePath, ::HIR::TraitImpl > m_trait_impls;
    ::std::multimap< ::HIR::SimplePath, ::HIR::MarkerImpl > m_marker_impls;
    
    /// Macros exported by this crate
    ::std::unordered_map< ::std::string, ::MacroRules >   m_exported_macros;
    
    /// Language items avaliable through this crate (includes ones from loaded externs)
    ::std::unordered_map< ::std::string, ::HIR::SimplePath> m_lang_items;
    
    const ::HIR::SimplePath& get_lang_item_path(const Span& sp, const char* name) const;
    
    const ::HIR::TypeItem& get_typeitem_by_path(const Span& sp, const ::HIR::SimplePath& path) const;
    const ::HIR::Trait& get_trait_by_path(const Span& sp, const ::HIR::SimplePath& path) const;
    const ::HIR::Struct& get_struct_by_path(const Span& sp, const ::HIR::SimplePath& path) const;
    const ::HIR::Enum& get_enum_by_path(const Span& sp, const ::HIR::SimplePath& path) const;
    const ::HIR::Module& get_mod_by_path(const Span& sp, const ::HIR::SimplePath& path) const;
    
    const ::HIR::ValueItem& get_valitem_by_path(const Span& sp, const ::HIR::SimplePath& path) const;
    const ::HIR::Function& get_function_by_path(const Span& sp, const ::HIR::SimplePath& path) const;
    const ::HIR::Static& get_static_by_path(const Span& sp, const ::HIR::SimplePath& path) const {
        const auto& ti = this->get_valitem_by_path(sp, path);
        TU_IFLET(::HIR::ValueItem, ti, Static, e,
            return e;
        )
        else {
            BUG(sp, "`static` path " << path << " didn't point to an enum");
        }
    }
    const ::HIR::Constant& get_constant_by_path(const Span& sp, const ::HIR::SimplePath& path) const {
        const auto& ti = this->get_valitem_by_path(sp, path);
        TU_IFLET(::HIR::ValueItem, ti, Constant, e,
            return e;
        )
        else {
            BUG(sp, "`const` path " << path << " didn't point to an enum");
        }
    }
    
    bool find_trait_impls(const ::HIR::SimplePath& path, const ::HIR::TypeRef& type, t_cb_resolve_type ty_res, ::std::function<bool(const ::HIR::TraitImpl&)> callback) const;
    bool find_type_impls(const ::HIR::TypeRef& type, t_cb_resolve_type ty_res, ::std::function<bool(const ::HIR::TypeImpl&)> callback) const;
};

}   // namespace HIR
