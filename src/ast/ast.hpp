/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * ast/ast.hpp
 * - Core AST header
 */
#ifndef AST_HPP_INCLUDED
#define AST_HPP_INCLUDED

#include <target_version.hpp>

#include <string>
#include <vector>
#include <stdexcept>
#include "../coretypes.hpp"
#include <memory>
#include <map>
#include <unordered_map>
#include <algorithm>

#include "../parse/tokentree.hpp"
#include "types.hpp"

#include <ast/pattern.hpp>
#include <ast/attrs.hpp>
#include <ast/expr_ptr.hpp>
#include <ast/item.hpp>
#include <ast/macro.hpp>    // MacroInvocation

#include "generics.hpp"

#include <macro_rules/macro_rules_ptr.hpp>
#include <expand/common.hpp>
#include "../hir/asm.hpp"

namespace AST {

class Crate;

class Module;
class Item;

using ::std::unique_ptr;
using ::std::move;

struct StructItem
{
    ::AST::AttributeList   m_attrs;
    ::AST::Visibility   m_vis;
    RcString   m_name;
    TypeRef m_type;

    //StructItem() {}

    StructItem(::AST::AttributeList attrs, AST::Visibility vis, RcString name, TypeRef ty):
        m_attrs( mv$(attrs) ),
        m_vis( mv$(vis) ),
        m_name( mv$(name) ),
        m_type( mv$(ty) )
    {
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const StructItem& x) {
        return os << x.m_vis << x.m_name << ": " << x.m_type;
    }

    StructItem clone() const;
};

struct TupleItem
{
    ::AST::AttributeList    m_attrs;
    ::AST::Visibility   m_vis;
    TypeRef m_type;

    //TupleItem() {}

    TupleItem(::AST::AttributeList attrs, AST::Visibility vis, TypeRef ty):
        m_attrs( mv$(attrs) ),
        m_vis(mv$(vis)),
        m_type( mv$(ty) )
    {
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const TupleItem& x) {
        return os << x.m_vis << x.m_type;
    }

    TupleItem clone() const;
};

class TypeAlias
{
public:
    /// Normal generic parameter definitions
    GenericParams  m_params;
    /// Holds bounds on this type, all bounds encoded as `Self: ...`
    GenericParams   m_self_bounds;
    TypeRef m_type;

    //TypeAlias() {}
    TypeAlias(GenericParams params, TypeRef type):
        m_params( std::move(params) ),
        m_type( std::move(type) )
    {}
    static TypeAlias new_associated_type(GenericParams params, GenericParams type_bounds, TypeRef default_type) {
        TypeAlias rv { std::move(params), std::move(default_type) };
        rv.m_self_bounds = std::move(type_bounds);
        return rv;
    }

    const GenericParams& params() const { return m_params; }
          GenericParams& params()       { return m_params; }
    const TypeRef& type() const { return m_type; }
          TypeRef& type()       { return m_type; }

    TypeAlias clone() const;
};
class TraitAlias
{
public:
    GenericParams  params;
    std::vector<Spanned<Type_TraitPath>>  traits;

    TraitAlias clone() const {
        TraitAlias  rv;
        for(const auto& p : this->traits)
            rv.traits.push_back(p);
        return rv;
    }
};

enum class Linkage
{
    // no `#[linkage]` specified
    Default,
    // "weak" - allow multiple definitions
    Weak,
    // "extern_weak" - This external symbol can be missing
    // - Must be on a `static`
    ExternWeak,
};

class Static
{
public:
    enum Class
    {
        CONST,
        STATIC,
        MUT,
    };
private:
    Class   m_class;
    TypeRef m_type;
    Expr    m_value;
public:
    struct Markings {
        std::string link_name;
        std::string link_section;
        Linkage linkage = Linkage::Default;
    } m_markings;

    Static(Class s_class, TypeRef type, Expr value):
        m_class(s_class),
        m_type( std::move(type) ),
        m_value( std::move(value) )
    {}

    const Class& s_class() const { return m_class; }
    const TypeRef& type() const { return m_type; }
    const Expr& value() const { return m_value; }

    TypeRef& type() { return m_type; }
    Expr& value() { return m_value; }

    Static clone() const;
};

class Function
{
public:
    struct Arg
    {
        ::AST::AttributeList    attrs;
        ::AST::Pattern  pat;
        TypeRef  ty;

        Arg(::AST::Pattern pat, TypeRef ty, ::AST::AttributeList attrs={})
            : attrs(mv$(attrs))
            , pat(mv$(pat))
            , ty(mv$(ty))
        {
        }
    };
    typedef ::std::vector<Arg>   Arglist;
    struct Flags {
        bool    is_const;
        bool    is_unsafe;
        bool    is_async;
        Flags()
            : is_const(false)
            , is_unsafe(false)
            , is_async(false)
        {}
        static Flags make_unsafe() {
            return Flags().set_unsafe();
        }
        Flags set_unsafe() const {
            auto rv = *this;
            rv.is_unsafe = true;
            return rv;
        }
        Flags set_const() const {
            auto rv = *this;
            rv.is_const = true;
            return rv;
        }
        Flags set_async() const {
            auto rv = *this;
            rv.is_async = true;
            return rv;
        }
    };

private:
    Span    m_span;
    GenericParams  m_params;
    Expr    m_code;
    TypeRef m_rettype;
    Arglist m_args;
    bool    m_is_variadic;  // extern only

    ::std::string   m_abi;
    Flags   m_flags;
public:
    struct Markings {
        enum Inline {
            Auto,
            Never,
            Normal,
            Always
        } inline_type = Inline::Auto;
        bool is_cold = false;
        std::vector<unsigned>   rustc_legacy_const_generics;

        std::string link_name;
        std::string link_section;
        Linkage linkage = Linkage::Default;
    } m_markings;


    Function(const Function&) = delete;
    Function& operator=(const Function&) = delete;
    Function(Function&&) = default;
    Function& operator=(Function&&) = default;

    Function(Span sp, ::std::string abi, Flags flags, GenericParams params, TypeRef ret_type, Arglist args, bool is_variadic);
    // Helper for derive, defines an ABI_RUST function with no generics
    Function(Span sp, TypeRef ret_type, Arglist args): Function(sp, ABI_RUST, Flags(), GenericParams(), std::move(ret_type), std::move(args), false) {}

    void set_code(Expr code) { m_code = ::std::move(code); }

    const Span& sp() const { return m_span; }

    const ::std::string& abi() const { return m_abi; };
    bool is_const() const { return m_flags.is_const; }
    bool is_unsafe() const { return m_flags.is_unsafe; }
    bool is_async() const { return m_flags.is_async; }

    const GenericParams& params() const { return m_params; }
          GenericParams& params()       { return m_params; }
    const Expr& code() const { return m_code; }
          Expr& code()       { return m_code; }
    const TypeRef& rettype() const { return m_rettype; }
          TypeRef& rettype()       { return m_rettype; }
    const Arglist& args() const { return m_args; }
          Arglist& args()       { return m_args; }
    bool is_variadic() const { return m_is_variadic; }

    Function clone() const;
};

class Trait
{
    GenericParams  m_params;
    ::std::vector< Spanned<Type_TraitPath> > m_supertraits;
    ::std::vector< Spanned<LifetimeRef> > m_lifetimes;

    bool m_is_marker;
    bool m_is_unsafe;
    NamedList<Item> m_items;
public:
    Trait():
        m_is_marker(false),
        m_is_unsafe(false)
    {}
    Trait(GenericParams params, ::std::vector< Spanned<Type_TraitPath> > supertraits, ::std::vector< Spanned<LifetimeRef> > lifetimes):
        m_params( mv$(params) ),
        m_supertraits( mv$(supertraits) ),
        m_lifetimes( mv$(lifetimes) ),
        m_is_marker(false),
        m_is_unsafe(false)
    {
    }

    const GenericParams& params() const { return m_params; }
          GenericParams& params()       { return m_params; }
    const ::std::vector<Spanned<Type_TraitPath> >& supertraits() const { return m_supertraits; }
          ::std::vector<Spanned<Type_TraitPath> >& supertraits()       { return m_supertraits; }
    const ::std::vector<Spanned<LifetimeRef> >& lifetimes() const { return m_lifetimes; }
          ::std::vector<Spanned<LifetimeRef> >& lifetimes()       { return m_lifetimes; }

    const NamedList<Item>& items() const { return m_items; }
          NamedList<Item>& items()       { return m_items; }

    void add_type(Span sp, RcString name, AttributeList attrs, TypeRef type);
    void add_function(Span sp, RcString name, AttributeList attrs, Function fcn);
    void add_static(Span sp, RcString name, AttributeList attrs, Static v);

    void set_is_marker();
    bool is_marker() const;
    void set_is_unsafe() { m_is_unsafe = true; }
    bool is_unsafe() const { return m_is_unsafe; }

    bool has_named_item(const RcString& name, bool& out_is_fcn) const;

    Trait clone() const;
};

TAGGED_UNION_EX(EnumVariantData, (), Value,
    (
    (Value, struct {
        ::AST::Expr m_value;
        }),
    (Tuple, struct {
        ::std::vector<TupleItem>    m_items;
        }),
    (Struct, struct {
        ::std::vector<StructItem>   m_fields;
        })
    ),
    (), (),
    (
    public:
    )
    );

struct EnumVariant
{
    AttributeList   m_attrs;
    RcString   m_name;
    EnumVariantData m_data;

    EnumVariant()
    {
    }

    EnumVariant(AttributeList attrs, RcString name, Expr&& value):
        m_attrs( mv$(attrs) ),
        m_name( mv$(name) ),
        m_data( EnumVariantData::make_Value({mv$(value)}) )
    {
    }

    EnumVariant(AttributeList attrs, RcString name, ::std::vector<TupleItem> sub_types):
        m_attrs( mv$(attrs) ),
        m_name( ::std::move(name) ),
        m_data( EnumVariantData::make_Tuple( {mv$(sub_types)} ) )
    {
    }

    EnumVariant(AttributeList attrs, RcString name, ::std::vector<StructItem> fields):
        m_attrs( mv$(attrs) ),
        m_name( ::std::move(name) ),
        m_data( EnumVariantData::make_Struct( {mv$(fields)} ) )
    {
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const EnumVariant& x)
    {
        os << "EnumVariant(" << x.m_name;
        TU_MATCH(EnumVariantData, (x.m_data), (e),
        (Value,
            os << " = " << e.m_value;
            ),
        (Tuple,
            os << "(" << e.m_items << ")";
            ),
        (Struct,
            os << " { " << e.m_fields << " }";
            )
        )
        return os << ")";
    }
};

class Enum
{
    GenericParams    m_params;
    ::std::vector<EnumVariant>   m_variants;
public:

    struct Markings {
        enum class Repr {
            Rust,
            U8 ,
            U16,
            U32,
            U64,
            Usize,
            I8,
            I16,
            I32,
            I64,
            Isize
        } repr = Repr::Rust;
        bool is_repr_c = false;
    } m_markings;
    
    Enum() {}
    Enum( GenericParams params, ::std::vector<EnumVariant> variants )
        : m_params( ::std::move(params) )
        , m_variants( ::std::move(variants) )
    {}

    const GenericParams& params() const { return m_params; }
          GenericParams& params()       { return m_params; }
    const ::std::vector<EnumVariant>& variants() const { return m_variants; }
          ::std::vector<EnumVariant>& variants()       { return m_variants; }

    Enum clone() const;
};

TAGGED_UNION_EX(StructData, (), Struct,
    (
    (Unit, struct {}),
    (Tuple, struct {
        ::std::vector<TupleItem>    ents;
        }),
    (Struct, struct {
        ::std::vector<StructItem>   ents;
        })
    ),
    (),(),
    (
    public:
        )
    );

class Struct
{
    GenericParams    m_params;
public:
    StructData  m_data;
    struct Markings {

        Markings() {
        }

        enum class Repr {
            Rust,
            C,
            Simd,
            Transparent,
        } repr = Repr::Rust;
        uint64_t align_value = 0;
        // Indicates packing
        uint64_t max_field_align = 0;

        // 1.39 nonzero etc
        bool    scalar_valid_start_set = false;
        U128    scalar_valid_start;
        bool    scalar_valid_end_set = false;
        U128    scalar_valid_end;
    }   m_markings;

    Struct() {}
    Struct(GenericParams params):
        m_params( ::std::move(params) ),
        m_data( StructData::make_Unit({}) )
    {
    }
    Struct( GenericParams params, ::std::vector<StructItem> fields ):
        m_params( ::std::move(params) ),
        m_data( StructData::make_Struct({mv$(fields)}) )
    {}
    Struct( GenericParams params, ::std::vector<TupleItem> fields ):
        m_params( ::std::move(params) ),
        m_data( StructData::make_Tuple({mv$(fields)}) )
    {}

    const GenericParams& params() const { return m_params; }
          GenericParams& params()       { return m_params; }

    Struct clone() const;
};

class Union
{
public:
    GenericParams   m_params;
    ::std::vector<StructItem>   m_variants;
    struct Markings {
        enum class Repr {
            Rust,
            C,
            Transparent,
        } repr = Repr::Rust;
    } m_markings;

    Union( GenericParams params, ::std::vector<StructItem> fields )
        : m_params( ::std::move(params) )
        , m_variants( ::std::move(fields) )
    {}

    const GenericParams& params() const { return m_params; }
          GenericParams& params()       { return m_params; }

    Union clone() const;
};

class ImplDef
{
    AttributeList   m_attrs;
    bool    m_is_unsafe;
    GenericParams  m_params;
    Spanned<Path>   m_trait;
    TypeRef m_type;
public:
    ImplDef(AttributeList attrs, GenericParams params, Spanned<Path> trait_type, TypeRef impl_type):
        m_attrs( mv$(attrs) ),
        m_is_unsafe( false ),
        m_params( mv$(params) ),
        m_trait( mv$(trait_type) ),
        m_type( mv$(impl_type) )
    {}

    ImplDef(ImplDef&&) /*noexcept*/ = default;
    ImplDef& operator=(ImplDef&&) = default;

    void set_is_unsafe() { m_is_unsafe = true; }
    bool is_unsafe() const { return m_is_unsafe; }

    // Accessors
    const AttributeList& attrs() const { return m_attrs; }
          AttributeList& attrs()       { return m_attrs; }

    const GenericParams& params() const { return m_params; }
          GenericParams& params()       { return m_params; }
    const Spanned<Path>& trait() const { return m_trait; }
          Spanned<Path>& trait()       { return m_trait; }
    const TypeRef& type() const { return m_type; }
          TypeRef& type()       { return m_type; }


    friend ::std::ostream& operator<<(::std::ostream& os, const ImplDef& impl);
};

class Impl
{
public:
    struct ImplItem {
        Span    sp;
        AttributeList   attrs;
        AST::Visibility vis; // Ignored for trait impls
        bool    is_specialisable;
        RcString   name;

        ::std::unique_ptr<Item> data;
    };

private:
    ImplDef m_def;

    ::std::vector< ImplItem >   m_items;
    //NamedList<TypeRef>   m_types;
    //NamedList<Function>  m_functions;
    //NamedList<Static>    m_statics;

public:
    Impl(Impl&&) /*noexcept*/ = default;
    Impl(ImplDef def):
        m_def( mv$(def) )
    {}
    Impl& operator=(Impl&&) = default;

    void add_function(Span sp, AttributeList attrs, AST::Visibility vis, bool is_specialisable, RcString name, Function fcn);
    void add_type    (Span sp, AttributeList attrs, AST::Visibility vis, bool is_specialisable, RcString name, GenericParams params, TypeRef type);
    void add_static  (Span sp, AttributeList attrs, AST::Visibility vis, bool is_specialisable, RcString name, Static v);
    void add_macro_invocation( MacroInvocation inv );

    const ImplDef& def() const { return m_def; }
          ImplDef& def()       { return m_def; }
    const ::std::vector<ImplItem>& items() const { return m_items; }
          ::std::vector<ImplItem>& items()       { return m_items; }

    bool has_named_item(const RcString& name) const;

    friend ::std::ostream& operator<<(::std::ostream& os, const Impl& impl);

private:
};

struct UseItem
{
    Span    sp; // Span covering the entire `use foo;`
    struct Ent {
        Span    sp; // Span covering just the path (final component)
        ::AST::Path path;
        RcString   name;   // If "", this is a glob/wildcard use
        friend ::std::ostream& operator<<(::std::ostream& os, const UseItem::Ent& x);
    };
    ::std::vector<Ent>  entries;

    UseItem clone() const;
    //friend ::std::ostream& operator<<(::std::ostream& os, const UseItem& x);
};

class ExternBlock
{
    ::std::string   m_abi;
    ::std::vector< Named<Item>> m_items;
public:
    struct Link {
        std::string lib_name;
    };
    std::vector<Link>   m_libraries;

    ExternBlock(::std::string abi):
        m_abi( mv$(abi) )
    {}

    const ::std::string& abi() const { return m_abi; }

    void add_item(Named<Item> named_item);

    // NOTE: Only Function and Static are valid.
          ::std::vector<Named<Item>>& items()       { return m_items; }
    const ::std::vector<Named<Item>>& items() const { return m_items; }

    ExternBlock clone() const;
};
class GlobalAsm
{
public:
    ::std::vector<AsmCommon::Line>  lines;
    ::std::vector<AST::Path>    symbols;
    AsmCommon::Options  options;
};

/// Representation of a parsed (and being converted) function
class Module
{
    ::AST::AbsolutePath m_my_path;

    // Module-level items
    /// General items
public:
    ::std::vector<std::unique_ptr<Named<Item>>>  m_items;

private:
    // --- Runtime caches and state ---
    ::std::vector< ::std::shared_ptr<Module> >  m_anon_modules;

    ::std::vector< Named<MacroRulesPtr> >  m_macros;

public:
    struct FileInfo
    {
        bool    controls_dir = false;
        bool    force_no_load = false;
        // Path to this module
        ::std::string   path = "!";
        // Directory controlled by this module
        ::std::string   dir = "";
    };

    FileInfo    m_file_info;

    bool    m_insert_prelude = true;    // Set to false by `#[no_prelude]` handler
    char    m_index_populated = 0;  // 0 = no, 1 = partial, 2 = complete
    struct IndexEnt {
        bool is_import; // Set if this item has a path that isn't `mod->path() + name`
        ::AST::Visibility   vis;
        ::AST::Path path;
    };

    // TODO: Document difference between namespace and Type
    // TODO: These should use IndexEnt<AST::PathBinding<AST::PathBinding_*>>` instead
    ::std::unordered_map< RcString, IndexEnt >    m_namespace_items;
    ::std::unordered_map< RcString, IndexEnt >    m_type_items;
    ::std::unordered_map< RcString, IndexEnt >    m_value_items;
    ::std::unordered_map< RcString, IndexEnt >    m_macro_items;
    // Imported traits are in a different list, because collisions still apply for method lookup
    ::std::vector<::AST::AbsolutePath> m_traits;

    // List of macros imported from other modules (via #[macro_use], includes proc macros)
    // - First value is an absolute path to the macro (including crate name)
    struct MacroImport {
        bool    is_pub;
        RcString   name;   // Can be different, if `use foo as bar` is used
        AST::AbsolutePath   path;
        MacroRef    ref;

        MacroImport clone() const {
            return MacroImport { is_pub, name, path, ref.clone() };
        }
    };
    ::std::vector<MacroImport>  m_macro_imports;

    struct Import {
        bool    is_pub;
        RcString   name;
        ::AST::Path path;   // If `name` is "", then this is a module/enum to glob
    };
    ::std::vector<Import>   m_item_imports;

public:
    Module() {}
    Module(::AST::AbsolutePath path):
        m_my_path( mv$(path) )
    {
    }

    bool is_anon() const {
        return m_my_path.nodes.size() > 0 && m_my_path.nodes.back().c_str()[0] == '#';
    }

    /// Create an anon module (for use inside expressions)
    ::std::shared_ptr<AST::Module> add_anon();

    void add_item(Named<Item> item);
    void add_item(Span sp, Visibility vis, RcString name, Item it, AttributeList attrs);
    void add_ext_crate(Span sp, Visibility vis, RcString ext_name, RcString imp_name, AttributeList attrs);
    void add_macro_invocation(MacroInvocation item);

    void add_macro(bool is_exported, RcString name, MacroRulesPtr macro);



    const ::AST::AbsolutePath& path() const { return m_my_path; }

    //      ::std::vector<Named<Item>>& items()       { return m_items; }
    //const ::std::vector<Named<Item>>& items() const { return m_items; }

          ::std::vector< ::std::shared_ptr<Module> >&   anon_mods()       { return m_anon_modules; }
    const ::std::vector< ::std::shared_ptr<Module> >&   anon_mods() const { return m_anon_modules; }


          NamedList<MacroRulesPtr>&    macros()        { return m_macros; }
    const NamedList<MacroRulesPtr>&    macros()  const { return m_macros; }
};

TAGGED_UNION_EX(Item, (), None,
    (
    (None, struct {} ),
    (MacroInv, MacroInvocation),
    // TODO: MacroDefinition
    (Use, UseItem),

    // Nameless items
    (ExternBlock, ExternBlock),
    (GlobalAsm, GlobalAsm),
    (Impl, Impl),
    (NegImpl, ImplDef),

    (Macro, MacroRulesPtr),
    (Module, Module),
    (Crate, struct {
        RcString   name;
        }),

    (Type, TypeAlias),
    (Struct, Struct),
    (Enum, Enum),
    (Union, Union),
    (Trait, Trait),
    (TraitAlias, TraitAlias),

    (Function, Function),
    (Static, Static)
    ),

    (), (),
    (
        Item clone() const;
    )
    );

} // namespace AST


#endif // AST_HPP_INCLUDED
