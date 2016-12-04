/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/hir.hpp
 * - Processed module tree (High-level Intermediate Representation)
 *
 * Contains the expanded and desugared AST
 */
#pragma once

#include <cassert>
#include <unordered_map>
#include <vector>
#include <memory>

#include <tagged_union.hpp>

#include <macro_rules/macro_rules_ptr.hpp>

#include <hir/type.hpp>
#include <hir/path.hpp>
#include <hir/pattern.hpp>
#include <hir/expr_ptr.hpp>
#include <hir/generic_params.hpp>
#include <hir/crate_ptr.hpp>

#define ABI_RUST    "Rust"

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
    // Variant = Enum variant
    (Variant, struct {
        unsigned int    idx;
        ::std::vector<Literal>  vals;
        }),
    // Literal values
    (Integer, uint64_t),
    (Float, double),
    (BorrowOf, ::HIR::Path),
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
    enum class Receiver {
        Free,
        Value,
        BorrowOwned,
        BorrowUnique,
        BorrowShared,
        //PointerMut,
        //PointerConst,
        Box,
    };
    
    typedef ::std::vector< ::std::pair< ::HIR::Pattern, ::HIR::TypeRef> >   args_t;
    
    Receiver    m_receiver;
    ::std::string   m_abi;
    bool    m_unsafe;
    bool    m_const;
    
    GenericParams   m_params;
    
    args_t  m_args;
    bool    m_variadic;
    TypeRef m_return;
    
    ExprPtr m_code;
    
    //::HIR::TypeRef make_ty(const Span& sp, const ::HIR::PathParams& params) const;
};

// --------------------------------------------------------------------
// Type structures
// --------------------------------------------------------------------
struct TypeAlias
{
    GenericParams   m_params;
    ::HIR::TypeRef  m_type;
};

typedef ::std::vector< VisEnt<::HIR::TypeRef> > t_tuple_fields;
typedef ::std::vector< ::std::pair< ::std::string, VisEnt<::HIR::TypeRef> > >   t_struct_fields;

/// Cache of the state of various language traits on an enum/struct
struct TraitMarkings
{
    /// There is at least one CoerceUnsized impl for this type
    bool    can_coerce = false;
    /// There is at least one CoerceUnsized impl for this type
    bool    can_unsize = false;
    
    /// Indicates that there is at least one Deref impl
    bool    has_a_deref = false;
    
    /// Type is always unsized (i.e. contains an unsized type)
    bool    is_always_unsized = false;
    /// Type is always sized (i.e. cannot contain any unsized types)
    bool    is_always_sized = false;
    /// `true` if there is a Copy impl
    bool    is_copy = false;

    struct AutoMarking {
        // If present, this impl is conditionally true based on the listed type parameters
        ::std::vector< ::HIR::TypeRef> conditions;
        // Implementation state
        bool is_impled;
    };
    
    // General auto trait impls
    mutable ::std::map< ::HIR::SimplePath, AutoMarking>    auto_impls;
};

class Enum
{
public:
    TAGGED_UNION(Variant, Unit,
        (Unit, struct{}),
        (Value, struct {
            ::HIR::ExprPtr  expr;
            Literal val;
            }),
        (Tuple, t_tuple_fields),
        (Struct, t_struct_fields)
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
    
    TraitMarkings   m_markings;
    
    const Variant* get_variant(const ::std::string& ) const;
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
        (Tuple, t_tuple_fields),
        (Named, t_struct_fields)
        );
    
    GenericParams   m_params;
    Repr    m_repr;
    Data    m_data;
    
    TraitMarkings   m_markings;
};
class Union
{
public:
    enum class Repr
    {
        Rust,
        C,
    };
    
    GenericParams   m_params;
    Repr    m_repr;
    t_struct_fields m_variants;
    
    TraitMarkings   m_markings;
};

struct AssociatedType
{
    bool    is_sized;
    ::std::string   m_lifetime_bound;
    ::std::vector< ::HIR::TraitPath>    m_trait_bounds;
    ::HIR::TypeRef  m_default;
};
TAGGED_UNION(TraitValueItem, Constant,
    (Constant,  Constant),
    (Static,    Static),
    (Function,  Function)
    );
struct Trait
{
    GenericParams   m_params;
    ::std::string   m_lifetime;
    ::std::vector< ::HIR::TraitPath >  m_parent_traits;
    
    bool    m_is_marker;    // aka OIBIT
    
    ::std::unordered_map< ::std::string, AssociatedType >   m_types;
    ::std::unordered_map< ::std::string, TraitValueItem >   m_values;
    
    // Indexes into the vtable for each present method and value
    ::std::unordered_map< ::std::string, unsigned int > m_value_indexes;
    // Indexes in the vtable parameter list for each associated type
    ::std::unordered_map< ::std::string, unsigned int > m_type_indexes;
    
    Trait( GenericParams gps, ::std::string lifetime, ::std::vector< ::HIR::TraitPath> parents):
        m_params( mv$(gps) ),
        m_lifetime( mv$(lifetime) ),
        m_parent_traits( mv$(parents) ),
        m_is_marker( false )
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
    (Import, struct { ::HIR::SimplePath path; bool is_variant; unsigned int idx; }),
    (Module, Module),
    (TypeAlias, TypeAlias), // NOTE: These don't introduce new values
    (Enum,      Enum),
    (Struct,    Struct),
    (Union,     Union),
    (Trait,     Trait)
    );
TAGGED_UNION(ValueItem, Import,
    (Import,    struct { ::HIR::SimplePath path; bool is_variant; unsigned int idx; }),
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
    template<typename T>
    struct VisImplEnt {
        bool is_pub;
        bool is_specialisable;
        T   data;
    };

    ::HIR::GenericParams    m_params;
    ::HIR::TypeRef  m_type;
    
    ::std::map< ::std::string, VisImplEnt< ::HIR::Function> >   m_methods;
    ::std::map< ::std::string, VisImplEnt< ::HIR::Constant> >   m_constants;

    ::HIR::SimplePath   m_src_module;
    
    bool matches_type(const ::HIR::TypeRef& tr, t_cb_resolve_type ty_res) const;
    bool matches_type(const ::HIR::TypeRef& tr) const {
        return matches_type(tr, [](const auto& x)->const auto&{ return x; });
    }
};

class TraitImpl
{
public:
    template<typename T>
    struct ImplEnt {
        bool is_specialisable;
        T   data;
    };

    ::HIR::GenericParams    m_params;
    ::HIR::PathParams   m_trait_args;
    ::HIR::TypeRef  m_type;
    
    ::std::map< ::std::string, ImplEnt< ::HIR::Function> > m_methods;
    ::std::map< ::std::string, ImplEnt< ::HIR::Constant> > m_constants;
    ::std::map< ::std::string, ImplEnt< ::HIR::Static> > m_statics;
    
    ::std::map< ::std::string, ImplEnt< ::HIR::TypeRef> > m_types;
    
    ::HIR::SimplePath   m_src_module;
    
    bool matches_type(const ::HIR::TypeRef& tr, t_cb_resolve_type ty_res) const;
    bool matches_type(const ::HIR::TypeRef& tr) const {
        return matches_type(tr, [](const auto& x)->const auto&{ return x; });
    }
    
    bool more_specific_than(const TraitImpl& x) const;
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
    ::std::unordered_map< ::std::string, ::MacroRulesPtr >  m_exported_macros;
    
    /// Language items avaliable through this crate (includes ones from loaded externs)
    ::std::unordered_map< ::std::string, ::HIR::SimplePath> m_lang_items;
    
    ::std::unordered_map< ::std::string, ::HIR::CratePtr>  m_ext_crates;
    
    /// Method called to populate runtime state after deserialisation
    /// See hir/crate_post_load.cpp
    void post_load_update(const ::std::string& loaded_name);
    
    const ::HIR::SimplePath& get_lang_item_path(const Span& sp, const char* name) const;
    const ::HIR::SimplePath& get_lang_item_path_opt(const char* name) const;
    
    const ::HIR::TypeItem& get_typeitem_by_path(const Span& sp, const ::HIR::SimplePath& path, bool ignore_crate_name=false) const;
    const ::HIR::Trait& get_trait_by_path(const Span& sp, const ::HIR::SimplePath& path) const;
    const ::HIR::Struct& get_struct_by_path(const Span& sp, const ::HIR::SimplePath& path) const;
    const ::HIR::Union& get_union_by_path(const Span& sp, const ::HIR::SimplePath& path) const;
    const ::HIR::Enum& get_enum_by_path(const Span& sp, const ::HIR::SimplePath& path) const;
    const ::HIR::Module& get_mod_by_path(const Span& sp, const ::HIR::SimplePath& path) const;
    
    const ::HIR::ValueItem& get_valitem_by_path(const Span& sp, const ::HIR::SimplePath& path, bool ignore_crate_name=false) const;
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
    bool find_auto_trait_impls(const ::HIR::SimplePath& path, const ::HIR::TypeRef& type, t_cb_resolve_type ty_res, ::std::function<bool(const ::HIR::MarkerImpl&)> callback) const;
    bool find_type_impls(const ::HIR::TypeRef& type, t_cb_resolve_type ty_res, ::std::function<bool(const ::HIR::TypeImpl&)> callback) const;
};

}   // namespace HIR
