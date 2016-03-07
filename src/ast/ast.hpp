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
#include "item.hpp"

#include "generics.hpp"

namespace AST {

class Crate;

using ::std::unique_ptr;
using ::std::move;

enum eItemType
{
    ITEM_TRAIT,
    ITEM_STRUCT,
    ITEM_FN,
};

typedef Named<TypeRef>    StructItem;
class Crate;

class TypeAlias:
    public Serialisable
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
    
    SERIALISABLE_PROTOTYPES();
};

class Function:
    public Serialisable
{
public:
    typedef ::std::vector< ::std::pair<AST::Pattern,TypeRef> >   Arglist;

private:
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
    Function(GenericParams params, TypeRef ret_type, Arglist args):
        m_params( move(params) ),
        m_rettype( move(ret_type) ),
        m_args( move(args) )
    {
    }
    
    void set_code(Expr code) { m_code = ::std::move(code); }
    void set_self_lifetime(::std::string s) { m_lifetime = s; }
    
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
    GenericParams  m_params;
    ::std::vector<AST::Path>    m_supertraits;
    NamedList<TypeAlias> m_types;
    NamedList<Function>  m_functions;
    NamedList<Static>    m_statics;
public:
    Trait() {}
    Trait(GenericParams params, ::std::vector<Path> supertraits):
        m_params( mv$(params) ),
        m_supertraits( mv$(supertraits) )
    {
    }
    
    const GenericParams& params() const { return m_params; }
    const ::std::vector<Path>& supertraits() const { return m_supertraits; }
    const NamedList<Function>& functions() const { return m_functions; }
    const NamedList<TypeAlias>& types() const { return m_types; }
    const NamedList<Static>& statics() const { return m_statics; }

    GenericParams& params() { return m_params; }
    ::std::vector<Path>& supertraits() { return m_supertraits; }
    NamedList<Function>& functions() { return m_functions; }
    NamedList<TypeAlias>& types() { return m_types; }
    
    void add_type(::std::string name, TypeRef type) {
        m_types.push_back( Named<TypeAlias>(move(name), TypeAlias(GenericParams(), move(type)), true) );
    }
    void add_function(::std::string name, Function fcn) {
        m_functions.push_back( Named<Function>(::std::move(name), ::std::move(fcn), true) );
    }
    void add_static(::std::string name, Static v) {
        m_statics.push_back( Named<Static>(mv$(name), mv$(v), true) );
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
        m_attrs( mv$(attrs) ),
        m_name( ::std::move(name) ),
        m_sub_types( ::std::move(sub_types) )
    {
    }
    
    EnumVariant(MetaItems attrs, ::std::string name, ::std::vector<StructItem> fields):
        m_attrs( mv$(attrs) ),
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
    GenericParams    m_params;
    ::std::vector<EnumVariant>   m_variants;
public:
    Enum() {}
    Enum( GenericParams params, ::std::vector<EnumVariant> variants ):
        m_params( move(params) ),
        m_variants( move(variants) )
    {}
    
    const GenericParams& params() const { return m_params; }
    const ::std::vector<EnumVariant>& variants() const { return m_variants; }
    
    GenericParams& params() { return m_params; }
    ::std::vector<EnumVariant>& variants() { return m_variants; }
    
    SERIALISABLE_PROTOTYPES();
};

class Struct:
    public Serialisable
{
    GenericParams    m_params;
    ::std::vector<StructItem>   m_fields;
public:
    Struct() {}
    Struct( GenericParams params, ::std::vector<StructItem> fields ):
        m_params( move(params) ),
        m_fields( move(fields) )
    {}
    
    const GenericParams&   params() const { return m_params; }
    const ::std::vector<StructItem>& fields() const { return m_fields; }
    
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
    
    NamedList<TypeRef>   m_types;
    NamedList<Function>  m_functions;
    NamedList<Static>    m_statics;
    ::std::vector<MacroInvocation>    m_macro_invocations;
    
    ::std::vector< ::std::pair< ::std::vector<TypeRef>, Impl > > m_concrete_impls;
public:
    Impl() {}
    Impl(Impl&&) noexcept = default;
    Impl(MetaItems attrs, GenericParams params, TypeRef impl_type, Path trait_type):
        m_def( move(attrs), move(params), move(trait_type), move(impl_type) )
    {}

    void add_function(bool is_public, ::std::string name, Function fcn) {
        m_functions.push_back( Named<Function>( ::std::move(name), ::std::move(fcn), is_public ) );
    }
    void add_type(bool is_public, ::std::string name, TypeRef type) {
        m_types.push_back( Named<TypeRef>( ::std::move(name), ::std::move(type), is_public ) );
    }
    void add_static(bool is_public, ::std::string name, Static v) {
        m_statics.push_back( Named<Static>( mv$(name), mv$(v), is_public ) );
    }
    void add_macro_invocation( MacroInvocation inv ) {
        m_macro_invocations.push_back( mv$(inv) );
    }
    
    const ImplDef& def() const { return m_def; }
    const NamedList<Function>& functions() const { return m_functions; }
    const NamedList<TypeRef>& types() const { return m_types; }

    ImplDef& def() { return m_def; }
    NamedList<Function>& functions() { return m_functions; }
    NamedList<TypeRef>& types() { return m_types; }
    
    bool has_named_item(const ::std::string& name) const;

    /// Obtain a concrete implementation based on the provided types (caches)
    Impl& get_concrete(const ::std::vector<TypeRef>& param_types);
    
    friend ::std::ostream& operator<<(::std::ostream& os, const Impl& impl);
    SERIALISABLE_PROTOTYPES();
    
private:
    /// Actually create a concrete impl
    Impl make_concrete(const ::std::vector<TypeRef>& types) const;
};


class Module;
class Item;

typedef void fcn_visitor_t(const AST::Crate& crate, const AST::Module& mod, Function& fcn);

/// Representation of a parsed (and being converted) function
class Module:
    public Serialisable
{
    typedef ::std::vector< Named<Path> > itemlist_use_t;
    
    ::std::string   m_name;

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
    ::std::vector<Module*>  m_anon_modules; // TODO: Should this be serialisable?
    
    ::std::vector< NamedNS<const MacroRules*> > m_macro_import_res; // Vec of imported macros (not serialised)
    ::std::vector< Named<MacroRules> >  m_macros;
public:
    Module() {}
    Module(::std::string name):
        m_name(name)
    {
    }
    
    // Called when module is loaded from a serialised format
    void prescan();
    
    void add_item(bool is_pub, ::std::string name, Item it, MetaItems attrs);
    void add_ext_crate(::std::string ext_name, ::std::string imp_name, MetaItems attrs);
    void add_alias(bool is_public, Path path, ::std::string name, MetaItems attrs);
    void add_typealias(bool is_public, ::std::string name, TypeAlias alias, MetaItems attrs);
    void add_static(bool is_public, ::std::string name, Static item, MetaItems attrs);
    void add_trait(bool is_public, ::std::string name, Trait item, MetaItems attrs);
    void add_struct(bool is_public, ::std::string name, Struct item, MetaItems attrs);
    void add_enum(bool is_public, ::std::string name, Enum inst, MetaItems attrs);
    void add_function(bool is_public, ::std::string name, Function item, MetaItems attrs);
    void add_submod(bool is_public, Module mod, MetaItems attrs);
    
    void add_impl(Impl impl) {
        m_impls.emplace_back( ::std::move(impl) );
    }
    void add_neg_impl(ImplDef impl) {
        m_neg_impls.emplace_back( ::std::move(impl) );
    }
    void add_macro(bool is_exported, ::std::string name, MacroRules macro) {
        m_macros.push_back( Named<MacroRules>( move(name), move(macro), is_exported ) );
    }
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
        _(AST::Named<Path>, Use)
        #undef _
    };
    ItemRef find_item(const ::std::string& needle, bool allow_leaves = true, bool ignore_private_wildcard = true) const;

    ::std::vector<Named<Item>>& items() { return m_items; }
    const ::std::vector<Named<Item>>& items() const { return m_items; }
    
    itemlist_use_t& imports() { return m_imports; }
    const itemlist_use_t& imports() const { return m_imports; }
    
    ::std::vector<Impl>&    impls() { return m_impls; }
    const ::std::vector<Impl>&  impls() const { return m_impls; }
    
    // No need to mutate this list
    const ::std::vector<ImplDef>&   neg_impls() const { return m_neg_impls; }

    ::std::vector<Module*>&   anon_mods() { return m_anon_modules; }
    const ::std::vector<Module*>&   anon_mods() const { return m_anon_modules; }
    

    ::std::vector<MacroInvocation>& macro_invs() { return m_macro_invocations; }
    const NamedList<MacroRules>&    macros()  const { return m_macros; }
    const ::std::vector<NamedNS<const MacroRules*> >  macro_imports_res() const { return m_macro_import_res; }


    SERIALISABLE_PROTOTYPES();
private:
    void resolve_macro_import(const Crate& crate, const ::std::string& modname, const ::std::string& macro_name);
};


TAGGED_UNION_EX(Item, (: public Serialisable), None,
    (
    (None, (
        )),
    (Module, (
        Module  e;
        )),
    (Crate, (
        ::std::string   name;
        )),
    
    (Type, (
        TypeAlias e;
        )),
    (Struct, (
        Struct e;
        )),
    (Enum, (
        Enum e;
        )),
    (Trait, (
        Trait e;
        )),
    
    (Function, (
        Function e;
        )),
    (Static, (
        Static e;
        ))
    ),
    
    (, attrs(mv$(x.attrs))), (attrs = mv$(x.attrs);),
    (
    public:
        MetaItems   attrs;
        
        SERIALISABLE_PROTOTYPES();
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
