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

class ItemPath;

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
        ::std::unique_ptr<Literal> val;
        }),
    // Literal values
    (Integer, uint64_t),
    (Float, double),
    // Borrow of a path (existing item)
    (BorrowPath, ::HIR::Path),
    // Borrow of inline data
    (BorrowData, ::std::unique_ptr<Literal>),
    // String = &'static str or &[u8; N]
    (String, ::std::string)
    );
extern ::std::ostream& operator<<(::std::ostream& os, const Literal& v);
extern bool operator==(const Literal& l, const Literal& r);
static inline bool operator!=(const Literal& l, const Literal& r) { return !(l == r); }

// --------------------------------------------------------------------
// Value structures
// --------------------------------------------------------------------
struct Linkage
{
    enum class Type
    {
        Auto,   // Default
        Weak,   // Weak linkage (multiple definitions are allowed
        External, // Force the symbol to be externally visible
    };

    // Linkage type
    Type    type = Type::Auto;

    // External symbol name
    ::std::string   name;
};

class Static
{
public:
    Linkage m_linkage;
    bool    m_is_mut;
    TypeRef m_type;

    ExprPtr m_value;

    Literal   m_value_res;
};
class Constant
{
public:
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

    bool    m_save_code;    // Filled by enumerate, defaults to false
    Linkage m_linkage;

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
    /// Indicates that there is at least one Deref impl
    bool    has_a_deref = false;

    /// Indicates that there is a Drop impl
    /// - If there is an impl, there must be an applicable impl to every instance.
    bool has_drop_impl = false;

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

// Trait implementations relevant only to structs
struct StructMarkings
{
    /// This type has a <T: ?Sized> parameter that is used directly
    bool    can_unsize = false;
    /// Index of the parameter that is ?Sized
    unsigned int unsized_param = ~0u;

    // TODO: This would have to be changed for custom DSTs
    enum class DstType {
        None,   // Sized
        Possible,   // A ?Sized parameter
        Slice,  // [T]
        TraitObject,    // (Trait)
    }   dst_type = DstType::None;
    unsigned int unsized_field = ~0u;

    enum class Coerce {
        None,   // No CoerceUnsized impl
        Passthrough,    // Is generic over T: CoerceUnsized
        Pointer,    // Contains a pointer to a ?Sized type
    } coerce_unsized = Coerce::None;

    // If populated, indicates the field that is the coercable pointer.
    unsigned int coerce_unsized_index = ~0u;
    // Index of the parameter that controls the CoerceUnsized (either a T: ?Sized, or a T: CoerceUnsized)
    unsigned int coerce_param = ~0u;
};

class Enum
{
public:
    struct DataVariant {
        ::std::string   name;
        bool is_struct; // Indicates that the variant does not show up in the value namespace
        ::HIR::TypeRef  type;
    };
    enum class Repr
    {
        Rust,
        C,
        Usize, U8, U16, U32, U64,
    };
    struct ValueVariant {
        ::std::string   name;
        ::HIR::ExprPtr  expr;
        // TODO: Signed.
        uint64_t val;
    };
    TAGGED_UNION(Class, Data,
        (Data, ::std::vector<DataVariant>),
        (Value, struct {
            Repr    repr;
            ::std::vector<ValueVariant> variants;
            })
        );

    GenericParams   m_params;
    Class   m_data;

    TraitMarkings   m_markings;

    size_t num_variants() const {
        return (m_data.is_Data() ? m_data.as_Data().size() : m_data.as_Value().variants.size());
    }
    size_t find_variant(const ::std::string& ) const;

    /// Returns true if this enum is a C-like enum (has values only)
    bool is_value() const;
    /// Returns the value for the given variant (onlu for value enums)
    uint32_t get_value(size_t variant) const;
};
class Struct
{
public:
    enum class Repr
    {
        Rust,
        C,
        Packed,
        Simd,
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
    StructMarkings  m_struct_markings;
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
class Trait
{
public:
    GenericParams   m_params;
    ::std::string   m_lifetime;
    ::std::vector< ::HIR::TraitPath >  m_parent_traits;

    bool    m_is_marker;    // aka OIBIT

    ::std::unordered_map< ::std::string, AssociatedType >   m_types;
    ::std::unordered_map< ::std::string, TraitValueItem >   m_values;

    // Indexes into the vtable for each present method and value
    ::std::unordered_multimap< ::std::string, ::std::pair<unsigned int,::HIR::GenericPath> > m_value_indexes;
    // Indexes in the vtable parameter list for each associated type
    ::std::unordered_map< ::std::string, unsigned int > m_type_indexes;

    // Flattend set of parent traits (monomorphised and associated types fixed)
    ::std::vector< ::HIR::TraitPath >  m_all_parent_traits;

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

    ::std::vector< ::std::pair<::std::string, Static> >  m_inline_statics;

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

    // 
    //const TraitImpl*    m_parent_spec_impl;

    bool matches_type(const ::HIR::TypeRef& tr, t_cb_resolve_type ty_res) const;
    bool matches_type(const ::HIR::TypeRef& tr) const {
        return matches_type(tr, [](const auto& x)->const auto&{ return x; });
    }

    bool more_specific_than(const TraitImpl& x) const;
    bool overlaps_with(const Crate& crate, const TraitImpl& other) const;
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

class ExternCrate
{
public:
    ::HIR::CratePtr m_data;
    ::std::string   m_basename; // Just the filename (serialised)
    ::std::string   m_path; // The path used to load this crate
};
class ExternLibrary
{
public:
    ::std::string   name;
};
class ProcMacro
{
public:
    // Name of the macro
    ::std::string   name;
    // Path to the handler
    ::HIR::SimplePath   path;
    // A list of attributes to hand to the handler
    ::std::vector<::std::string>    attributes;
};
class Crate
{
public:
    ::std::string   m_crate_name;

    Module  m_root_module;

    /// Impl blocks on just a type
    ::std::vector< ::HIR::TypeImpl > m_type_impls;
    /// Impl blocks
    ::std::multimap< ::HIR::SimplePath, ::HIR::TraitImpl > m_trait_impls;
    ::std::multimap< ::HIR::SimplePath, ::HIR::MarkerImpl > m_marker_impls;

    /// Macros exported by this crate
    ::std::unordered_map< ::std::string, ::MacroRulesPtr >  m_exported_macros;
    /// Procedural macros presented
    ::std::vector< ::HIR::ProcMacro>    m_proc_macros;

    /// Language items avaliable through this crate (includes ones from loaded externs)
    ::std::unordered_map< ::std::string, ::HIR::SimplePath> m_lang_items;

    /// Referenced crates
    ::std::unordered_map< ::std::string, ExternCrate>  m_ext_crates;
    /// Referenced system libraries
    ::std::vector<ExternLibrary>    m_ext_libs;
    /// Extra paths for the linker
    ::std::vector<::std::string>    m_link_paths;

    /// Method called to populate runtime state after deserialisation
    /// See hir/crate_post_load.cpp
    void post_load_update(const ::std::string& loaded_name);

    const ::HIR::SimplePath& get_lang_item_path(const Span& sp, const char* name) const;
    const ::HIR::SimplePath& get_lang_item_path_opt(const char* name) const;

    const ::HIR::TypeItem& get_typeitem_by_path(const Span& sp, const ::HIR::SimplePath& path, bool ignore_crate_name=false, bool ignore_last_node=false) const;
    const ::HIR::Trait& get_trait_by_path(const Span& sp, const ::HIR::SimplePath& path) const;
    const ::HIR::Struct& get_struct_by_path(const Span& sp, const ::HIR::SimplePath& path) const;
    const ::HIR::Union& get_union_by_path(const Span& sp, const ::HIR::SimplePath& path) const;
    const ::HIR::Enum& get_enum_by_path(const Span& sp, const ::HIR::SimplePath& path) const;
    const ::HIR::Module& get_mod_by_path(const Span& sp, const ::HIR::SimplePath& path, bool ignore_last_node=false) const;

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

    const ::MIR::Function* get_or_gen_mir(const ::HIR::ItemPath& ip, const ::HIR::ExprPtr& ep, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_ty) const;
    const ::MIR::Function* get_or_gen_mir(const ::HIR::ItemPath& ip, const ::HIR::Function& fcn) const {
        return get_or_gen_mir(ip, fcn.m_code, fcn.m_args, fcn.m_return);
    }
    const ::MIR::Function* get_or_gen_mir(const ::HIR::ItemPath& ip, const ::HIR::ExprPtr& ep, const ::HIR::TypeRef& exp_ty) const {
        static ::HIR::Function::args_t  s_args;
        return get_or_gen_mir(ip, ep, s_args, exp_ty);
    }
};

}   // namespace HIR
