/*
 */
#include "ast.hpp"
#include "crate.hpp"
#include "types.hpp"
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

MetaItems::~MetaItems()
{
}
MetaItems MetaItems::clone() const
{
    return MetaItems( m_span, clone_mivec(m_items) );
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

MetaItem::~MetaItem()
{
}
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

::std::ostream& operator<<(::std::ostream& os, const ImplDef& impl)
{
    return os << "impl<" << impl.m_params << "> " << impl.m_trait.ent << " for " << impl.m_type << "";
}

void Impl::add_function(bool is_public, bool is_specialisable, ::std::string name, Function fcn)
{
    DEBUG("impl fn " << name);
    m_items.push_back( ImplItem { is_public, is_specialisable, mv$(name), box$( Item::make_Function(mv$(fcn)) ) } );
}
void Impl::add_type(bool is_public, bool is_specialisable, ::std::string name, TypeRef type)
{
    m_items.push_back( ImplItem { is_public, is_specialisable, mv$(name), box$( Item::make_Type(TypeAlias(GenericParams(), mv$(type))) ) } );
}
void Impl::add_static(bool is_public, bool is_specialisable, ::std::string name, Static v)
{
    m_items.push_back( ImplItem { is_public, is_specialisable, mv$(name), box$( Item::make_Static(mv$(v)) ) } );
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

::std::ostream& operator<<(::std::ostream& os, const Impl& impl)
{
    return os << impl.m_def;
}

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



MacroInvocation MacroInvocation::clone() const
{
    return MacroInvocation(m_span, m_attrs.clone(), m_macro_name, m_ident, m_input.clone());
}


::std::unique_ptr<AST::Module> Module::add_anon() {
    auto rv = box$( Module(m_my_path + FMT("#" << m_anon_modules.size())) );
    
    m_anon_modules.push_back( rv.get() );
    
    return rv;
}

void Module::add_item( Named<Item> i ) {
    m_items.push_back( mv$(i) );
    DEBUG("Item " << m_items.back().data.tag_str() << " - attrs = " << m_items.back().data.attrs);
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
            (MacroInv,
                throw ::std::runtime_error("BUG: Hit a macro invocation");
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


Function::Function(Span sp, GenericParams params, TypeRef ret_type, Arglist args):
    m_span(sp),
    m_params( move(params) ),
    m_rettype( move(ret_type) ),
    m_args( move(args) )
{
}

void Trait::add_type(::std::string name, TypeRef type) {
    m_items.push_back( Named<Item>(mv$(name), Item::make_Type({TypeAlias(GenericParams(), mv$(type))}), true) );
}
void Trait::add_function(::std::string name, Function fcn) {
    DEBUG("trait fn " << name);
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


::std::ostream& operator<<(::std::ostream& os, const TypeParam& tp)
{
    //os << "TypeParam(";
    os << tp.m_name;
    os << " = ";
    os << tp.m_default;
    //os << ")";
    return os;
}

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

}    // namespace AST
