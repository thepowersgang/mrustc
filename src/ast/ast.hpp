#ifndef AST_HPP_INCLUDED
#define AST_HPP_INCLUDED

#include <string>
#include <vector>
#include <stdexcept>
#include "../coretypes.hpp"
#include <memory>
#include <map>

#include "../parse/tokentree.hpp"
#include "../types.hpp"
#include "../macros.hpp"
#include <serialise.hpp>

#include "pattern.hpp"

#include "expr.hpp"

namespace AST {

using ::std::unique_ptr;
using ::std::move;

class TypeParam:
    public Serialisable
{
    enum Class {
        LIFETIME,
        TYPE,
        //INTEGER,
    };
    Class   m_class;
    ::std::string   m_name;
    TypeRef m_default;
public:
    TypeParam():
        m_class(LIFETIME)
    {}
    TypeParam(bool is_lifetime, ::std::string name):
        m_class( is_lifetime ? LIFETIME : TYPE ),
        m_name( ::std::move(name) )
    {}
    void setDefault(TypeRef type);
    
    const ::std::string&    name() const { return m_name; }
    const TypeRef& get_default() const { return m_default; }
    
    bool is_type() const { return m_class == TYPE; }
    
    friend ::std::ostream& operator<<(::std::ostream& os, const TypeParam& tp);
    SERIALISABLE_PROTOTYPES();
};

class GenericBound:
    public Serialisable
{
    ::std::string   m_argname;
    ::std::string   m_lifetime;
    TypeRef m_trait;
public:
    GenericBound() {}
    GenericBound(::std::string argname, ::std::string lifetime):
        m_argname(argname),
        m_lifetime(lifetime)
    { }
    GenericBound(::std::string argname, TypeRef trait):
        m_argname(argname),
        m_trait( ::std::move(trait) )
    { }
    
    bool is_trait() const { return m_lifetime == ""; }
    const ::std::string& name() const { return m_argname; }
    const TypeRef& type() const { return m_trait; }
    
    TypeRef& type() { return m_trait; }
    
    friend ::std::ostream& operator<<(::std::ostream& os, const GenericBound& x);
    SERIALISABLE_PROTOTYPES();
};

class TypeParams:
    public Serialisable
{
    ::std::vector<TypeParam>    m_params;
    ::std::vector<GenericBound>    m_bounds;
public:
    TypeParams() {}
    
    size_t n_params() const { return m_params.size(); }
    const ::std::vector<TypeParam>& params() const { return m_params; }
    const ::std::vector<GenericBound>& bounds() const { return m_bounds; }
    ::std::vector<TypeParam>& params() { return m_params; }
    ::std::vector<GenericBound>& bounds() { return m_bounds; }
    
    void add_param(TypeParam param) {
        m_params.push_back( ::std::move(param) );
    }
    void add_bound(GenericBound bound) {
        m_bounds.push_back( ::std::move(bound) );
    }
    
    int find_name(const char* name) const;
    bool check_params(Crate& crate, const ::std::vector<TypeRef>& types) const;
    bool check_params(Crate& crate, ::std::vector<TypeRef>& types, bool allow_infer) const;
    
    friend ::std::ostream& operator<<(::std::ostream& os, const TypeParams& tp);
    SERIALISABLE_PROTOTYPES();
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
    ItemNS(::std::string&& name, T&& data, bool is_pub):
        name( move(name) ),
        data( move(data) ),
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
    Item(::std::string&& name, T&& data, bool is_pub):
        ItemNS<T>( move(name), move(data), is_pub )
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

class MetaItem;

class MetaItems:
    public Serialisable
{
    ::std::vector<MetaItem> m_items;
public:
    MetaItems() {}
    MetaItems(::std::vector<MetaItem> items):
        m_items(items)
    {
    }
    
    void push_back(MetaItem i);
    
    MetaItem* get(const char *name);
    bool has(const char *name) {
        return get(name) != 0;
    }
    
    SERIALISABLE_PROTOTYPES();
};

class MetaItem:
    public Serialisable
{
    ::std::string   m_name;
    MetaItems   m_sub_items;
    ::std::string   m_str_val;
public:
    MetaItem() {}
    MetaItem(::std::string name):
        m_name(name)
    {
    }
    MetaItem(::std::string name, ::std::vector<MetaItem> items):
        m_name(name),
        m_sub_items(items)
    {
    }
    
    void mark_used() {}
    const ::std::string& name() const { return m_name; }
    
    SERIALISABLE_PROTOTYPES();
};

class TypeAlias:
    public Serialisable
{
    TypeParams  m_params;
    TypeRef m_type;
public:
    TypeAlias() {}
    TypeAlias(TypeParams params, TypeRef type):
        m_params( move(params) ),
        m_type( move(type) )
    {}
    
    const TypeParams& params() const { return m_params; }
    const TypeRef& type() const { return m_type; }
    
    TypeParams& params() { return m_params; }
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
    
    SERIALISABLE_PROTOTYPES();
};

class Function:
    public Serialisable
{
public:
    enum Class
    {
        CLASS_UNBOUND,
        CLASS_REFMETHOD,
        CLASS_MUTMETHOD,
        CLASS_VALMETHOD,
    };
    typedef ::std::vector< ::std::pair<AST::Pattern,TypeRef> >   Arglist;

private:
    Class   m_fcn_class;
    ::std::string   m_lifetime;
    TypeParams  m_params;
    Expr    m_code;
    TypeRef m_rettype;
    Arglist m_args;
public:
    Function():
        m_fcn_class(CLASS_UNBOUND)
    {}
    Function(TypeParams params, Class fcn_class, TypeRef ret_type, Arglist args, Expr code):
        m_fcn_class(fcn_class),
        m_params(params),
        m_code( ::std::move(code) ),
        m_rettype( move(ret_type) ),
        m_args( move(args) )
    {
    }
    
    void set_self_lifetime(::std::string s) { m_lifetime = s; }
    
    const Class fcn_class() const { return m_fcn_class; }
    
    TypeParams& params() { return m_params; }
    Expr& code() { return m_code; }
    TypeRef& rettype() { return m_rettype; }
    Arglist& args() { return m_args; }

    const TypeParams& params() const { return m_params; }
    const Expr& code() const { return m_code; }
    const TypeRef& rettype() const { return m_rettype; }
    const Arglist& args() const { return m_args; }
    
    SERIALISABLE_PROTOTYPES();
};

class Trait:
    public Serialisable
{
    TypeParams  m_params;
    ItemList<TypeRef>   m_types;
    ItemList<Function>  m_functions;
public:
    Trait() {}
    Trait(TypeParams params):
        m_params(params)
    {
    }
    
    const TypeParams& params() const { return m_params; }
    const ItemList<Function>& functions() const { return m_functions; }
    const ItemList<TypeRef>& types() const { return m_types; }

    TypeParams& params() { return m_params; }
    ItemList<Function>& functions() { return m_functions; }
    ItemList<TypeRef>& types() { return m_types; }
    
    void add_type(::std::string name, TypeRef type) {
        m_types.push_back( Item<TypeRef>(move(name), move(type), true) );
    }
    void add_function(::std::string name, Function fcn) {
        m_functions.push_back( Item<Function>(move(name), move(fcn), true) );
    }
    
    SERIALISABLE_PROTOTYPES();
};

class Enum:
    public Serialisable
{
    TypeParams    m_params;
    ::std::vector<StructItem>   m_variants;
public:
    Enum() {}
    Enum( TypeParams params, ::std::vector<StructItem> variants ):
        m_params( move(params) ),
        m_variants( move(variants) )
    {}
    
    const TypeParams& params() const { return m_params; }
    const ::std::vector<StructItem>& variants() const { return m_variants; }
    
    TypeParams& params() { return m_params; }
    ::std::vector<StructItem>& variants() { return m_variants; }
    
    SERIALISABLE_PROTOTYPES();
};

class Struct:
    public Serialisable
{
    TypeParams    m_params;
    ::std::vector<StructItem>   m_fields;
public:
    Struct() {}
    Struct( TypeParams params, ::std::vector<StructItem> fields ):
        m_params( move(params) ),
        m_fields( move(fields) )
    {}
    
    const TypeParams& params() const { return m_params; }
    const ::std::vector<StructItem>& fields() const { return m_fields; }
    
    TypeParams& params() { return m_params; }
    ::std::vector<StructItem>& fields() { return m_fields; }
    
    TypeRef get_field_type(const char *name, const ::std::vector<TypeRef>& args);
    
    SERIALISABLE_PROTOTYPES();
};

class Impl:
    public Serialisable
{
    TypeParams  m_params;
    TypeRef m_trait;
    TypeRef m_type;
    
    ItemList<TypeRef>   m_types;
    ItemList<Function>  m_functions;
    
    ::std::vector< ::std::pair< ::std::vector<TypeRef>, Impl > > m_concrete_impls;
public:
    Impl() {}
    Impl(TypeParams params, TypeRef impl_type, TypeRef trait_type):
        m_params( move(params) ),
        m_trait( move(trait_type) ),
        m_type( move(impl_type) )
    {}

    void add_function(bool is_public, ::std::string name, Function fcn) {
        m_functions.push_back( Item<Function>( ::std::move(name), ::std::move(fcn), is_public ) );
    }
    void add_type(bool is_public, ::std::string name, TypeRef type) {
        m_types.push_back( Item<TypeRef>( ::std::move(name), ::std::move(type), is_public ) );
    }
    
    const TypeParams& params() const { return m_params; }
    const TypeRef& trait() const { return m_trait; }
    const TypeRef& type() const { return m_type; }
    const ItemList<Function>& functions() const { return m_functions; }
    const ItemList<TypeRef>& types() const { return m_types; }

    TypeParams& params() { return m_params; }
    TypeRef& trait() { return m_trait; }
    TypeRef& type() { return m_type; }
    ItemList<Function>& functions() { return m_functions; }
    ItemList<TypeRef>& types() { return m_types; }

    Impl make_concrete(const ::std::vector<TypeRef>& types) const;

    ::rust::option<Impl&> matches(const TypeRef& trait, const TypeRef& type);
    
    friend ::std::ostream& operator<<(::std::ostream& os, const Impl& impl);
    SERIALISABLE_PROTOTYPES();
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
    
    ::std::string   m_name;
    ::std::vector<MetaItem> m_attrs;
    itemlist_fcn_t  m_functions;
    itemlist_mod_t  m_submods;
    itemlist_use_t  m_imports;
    ::std::vector<Item<TypeAlias> > m_type_aliases;
    itemlist_ext_t  m_extern_crates;
    
    itemlist_macros_t   m_macros;
    macro_imports_t m_macro_imports;    // module => macro
    ::std::vector< ItemNS<const MacroRules*> > m_macro_import_res; // Vec of imported macros (not serialised)
    
    
    
    itemlist_static_t   m_statics;
    ItemList<Trait> m_traits;
    itemlist_enum_t m_enums;
    itemlist_struct_t m_structs;
    ::std::vector<Impl> m_impls;
public:
    Module() {}
    Module(::std::string name):
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
    void add_constant(bool is_public, ::std::string name, TypeRef type, Expr val) {
        m_statics.push_back( Item<Static>( move(name), Static(Static::CONST, move(type), move(val)), is_public) );
    }
    void add_global(bool is_public, bool is_mut, ::std::string name, TypeRef type, Expr val) {
        m_statics.push_back( Item<Static>( move(name), Static(is_mut ? Static::MUT : Static::STATIC, move(type), move(val)), is_public) );
    }
    void add_trait(bool is_public, ::std::string name, Trait trait) {
        m_traits.push_back( Item<Trait>( move(name), move(trait), is_public) );
    }
    void add_struct(bool is_public, ::std::string name, TypeParams params, ::std::vector<StructItem> items) {
        m_structs.push_back( Item<Struct>( move(name), Struct(move(params), move(items)), is_public) );
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
        m_impls.push_back( ::std::move(impl) );
    }
    void add_macro(bool is_exported, ::std::string name, MacroRules macro) {
        m_macros.push_back( Item<MacroRules>( move(name), move(macro), is_exported ) );
    }
    void add_macro_import(const Crate& crate, ::std::string mod, ::std::string name);

    void add_attr(MetaItem item) {
        m_attrs.push_back(item);
    }

    void iterate_functions(fcn_visitor_t* visitor, const Crate& crate);

    const ::std::string& name() const { return m_name; }
    
    ::std::vector<MetaItem>& attrs() { return m_attrs; } 
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

    const ::std::vector<MetaItem>& attrs() const { return m_attrs; } 
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
    const itemlist_macros_t&    macros()  const { return m_macros; }
    const macro_imports_t&      macro_imports()  const { return m_macro_imports; }
    const ::std::vector<ItemNS<const MacroRules*> >  macro_imports_res() const { return m_macro_import_res; }

    SERIALISABLE_PROTOTYPES();
private:
    void resolve_macro_import(const Crate& crate, const ::std::string& modname, const ::std::string& macro_name);
};

class Crate:
    public Serialisable
{
    ::std::vector<Impl*>    m_impl_index;
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
    
    ::rust::option<Impl&> find_impl(const TypeRef& trait, const TypeRef& type);
    Function& lookup_method(const TypeRef& type, const char *name);
    
    void load_extern_crate(::std::string name);
    
    void iterate_functions( fcn_visitor_t* visitor );

    SERIALISABLE_PROTOTYPES();
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
    ::std::vector<StructItem>   m_fields;
public:
    const char* name() const { return "TODO"; }
    const char* mangled_name() const { return "TODO"; }
    const ::std::vector<StructItem>& fields() const { return m_fields; }
};

class Flat
{
    ::std::vector<CStruct>  m_structs;
    ::std::vector< ::std::pair< ::std::string,Function> > m_functions;
public:
    
    const ::std::vector< ::std::pair< ::std::string, Function> >& functions() const { return m_functions; }
    const ::std::vector<CStruct>& structs() const { return m_structs; }
};

}

#endif // AST_HPP_INCLUDED
