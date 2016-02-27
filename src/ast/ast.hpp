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
#include <algorithm>

#include "../parse/tokentree.hpp"
#include "../types.hpp"
#include "../macros.hpp"
#include <serialise.hpp>

#include "pattern.hpp"
#include "attrs.hpp"
#include "expr.hpp"
#include "macro.hpp"

namespace AST {

using ::std::unique_ptr;
using ::std::move;

class TypeParam:
    public Serialisable
{
    ::std::string   m_name;
    TypeRef m_default;
public:
    TypeParam(): m_name("") {}
    TypeParam(::std::string name):
        m_name( ::std::move(name) )
    {}
    void setDefault(TypeRef type) {
        assert(m_default.is_wildcard());
        m_default = ::std::move(type);
    }
    
    const ::std::string&    name() const { return m_name; }
    const TypeRef& get_default() const { return m_default; }
    
    TypeRef& get_default() { return m_default; }
    
    friend ::std::ostream& operator<<(::std::ostream& os, const TypeParam& tp);
    SERIALISABLE_PROTOTYPES();
};

TAGGED_UNION_EX( GenericBound, (: public Serialisable), Lifetime,
    (
    // Lifetime bound: 'test must be valid for 'bound
    (Lifetime, (
        ::std::string   test;
        ::std::string   bound;
        )),
    // Type lifetime bound
    (TypeLifetime, (
        TypeRef type;
        ::std::string   bound;
        )),
    // Standard trait bound: "Type: [for<'a>] Trait"
    (IsTrait, (
        TypeRef type;
        ::std::vector< ::std::string>   hrls; // Higher-ranked lifetimes
        AST::Path   trait;
        )),
    // Removed trait bound: "Type: ?Trait"
    (MaybeTrait, (
        TypeRef type;
        AST::Path   trait;
        )),
    // Negative trait bound: "Type: !Trait"
    (NotTrait, (
        TypeRef type;
        AST::Path   trait;
        )),
    // Type equality: "Type = Replacement"
    (Equality, (
        TypeRef type;
        TypeRef replacement;
        ))
    ),
    (
    public:
        SERIALISABLE_PROTOTYPES();
        
        GenericBound clone() const {
            TU_MATCH(GenericBound, ( (*this) ), (ent),
            (Lifetime,     return make_Lifetime({ent.test, ent.bound});     ),
            (TypeLifetime, return make_TypeLifetime({ent.type, ent.bound}); ),
            (IsTrait,    return make_IsTrait({ent.type, ent.hrls, ent.trait}); ),
            (MaybeTrait, return make_MaybeTrait({ent.type, ent.trait}); ),
            (NotTrait,   return make_NotTrait({ent.type, ent.trait}); ),
            (Equality,   return make_Equality({ent.type, ent.replacement}); )
            )
            return GenericBound();
        }
        )
    );

::std::ostream& operator<<(::std::ostream& os, const GenericBound& x);

class GenericParams:
    public Serialisable
{
    ::std::vector<TypeParam>    m_type_params;
    ::std::vector< ::std::string > m_lifetime_params;
    ::std::vector<GenericBound>    m_bounds;
public:
    GenericParams() {}
    GenericParams(GenericParams&& x) noexcept:
        m_type_params( mv$(x.m_type_params) ),
        m_lifetime_params( mv$(x.m_lifetime_params) ),
        m_bounds( mv$(x.m_bounds) )
    {}
    GenericParams& operator=(GenericParams&& x) {
        m_type_params = mv$(x.m_type_params);
        m_lifetime_params = mv$(x.m_lifetime_params);
        m_bounds = mv$(x.m_bounds);
        return *this;
    }
    GenericParams(const GenericParams& x):
        m_type_params(x.m_type_params),
        m_lifetime_params(x.m_lifetime_params),
        m_bounds()
    {
        m_bounds.reserve( x.m_bounds.size() );
        for(auto& e: x.m_bounds)
            m_bounds.push_back( e.clone() );
    }
    
    const ::std::vector<TypeParam>& ty_params() const { return m_type_params; }
    const ::std::vector< ::std::string>&    lft_params() const { return m_lifetime_params; }
    const ::std::vector<GenericBound>& bounds() const { return m_bounds; }
    ::std::vector<TypeParam>& ty_params() { return m_type_params; }
    ::std::vector<GenericBound>& bounds() { return m_bounds; }
    
    void add_ty_param(TypeParam param) { m_type_params.push_back( ::std::move(param) ); }
    void add_lft_param(::std::string name) { m_lifetime_params.push_back( ::std::move(name) ); }
    void add_bound(GenericBound bound) {
        m_bounds.push_back( ::std::move(bound) );
    }
    
    int find_name(const char* name) const;
    bool check_params(Crate& crate, const ::std::vector<TypeRef>& types) const;
    bool check_params(Crate& crate, ::std::vector<TypeRef>& types, bool allow_infer) const;
    
    friend ::std::ostream& operator<<(::std::ostream& os, const GenericParams& tp);
    SERIALISABLE_PROTOTYPES();
};

enum eItemType
{
    ITEM_TRAIT,
    ITEM_STRUCT,
    ITEM_FN,
};

template <typename T>
struct ItemNS
{
    ::std::string   name;
    T   data;
    bool    is_pub;
    
    ItemNS():
        is_pub(false)
    {}
    ItemNS(ItemNS&&) noexcept = default;
    ItemNS(const ItemNS&) = default;
    ItemNS(::std::string name, T data, bool is_pub):
        name( ::std::move(name) ),
        data( ::std::move(data) ),
        is_pub( is_pub )
    {
    }
    
    //friend ::std::ostream& operator<<(::std::ostream& os, const Item& i) {
    //    return os << (i.is_pub ? "pub " : " ") << i.name << ": " << i.data;
    //}
};

template <typename T>
struct Item:
    public ItemNS<T>,
    public Serialisable
{
    Item():
        ItemNS<T>()
    {}
    Item(Item&&) noexcept = default;
    Item(const Item&) = default;
    Item(::std::string name, T data, bool is_pub):
        ItemNS<T>( ::std::move(name), ::std::move(data), is_pub )
    {}
    SERIALISE_TYPE_A(, "Item", {
        s.item(this->name);
        s.item(this->data);
        s.item(this->is_pub);
    })
};

template <typename T>
using ItemList = ::std::vector<Item<T> >;

typedef Item<TypeRef>    StructItem;
class Crate;

class TypeAlias:
    public Serialisable
{
    MetaItems   m_attrs;
    GenericParams  m_params;
    TypeRef m_type;
public:
    TypeAlias() {}
    TypeAlias(MetaItems attrs, GenericParams params, TypeRef type):
        m_attrs( move(attrs) ),
        m_params( move(params) ),
        m_type( move(type) )
    {}
    
    const MetaItems& attrs() const { return m_attrs; }
    const GenericParams& params() const { return m_params; }
    const TypeRef& type() const { return m_type; }
    
    GenericParams& params() { return m_params; }
    TypeRef& type() { return m_type; }
    
    SERIALISABLE_PROTOTYPES();
};

class Static:
    public Serialisable
{
public:
    enum Class
    {
        CONST,
        STATIC,
        MUT,
    };
private:
    MetaItems   m_attrs;
    Class   m_class;
    TypeRef m_type;
    Expr    m_value;
public:
    Static():
        m_class(CONST)
    {}
    Static(MetaItems attrs, Class s_class, TypeRef type, Expr value):
        m_attrs( move(attrs) ),
        m_class(s_class),
        m_type( move(type) ),
        m_value( move(value) )
    {}
   
    const MetaItems& attrs() const { return m_attrs; }
    const Class& s_class() const { return m_class; } 
    const TypeRef& type() const { return m_type; }
    const Expr& value() const { return m_value; }
    
    TypeRef& type() { return m_type; }
    Expr& value() { return m_value; }
    
    SERIALISABLE_PROTOTYPES();
};

class Function:
    public Serialisable
{
public:
    typedef ::std::vector< ::std::pair<AST::Pattern,TypeRef> >   Arglist;

private:
    MetaItems   m_attrs;
    ::std::string   m_lifetime;
    GenericParams  m_params;
    Expr    m_code;
    TypeRef m_rettype;
    Arglist m_args;
public:
    Function()
    {}
    Function(const Function&) = delete;
    Function(Function&&) noexcept = default;
    Function(MetaItems attrs, GenericParams params, TypeRef ret_type, Arglist args):
        m_attrs( move(attrs) ),
        m_params( move(params) ),
        m_rettype( move(ret_type) ),
        m_args( move(args) )
    {
    }
    
    void set_code(Expr code) { m_code = ::std::move(code); }
    void set_self_lifetime(::std::string s) { m_lifetime = s; }
    
    const MetaItems& attrs() const { return m_attrs; }
    const GenericParams& params() const { return m_params; }
    const Expr& code() const { return m_code; }
    const TypeRef& rettype() const { return m_rettype; }
    const Arglist& args() const { return m_args; }
    
    GenericParams& params() { return m_params; }
    Expr& code() { return m_code; }
    TypeRef& rettype() { return m_rettype; }
    Arglist& args() { return m_args; }
    
    SERIALISABLE_PROTOTYPES();
};

class Trait:
    public Serialisable
{
    MetaItems   m_attrs;
    GenericParams  m_params;
    ::std::vector<AST::Path>    m_supertraits;
    ItemList<TypeAlias> m_types;
    ItemList<Function>  m_functions;
    ItemList<Static>    m_statics;
public:
    Trait() {}
    Trait(MetaItems attrs, GenericParams params, ::std::vector<Path> supertraits):
        m_attrs( mv$(attrs) ),
        m_params( mv$(params) ),
        m_supertraits( mv$(supertraits) )
    {
    }
    
    const MetaItems& attrs() const { return m_attrs; }
    const GenericParams& params() const { return m_params; }
    const ::std::vector<Path>& supertraits() const { return m_supertraits; }
    const ItemList<Function>& functions() const { return m_functions; }
    const ItemList<TypeAlias>& types() const { return m_types; }
    const ItemList<Static>& statics() const { return m_statics; }

    GenericParams& params() { return m_params; }
    ::std::vector<Path>& supertraits() { return m_supertraits; }
    ItemList<Function>& functions() { return m_functions; }
    ItemList<TypeAlias>& types() { return m_types; }
    
    void add_type(::std::string name, TypeRef type) {
        m_types.push_back( Item<TypeAlias>(move(name), TypeAlias(MetaItems(), GenericParams(), move(type)), true) );
    }
    void add_function(::std::string name, Function fcn) {
        m_functions.push_back( Item<Function>(::std::move(name), ::std::move(fcn), true) );
    }
    void add_static(::std::string name, Static v) {
        m_statics.push_back( Item<Static>(mv$(name), mv$(v), true) );
    }
    
    bool has_named_item(const ::std::string& name, bool& out_is_fcn) const {
        for( const auto& f : m_functions )
            if( f.name == name ) {
                out_is_fcn = true;
                return true;
            }
        for( const auto& f : m_types )
            if( f.name == name ) {
                out_is_fcn = false;
                return true;
            }
        
        //for( const auto& st : 
        return false;
    }
    
    SERIALISABLE_PROTOTYPES();
};

struct EnumVariant:
    public Serialisable
{
    MetaItems   m_attrs;
    ::std::string   m_name;
    ::std::vector<TypeRef>  m_sub_types;
    ::std::vector<StructItem>  m_fields;
    AST::Expr m_value;
    
    EnumVariant()
    {
    }
    
    EnumVariant(MetaItems attrs, ::std::string name, Expr&& value):
        m_attrs( mv$(attrs) ),
        m_name( mv$(name) ),
        m_value( mv$(value) )
    {
    }
    
    EnumVariant(MetaItems attrs, ::std::string name, ::std::vector<TypeRef> sub_types):
        m_attrs( move(attrs) ),
        m_name( ::std::move(name) ),
        m_sub_types( ::std::move(sub_types) )
    {
    }
    
    EnumVariant(MetaItems attrs, ::std::string name, ::std::vector<StructItem> fields):
        m_attrs( move(attrs) ),
        m_name( ::std::move(name) ),
        m_fields( ::std::move(fields) )
    {
    }
    
    friend ::std::ostream& operator<<(::std::ostream& os, const EnumVariant& x) {
        return os << "EnumVariant(" << x.m_name << "(" << x.m_sub_types << ") = " << x.m_value << ")";
    }
    
    SERIALISABLE_PROTOTYPES();
};

class Enum:
    public Serialisable
{
    MetaItems   m_attrs;
    GenericParams    m_params;
    ::std::vector<EnumVariant>   m_variants;
public:
    Enum() {}
    Enum( MetaItems attrs, GenericParams params, ::std::vector<EnumVariant> variants ):
        m_attrs( move(attrs) ),
        m_params( move(params) ),
        m_variants( move(variants) )
    {}
    
    const MetaItems& attrs() const { return m_attrs; }
    const GenericParams& params() const { return m_params; }
    const ::std::vector<EnumVariant>& variants() const { return m_variants; }
    
    GenericParams& params() { return m_params; }
    ::std::vector<EnumVariant>& variants() { return m_variants; }
    
    SERIALISABLE_PROTOTYPES();
};

class Struct:
    public Serialisable
{
    MetaItems   m_attrs;
    GenericParams    m_params;
    ::std::vector<StructItem>   m_fields;
public:
    Struct() {}
    Struct( MetaItems attrs, GenericParams params, ::std::vector<StructItem> fields ):
        m_attrs( move(attrs) ),
        m_params( move(params) ),
        m_fields( move(fields) )
    {}
    
    const MetaItems&    attrs()  const { return m_attrs; }
    const GenericParams&   params() const { return m_params; }
    const ::std::vector<StructItem>& fields() const { return m_fields; }
    
    MetaItems&  attrs()  { return m_attrs; }
    GenericParams& params() { return m_params; }
    ::std::vector<StructItem>& fields() { return m_fields; }
    
    TypeRef get_field_type(const char *name, const ::std::vector<TypeRef>& args);
    
    SERIALISABLE_PROTOTYPES();
};

class ImplDef:
    public Serialisable
{
    MetaItems   m_attrs;
    GenericParams  m_params;
    Path    m_trait;
    TypeRef m_type;
public:
    ImplDef() {}
    ImplDef(ImplDef&&) noexcept = default;
    ImplDef(MetaItems attrs, GenericParams params, Path trait_type, TypeRef impl_type):
        m_attrs( move(attrs) ),
        m_params( move(params) ),
        m_trait( move(trait_type) ),
        m_type( move(impl_type) )
    {}
    
    // Accessors
    const MetaItems& attrs() const { return m_attrs; }
    const GenericParams& params() const { return m_params; }
    const Path& trait() const { return m_trait; }
    const TypeRef& type() const { return m_type; }

    GenericParams& params() { return m_params; }
    Path& trait() { return m_trait; }
    TypeRef& type() { return m_type; }
    
    /// Compare this impl against a trait,type pair
    bool matches(::std::vector<TypeRef>& types, const Path& trait, const TypeRef& type) const;
    
    friend ::std::ostream& operator<<(::std::ostream& os, const ImplDef& impl);
    SERIALISABLE_PROTOTYPES();
};

class Impl:
    public Serialisable
{
    ImplDef m_def;
    
    ItemList<TypeRef>   m_types;
    ItemList<Function>  m_functions;
    ItemList<Static>    m_statics;
    ::std::vector<MacroInvocation>    m_macro_invocations;
    
    ::std::vector< ::std::pair< ::std::vector<TypeRef>, Impl > > m_concrete_impls;
public:
    Impl() {}
    Impl(Impl&&) noexcept = default;
    Impl(MetaItems attrs, GenericParams params, TypeRef impl_type, Path trait_type):
        m_def( move(attrs), move(params), move(trait_type), move(impl_type) )
    {}

    void add_function(bool is_public, ::std::string name, Function fcn) {
        m_functions.push_back( Item<Function>( ::std::move(name), ::std::move(fcn), is_public ) );
    }
    void add_type(bool is_public, ::std::string name, TypeRef type) {
        m_types.push_back( Item<TypeRef>( ::std::move(name), ::std::move(type), is_public ) );
    }
    void add_static(bool is_public, ::std::string name, Static v) {
        m_statics.push_back( Item<Static>( mv$(name), mv$(v), is_public ) );
    }
    void add_macro_invocation( MacroInvocation inv ) {
        m_macro_invocations.push_back( mv$(inv) );
    }
    
    const ImplDef& def() const { return m_def; }
    const ItemList<Function>& functions() const { return m_functions; }
    const ItemList<TypeRef>& types() const { return m_types; }

    ImplDef& def() { return m_def; }
    ItemList<Function>& functions() { return m_functions; }
    ItemList<TypeRef>& types() { return m_types; }
    
    bool has_named_item(const ::std::string& name) const;

    /// Obtain a concrete implementation based on the provided types (caches)
    Impl& get_concrete(const ::std::vector<TypeRef>& param_types);
    
    friend ::std::ostream& operator<<(::std::ostream& os, const Impl& impl);
    SERIALISABLE_PROTOTYPES();
    
private:
    /// Actually create a concrete impl
    Impl make_concrete(const ::std::vector<TypeRef>& types) const;
};


class Crate;
class ExternCrate;
class Module;

typedef void fcn_visitor_t(const AST::Crate& crate, const AST::Module& mod, Function& fcn);

/// Representation of a parsed (and being converted) function
class Module:
    public Serialisable
{
    typedef ::std::vector< Item<Function> >   itemlist_fcn_t;
    typedef ::std::vector< ::std::pair<Module, bool> >   itemlist_mod_t;
    typedef ::std::vector< Item<Path> > itemlist_use_t;
    typedef ::std::vector< Item< ::std::string> >  itemlist_ext_t;
    typedef ::std::vector< Item<Static> >  itemlist_static_t;
    typedef ::std::vector< Item<Enum> >  itemlist_enum_t;
    typedef ::std::vector< Item<Struct> >  itemlist_struct_t;
    typedef ::std::vector< Item<MacroRules> >   itemlist_macros_t;
    typedef ::std::multimap< ::std::string, ::std::string > macro_imports_t;
    
    MetaItems   m_attrs;
    ::std::string   m_name;
    itemlist_fcn_t  m_functions;
    itemlist_mod_t  m_submods;
    itemlist_use_t  m_imports;
    ::std::vector<Item<TypeAlias> > m_type_aliases;
    itemlist_ext_t  m_extern_crates;
    ::std::vector<Module*>  m_anon_modules; // TODO: Should this be serialisable?
    
    itemlist_macros_t   m_macros;
    macro_imports_t m_macro_imports;    // module => macro
    ::std::vector< ItemNS<const MacroRules*> > m_macro_import_res; // Vec of imported macros (not serialised)
    ::std::vector<MacroInvocation>    m_macro_invocations;
    
    
    
    itemlist_static_t   m_statics;
    ItemList<Trait> m_traits;
    itemlist_enum_t m_enums;
    itemlist_struct_t m_structs;
    ::std::vector<Impl> m_impls;
    ::std::vector<ImplDef> m_neg_impls;
public:
    Module() {}
    Module(MetaItems attrs, ::std::string name):
        m_attrs( move(attrs) ),
        m_name(name)
    {
    }
    
    // Called when module is loaded from a serialised format
    void prescan();
    
    void add_ext_crate(::std::string ext_name, ::std::string imp_name) {
        m_extern_crates.push_back( Item< ::std::string>( move(imp_name), move(ext_name), false ) );
    }
    void add_alias(bool is_public, Path path, ::std::string name) {
        m_imports.push_back( Item<Path>( move(name), move(path), is_public) );
    }
    void add_typealias(bool is_public, ::std::string name, TypeAlias alias) {
        m_type_aliases.push_back( Item<TypeAlias>( move(name), move(alias), is_public ) );
    }
    //void add_constant(bool is_public, ::std::string name, TypeRef type, Expr val) {
    //    m_statics.push_back( Item<Static>( move(name), Static(Static::CONST, move(type), move(val)), is_public) );
    //}
    //void add_global(bool is_public, bool is_mut, ::std::string name, TypeRef type, Expr val) {
    //    m_statics.push_back( Item<Static>( move(name), Static(is_mut ? Static::MUT : Static::STATIC, move(type), move(val)), is_public) );
    //}
    void add_static(bool is_public, ::std::string name, Static item) {
        m_statics.push_back( Item<Static>( move(name), ::std::move(item), is_public) );
    }
    void add_trait(bool is_public, ::std::string name, Trait trait) {
        m_traits.push_back( Item<Trait>( move(name), move(trait), is_public) );
    }
    void add_struct(bool is_public, ::std::string name, Struct item) {
        m_structs.push_back( Item<Struct>( move(name), move(item), is_public) );
    }
    void add_enum(bool is_public, ::std::string name, Enum inst) {
        m_enums.push_back( Item<Enum>( move(name), move(inst), is_public ) );
    }
    void add_function(bool is_public, ::std::string name, Function func) {
        m_functions.push_back( Item<Function>( move(name), move(func), is_public ) );
    }
    void add_submod(bool is_public, Module mod) {
        m_submods.push_back( ::std::make_pair( move(mod), is_public ) );
    }
    void add_impl(Impl impl) {
        m_impls.emplace_back( ::std::move(impl) );
    }
    void add_neg_impl(ImplDef impl) {
        m_neg_impls.emplace_back( ::std::move(impl) );
    }
    void add_macro(bool is_exported, ::std::string name, MacroRules macro) {
        m_macros.push_back( Item<MacroRules>( move(name), move(macro), is_exported ) );
    }
    void add_macro_import(const Crate& crate, ::std::string mod, ::std::string name);
    void add_macro_invocation(MacroInvocation item) {
        m_macro_invocations.push_back( mv$(item) );
    }

    void add_attr(MetaItem item) {
        m_attrs.push_back(item);
    }
    unsigned int add_anon_module(Module* mod_ptr) {
        auto it = ::std::find(m_anon_modules.begin(), m_anon_modules.end(), mod_ptr);
        if( it != m_anon_modules.end() )
            return it - m_anon_modules.begin();
        m_anon_modules.push_back(mod_ptr);
        return m_anon_modules.size()-1;
    }

    void iterate_functions(fcn_visitor_t* visitor, const Crate& crate);

    const ::std::string& name() const { return m_name; }
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
        _(AST::Item<Path>, Use)
        #undef _
    };
    ItemRef find_item(const ::std::string& needle, bool allow_leaves = true, bool ignore_private_wildcard = true) const;
    
    MetaItems& attrs() { return m_attrs; } 
    itemlist_fcn_t& functions() { return m_functions; }
    itemlist_mod_t& submods() { return m_submods; }
    itemlist_use_t& imports() { return m_imports; }
    ::std::vector<Item<TypeAlias> >& type_aliases() { return m_type_aliases; }
    itemlist_ext_t& extern_crates() { return m_extern_crates; }
    ::std::vector<Impl>&    impls() { return m_impls; }
    itemlist_static_t&    statics() { return m_statics; }
    ItemList<Trait>& traits() { return m_traits; }
    itemlist_enum_t&      enums  () { return m_enums; }
    itemlist_struct_t&    structs() { return m_structs; }
    ::std::vector<Module*>&   anon_mods() { return m_anon_modules; }

    const MetaItems& attrs() const { return m_attrs; } 
    const itemlist_fcn_t& functions() const { return m_functions; }
    const itemlist_mod_t& submods() const { return m_submods; }
    const itemlist_use_t& imports() const { return m_imports; }
    const ::std::vector<Item<TypeAlias> >& type_aliases() const { return m_type_aliases; }
    const itemlist_ext_t& extern_crates() const { return m_extern_crates; }
    const ::std::vector<Impl>&  impls() const { return m_impls; }
    const itemlist_static_t&    statics() const { return m_statics; }
    const ItemList<Trait>& traits() const { return m_traits; }
    const itemlist_enum_t&      enums  () const { return m_enums; }
    const itemlist_struct_t&    structs() const { return m_structs; }
    const ::std::vector<Module*>&   anon_mods() const { return m_anon_modules; }
    
    const ::std::vector<ImplDef>&   neg_impls() const { return m_neg_impls; }
    
    const itemlist_macros_t&    macros()  const { return m_macros; }
    const macro_imports_t&      macro_imports()  const { return m_macro_imports; }
    const ::std::vector<ItemNS<const MacroRules*> >  macro_imports_res() const { return m_macro_import_res; }


    SERIALISABLE_PROTOTYPES();
private:
    void resolve_macro_import(const Crate& crate, const ::std::string& modname, const ::std::string& macro_name);
};
}
extern void handle_lang_item(AST::Crate& , const AST::Path& h, const ::std::string& , AST::eItemType );
namespace AST {


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

class Crate:
    public Serialisable
{
public:
    ::std::map< TypeRef, ::std::vector<Impl*> >  m_impl_map;
    ::std::vector<Impl*>    m_impl_index;
    ::std::vector<const ImplDef*> m_neg_impl_index;
    
    // XXX: EVIL - Make the method that handles #[lang] a friend, so it can twiddle these paths
    friend void ::handle_lang_item(AST::Crate& , const AST::Path& h, const ::std::string& , AST::eItemType );
    AST::Path   m_lang_item_PhantomFn;
public:
    Module  m_root_module;
    ::std::map< ::std::string, ExternCrate> m_extern_crates;
    // Mapping filled by searching for (?visible) macros with is_pub=true
    ::std::map< ::std::string, const MacroRules*> m_exported_macros;
    
    bool    m_load_std;

    Crate();

    Module& root_module() { return m_root_module; }
    Module& get_root_module(const ::std::string& name);
    ::std::map< ::std::string, ExternCrate>& extern_crates() { return m_extern_crates; }   
    
    const Module& root_module() const { return m_root_module; }
    const Module& get_root_module(const ::std::string& name) const;
    const ::std::map< ::std::string, ExternCrate>& extern_crates() const { return m_extern_crates; }   
 
    void post_parse();
    
    bool is_trait_implicit(const Path& trait) const;
    

    //::std::vector<ImplRef> find_inherent_impls(const TypeRef& type) const;
    bool find_inherent_impls(const TypeRef& type, ::std::function<bool(const Impl& , ::std::vector<TypeRef> )>) const;
    ::rust::option<ImplRef> find_impl(const Path& trait, const TypeRef& type) const;
    bool find_impl(const Path& trait, const TypeRef& type, Impl** out_impl, ::std::vector<TypeRef>* out_prams=nullptr) const;
    const ::rust::option<Impl&> get_impl(const Path& trait, const TypeRef& type) {
        Impl*   impl_ptr;
        ::std::vector<TypeRef>  params;
        if( find_impl(trait, type, &impl_ptr, &params) ) {
            return ::rust::option<Impl&>( impl_ptr->get_concrete(params) );
        }
        else {
            return ::rust::option<Impl&>();
        }
    }
    Function& lookup_method(const TypeRef& type, const char *name);
    
    void load_extern_crate(::std::string name);
    
    void iterate_functions( fcn_visitor_t* visitor );

    SERIALISABLE_PROTOTYPES();
private:
    bool check_impls_wildcard(const Path& trait, const TypeRef& type) const;
};

/// Representation of an imported crate
/// - Functions are stored as resolved+typechecked ASTs
class ExternCrate:
    public Serialisable
{
    Crate   m_crate;
public:
    ExternCrate();
    ExternCrate(const char *path);
    Crate& crate() { return m_crate; }
    const Crate& crate() const { return m_crate; }
    Module& root_module() { return m_crate.root_module(); }
    const Module& root_module() const { return m_crate.root_module(); }
    
    void prescan();
    
    SERIALISABLE_PROTOTYPES();
};

class CStruct
{
//    ::std::vector<StructItem>   m_fields;
public:
    const char* name() const { return "TODO"; }
    const char* mangled_name() const { return "TODO"; }
//    const ::std::vector<StructItem>& fields() const { return m_fields; }
};

class Flat
{
    ::std::vector<CStruct>  m_structs;
//    ::std::vector< ::std::pair< ::std::string,Function> > m_functions;
public:
    
//    const ::std::vector< ::std::pair< ::std::string, Function> >& functions() const { return m_functions; }
    const ::std::vector<CStruct>& structs() const { return m_structs; }
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

extern AST::Module  g_compiler_module;
extern void AST_InitProvidedModule();  


#endif // AST_HPP_INCLUDED
