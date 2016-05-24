/*
 */
#include "ast.hpp"
#include "crate.hpp"
#include "../types.hpp"
#include "../common.hpp"
#include <iostream>
#include "../parse/parseerror.hpp"
#include <algorithm>
#include <serialiser_texttree.hpp>

namespace AST {


namespace {
    ::std::vector<MetaItem> clone_mivec(const ::std::vector<MetaItem>& v) {
        ::std::vector<MetaItem>    ri;
        ri.reserve(v.size());
        for(const auto& i : v)
            ri.push_back( i.clone() );
        return ri;
    }
}

MetaItems MetaItems::clone() const
{
    return MetaItems( clone_mivec(m_items) );
}

void MetaItems::push_back(MetaItem i)
{
    m_items.push_back( ::std::move(i) );
}
MetaItem* MetaItems::get(const char *name)
{
    for( auto& i : m_items ) {
        if(i.name() == name) {
            i.mark_used();
            return &i;
        }
    }
    return 0;
}
SERIALISE_TYPE_A(MetaItems::, "AST_MetaItems", {
    s.item(m_items);
})


MetaItem MetaItem::clone() const
{
    TU_MATCH(MetaItemData, (m_data), (e),
    (None,
        return MetaItem(m_name);
        ),
    (String,
        return MetaItem(m_name, e.val);
        ),
    (List,
        return MetaItem(m_name, clone_mivec(e.sub_items));
        )
    )
    throw ::std::runtime_error("MetaItem::clone - Fell off end");
}


SERIALISE_TYPE(MetaItem::, "AST_MetaItem", {
    s << m_name;
    //s << m_str_val;
    //s << m_sub_items;
},{
    s.item(m_name);
    //s.item(m_str_val);
    //s.item(m_sub_items);
})

template<typename T>
void operator<<(Serialiser& s, const Spanned<T>& x) {
    //s << x.sp;
    s << x.ent;
}
template<typename T>
void operator>>(Deserialiser& s, Spanned<T>& x) {
    //s >> x.sp;
    s >> x.ent;
}

::std::ostream& operator<<(::std::ostream& os, const ImplDef& impl)
{
    return os << "impl<" << impl.m_params << "> " << impl.m_trait.ent << " for " << impl.m_type << "";
}
SERIALISE_TYPE(ImplDef::, "AST_ImplDef", {
    s << m_params;
    s << m_trait;
    s << m_type;
},{
    s.item(m_params);
    s >> m_trait;
    s.item(m_type);
})

void Impl::add_function(bool is_public, ::std::string name, Function fcn)
{
    m_items.push_back( Named<Item>( mv$(name), Item::make_Function({::std::move(fcn)}), is_public ) );
}
void Impl::add_type(bool is_public, ::std::string name, TypeRef type)
{
    m_items.push_back( Named<Item>( mv$(name), Item::make_Type({TypeAlias(GenericParams(), mv$(type))}), is_public ) );
}
void Impl::add_static(bool is_public, ::std::string name, Static v)
{
    m_items.push_back( Named<Item>( mv$(name), Item::make_Static({mv$(v)}), is_public ) );
}

bool Impl::has_named_item(const ::std::string& name) const
{
    for( const auto& it : this->items() )
    {
        if( it.name == name ) {
            return true;
        }
    }
    return false;
}

Impl& Impl::get_concrete(const ::std::vector<TypeRef>& param_types)
{
    if( param_types.size() > 0 )
    { 
        for( auto& i : m_concrete_impls )
        {
            if( i.first == param_types )
            {
                return i.second;
            }
        }
        
        m_concrete_impls.push_back( make_pair(param_types, this->make_concrete(param_types)) );
        return m_concrete_impls.back().second;
    }
    else
    {
        return *this;
    }
}

Impl Impl::make_concrete(const ::std::vector<TypeRef>& types) const
{
    throw ParseError::Todo("Impl::make_concrete");
}

::std::ostream& operator<<(::std::ostream& os, const Impl& impl)
{
    return os << impl.m_def;
}
SERIALISE_TYPE(Impl::, "AST_Impl", {
    s << m_def;
    s << m_items;
},{
    s.item(m_def);
    s.item(m_items);
})

::rust::option<char> ImplRef::find_named_item(const ::std::string& name) const
{
    if( this->impl.has_named_item(name) ) {
        return ::rust::Some(' ');
    }
    else {
        return ::rust::None<char>();
    }
}

::std::ostream& operator<<(::std::ostream& os, const UseStmt& x)
{
    os << "Use(" << x.path << ")";
    return os;
}

SERIALISE_TYPE_A(UseStmt::, "AST_UseStmt", {
    //s.item(sp);
    s.item(attrs);
    s.item(path);
})


SERIALISE_TYPE_A(MacroInvocation::, "AST_MacroInvocation", {
    s.item(m_attrs);
    s.item(m_macro_name);
    s.item(m_ident);
    s.item(m_input);
})

SERIALISE_TYPE_A(Module::, "AST_Module", {
    s.item(m_my_path);
    
    s.item(m_items);
    
    s.item(m_macros);
    
    s.item(m_imports);
    
    s.item(m_impls);
})

::std::unique_ptr<AST::Module> Module::add_anon() {
    auto rv = box$( Module(m_my_path + FMT("#" << m_anon_modules.size())) );
    
    m_anon_modules.push_back( rv.get() );
    
    return rv;
}

void Module::add_item(bool is_pub, ::std::string name, Item it, MetaItems attrs) {
    m_items.push_back( Named<Item>( mv$(name), mv$(it), is_pub ) );
    m_items.back().data.attrs = mv$(attrs);
    DEBUG("Item " << ::AST::Item::tag_to_str( m_items.back().data.tag() ) << " - attrs = " << m_items.back().data.attrs);
}
void Module::add_ext_crate(bool is_public, ::std::string ext_name, ::std::string imp_name, MetaItems attrs) {
    this->add_item( is_public, imp_name, Item::make_Crate({mv$(ext_name)}), mv$(attrs) );
}
void Module::add_alias(bool is_public, UseStmt us, ::std::string name, MetaItems attrs) {
    us.attrs = mv$(attrs);
    m_imports.push_back( Named<UseStmt>( mv$(name), mv$(us), is_public) );
}
void Module::add_typealias(bool is_public, ::std::string name, TypeAlias alias, MetaItems attrs) {
    this->add_item( is_public, name, Item::make_Type({mv$(alias)}), mv$(attrs) );
}
void Module::add_static(bool is_public, ::std::string name, Static item, MetaItems attrs) {
    this->add_item( is_public, name, Item::make_Static({mv$(item)}), mv$(attrs) );
}
void Module::add_trait(bool is_public, ::std::string name, Trait item, MetaItems attrs) {
    this->add_item( is_public, name, Item::make_Trait({mv$(item)}), mv$(attrs) );
}
void Module::add_struct(bool is_public, ::std::string name, Struct item, MetaItems attrs) {
    this->add_item( is_public, name, Item::make_Struct({mv$(item)}), mv$(attrs) );
}
void Module::add_enum(bool is_public, ::std::string name, Enum item, MetaItems attrs) {
    this->add_item( is_public, name, Item::make_Enum({mv$(item)}), mv$(attrs) );
}
void Module::add_function(bool is_public, ::std::string name, Function item, MetaItems attrs) {
    this->add_item( is_public, name, Item::make_Function({mv$(item)}), mv$(attrs) );
}
void Module::add_submod(bool is_public, ::std::string name, Module mod, MetaItems attrs) {
    DEBUG("mod.m_name = " << name << ", attrs = " << attrs);
    this->add_item( is_public, mv$(name), Item::make_Module({mv$(mod)}), mv$(attrs) );
}
void Module::add_macro(bool is_exported, ::std::string name, MacroRulesPtr macro) {
    m_macros.push_back( Named<MacroRulesPtr>( mv$(name), mv$(macro), is_exported ) );
}

void Module::prescan()
{
    //TRACE_FUNCTION;
    //DEBUG("- '"<<m_name<<"'"); 
    //
    //for( auto& sm_p : m_submods )
    //{
    //    sm_p.first.prescan();
    //}
}

template<typename T>
typename ::std::vector<Named<T> >::const_iterator find_named(const ::std::vector<Named<T> >& vec, const ::std::string& name)
{
    return ::std::find_if(vec.begin(), vec.end(), [&name](const Named<T>& x) {
        //DEBUG("find_named - x.name = " << x.name);
        return x.name == name && !x.data.is_None();
    });
}

Module::ItemRef Module::find_item(const ::std::string& needle, bool allow_leaves, bool ignore_private_wildcard) const
{
    TRACE_FUNCTION_F("path = " << m_my_path << ", needle = " << needle);
    
    {
        auto it = find_named(this->items(), needle);
        if( it != this->items().end() ) {
            TU_MATCH(::AST::Item, (it->data), (e),
            (None,
                throw ::std::runtime_error("BUG: Hit a None item");
                ),
            (Module, return ItemRef(e); ),
            (Crate, return ItemRef(e.name); ),
            (Type,  return ItemRef(e); ),
            (Struct, return ItemRef(e); ),
            (Enum,  return ItemRef(e); ),
            (Trait, return ItemRef(e); ),
            (Function,
                if( allow_leaves )
                    return ItemRef(e);
                else
                    DEBUG("Skipping function, leaves not allowed");
                ),
            (Static,
                if( allow_leaves )
                    return ItemRef(e);
                else
                    DEBUG("Skipping function, leaves not allowed");
                )
            )
            DEBUG("Item not checked at this level, try re-export list");
        }
    }

    // - Re-exports
    //  > Comes last, as it's a potentially expensive operation
    {
        for( const auto& imp : this->imports() )
        {
            //DEBUG("imp: '" << imp.name << "' = " << imp.data);
            if( !imp.is_pub && ignore_private_wildcard )
            {
                // not public, ignore
                //DEBUG("Private import, '" << imp.name << "' = " << imp.data);
            }
            else if( imp.name == needle )
            {
                DEBUG("Match " << needle << " = " << imp.data);
                return ItemRef(imp);
            }
            else if( imp.name == "" )
            {
                // Loop avoidance, don't check this
                //if( &imp.data == this )
                //    continue ;
                //
                const auto& binding = imp.data.path.binding();
                if( binding.is_Unbound() )
                {
                    // not yet bound, so run resolution (recursion)
                    DEBUG("Recursively resolving pub wildcard use " << imp.data);
                    //imp.data.resolve(root_crate);
                    throw ParseError::Todo("AST::Module::find_item() - Wildcard `use` not bound, call resolve here?");
                }
                
                TU_MATCH_DEF(AST::PathBinding, (binding), (info),
                // - any other type - error
                (
                    DEBUG("ERROR: Import of invalid class : " << imp.data.path);
                    throw ParseError::Generic("Wildcard import of non-module/enum");
                    ),
                (Unbound,
                    throw ParseError::BugCheck("Wildcard import path not bound");
                    ),
                // - If it's a module, recurse
                (Module,
                    auto rv = info.module_->find_item(needle);
                    if( !rv.is_None() ) {
                        // Don't return RV, return the import (so caller can rewrite path if need be)
                        return ItemRef(imp);
                        //return rv;
                    }
                    ),
                // - If it's an enum, search for this name and then pass to resolve
                (Enum,
                    auto& vars = info.enum_->variants();
                    // Damnit C++ "let it = vars.find(|a| a.name == needle);"
                    auto it = ::std::find_if(vars.begin(), vars.end(),
                        [&needle](const EnumVariant& ev) { return ev.m_name == needle; });
                    if( it != vars.end() ) {
                        DEBUG("Found enum variant " << it->m_name);
                        return ItemRef(imp);
                        //throw ParseError::Todo("Handle lookup_path_in_module for wildcard imports - enum");
                    }
                    )
                )
            }
            else
            {
                // Can't match, ignore
            }
        }
        
    }
    
    return Module::ItemRef();
}

SERIALISE_TYPE(Item::, "AST_Item", {
    s.item(attrs);
},{
    s.item(attrs);
})

SERIALISE_TYPE(TypeAlias::, "AST_TypeAlias", {
    s << m_params;
    s << m_type;
},{
    s.item(m_params);
    s.item(m_type);
})

::Serialiser& operator<<(::Serialiser& s, Static::Class fc)
{
    switch(fc)
    {
    case Static::CONST:  s << "CONST"; break;
    case Static::STATIC: s << "STATIC"; break;
    case Static::MUT:    s << "MUT"; break;
    }
    return s;
}
void operator>>(::Deserialiser& s, Static::Class& fc)
{
    ::std::string   n;
    s.item(n);
         if(n == "CONST")   fc = Static::CONST;
    else if(n == "STATIC")  fc = Static::STATIC;
    else if(n == "MUT")     fc = Static::MUT;
    else
        throw ::std::runtime_error("Deserialise Static::Class");
}
SERIALISE_TYPE(Static::, "AST_Static", {
    s << m_class;
    s << m_type;
    s << m_value;
},{
    s >> m_class;
    s.item(m_type);
    s.item(m_value);
})

SERIALISE_TYPE(Function::, "AST_Function", {
    s << m_params;
    s << m_rettype;
    s << m_args;
    s << m_code;
},{
    s.item(m_params);
    s.item(m_rettype);
    s.item(m_args);
    s.item(m_code);
})

SERIALISE_TYPE(Trait::, "AST_Trait", {
    s << m_params;
    s << m_items;
},{
    s.item(m_params);
    s.item(m_items);
})
void Trait::add_type(::std::string name, TypeRef type) {
    m_items.push_back( Named<Item>(mv$(name), Item::make_Type({TypeAlias(GenericParams(), mv$(type))}), true) );
}
void Trait::add_function(::std::string name, Function fcn) {
    m_items.push_back( Named<Item>(mv$(name), Item::make_Function({mv$(fcn)}), true) );
}
void Trait::add_static(::std::string name, Static v) {
    m_items.push_back( Named<Item>(mv$(name), Item::make_Static({mv$(v)}), true) );
}
void Trait::set_is_marker() {
    m_is_marker = true;
}
bool Trait::is_marker() const {
    return m_is_marker;
}
bool Trait::has_named_item(const ::std::string& name, bool& out_is_fcn) const
{
    for( const auto& i : m_items )
    {
        if( i.name == name ) {
            out_is_fcn = i.data.is_Function();
            return true;
        }
    }
    return false;
}

SERIALISE_TYPE_A(EnumVariant::, "AST_EnumVariant", {
    s.item(m_attrs);
    s.item(m_name);
    s.item(m_data);
})
SERIALISE_TYPE(EnumVariantData::, "AST_EnumVariantData", {
    // TODO: Serialise AST::EnumVariantData
    (void)s;
},{
    (void)s;
})

SERIALISE_TYPE(Enum::, "AST_Enum", {
    s << m_params;
    s << m_variants;
},{
    s.item(m_params);
    s.item(m_variants);
})

TypeRef Struct::get_field_type(const char *name, const ::std::vector<TypeRef>& args)
{
    if( args.size() != m_params.ty_params().size() )
    {
        throw ::std::runtime_error("Incorrect parameter count for struct");
    }
    // TODO: Should the bounds be checked here? Or is the count sufficient?
    for(const auto& f : m_data.as_Struct().ents)
    {
        if( f.m_name == name )
        {
            // Found it!
            if( args.size() )
            {
                TypeRef res = f.m_type;
                res.resolve_args( GenericResolveClosure(m_params, args) );
                return res;
            }
            else
            {
                return f.m_type;
            }
        }
    }
    
    throw ::std::runtime_error(FMT("No such field " << name));
}

SERIALISE_TYPE(Struct::, "AST_Struct", {
    s << m_params;
    s << m_data;
},{
    s.item(m_params);
    s.item(m_data);
})
SERIALISE_TYPE(StructData::, "AST_StructData", {
    // TODO: AST::StructData serialise
    (void)s;
},{
    (void)s;
})
SERIALISE_TYPE(StructItem::, "AST_StructItem", {
    // TODO: AST::StructItem serialise
    (void)s;
},{
    (void)s;
})
SERIALISE_TYPE(TupleItem::, "AST_TupleItem", {
    // TODO: AST::TupleItem serialise
    (void)s;
},{
    (void)s;
})

::std::ostream& operator<<(::std::ostream& os, const TypeParam& tp)
{
    //os << "TypeParam(";
    os << tp.m_name;
    os << " = ";
    os << tp.m_default;
    //os << ")";
    return os;
}
SERIALISE_TYPE(TypeParam::, "AST_TypeParam", {
    s << m_name;
    s << m_default;
},{
    s.item(m_name);
    s.item(m_default);
})

::std::ostream& operator<<(::std::ostream& os, const GenericBound& x)
{
    TU_MATCH(GenericBound, (x), (ent),
    (Lifetime,
        os << "'" << ent.test << ": '" << ent.bound;
        ),
    (TypeLifetime,
        os << ent.type << ": '" << ent.bound;
        ),
    (IsTrait,
        if( ! ent.hrls.empty() )
        {
            os << "for<";
            for(const auto& l : ent.hrls)
                os << "'" << l;
            os << ">";
        }
        os << ent.type << ":  " << ent.trait;
        ),
    (MaybeTrait,
        os << ent.type << ": ?" << ent.trait;
        ),
    (NotTrait,
        os << ent.type << ": !" << ent.trait;
        ),
    (Equality,
        os << ent.type << " = " << ent.replacement;
        )
    )
    return os;
}


#define SERIALISE_TU_ARM(CLASS, NAME, TAG, ...)    case CLASS::TAG_##TAG: { *this = CLASS::make_##TAG({}); auto& NAME = this->as_##TAG(); (void)&NAME; __VA_ARGS__ } break;
#define SERIALISE_TU_ARMS(CLASS, NAME, ...)    TU_GMA(__VA_ARGS__)(SERIALISE_TU_ARM, (CLASS, NAME), __VA_ARGS__)
#define SERIALISE_TU(PATH, TAG, NAME, ...) \
    void operator%(::Serialiser& s, PATH::Tag c) { s << PATH::tag_to_str(c); } \
    void operator%(::Deserialiser& s, PATH::Tag& c) { ::std::string n; s.item(n); c = PATH::tag_from_str(n); }\
    SERIALISE_TYPE(PATH::, TAG, {\
        s % this->tag(); TU_MATCH(PATH, ((*this)), (NAME), __VA_ARGS__)\
    }, {\
        PATH::Tag tag; s % tag; switch(tag) { case PATH::TAGDEAD: throw ""; SERIALISE_TU_ARMS(PATH, NAME, __VA_ARGS__) } \
    })

SERIALISE_TU(GenericBound, "GenericBound", ent,
    (Lifetime,
        s.item(ent.test);
        s.item(ent.bound);
        ),
    (TypeLifetime,
        s.item(ent.type);
        s.item(ent.bound);
        ),
    (IsTrait,
        s.item(ent.type);
        s.item(ent.hrls);
        s.item(ent.trait);
        ),
    (MaybeTrait,
        s.item(ent.type);
        s.item(ent.trait);
        ),
    (NotTrait,
        s.item(ent.type);
        s.item(ent.trait);
        ),
    (Equality,
        s.item(ent.type);
        s.item(ent.replacement);
        )
)

int GenericParams::find_name(const char* name) const
{
    for( unsigned int i = 0; i < m_type_params.size(); i ++ )
    {
        if( m_type_params[i].name() == name )
            return i;
    }
    DEBUG("Type param '" << name << "' not in list");
    return -1;
}

::std::ostream& operator<<(::std::ostream& os, const GenericParams& tps)
{
    return os << "<" << tps.m_lifetime_params << "," << tps.m_type_params << "> where {" << tps.m_bounds << "}";
}
SERIALISE_TYPE_S(GenericParams, {
    s.item(m_type_params);
    s.item(m_lifetime_params);
    s.item(m_bounds);
})

}
