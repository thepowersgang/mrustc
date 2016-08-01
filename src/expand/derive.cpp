
#include <synext.hpp>
#include "../common.hpp"
#include "../ast/ast.hpp"
#include "../ast/expr.hpp"

template<typename T>
static inline ::std::vector<T> vec$(T v1) {
    ::std::vector<T> tmp;
    tmp.push_back( mv$(v1) );
    return mv$(tmp);
}
template<typename T>
static inline ::std::vector<T> vec$(T v1, T v2) {
    ::std::vector<T> tmp;
    tmp.push_back( mv$(v1) );
    tmp.push_back( mv$(v2) );
    return mv$(tmp);
}

static inline AST::ExprNodeP mk_exprnodep(AST::ExprNode* en){ return AST::ExprNodeP(en); }
#define NEWNODE(type, ...)  mk_exprnodep(new type(__VA_ARGS__))

/// Interface for derive handlers
struct Deriver
{
    virtual AST::Impl handle_item(Span sp, const AST::GenericParams& params, const TypeRef& type, const AST::Struct& str) const = 0;
    virtual AST::Impl handle_item(Span sp, const AST::GenericParams& p, const TypeRef& type, const AST::Enum& enm) const = 0;
};

/// 'Debug' derive handler
class Deriver_Debug:
    public Deriver
{
    //static AST::ExprNodeP _print_l(::std::string val)
    //{
    //    return NEWNODE(AST::ExprNode_CallMethod,
    //            NEWNODE(AST::ExprNode_NamedValue, AST::Path("f")),
    //            AST::PathNode("write_str",{}),
    //            { NEWNODE(AST::ExprNode_String, mv$(val)) }
    //            );
    //}
    //static AST::ExprNodeP _try(AST::ExprNodeP expr)
    //{
    //    throw CompileError::Todo("derive(Debug) - _try");
    //}
    
    AST::Impl make_ret(Span sp, const AST::GenericParams& p, const TypeRef& type, AST::ExprNodeP node) const
    {
        const AST::Path    debug_trait = AST::Path("", { AST::PathNode("fmt", {}), AST::PathNode("Debug", {}) });
        const TypeRef  ret_type(sp, AST::Path("", {AST::PathNode("fmt",{}), AST::PathNode("Result",{})}) );
        const TypeRef  f_type(TypeRef::TagReference(), sp, true,
            TypeRef(sp, AST::Path("", {AST::PathNode("fmt",{}), AST::PathNode("Formatter", {})}))
            );
        
        DEBUG("node = " << *node);
        
        AST::Function fcn(
            AST::GenericParams(),
            ret_type,
            vec$(
                ::std::make_pair( AST::Pattern(AST::Pattern::TagBind(), "self"), TypeRef(TypeRef::TagReference(), sp, false, TypeRef("Self", 0xFFFF)) ),
                ::std::make_pair( AST::Pattern(AST::Pattern::TagBind(), "f"), f_type )
                )
            );
        fcn.set_code( NEWNODE(AST::ExprNode_Block, vec$(mv$(node)), ::std::unique_ptr<AST::Module>()) );
        
        AST::Impl   rv( AST::ImplDef( sp, AST::MetaItems(), p, make_spanned(sp, debug_trait), type ) );
        rv.add_function(false, false, "fmt", mv$(fcn));
        return mv$(rv);
    }
    
public:
    AST::Impl handle_item(Span sp, const AST::GenericParams& p, const TypeRef& type, const AST::Struct& str) const override
    {
        // TODO: be correct herhe and use "core" as the crate name
        // - Requires handling the crate_name crate attribute correctly
        const ::std::string& name = type.path().nodes().back().name();
        
        // Generate code for Debug
        AST::ExprNodeP  node;
        TU_MATCH(AST::StructData, (str.m_data), (e),
        (Struct,
            node = NEWNODE(AST::ExprNode_NamedValue, AST::Path("f"));
            node = NEWNODE(AST::ExprNode_CallMethod,
                mv$(node), AST::PathNode("debug_struct",{}),
                vec$( NEWNODE(AST::ExprNode_String, name) )
                );
            for( const auto& fld : e.ents )
            {
                node = NEWNODE(AST::ExprNode_CallMethod,
                    mv$(node), AST::PathNode("field",{}),
                    vec$(
                        NEWNODE(AST::ExprNode_String, fld.m_name),
                        NEWNODE(AST::ExprNode_UniOp, AST::ExprNode_UniOp::REF,
                            NEWNODE(AST::ExprNode_Field,
                                NEWNODE(AST::ExprNode_NamedValue, AST::Path("self")),
                                fld.m_name
                                )
                            )
                    )
                    );
            }
            node = NEWNODE(AST::ExprNode_CallMethod, mv$(node), AST::PathNode("finish",{}), {});
            ),
        (Tuple,
            node = NEWNODE(AST::ExprNode_NamedValue, AST::Path("f"));
            node = NEWNODE(AST::ExprNode_CallMethod,
                mv$(node), AST::PathNode("debug_tuple",{}),
                vec$( NEWNODE(AST::ExprNode_String, name) )
                );
            for( unsigned int idx = 0; idx < e.ents.size(); idx ++ )
            {
                node = NEWNODE(AST::ExprNode_CallMethod,
                    mv$(node), AST::PathNode("field",{}),
                    vec$(
                        NEWNODE(AST::ExprNode_UniOp, AST::ExprNode_UniOp::REF,
                            NEWNODE(AST::ExprNode_Field,
                                NEWNODE(AST::ExprNode_NamedValue, AST::Path("self")),
                                FMT(idx)
                                )
                            )
                        )
                    );
            }
            node = NEWNODE(AST::ExprNode_CallMethod, mv$(node), AST::PathNode("finish",{}), {});
            )
        )
        
        return this->make_ret(sp, p, type, mv$(node));
    }
    AST::Impl handle_item(Span sp, const AST::GenericParams& p, const TypeRef& type, const AST::Enum& enm) const override
    {
        ::std::vector< AST::ExprNode_Match_Arm> arms;
        //for(const auto& v : enm.variants())
        //{
            // TODO: Debug for enums
        //}
        AST::ExprNodeP  node = NEWNODE(AST::ExprNode_Match,
            NEWNODE(AST::ExprNode_NamedValue, AST::Path("self")),
            mv$(arms)
            );
        
        return this->make_ret(sp, p, type, mv$(node));
    }
} g_derive_debug;

class Deriver_PartialEq:
    public Deriver
{
    AST::Impl make_ret(Span sp, const AST::GenericParams& p, const TypeRef& type, AST::ExprNodeP node) const
    {
        const AST::Path    trait_path("", { AST::PathNode("cmp", {}), AST::PathNode("PartialEq", {}) });
        
        AST::Function fcn(
            AST::GenericParams(),
            TypeRef(sp, CORETYPE_BOOL),
            vec$(
                ::std::make_pair( AST::Pattern(AST::Pattern::TagBind(), "self"), TypeRef(TypeRef::TagReference(), sp, false, TypeRef("Self", 0xFFFF)) ),
                ::std::make_pair( AST::Pattern(AST::Pattern::TagBind(), "v"), TypeRef(TypeRef::TagReference(), sp, false, TypeRef("Self", 0xFFFF)) )
                )
            );
        fcn.set_code( NEWNODE(AST::ExprNode_Block, vec$(mv$(node)), ::std::unique_ptr<AST::Module>()) );
        
        AST::GenericParams  params = p;
        for(const auto& typ : params.ty_params())
        {
            params.bounds().push_back( ::AST::GenericBound::make_IsTrait({ TypeRef(typ.name()), {}, trait_path }) );
        }
        
        AST::Impl   rv( AST::ImplDef( sp, AST::MetaItems(), mv$(params), make_spanned(sp, trait_path), type ) );
        rv.add_function(false, false, "eq", mv$(fcn));
        return mv$(rv);
    }
public:
    AST::Impl handle_item(Span sp, const AST::GenericParams& p, const TypeRef& type, const AST::Struct& str) const override
    {
        ::std::vector<AST::ExprNodeP>   nodes;
        
        TU_MATCH(AST::StructData, (str.m_data), (e),
        (Struct,
            for( const auto& fld : e.ents )
            {
                nodes.push_back(NEWNODE(AST::ExprNode_If,
                    NEWNODE(AST::ExprNode_BinOp, AST::ExprNode_BinOp::CMPNEQU,
                        NEWNODE(AST::ExprNode_Field, NEWNODE(AST::ExprNode_NamedValue, AST::Path("self")), fld.m_name),
                        NEWNODE(AST::ExprNode_Field, NEWNODE(AST::ExprNode_NamedValue, AST::Path("v")), fld.m_name)
                        ),
                    NEWNODE(AST::ExprNode_Flow, AST::ExprNode_Flow::RETURN, "", NEWNODE(AST::ExprNode_Bool, false)),
                    nullptr
                    ));
            }
            ),
        (Tuple,
            for( unsigned int idx = 0; idx < e.ents.size(); idx ++ )
            {
                auto fld_name = FMT(idx);
                nodes.push_back(NEWNODE(AST::ExprNode_If,
                    NEWNODE(AST::ExprNode_BinOp, AST::ExprNode_BinOp::CMPNEQU,
                        NEWNODE(AST::ExprNode_Field, NEWNODE(AST::ExprNode_NamedValue, AST::Path("self")), fld_name),
                        NEWNODE(AST::ExprNode_Field, NEWNODE(AST::ExprNode_NamedValue, AST::Path("v")), fld_name)
                        ),
                    NEWNODE(AST::ExprNode_Flow, AST::ExprNode_Flow::RETURN, "", NEWNODE(AST::ExprNode_Bool, false)),
                    nullptr
                    ));
            }
            )
        )
        nodes.push_back( NEWNODE(AST::ExprNode_Bool, true) );
        
        return this->make_ret(sp, p, type, NEWNODE(AST::ExprNode_Block, mv$(nodes), nullptr));
    }
    
    AST::Impl handle_item(Span sp, const AST::GenericParams& p, const TypeRef& type, const AST::Enum& enm) const override
    {
        AST::Path base_path = type.m_data.as_Path().path;
        base_path.nodes().back().args() = ::AST::PathParams();
        ::std::vector<AST::ExprNode_Match_Arm>   arms;
        
        // TODO: PartialEq for enums
        for(const auto& v : enm.variants())
        {
            AST::ExprNodeP  code;
            AST::Pattern    pat_a;
            AST::Pattern    pat_b;
            
            TU_MATCH(::AST::EnumVariantData, (v.m_data), (e),
            (Value,
                code = NEWNODE(AST::ExprNode_Bool, true);
                pat_a = AST::Pattern(AST::Pattern::TagValue(), AST::Pattern::Value::make_Named(base_path + v.m_name));
                pat_b = AST::Pattern(AST::Pattern::TagValue(), AST::Pattern::Value::make_Named(base_path + v.m_name));
                ),
            (Tuple,
                if( e.m_sub_types.size() == 0 )
                {
                    code = NEWNODE(AST::ExprNode_Bool, true);
                    pat_a = AST::Pattern(AST::Pattern::TagValue(), AST::Pattern::Value::make_Named(base_path + v.m_name));
                    pat_b = AST::Pattern(AST::Pattern::TagValue(), AST::Pattern::Value::make_Named(base_path + v.m_name));
                }
                else
                {
                    ::std::vector<AST::Pattern>    pats_a;
                    ::std::vector<AST::Pattern>    pats_b;
                    ::std::vector<AST::ExprNodeP>   nodes;
                    
                    for( unsigned int idx = 0; idx < e.m_sub_types.size(); idx ++ )
                    {
                        auto name_a = FMT("a" << idx);
                        auto name_b = FMT("b" << idx);
                        pats_a.push_back( ::AST::Pattern(::AST::Pattern::TagBind(), name_a) );
                        pats_b.push_back( ::AST::Pattern(::AST::Pattern::TagBind(), name_b) );
                        nodes.push_back(NEWNODE(AST::ExprNode_If,
                            NEWNODE(AST::ExprNode_BinOp, AST::ExprNode_BinOp::CMPNEQU,
                                NEWNODE(AST::ExprNode_NamedValue, AST::Path(name_a)),
                                NEWNODE(AST::ExprNode_NamedValue, AST::Path(name_b))
                                ),
                            NEWNODE(AST::ExprNode_Flow, AST::ExprNode_Flow::RETURN, "", NEWNODE(AST::ExprNode_Bool, false)),
                            nullptr
                            ));
                    }
                    
                    nodes.push_back( NEWNODE(AST::ExprNode_Bool, true) );
                    pat_a = AST::Pattern(AST::Pattern::TagEnumVariant(), base_path + v.m_name, mv$(pats_a));
                    pat_b = AST::Pattern(AST::Pattern::TagEnumVariant(), base_path + v.m_name, mv$(pats_b));
                    code = NEWNODE(AST::ExprNode_Block, mv$(nodes), nullptr);
                }
                ),
            (Struct,
                ::std::vector< ::std::pair<std::string, AST::Pattern> > pats_a;
                ::std::vector< ::std::pair<std::string, AST::Pattern> > pats_b;
                ::std::vector<AST::ExprNodeP>   nodes;
                
                for( const auto& fld : e.m_fields )
                {
                    auto name_a = FMT("a" << fld.m_name);
                    auto name_b = FMT("b" << fld.m_name);
                    pats_a.push_back( ::std::make_pair(fld.m_name, ::AST::Pattern(::AST::Pattern::TagBind(), name_a)) );
                    pats_b.push_back( ::std::make_pair(fld.m_name, ::AST::Pattern(::AST::Pattern::TagBind(), name_b)) );
                    nodes.push_back(NEWNODE(AST::ExprNode_If,
                        NEWNODE(AST::ExprNode_BinOp, AST::ExprNode_BinOp::CMPNEQU,
                            NEWNODE(AST::ExprNode_NamedValue, AST::Path(name_a)),
                            NEWNODE(AST::ExprNode_NamedValue, AST::Path(name_b))
                            ),
                        NEWNODE(AST::ExprNode_Flow, AST::ExprNode_Flow::RETURN, "", NEWNODE(AST::ExprNode_Bool, false)),
                        nullptr
                        ));
                }
                
                nodes.push_back( NEWNODE(AST::ExprNode_Bool, true) );
                pat_a = AST::Pattern(AST::Pattern::TagStruct(), base_path + v.m_name, mv$(pats_a), true);
                pat_b = AST::Pattern(AST::Pattern::TagStruct(), base_path + v.m_name, mv$(pats_b), true);
                code = NEWNODE(AST::ExprNode_Block, mv$(nodes), nullptr);
                )
            )
            
            ::std::vector< AST::Pattern>    pats;
            {
                ::std::vector< AST::Pattern>    tuple_pats;
                tuple_pats.push_back( AST::Pattern(AST::Pattern::TagReference(), mv$(pat_a)) );
                tuple_pats.push_back( AST::Pattern(AST::Pattern::TagReference(), mv$(pat_b)) );
                pats.push_back( AST::Pattern(AST::Pattern::TagTuple(), mv$(tuple_pats)) );
            }
            
            arms.push_back(AST::ExprNode_Match_Arm(
                mv$(pats),
                nullptr,
                mv$(code)
                ));
        }

        ::std::vector<AST::ExprNodeP>   vals;
        vals.push_back( NEWNODE(AST::ExprNode_NamedValue, AST::Path("self")) );
        vals.push_back( NEWNODE(AST::ExprNode_NamedValue, AST::Path("v")) );
        return this->make_ret(sp, p, type, NEWNODE(AST::ExprNode_Match,
            NEWNODE(AST::ExprNode_Tuple, mv$(vals)),
            mv$(arms)
            ));
    }
} g_derive_partialeq;

class Deriver_Eq:
    public Deriver
{
    AST::Path get_trait_path() const {
        return AST::Path("", { AST::PathNode("cmp", {}), AST::PathNode("Eq", {}) });
    }
    
    AST::Impl make_ret(Span sp, const AST::GenericParams& p, const TypeRef& type, AST::ExprNodeP node) const
    {
        const AST::Path    trait_path = this->get_trait_path();
        
        AST::Function fcn(
            AST::GenericParams(),
            TypeRef(TypeRef::TagUnit(), sp),
            vec$(
                ::std::make_pair( AST::Pattern(AST::Pattern::TagBind(), "self"), TypeRef(TypeRef::TagReference(), sp, false, TypeRef("Self", 0xFFFF)) )
                )
            );
        fcn.set_code( NEWNODE(AST::ExprNode_Block, vec$(mv$(node)), ::std::unique_ptr<AST::Module>()) );
        
        AST::GenericParams  params = p;
        for(const auto& typ : params.ty_params())
        {
            params.bounds().push_back( ::AST::GenericBound::make_IsTrait({ TypeRef(typ.name()), {}, trait_path }) );
        }
        
        AST::Impl   rv( AST::ImplDef( sp, AST::MetaItems(), mv$(params), make_spanned(sp, trait_path), type ) );
        rv.add_function(false, false, "assert_receiver_is_total_eq", mv$(fcn));
        return mv$(rv);
    }
    AST::ExprNodeP assert_is_eq(const AST::Path& method_path, AST::ExprNodeP val) const {
        return NEWNODE(AST::ExprNode_CallPath,
            AST::Path(method_path),
            vec$( NEWNODE(AST::ExprNode_UniOp, AST::ExprNode_UniOp::REF, mv$(val) ) )
            );
    }
    AST::ExprNodeP field(const ::std::string& name) const {
        return NEWNODE(AST::ExprNode_Field, NEWNODE(AST::ExprNode_NamedValue, AST::Path("self")), name);
    }
    
public:
    AST::Impl handle_item(Span sp, const AST::GenericParams& p, const TypeRef& type, const AST::Struct& str) const override
    {
        const AST::Path    assert_method_path = this->get_trait_path() + "assert_receiver_is_total_eq";
        ::std::vector<AST::ExprNodeP>   nodes;
        
        TU_MATCH(AST::StructData, (str.m_data), (e),
        (Struct,
            for( const auto& fld : e.ents )
            {
                nodes.push_back( this->assert_is_eq(assert_method_path, this->field(fld.m_name)) );
            }
            ),
        (Tuple,
            for( unsigned int idx = 0; idx < e.ents.size(); idx ++ )
            {
                nodes.push_back( this->assert_is_eq(assert_method_path, this->field(FMT(idx))) );
            }
            )
        )
        
        return this->make_ret(sp, p, type, NEWNODE(AST::ExprNode_Block, mv$(nodes), nullptr));
    }
    
    AST::Impl handle_item(Span sp, const AST::GenericParams& p, const TypeRef& type, const AST::Enum& enm) const override
    {
        const AST::Path    assert_method_path = this->get_trait_path() + "assert_receiver_is_total_eq";

        AST::Path base_path = type.m_data.as_Path().path;
        base_path.nodes().back().args() = ::AST::PathParams();
        ::std::vector<AST::ExprNode_Match_Arm>   arms;
        
        for(const auto& v : enm.variants())
        {
            AST::ExprNodeP  code;
            AST::Pattern    pat_a;
            
            TU_MATCH(::AST::EnumVariantData, (v.m_data), (e),
            (Value,
                code = NEWNODE(AST::ExprNode_Block, {}, nullptr);
                pat_a = AST::Pattern(AST::Pattern::TagValue(), AST::Pattern::Value::make_Named(base_path + v.m_name));
                ),
            (Tuple,
                if( e.m_sub_types.size() == 0 )
                {
                    code = NEWNODE(AST::ExprNode_Block, {}, nullptr);
                    pat_a = AST::Pattern(AST::Pattern::TagValue(), AST::Pattern::Value::make_Named(base_path + v.m_name));
                }
                else
                {
                    ::std::vector<AST::Pattern>    pats_a;
                    ::std::vector<AST::ExprNodeP>   nodes;
                    
                    for( unsigned int idx = 0; idx < e.m_sub_types.size(); idx ++ )
                    {
                        auto name_a = FMT("a" << idx);
                        pats_a.push_back( ::AST::Pattern(::AST::Pattern::TagBind(), name_a) );
                        nodes.push_back( this->assert_is_eq(assert_method_path, NEWNODE(AST::ExprNode_NamedValue, AST::Path(name_a))) );
                    }
                    
                    pat_a = AST::Pattern(AST::Pattern::TagEnumVariant(), base_path + v.m_name, mv$(pats_a));
                    code = NEWNODE(AST::ExprNode_Block, mv$(nodes), nullptr);
                }
                ),
            (Struct,
                ::std::vector< ::std::pair<std::string, AST::Pattern> > pats_a;
                ::std::vector<AST::ExprNodeP>   nodes;
                
                for( const auto& fld : e.m_fields )
                {
                    auto name_a = FMT("a" << fld.m_name);
                    pats_a.push_back( ::std::make_pair(fld.m_name, ::AST::Pattern(::AST::Pattern::TagBind(), name_a)) );
                    nodes.push_back( this->assert_is_eq(assert_method_path, NEWNODE(AST::ExprNode_NamedValue, AST::Path(name_a))) );
                }
                
                pat_a = AST::Pattern(AST::Pattern::TagStruct(), base_path + v.m_name, mv$(pats_a), true);
                code = NEWNODE(AST::ExprNode_Block, mv$(nodes), nullptr);
                )
            )
            
            ::std::vector< AST::Pattern>    pats;
            pats.push_back( AST::Pattern(AST::Pattern::TagReference(), mv$(pat_a)) );
            
            arms.push_back(AST::ExprNode_Match_Arm(
                mv$(pats),
                nullptr,
                mv$(code)
                ));
        }

        return this->make_ret(sp, p, type, NEWNODE(AST::ExprNode_Match,
            NEWNODE(AST::ExprNode_NamedValue, AST::Path("self")),
            mv$(arms)
            ));
    }
} g_derive_eq;

class Deriver_Clone:
    public Deriver
{
    AST::Path get_trait_path() const {
        return AST::Path("", { AST::PathNode("clone", {}), AST::PathNode("Clone", {}) });
    }
    AST::Path get_method_path() const {
        return get_trait_path() + "clone";
    }
    
    AST::Impl make_ret(Span sp, const AST::GenericParams& p, const TypeRef& type, AST::ExprNodeP node) const
    {
        const AST::Path    trait_path = this->get_trait_path();
        
        AST::Function fcn(
            AST::GenericParams(),
            TypeRef("Self", 0xFFFF),
            vec$(
                ::std::make_pair( AST::Pattern(AST::Pattern::TagBind(), "self"), TypeRef(TypeRef::TagReference(), sp, false, TypeRef("Self", 0xFFFF)) )
                )
            );
        fcn.set_code( NEWNODE(AST::ExprNode_Block, vec$(mv$(node)), ::std::unique_ptr<AST::Module>()) );
        
        AST::GenericParams  params = p;
        for(const auto& typ : params.ty_params())
        {
            params.bounds().push_back( ::AST::GenericBound::make_IsTrait({ TypeRef(typ.name()), {}, trait_path }) );
        }
        
        AST::Impl   rv( AST::ImplDef( sp, AST::MetaItems(), mv$(params), make_spanned(sp, trait_path), type ) );
        rv.add_function(false, false, "clone", mv$(fcn));
        return mv$(rv);
    }
    AST::ExprNodeP clone_val(AST::ExprNodeP val) const {
        return NEWNODE(AST::ExprNode_CallPath,
            this->get_method_path(),
            vec$( NEWNODE(AST::ExprNode_UniOp, AST::ExprNode_UniOp::REF, mv$(val) ) )
            );
    }
    AST::ExprNodeP field(const ::std::string& name) const {
        return NEWNODE(AST::ExprNode_Field, NEWNODE(AST::ExprNode_NamedValue, AST::Path("self")), name);
    }
    
public:
    AST::Impl handle_item(Span sp, const AST::GenericParams& p, const TypeRef& type, const AST::Struct& str) const override
    {
        const AST::Path& ty_path = type.m_data.as_Path().path;
        ::std::vector<AST::ExprNodeP>   nodes;
        
        TU_MATCH(AST::StructData, (str.m_data), (e),
        (Struct,
            ::std::vector< ::std::pair< ::std::string, AST::ExprNodeP> >    vals;
            for( const auto& fld : e.ents )
            {
                vals.push_back( ::std::make_pair(fld.m_name, this->clone_val(this->field(fld.m_name)) ) );
            }
            nodes.push_back( NEWNODE(AST::ExprNode_StructLiteral, ty_path, nullptr, mv$(vals)) );
            ),
        (Tuple,
            if( e.ents.size() == 0 )
            {
                nodes.push_back( NEWNODE(AST::ExprNode_NamedValue, AST::Path(ty_path)) );
            }
            else
            {
                ::std::vector<AST::ExprNodeP>   vals;
                for( unsigned int idx = 0; idx < e.ents.size(); idx ++ )
                {
                    vals.push_back( this->clone_val(this->field(FMT(idx))) );
                }
                nodes.push_back( NEWNODE(AST::ExprNode_CallPath, AST::Path(ty_path), mv$(vals)) );
            }
            )
        )
        
        return this->make_ret(sp, p, type, NEWNODE(AST::ExprNode_Block, mv$(nodes), nullptr));
    }
    
    AST::Impl handle_item(Span sp, const AST::GenericParams& p, const TypeRef& type, const AST::Enum& enm) const override
    {
        const AST::Path    assert_method_path = this->get_trait_path() + "assert_receiver_is_total_eq";

        AST::Path base_path = type.m_data.as_Path().path;
        base_path.nodes().back().args() = ::AST::PathParams();
        ::std::vector<AST::ExprNode_Match_Arm>   arms;
        
        for(const auto& v : enm.variants())
        {
            AST::ExprNodeP  code;
            AST::Pattern    pat_a;
            
            TU_MATCH(::AST::EnumVariantData, (v.m_data), (e),
            (Value,
                code = NEWNODE(AST::ExprNode_NamedValue, base_path + v.m_name);
                pat_a = AST::Pattern(AST::Pattern::TagValue(), AST::Pattern::Value::make_Named(base_path + v.m_name));
                ),
            (Tuple,
                if( e.m_sub_types.size() == 0 )
                {
                    code = NEWNODE(AST::ExprNode_NamedValue, base_path + v.m_name);
                    pat_a = AST::Pattern(AST::Pattern::TagValue(), AST::Pattern::Value::make_Named(base_path + v.m_name));
                }
                else
                {
                    ::std::vector<AST::Pattern>    pats_a;
                    ::std::vector<AST::ExprNodeP>   nodes;
                    
                    for( unsigned int idx = 0; idx < e.m_sub_types.size(); idx ++ )
                    {
                        auto name_a = FMT("a" << idx);
                        pats_a.push_back( ::AST::Pattern(::AST::Pattern::TagBind(), name_a) );
                        nodes.push_back( this->clone_val(NEWNODE(AST::ExprNode_NamedValue, AST::Path(name_a))) );
                    }
                    
                    pat_a = AST::Pattern(AST::Pattern::TagEnumVariant(), base_path + v.m_name, mv$(pats_a));
                    code = NEWNODE(AST::ExprNode_CallPath, base_path + v.m_name, mv$(nodes));
                }
                ),
            (Struct,
                ::std::vector< ::std::pair<std::string, AST::Pattern> > pats_a;
                ::std::vector< ::std::pair<std::string, AST::ExprNodeP> >   vals;
                
                for( const auto& fld : e.m_fields )
                {
                    auto name_a = FMT("a" << fld.m_name);
                    pats_a.push_back( ::std::make_pair(fld.m_name, ::AST::Pattern(::AST::Pattern::TagBind(), name_a)) );
                    vals.push_back( ::std::make_pair( fld.m_name, this->clone_val(NEWNODE(AST::ExprNode_NamedValue, AST::Path(name_a))) ) );
                }
                
                pat_a = AST::Pattern(AST::Pattern::TagStruct(), base_path + v.m_name, mv$(pats_a), true);
                code = NEWNODE(AST::ExprNode_StructLiteral, base_path + v.m_name, nullptr, mv$(vals));
                )
            )
            
            ::std::vector< AST::Pattern>    pats;
            pats.push_back( AST::Pattern(AST::Pattern::TagReference(), mv$(pat_a)) );
            
            arms.push_back(AST::ExprNode_Match_Arm(
                mv$(pats),
                nullptr,
                mv$(code)
                ));
        }

        return this->make_ret(sp, p, type, NEWNODE(AST::ExprNode_Match,
            NEWNODE(AST::ExprNode_NamedValue, AST::Path("self")),
            mv$(arms)
            ));
    }
} g_derive_clone;

class Deriver_Copy:
    public Deriver
{
    AST::Path get_trait_path() const {
        return AST::Path("", { AST::PathNode("marker", {}), AST::PathNode("Copy", {}) });
    }
    
    AST::Impl make_ret(Span sp, const AST::GenericParams& p, const TypeRef& type, AST::ExprNodeP node) const
    {
        const AST::Path    trait_path = this->get_trait_path();
        
        AST::GenericParams  params = p;
        for(const auto& typ : params.ty_params())
        {
            params.bounds().push_back( ::AST::GenericBound::make_IsTrait({ TypeRef(typ.name()), {}, trait_path }) );
        }
        
        AST::Impl   rv( AST::ImplDef( sp, AST::MetaItems(), mv$(params), make_spanned(sp, trait_path), type ) );
        return mv$(rv);
    }
    
public:
    AST::Impl handle_item(Span sp, const AST::GenericParams& p, const TypeRef& type, const AST::Struct& str) const override
    {
        return this->make_ret(sp, p, type, nullptr);
    }
    
    AST::Impl handle_item(Span sp, const AST::GenericParams& p, const TypeRef& type, const AST::Enum& enm) const override
    {
        return this->make_ret(sp, p, type, nullptr);
    }
} g_derive_copy;

// --------------------------------------------------------------------
// Select and dispatch the correct derive() handler
// --------------------------------------------------------------------
static const Deriver* find_impl(const ::std::string& trait_name)
{
    if( trait_name == "Debug" )
        return &g_derive_debug;
    else if( trait_name == "PartialEq" )
        return &g_derive_partialeq;
    else if( trait_name == "Eq" )
        return &g_derive_eq;
    else if( trait_name == "Clone" )
        return &g_derive_clone;
    else if( trait_name == "Copy" )
        return &g_derive_copy;
    else
        return nullptr;
}

template<typename T>
static void derive_item(const Span& sp, AST::Module& mod, const AST::MetaItem& attr, const AST::Path& path, const T& item)
{
    if( !attr.has_sub_items() ) {
        //ERROR(sp, E0000, "#[derive()] requires a list of known traits to derive");
        return ;
    }
    
    DEBUG("path = " << path);
    bool    fail = false;
    
    const auto& params = item.params();
    TypeRef type(sp, path);
    auto& types_args = type.path().nodes().back().args();
    for( const auto& param : params.ty_params() ) {
        types_args.m_types.push_back( TypeRef(TypeRef::TagArg(), param.name()) );
    }
    
    ::std::vector< ::std::string>   missing_handlers;
    for( const auto& trait : attr.items() )
    {
        DEBUG("- " << trait.name());
        auto dp = find_impl(trait.name());
        if( !dp ) {
            DEBUG("> No handler for " << trait.name());
            missing_handlers.push_back( trait.name() );
            fail = true;
            continue ;
        }
        
        mod.add_impl( dp->handle_item(sp, params, type, item) );
    }
    
    if( fail ) {
        //ERROR(sp, E0000, "Failed to apply #[derive] - Missing handlers for " << missing_handlers);
    }
}

class Decorator_Derive:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::LatePost; }
    void handle(const Span& sp, const AST::MetaItem& attr, ::AST::Crate& crate, const AST::Path& path, AST::Module& mod, AST::Item& i) const override
    {
        TU_MATCH_DEF(::AST::Item, (i), (e),
        (
            TODO(sp, "Handle #[derive] for other item types");
            ),
        (Enum,
            derive_item(sp, mod, attr, path, e);
            ),
        (Struct,
            derive_item(sp, mod, attr, path, e);
            )
        )
    }
};

STATIC_DECORATOR("derive", Decorator_Derive)

