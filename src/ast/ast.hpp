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
#include <serialise.hpp>

#include "pattern.hpp"

#include "expr.hpp"

namespace AST {

using ::std::unique_ptr;
using ::std::move;

class TypeParam:
    public Serialisable
{
public:
    TypeParam(bool is_lifetime, ::std::string name);
    void addLifetimeBound(::std::string name);
    void addTypeBound(TypeRef type);
    
    SERIALISABLE_PROTOTYPES();
};

typedef ::std::vector<TypeParam>    TypeParams;
typedef ::std::pair< ::std::string, TypeRef>    StructItem;

class Crate;

class MetaItem:
    public Serialisable
{
    ::std::string   m_name;
    ::std::vector<MetaItem> m_items;
    ::std::string   m_str_val;
public:
    MetaItem(::std::string name):
        m_name(name)
    {
    }
    MetaItem(::std::string name, ::std::vector<MetaItem> items):
        m_name(name),
        m_items(items)
    {
    }
    
    const ::std::string& name() const { return m_name; }
    
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
    Static(Class s_class, TypeRef type, Expr value):
        m_class(s_class),
        m_type( move(type) ),
        m_value( move(value) )
    {}
    
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
    typedef ::std::vector<StructItem>   Arglist;

private:
    Class   m_fcn_class;
    TypeParams  m_generic_params;
    Expr    m_code;
    TypeRef m_rettype;
    Arglist m_args;
public:
    Function(TypeParams params, Class fcn_class, TypeRef ret_type, Arglist args, Expr code):
        m_fcn_class(fcn_class),
        m_generic_params(params),
        m_code(code),
        m_rettype(ret_type),
        m_args(args)
    {
    }
    
    TypeParams& generic_params() { return m_generic_params; }
    Expr& code() { return m_code; }
    TypeRef& rettype() { return m_rettype; }
    Arglist& args() { return m_args; }

    const TypeParams& generic_params() const { return m_generic_params; }
    const Expr& code() const { return m_code; }
    const TypeRef& rettype() const { return m_rettype; }
    const Arglist& args() const { return m_args; }
    
    SERIALISABLE_PROTOTYPES();
};

class Enum:
    public Serialisable
{
    ::std::vector<TypeParam>    m_params;
    ::std::vector<StructItem>   m_variants;
public:
    Enum( ::std::vector<TypeParam> params, ::std::vector<StructItem> variants ):
        m_params( move(params) ),
        m_variants( move(variants) )
    {}
    
    const ::std::vector<TypeParam> params() const { return m_params; }
    const ::std::vector<StructItem> variants() const { return m_variants; }
    
    SERIALISABLE_PROTOTYPES();
};

class Struct:
    public Serialisable
{
    ::std::vector<TypeParam>    m_params;
    ::std::vector<StructItem>   m_fields;
public:
    Struct( ::std::vector<TypeParam> params, ::std::vector<StructItem> fields ):
        m_params( move(params) ),
        m_fields( move(fields) )
    {}
    
    const ::std::vector<StructItem> fields() const { return m_fields; }
    
    SERIALISABLE_PROTOTYPES();
};

class Impl
{
public:
    Impl(TypeRef impl_type, TypeRef trait_type);

    void add_function(bool is_public, ::std::string name, Function fcn);
};


class Crate;
class ExternCrate;
class Module;

typedef void fcn_visitor_t(const AST::Crate& crate, const AST::Module& mod, Function& fcn);

template <typename T>
struct Item:
    public Serialisable
{
    ::std::string   name;
    T   data;
    bool    is_pub;
    
    Item(::std::string&& name, T&& data, bool is_pub):
        name( move(name) ),
        data( move(data) ),
        is_pub( is_pub )
    {
    }
    
    SERIALISE_TYPE(, "Item", {
        s << is_pub;
        s << name;
        s << data;
    })
};

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

    Crate& m_crate;
    ::std::string   m_name;
    ::std::vector<MetaItem> m_attrs;
    itemlist_fcn_t  m_functions;
    itemlist_mod_t  m_submods;
    itemlist_use_t  m_imports;
    itemlist_ext_t  m_extern_crates;
    
    itemlist_static_t   m_statics;
    itemlist_enum_t m_enums;
    itemlist_struct_t m_structs;
public:
    Module(Crate& crate, ::std::string name):
        m_crate(crate),
        m_name(name)
    {
    }
    void add_ext_crate(::std::string ext_name, ::std::string imp_name);
    void add_alias(bool is_public, Path path, ::std::string name) {
        m_imports.push_back( Item<Path>( move(name), move(path), is_public) );
    }
    void add_constant(bool is_public, ::std::string name, TypeRef type, Expr val) {
        m_statics.push_back( Item<Static>( move(name), Static(Static::CONST, move(type), move(val)), is_public) );
    }
    void add_global(bool is_public, bool is_mut, ::std::string name, TypeRef type, Expr val) {
        m_statics.push_back( Item<Static>( move(name), Static(is_mut ? Static::MUT : Static::STATIC, move(type), move(val)), is_public) );
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
    void add_impl(Impl impl);

    void add_attr(MetaItem item) {
        m_attrs.push_back(item);
    }

    void iterate_functions(fcn_visitor_t* visitor, const Crate& crate);

    Crate& crate() { return m_crate; }  
    const Crate& crate() const { return m_crate; }  
 
    const ::std::string& name() const { return m_name; }
    
    ::std::vector<MetaItem>& attrs() { return m_attrs; } 
    itemlist_fcn_t& functions() { return m_functions; }
    itemlist_mod_t& submods() { return m_submods; }
    itemlist_use_t& imports() { return m_imports; }
    itemlist_ext_t& extern_crates() { return m_extern_crates; }

    const ::std::vector<MetaItem>& attrs() const { return m_attrs; } 
    const itemlist_fcn_t& functions() const { return m_functions; }
    const itemlist_mod_t& submods() const { return m_submods; }
    const itemlist_use_t& imports() const { return m_imports; }
    const itemlist_ext_t& extern_crates() const { return m_extern_crates; }
    const itemlist_enum_t&  enums() const { return m_enums; }
    const itemlist_struct_t&    structs() const { return m_structs; }

    SERIALISABLE_PROTOTYPES();
};

class Crate:
    public Serialisable
{
public:
    Module  m_root_module;
    ::std::map< ::std::string, ExternCrate> m_extern_crates;
    bool    m_load_std;

    Crate();

    Module& root_module() { return m_root_module; }
    const Module& root_module() const { return m_root_module; }
    
    Module& get_root_module(const ::std::string& name);
    const Module& get_root_module(const ::std::string& name) const;
    
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
    
    const ::std::vector< ::std::pair<::std::string, Function> >& functions() const { return m_functions; }
    const ::std::vector<CStruct>& structs() const { return m_structs; }
};

}

#endif // AST_HPP_INCLUDED
