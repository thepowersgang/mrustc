/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * ast/ast.hpp
 * - Core AST header
 */
#ifndef AST_HPP_INCLUDED
#define AST_HPP_INCLUDED

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
#include <serialise.hpp>

#include <ast/pattern.hpp>
#include <ast/attrs.hpp>
#include <ast/expr_ptr.hpp>
#include <ast/item.hpp>
#include <ast/macro.hpp>

#include "generics.hpp"

#include <macro_rules/macro_rules_ptr.hpp>

namespace AST {

class Crate;

class Module;
class Item;

using ::std::unique_ptr;
using ::std::move;

enum eItemType
{
    ITEM_TRAIT,
    ITEM_STRUCT,
    ITEM_FN,
};

struct StructItem
{
    ::AST::MetaItems    m_attrs;
    bool    m_is_public;
    ::std::string   m_name;
    TypeRef m_type;
    
    StructItem()
    {
    }
    
    StructItem(::AST::MetaItems attrs, bool is_pub, ::std::string name, TypeRef ty):
        m_attrs( mv$(attrs) ),
        m_is_public(is_pub),
        m_name( mv$(name) ),
        m_type( mv$(ty) )
    {
    }
    
    friend ::std::ostream& operator<<(::std::ostream& os, const StructItem& x) {
        return os << (x.m_is_public ? "pub " : "") << x.m_name << ": " << x.m_type;
    }
    
    StructItem clone() const;
};

struct TupleItem
{
    ::AST::MetaItems    m_attrs;
    bool    m_is_public;
    TypeRef m_type;
    
    TupleItem()
    {
    }
    
    TupleItem(::AST::MetaItems attrs, bool is_pub, TypeRef ty):
        m_attrs( mv$(attrs) ),
        m_is_public(is_pub),
        m_type( mv$(ty) )
    {
    }
    
    friend ::std::ostream& operator<<(::std::ostream& os, const TupleItem& x) {
        return os << (x.m_is_public ? "pub " : "") << x.m_type;
    }
    
    TupleItem clone() const;
};

class TypeAlias
{
    GenericParams  m_params;
    TypeRef m_type;
public:
    TypeAlias() {}
    TypeAlias(GenericParams params, TypeRef type):
        m_params( move(params) ),
        m_type( move(type) )
    {}
    
    const GenericParams& params() const { return m_params; }
    const TypeRef& type() const { return m_type; }
    
    GenericParams& params() { return m_params; }
    TypeRef& type() { return m_type; }
    
    TypeAlias clone() const;
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
    Static():
        m_class(CONST)
    {}
    Static(Class s_class, TypeRef type, Expr value):
        m_class(s_class),
        m_type( move(type) ),
        m_value( move(value) )
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
    typedef ::std::vector< ::std::pair<AST::Pattern,TypeRef> >   Arglist;

private:
    Span    m_span;
    //::std::string   m_lifetime;
    GenericParams  m_params;
    Expr    m_code;
    TypeRef m_rettype;
    Arglist m_args;
    // TODO: ABI, const, and unsafe
public:
    Function()
    {}
    Function(const Function&) = delete;
    Function& operator=(const Function&) = delete;
    Function(Function&&) = default;
    Function& operator=(Function&&) = default;
    Function(Span sp, GenericParams params, TypeRef ret_type, Arglist args);
    
    void set_code(Expr code) { m_code = ::std::move(code); }
    
    const GenericParams& params() const { return m_params; }
          GenericParams& params()       { return m_params; }
    const Expr& code() const { return m_code; }
          Expr& code()       { return m_code; }
    const TypeRef& rettype() const { return m_rettype; }
          TypeRef& rettype()       { return m_rettype; }
    const Arglist& args() const { return m_args; }
          Arglist& args()       { return m_args; }
    
    Function clone() const;
};

class Trait
{
    GenericParams  m_params;
    ::std::vector< Spanned<AST::Path> > m_supertraits;
    
    bool m_is_marker;
    NamedList<Item> m_items;
public:
    Trait():
        m_is_marker(false)
    {}
    Trait(GenericParams params, ::std::vector< Spanned<Path> > supertraits):
        m_params( mv$(params) ),
        m_supertraits( mv$(supertraits) ),
        m_is_marker(false)
    {
    }
    
    const GenericParams& params() const { return m_params; }
          GenericParams& params()       { return m_params; }
    const ::std::vector<Spanned<Path> >& supertraits() const { return m_supertraits; }
          ::std::vector<Spanned<Path> >& supertraits()       { return m_supertraits; }
    
    const NamedList<Item>& items() const { return m_items; }
          NamedList<Item>& items()       { return m_items; }
    
    void add_type(::std::string name, TypeRef type);
    void add_function(::std::string name, Function fcn);
    void add_static(::std::string name, Static v);
    
    void set_is_marker();
    bool is_marker() const;
    
    bool has_named_item(const ::std::string& name, bool& out_is_fcn) const;
    
    Trait clone() const;
};

TAGGED_UNION_EX(EnumVariantData, (), Value,
    (
    (Value, struct {
        ::AST::Expr m_value;
        }),
    (Tuple, struct {
        ::std::vector<TypeRef>  m_sub_types;
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
    MetaItems   m_attrs;
    ::std::string   m_name;
    EnumVariantData m_data;
    
    EnumVariant()
    {
    }
    
    EnumVariant(MetaItems attrs, ::std::string name, Expr&& value):
        m_attrs( mv$(attrs) ),
        m_name( mv$(name) ),
        m_data( EnumVariantData::make_Value({mv$(value)}) )
    {
    }
    
    EnumVariant(MetaItems attrs, ::std::string name, ::std::vector<TypeRef> sub_types):
        m_attrs( mv$(attrs) ),
        m_name( ::std::move(name) ),
        m_data( EnumVariantData::make_Tuple( {mv$(sub_types)} ) )
    {
    }
    
    EnumVariant(MetaItems attrs, ::std::string name, ::std::vector<StructItem> fields):
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
            os << "(" << e.m_sub_types << ")";
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
    Enum() {}
    Enum( GenericParams params, ::std::vector<EnumVariant> variants ):
        m_params( move(params) ),
        m_variants( move(variants) )
    {}
    
    const GenericParams& params() const { return m_params; }
          GenericParams& params()       { return m_params; }
    const ::std::vector<EnumVariant>& variants() const { return m_variants; }
          ::std::vector<EnumVariant>& variants()       { return m_variants; }
    
    Enum clone() const;
};

TAGGED_UNION_EX(StructData, (), Struct,
    (
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
    
    Struct() {}
    Struct( GenericParams params, ::std::vector<StructItem> fields ):
        m_params( move(params) ),
        m_data( StructData::make_Struct({mv$(fields)}) )
    {}
    Struct( GenericParams params, ::std::vector<TupleItem> fields ):
        m_params( move(params) ),
        m_data( StructData::make_Tuple({mv$(fields)}) )
    {}
    
    const GenericParams& params() const { return m_params; }
          GenericParams& params()       { return m_params; }
    
    TypeRef get_field_type(const char *name, const ::std::vector<TypeRef>& args);
    
    Struct clone() const;
};

class ImplDef
{
    Span    m_span;
    MetaItems   m_attrs;
    GenericParams  m_params;
    Spanned<Path>   m_trait;
    TypeRef m_type;
public:
    ImplDef() {}
    ImplDef(ImplDef&&) /*noexcept*/ = default;
    ImplDef(Span sp, MetaItems attrs, GenericParams params, Spanned<Path> trait_type, TypeRef impl_type):
        m_span( mv$(sp) ),
        m_attrs( mv$(attrs) ),
        m_params( mv$(params) ),
        m_trait( mv$(trait_type) ),
        m_type( mv$(impl_type) )
    {}
    ImplDef& operator=(ImplDef&&) = default;
    
    // Accessors
    const Span& span() const { return m_span; }
    const MetaItems& attrs() const { return m_attrs; }
    
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
        bool    is_pub; // Ignored for trait impls
        bool    is_specialisable;
        ::std::string   name;
        
        ::std::unique_ptr<Item> data;
    };

private:
    ImplDef m_def;
    Span    m_span;
    
    ::std::vector< ImplItem >   m_items;
    //NamedList<TypeRef>   m_types;
    //NamedList<Function>  m_functions;
    //NamedList<Static>    m_statics;
    
public:
    ::std::vector<MacroInvocation>    m_macro_invocations;
    
    Impl() {}
    Impl(Impl&&) /*noexcept*/ = default;
    Impl(ImplDef def):
        m_def( mv$(def) )
    {}
    Impl& operator=(Impl&&) = default;

    void add_function(bool is_public, bool is_specialisable, ::std::string name, Function fcn);
    void add_type(bool is_public, bool is_specialisable, ::std::string name, TypeRef type);
    void add_static(bool is_public, bool is_specialisable, ::std::string name, Static v);
    void add_macro_invocation( MacroInvocation inv ) {
        m_macro_invocations.push_back( mv$(inv) );
    }
    
    const ImplDef& def() const { return m_def; }
          ImplDef& def()       { return m_def; }
    const ::std::vector<ImplItem>& items() const { return m_items; }
          ::std::vector<ImplItem>& items()       { return m_items; }
    
    bool has_named_item(const ::std::string& name) const;

    friend ::std::ostream& operator<<(::std::ostream& os, const Impl& impl);
    
private:
};

struct UseStmt
{
    Span    sp;
    ::AST::MetaItems    attrs;
    ::AST::Path path;
    ::AST::PathBinding  alt_binding;
    
    UseStmt()
    {}
    UseStmt(Span sp, Path p):
        sp(sp),
        path(p)
    {
    }
    
    friend ::std::ostream& operator<<(::std::ostream& os, const UseStmt& x);
};

/// Representation of a parsed (and being converted) function
class Module
{
public:
    class ItemRef
    {
    public:
        enum Type
        {
            TAG_None,
            TAG_Module,
            TAG_Crate,
            TAG_TypeAlias,
            TAG_Function,
            TAG_Trait,
            TAG_Struct,
            TAG_Enum,
            TAG_Static,
            TAG_Use,
        };
    private:    
        Type    m_type;
        const void* m_ref;
    public:
        ItemRef(): m_type(TAG_None) {}
        
        Type tag() const { return m_type; }
        bool is_None() const { return m_type == TAG_None; }
        const Type& as_None() const { return m_type; }  // HACK: Returns &Type in place of &void
        #define _(ty,ident) \
            ItemRef(const ty& ref): m_type(TAG_##ident), m_ref(&ref) {} \
            bool is_##ident() const { return m_type == TAG_##ident; } \
            const ty& as_##ident() const { assert(m_type == TAG_##ident); return *(const ty*)m_ref; }
        _(AST::Module, Module)
        _(::std::string, Crate)
        _(AST::TypeAlias, TypeAlias)
        _(AST::Function, Function)
        _(AST::Trait, Trait)
        _(AST::Struct, Struct)
        _(AST::Enum, Enum)
        _(AST::Static, Static)
        _(AST::Named<UseStmt>, Use)
        #undef _
        friend ::std::ostream& operator<<(::std::ostream& os, const ItemRef& x) {
            switch(x.m_type)
            {
            #define _(ident)    case TAG_##ident:  return os << "ItemRef(" #ident ")";
            _(None)
            _(Module)
            _(Crate)
            _(TypeAlias)
            _(Function)
            _(Trait)
            _(Struct)
            _(Enum)
            _(Static)
            _(Use)
            #undef _
            }
            throw "";
        }
    };
    
private:
    typedef ::std::vector< Named<UseStmt> > itemlist_use_t;
    
    ::AST::Path m_my_path;

    // Module-level items
    /// General items
    ::std::vector<Named<Item>>  m_items;
    /// `use` imports (public and private)
    itemlist_use_t  m_imports;
    /// Macro invocations
    ::std::vector<MacroInvocation>    m_macro_invocations;
    
    /// Impl blocks
    ::std::vector<Impl> m_impls;
    /// Negative impl blocks
    ::std::vector<ImplDef> m_neg_impls;
    

    // --- Runtime caches and state ---
    ::std::vector<Module*>  m_anon_modules;

    ::std::vector< NamedNS<const MacroRules*> > m_macro_import_res; // Vec of imported macros (not serialised)
    ::std::vector< Named<MacroRulesPtr> >  m_macros;

public:
    struct FileInfo
    {
        bool    controls_dir = false;
        ::std::string   path = "!";
    };
    
    FileInfo    m_file_info;
    
    bool    m_insert_prelude = true;    // Set to false by `#[no_prelude]` handler
    char    m_index_populated = 0;  // 0 = no, 1 = partial, 2 = complete
    struct IndexEnt {
        bool is_pub;    // Used as part of glob import checking
        bool is_import; // Set if this item has a path that isn't `mod->path() + name`
        ::AST::Path path;
    };
    // TODO: Add "namespace" list (separate to types)
    ::std::unordered_map< ::std::string, IndexEnt >    m_namespace_items;
    ::std::unordered_map< ::std::string, IndexEnt >    m_type_items;
    ::std::unordered_map< ::std::string, IndexEnt >    m_value_items;

public:
    Module() {}
    Module(::AST::Path path):
        m_my_path( mv$(path) )
    {
    }
    
    bool is_anon() const {
        return m_my_path.nodes().back().name()[0] == '#';
    }
    
    // Called when module is loaded from a serialised format
    void prescan();
    
    /// Create an anon module (for use inside expressions)
    ::std::unique_ptr<AST::Module> add_anon();
    
    void add_item(Named<Item> item);
    void add_item(bool is_pub, ::std::string name, Item it, MetaItems attrs);
    void add_ext_crate(bool is_public, ::std::string ext_name, ::std::string imp_name, MetaItems attrs);
    void add_alias(bool is_public, UseStmt path, ::std::string name, MetaItems attrs);
    
    void add_impl(Impl impl) {
        m_impls.emplace_back( mv$(impl) );
    }
    void add_neg_impl(ImplDef impl) {
        m_neg_impls.emplace_back( mv$(impl) );
    }
    void add_macro(bool is_exported, ::std::string name, MacroRulesPtr macro);
    void add_macro_import(::std::string name, const MacroRules& mr) {
        m_macro_import_res.push_back( NamedNS<const MacroRules*>( mv$(name), &mr, false ) );
    }
    void add_macro_invocation(MacroInvocation item) {
        m_macro_invocations.push_back( mv$(item) );
    }
    
    

    unsigned int add_anon_module(Module* mod_ptr) {
        auto it = ::std::find(m_anon_modules.begin(), m_anon_modules.end(), mod_ptr);
        if( it != m_anon_modules.end() )
            return it - m_anon_modules.begin();
        m_anon_modules.push_back(mod_ptr);
        return m_anon_modules.size()-1;
    }

    const ::AST::Path& path() const { return m_my_path; }
    ItemRef find_item(const ::std::string& needle, bool allow_leaves = true, bool ignore_private_wildcard = true) const;

          ::std::vector<Named<Item>>& items()       { return m_items; }
    const ::std::vector<Named<Item>>& items() const { return m_items; }
    
          itemlist_use_t& imports()       { return m_imports; }
    const itemlist_use_t& imports() const { return m_imports; }
    
          ::std::vector<Impl>&  impls()       { return m_impls; }
    const ::std::vector<Impl>&  impls() const { return m_impls; }
    
          ::std::vector<ImplDef>&   neg_impls()       { return m_neg_impls; }
    const ::std::vector<ImplDef>&   neg_impls() const { return m_neg_impls; }

          ::std::vector<Module*>&   anon_mods()       { return m_anon_modules; }
    const ::std::vector<Module*>&   anon_mods() const { return m_anon_modules; }
    

    ::std::vector<MacroInvocation>& macro_invs() { return m_macro_invocations; }
          NamedList<MacroRulesPtr>&    macros()        { return m_macros; }
    const NamedList<MacroRulesPtr>&    macros()  const { return m_macros; }
    const ::std::vector<NamedNS<const MacroRules*> >  macro_imports_res() const { return m_macro_import_res; }

private:
    void resolve_macro_import(const Crate& crate, const ::std::string& modname, const ::std::string& macro_name);
};


TAGGED_UNION_EX(Item, (), None,
    (
    (None, struct {} ),
    (MacroInv, MacroInvocation),
    (Module, Module),
    (Crate, struct {
        ::std::string   name;
        }),
    
    (Type, TypeAlias),
    (Struct, Struct),
    (Enum, Enum),
    (Trait, Trait),
    
    (Function, Function),
    (Static, Static)
    ),
    
    (, attrs(mv$(x.attrs))), (attrs = mv$(x.attrs);),
    (
    public:
        MetaItems   attrs;
        Span    span;
        
        Item clone() const;
    )
    );


struct ImplRef
{
    const Impl& impl;
    ::std::vector<TypeRef>  params;
    
    ImplRef(const Impl& impl, ::std::vector<TypeRef> params):
        impl(impl),
        params(params)
    {}
    
    ::rust::option<char> find_named_item(const ::std::string& name) const;
};

} // namespace AST

class GenericResolveClosure
{
    const ::AST::GenericParams&  m_params;
    const ::std::vector<TypeRef>&   m_args;
public:
    GenericResolveClosure(const AST::GenericParams& params, const ::std::vector<TypeRef>& args):
        m_params(params),
        m_args(args)
    {}
    const TypeRef& operator()(const char *argname) {
        for(unsigned int i = 0; i < m_params.ty_params().size(); i ++)
        {
            if( m_params.ty_params()[i].name() == argname ) {
                return m_args.at(i);
            }
        }
        throw ::std::runtime_error("BUGCHECK - Unknown arg in field type");
    }
};


#endif // AST_HPP_INCLUDED
