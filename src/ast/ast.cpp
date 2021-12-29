/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * ast/ast.cpp
 * - Implementation of the various AST classes
 */
#include "ast.hpp"
#include "crate.hpp"
#include "types.hpp"
#include "expr.hpp"
#include "../common.hpp"
#include <iostream>
#include "../parse/parseerror.hpp"
#include <algorithm>

#include <parse/ttstream.hpp>
#include <parse/common.hpp>
#include <parse/interpolated_fragment.hpp>
#include <synext.hpp>   // Expand_ParseAndExpand_ExprVal

namespace AST {


namespace {
    ::std::vector<Attribute> clone_mivec(const ::std::vector<Attribute>& v) {
        ::std::vector<Attribute>    ri;
        ri.reserve(v.size());
        for(const auto& i : v)
            ri.push_back( i.clone() );
        return ri;
    }
}

AttributeList AttributeList::clone() const
{
    return AttributeList( clone_mivec(m_items) );
}

void AttributeList::push_back(Attribute i)
{
    m_items.push_back( ::std::move(i) );
}
const Attribute* AttributeList::get(const char *name) const
{
    for( auto& i : m_items ) {
        if(i.name() == name) {
            //i.mark_used();
            return &i;
        }
    }
    return 0;
}

::std::ostream& operator<<(::std::ostream& os, const AttributeList& x) {
    for(const auto& i : x.m_items) {
        os << "#[" << i << "]";
    }
    return os;
}
::std::ostream& operator<<(::std::ostream& os, const AttributeName& x) {
    if(x.elems.empty())
    {
        os << "<empty>";
    }
    else
    {
        for(const auto& i : x.elems)
        {
            if(&i != &x.elems.front())
                os << "::";
            os << i;
        }
    }
    return os;
}

Attribute::Attribute(const Attribute& x):
    m_span(x.m_span),
    m_name(x.m_name)
    , m_data(x.m_data.clone())
    , m_is_used(x.m_is_used)
{
}
Attribute Attribute::clone() const
{
    return Attribute(*this);
}
void Attribute::fmt(std::ostream& os) const
{
    os << m_name;
    os << m_data;
}


std::string Attribute::parse_equals_string(const AST::Crate& crate, const AST::Module& mod) const
{
    TTStream    lex(this->m_span, ParseState(), this->data());
    lex.getTokenCheck(TOK_EQUAL);
    auto n = Expand_ParseAndExpand_ExprVal(crate, mod, lex);

    std::string rv;
    if( auto* v = dynamic_cast<::AST::ExprNode_String*>(&*n) ) {
        rv = v->m_value;
    }
    else
    {
        throw ParseError::Unexpected(lex, Token(InterpolatedFragment(InterpolatedFragment::EXPR, n.release())), TOK_STRING);
    }
    lex.getTokenCheck(TOK_EOF);
    return rv;
}

std::string Attribute::parse_paren_string() const
{
    TTStream    lex(this->m_span, ParseState(), this->data());
    lex.getTokenCheck(TOK_PAREN_OPEN);
    auto rv = lex.getTokenCheck(TOK_STRING).str();
    lex.getTokenCheck(TOK_PAREN_CLOSE);
    return rv;
}
void Attribute::parse_paren_ident_list(std::function<void(const Span& sp, RcString ident)> item_cb) const
{
    TTStream    lex(this->m_span, ParseState(), this->data());
    lex.getTokenCheck(TOK_PAREN_OPEN);
    while(lex.lookahead(0) != TOK_PAREN_CLOSE) {
        item_cb(lex.point_span(), lex.getTokenCheck(TOK_IDENT).ident().name);
        if(lex.lookahead(0) != TOK_COMMA)
            break;
        lex.getTokenCheck(TOK_COMMA);
    }
    lex.getTokenCheck(TOK_PAREN_CLOSE);
}

StructItem StructItem::clone() const
{
    return StructItem(m_attrs.clone(), m_is_public, m_name, m_type.clone());
}
TupleItem TupleItem::clone() const
{
    return TupleItem(m_attrs.clone(), m_is_public, m_type.clone());
}


TypeAlias TypeAlias::clone() const
{
    return TypeAlias( m_params.clone(), m_type.clone() );
}
Static Static::clone() const
{
    return Static( m_class, m_type.clone(), m_value.is_valid() ? AST::Expr( m_value.node().clone() ) : AST::Expr() );
}

Function::Function(Span sp, GenericParams params, ::std::string abi, bool is_unsafe, bool is_const, bool is_variadic, TypeRef ret_type, Arglist args):
    m_span(sp),
    m_params( move(params) ),
    m_rettype( move(ret_type) ),
    m_args( move(args) ),
    m_abi( mv$(abi) ),
    m_is_const(is_const),
    m_is_unsafe(is_unsafe),
    m_is_variadic(is_variadic)
{
}
Function Function::clone() const
{
    decltype(m_args)    new_args;
    for(const auto& arg : m_args)
        new_args.push_back( AST::Function::Arg( arg.pat.clone(), arg.ty.clone(), arg.attrs.clone() ) );

    auto rv = Function( m_span, m_params.clone(), m_abi, m_is_unsafe, m_is_const, m_is_variadic, m_rettype.clone(), mv$(new_args) );
    if( m_code.is_valid() )
    {
        rv.m_code = AST::Expr( m_code.node().clone() );
    }
    return rv;
}

void Trait::add_type(Span sp, RcString name, AttributeList attrs, TypeRef type) {
    m_items.push_back( Named<Item>(sp, mv$(attrs), true, mv$(name), Item::make_Type({TypeAlias(GenericParams(), mv$(type))})) );
}
void Trait::add_function(Span sp, RcString name, AttributeList attrs, Function fcn) {
    DEBUG("trait fn " << name);
    m_items.push_back( Named<Item>(sp, mv$(attrs), true, mv$(name), Item::make_Function({mv$(fcn)})) );
}
void Trait::add_static(Span sp, RcString name, AttributeList attrs, Static v) {
    m_items.push_back( Named<Item>(sp, mv$(attrs), true, mv$(name), Item::make_Static({mv$(v)})) );
}
void Trait::set_is_marker() {
    m_is_marker = true;
}
bool Trait::is_marker() const {
    return m_is_marker;
}
bool Trait::has_named_item(const RcString& name, bool& out_is_fcn) const
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

Trait Trait::clone() const
{
    auto rv = Trait(m_params.clone(), m_supertraits);
    for(const auto& item : m_items)
    {
        rv.m_items.push_back( Named<Item> { item.span, item.attrs.clone(), item.is_pub, item.name, item.data.clone() } );
    }
    return rv;
}

Enum Enum::clone() const
{
    decltype(m_variants)    new_variants;
    for(const auto& var : m_variants)
    {
        TU_MATCHA( (var.m_data), (e),
        (Value,
            new_variants.push_back( EnumVariant(var.m_attrs.clone(), var.m_name, e.m_value.clone()) );
            ),
        (Tuple,
            decltype(e.m_sub_types) new_st;
            for(const auto& f : e.m_sub_types)
                new_st.push_back( f.clone() );
            new_variants.push_back( EnumVariant(var.m_attrs.clone(), var.m_name, mv$(new_st)) );
            ),
        (Struct,
            decltype(e.m_fields)    new_fields;
            for(const auto& f : e.m_fields)
                new_fields.push_back( f.clone() );
            new_variants.push_back( EnumVariant(var.m_attrs.clone(), var.m_name, mv$(new_fields)) );
            )
        )
    }
    return Enum(m_params.clone(), mv$(new_variants));
}
Struct Struct::clone() const
{
    TU_MATCHA( (m_data), (e),
    (Unit,
        return Struct(m_params.clone());
        ),
    (Tuple,
        decltype(e.ents)    new_fields;
        for(const auto& f : e.ents)
            new_fields.push_back( f.clone() );
        return Struct(m_params.clone(), mv$(new_fields));
        ),
    (Struct,
        decltype(e.ents)    new_fields;
        for(const auto& f : e.ents)
            new_fields.push_back( f.clone() );
        return Struct(m_params.clone(), mv$(new_fields));
        )
    )
    throw "";
}

Union Union::clone() const
{
    decltype(m_variants)    new_vars;
    for(const auto& f : m_variants)
        new_vars.push_back( f.clone() );
    return Union(m_params.clone(), mv$(new_vars));
}

::std::ostream& operator<<(::std::ostream& os, const ImplDef& impl)
{
    return os << "impl<" << impl.m_params << "> " << impl.m_trait.ent << " for " << impl.m_type << "";
}

void Impl::add_function(Span sp, AttributeList attrs, bool is_public, bool is_specialisable, RcString name, Function fcn)
{
    DEBUG("impl fn " << name);
    m_items.push_back( ImplItem { sp, mv$(attrs), is_public, is_specialisable, mv$(name), box$( Item::make_Function(mv$(fcn)) ) } );
}
void Impl::add_type(Span sp, AttributeList attrs, bool is_public, bool is_specialisable, RcString name, TypeRef type)
{
    m_items.push_back( ImplItem { sp, mv$(attrs), is_public, is_specialisable, mv$(name), box$( Item::make_Type(TypeAlias(GenericParams(), mv$(type))) ) } );
}
void Impl::add_static(Span sp, AttributeList attrs, bool is_public, bool is_specialisable, RcString name, Static v)
{
    m_items.push_back( ImplItem { sp, mv$(attrs), is_public, is_specialisable, mv$(name), box$( Item::make_Static(mv$(v)) ) } );
}
void Impl::add_macro_invocation(MacroInvocation item) {
    m_items.push_back( ImplItem { item.span(), {}, false, false, "", box$( Item::make_MacroInv(mv$(item)) ) } );
}

bool Impl::has_named_item(const RcString& name) const
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

::std::ostream& operator<<(::std::ostream& os, const UseItem::Ent& x)
{
    return os << x.name << "=" << x.path;
}

MacroInvocation MacroInvocation::clone() const
{
    return MacroInvocation(m_span, AST::Path(m_macro_path), m_ident, m_input.clone());
}

UseItem UseItem::clone() const
{
    decltype(this->entries) entries;
    for(const auto& e : this->entries)
    {
        entries.push_back({ e.sp, e.path, e.name });
    }
    return UseItem {
        this->sp,
        mv$(entries)
        };
}

void ExternBlock::add_item(Named<Item> named_item)
{
    ASSERT_BUG(named_item.span, named_item.data.is_Function() || named_item.data.is_Static() || named_item.data.is_Type(), "Incorrect item type for ExternBlock - " << named_item.data.tag_str());
    m_items.push_back( mv$(named_item) );
}
ExternBlock ExternBlock::clone() const
{
    TODO(Span(), "Clone an extern block");
}

::std::shared_ptr<AST::Module> Module::add_anon() {
    auto rv = ::std::shared_ptr<AST::Module>( new Module(m_my_path + RcString::new_interned(FMT("#" << m_anon_modules.size()))) );
    DEBUG("New anon " << rv->m_my_path);
    rv->m_file_info = m_file_info;

    m_anon_modules.push_back( rv );

    return rv;
}

void Module::add_item( Named<Item> named_item ) {
    m_items.push_back( box$(named_item) );
    const auto& i = m_items.back();
    if( i->name == "" ) {
    }
    else {
        DEBUG(m_my_path << "::" << i->name << " = " << i->data.tag_str() << ", attrs = " << i->attrs);
    }
}
void Module::add_item(Span sp, bool is_pub, RcString name, Item it, AttributeList attrs) {
    add_item( Named<Item>( mv$(sp), mv$(attrs), is_pub, mv$(name), mv$(it) ) );
}
void Module::add_ext_crate(Span sp, bool is_pub, RcString ext_name, RcString imp_name, AttributeList attrs) {
    this->add_item( mv$(sp), is_pub, imp_name, Item::make_Crate({mv$(ext_name)}), mv$(attrs) );
}
void Module::add_macro_invocation(MacroInvocation item) {
    this->add_item( item.span(), false, "", Item( mv$(item) ), ::AST::AttributeList {} );
}
void Module::add_macro(bool is_exported, RcString name, MacroRulesPtr macro) {
    assert(macro);
    assert(macro->m_rules.size() > 0);
    m_macros.push_back( Named<MacroRulesPtr>( Span(), {}, /*is_pub=*/is_exported, mv$(name), mv$(macro) ) );
}

Item Item::clone() const
{
    TU_MATCHA( (*this), (e),
    (None,
        return Item(e);
        ),
    (MacroInv,
        TODO(Span(), "Clone on Item::MacroInv");
        ),
    (Macro,
        TODO(Span(), "Clone on Item::Macro");
        ),
    (Use,
        return Item(e.clone());
        ),
    (ExternBlock,
        TODO(Span(), "Clone on Item::" << this->tag_str());
        ),
    (Impl,
        TODO(Span(), "Clone on Item::" << this->tag_str());
        ),
    (NegImpl,
        TODO(Span(), "Clone on Item::" << this->tag_str());
        ),
    (Module,
        TODO(Span(), "Clone on Item::" << this->tag_str());
        ),
    (Crate,
        return Item(e);
        ),
    (Type,
        return AST::Item(e.clone());
        ),
    (Struct,
        return AST::Item(e.clone());
        ),
    (Enum,
        return AST::Item(e.clone());
        ),
    (Union,
        return AST::Item(e.clone());
        ),
    (Trait,
        return AST::Item(e.clone());
        ),
    (TraitAlias,
        return AST::Item(e.clone());
        ),

    (Function,
        return AST::Item(e.clone());
        ),
    (Static,
        return AST::Item(e.clone());
        )
    )
    throw "";
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
::std::ostream& operator<<(::std::ostream& os, const LifetimeParam& p)
{
    os << "'" << p.m_name;
    return os;
}
::std::ostream& operator<<(::std::ostream& os, const ValueParam& p)
{
    os << "const " << p.m_name << ": " << p.m_type;
    return os;
}

::std::ostream& operator<<(::std::ostream& os, const HigherRankedBounds& x)
{
    if( !x.empty() )
    {
        os << "for<";
        for(const auto& l : x.m_lifetimes)
            os << "'" << l << ",";
        os << "> ";
    }
    return os;
}


GenericParam GenericParam::clone() const
{
    TU_MATCH_HDRA( (*this), {)
    TU_ARMA(None, e)
        return e;
    TU_ARMA(Lifetime, e)
        return LifetimeParam(e);
    TU_ARMA(Type, e)
        return TypeParam(e);
    TU_ARMA(Value, e)
        return ValueParam(e);
    }
    throw "";
}

std::ostream& operator<<(std::ostream& os, const GenericParam& x)
{
    TU_MATCH_HDRA( (x), {)
    TU_ARMA(None, e)
        os << "/*-*/";
    TU_ARMA(Lifetime, e)
        os << e;
    TU_ARMA(Type, e)
        os << e;
    TU_ARMA(Value, e)
        os << e;
    }
    return os;
}

::std::ostream& operator<<(::std::ostream& os, const GenericBound& x)
{
    TU_MATCH_HDRA( (x), {)
    TU_ARMA(None, ent) {
        os << "/*-*/";
        }
    TU_ARMA(Lifetime, ent) {
        os << "'" << ent.test << ": '" << ent.bound;
        }
    TU_ARMA(TypeLifetime, ent) {
        os << ent.type << ": '" << ent.bound;
        }
    TU_ARMA(IsTrait, ent) {
        os << ent.outer_hrbs << ent.type << ": " << ent.inner_hrbs << ent.trait;
        }
    TU_ARMA(MaybeTrait, ent) {
        os << ent.type << ": ?" << ent.trait;
        }
    TU_ARMA(NotTrait, ent) {
        os << ent.type << ": !" << ent.trait;
        }
    TU_ARMA(Equality, ent) {
        os << ent.type << " = " << ent.replacement;
        }
    }
    return os;
}


//int GenericParams::find_name(const char* name) const
//{
//    for( unsigned int i = 0; i < m_type_params.size(); i ++ )
//    {
//        if( m_type_params[i].name() == name )
//            return i;
//    }
//    DEBUG("Type param '" << name << "' not in list");
//    return -1;
//}

::std::ostream& operator<<(::std::ostream& os, const GenericParams& tps)
{
    return os << "<" << tps.m_params << "> where {" << tps.m_bounds << "}";
}

}    // namespace AST
