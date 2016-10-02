
#include <synext.hpp>
#include "../common.hpp"
#include "../ast/ast.hpp"
#include "../ast/expr.hpp"
#include "../ast/crate.hpp"

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
//#define NEWNODE(type, ...)  mk_exprnodep(new type(__VA_ARGS__))
#define NEWNODE(type, ...)  mk_exprnodep(new AST::ExprNode_##type(__VA_ARGS__))

/// Interface for derive handlers
struct Deriver
{
    virtual AST::Impl handle_item(Span sp, const ::std::string& core_name, const AST::GenericParams& p, const TypeRef& type, const AST::Struct& str) const = 0;
    virtual AST::Impl handle_item(Span sp, const ::std::string& core_name, const AST::GenericParams& p, const TypeRef& type, const AST::Enum& enm) const = 0;
    
    
    AST::GenericParams get_params_with_bounds(const AST::GenericParams& p, const AST::Path& trait_path, ::std::vector<TypeRef> additional_bounded_types) const
    {
        AST::GenericParams  params = p;
        
        // TODO: Get bounds based on generic (or similar) types used within the type.
        // - How would this code (that runs before resolve) know what's a generic and what's a local type?
        // - Searches within the type for a Path that starts with that param.
        
        unsigned int i = 0;
        for(const auto& arg : params.ty_params())
        {
            params.add_bound( ::AST::GenericBound::make_IsTrait({
                TypeRef(arg.name(), i), {}, trait_path
                }) );
            i ++;
        }
        
        // For each field type
        // - Locate used generic parameters in the type (and sub-types that directly use said parameter)
        for(auto& ty : additional_bounded_types)
        {
            params.add_bound( ::AST::GenericBound::make_IsTrait({
                mv$(ty), {}, trait_path
                }) );
        }
        
        return params;
    }
    
    
    ::std::vector<TypeRef> get_field_bounds(const AST::Struct& str) const
    {
        ::std::vector<TypeRef>  ret;
        TU_MATCH(AST::StructData, (str.m_data), (e),
        (Struct,
            for( const auto& fld : e.ents )
            {
                add_field_bound_from_ty(str.params(), ret, fld.m_type);
            }
            ),
        (Tuple,
            for(const auto& ent : e.ents)
            {
                add_field_bound_from_ty(str.params(), ret, ent.m_type);
            }
            )
        )
        return ret;
    }
    ::std::vector<TypeRef> get_field_bounds(const AST::Enum& enm) const
    {
        ::std::vector<TypeRef>  ret;

        for(const auto& v : enm.variants())
        {
            TU_MATCH(::AST::EnumVariantData, (v.m_data), (e),
            (Value,
                ),
            (Tuple,
                for(const auto& ty : e.m_sub_types)
                {
                    add_field_bound_from_ty(enm.params(), ret, ty);
                }
                ),
            (Struct,
                for( const auto& fld : e.m_fields )
                {
                    add_field_bound_from_ty(enm.params(), ret, fld.m_type);
                }
                )
            )
        }
        
        return ret;
    }
    
    void add_field_bound_from_ty(const AST::GenericParams& params, ::std::vector<TypeRef>& out_list, const TypeRef& ty) const
    {
        struct H {
            static void visit_nodes(const Deriver& self, const AST::GenericParams& params, ::std::vector<TypeRef>& out_list, const ::std::vector<AST::PathNode>& nodes) {
                for(const auto& node : nodes) {
                    for(const auto& ty : node.args().m_types) {
                        self.add_field_bound_from_ty(params, out_list, ty);
                    }
                    for(const auto& aty : node.args().m_assoc) {
                        self.add_field_bound_from_ty(params, out_list, aty.second);
                    }
                }
            }
        };
        // TODO: Locate type that is directly related to the type param.
        TU_MATCH(TypeData, (ty.m_data), (e),
        (None,
            // Wat?
            ),
        (Any,
            // Nope.
            ),
        (Unit,
            ),
        (Bang,
            ),
        (Macro,
            // not allowed
            ),
        (Primitive,
            ),
        (Function,
            // TODO? Well... function types don't tend to depend on the trait?
            ),
        (Tuple,
            for(const auto& sty : e.inner_types) {
                add_field_bound_from_ty(params, out_list, sty);
            }
            ),
        (Borrow,
            add_field_bound_from_ty(params, out_list, *e.inner);
            ),
        (Pointer,
            add_field_bound_from_ty(params, out_list, *e.inner);
            ),
        (Array,
            add_field_bound_from_ty(params, out_list, *e.inner);
            ),
        (Generic,
            // Although this is what we're looking for, it's already handled.
            ),
        (Path,
            TU_MATCH(AST::Path::Class, (e.path.m_class), (pe),
            (Invalid,
                // wut.
                ),
            (Local,
                ),
            (Relative,
                if( pe.nodes.size() > 1 )
                {
                    // Check if the first node of a relative is a generic param.
                    for(const auto& typ : params.ty_params())
                    {
                        if( pe.nodes.front().name() == typ.name() )
                        {
                            add_field_bound(out_list, ty);
                            break ;
                        }
                    }
                }
                H::visit_nodes(*this, params, out_list, pe.nodes);
                ),
            (Self,
                ),
            (Super,
                ),
            (Absolute,
                ),
            (UFCS,
                )
            )
            ),
        (TraitObject,
            // TODO: Should this be recursed?
            )
        )
    }
    void add_field_bound(::std::vector<TypeRef>& out_list, const TypeRef& type) const
    {
        for( const auto& ty : out_list )
        {
            if( ty == type ) {
                return ;
            }
        }

        out_list.push_back(type);
    }
};

/// 'Debug' derive handler
class Deriver_Debug:
    public Deriver
{
    //static AST::ExprNodeP _print_l(::std::string val)
    //{
    //    return NEWNODE(CallMethod,
    //            NEWNODE(NamedValue, AST::Path("f")),
    //            AST::PathNode("write_str",{}),
    //            { NEWNODE(String, mv$(val)) }
    //            );
    //}
    //static AST::ExprNodeP _try(AST::ExprNodeP expr)
    //{
    //    throw CompileError::Todo("derive(Debug) - _try");
    //}
    
    AST::Impl make_ret(Span sp, const ::std::string& core_name, const AST::GenericParams& p, const TypeRef& type, ::std::vector<TypeRef> types_to_bound, AST::ExprNodeP node) const
    {
        const AST::Path    debug_trait = AST::Path(core_name, { AST::PathNode("fmt", {}), AST::PathNode("Debug", {}) });
        const TypeRef  ret_type(sp, AST::Path(core_name, {AST::PathNode("fmt",{}), AST::PathNode("Result",{})}) );
        const TypeRef  f_type(TypeRef::TagReference(), sp, true,
            TypeRef(sp, AST::Path(core_name, {AST::PathNode("fmt",{}), AST::PathNode("Formatter", {})}))
            );
        
        DEBUG("node = " << *node);
        
        AST::Function fcn(
            sp,
            AST::GenericParams(),
            "rust", false, false, false,
            ret_type,
            vec$(
                ::std::make_pair( AST::Pattern(AST::Pattern::TagBind(), "self"), TypeRef(TypeRef::TagReference(), sp, false, TypeRef("Self", 0xFFFF)) ),
                ::std::make_pair( AST::Pattern(AST::Pattern::TagBind(), "f"), f_type )
                )
            );
        fcn.set_code( NEWNODE(Block, vec$(mv$(node))) );
        
        AST::GenericParams  params = get_params_with_bounds(p, debug_trait, mv$(types_to_bound));
        
        AST::Impl   rv( AST::ImplDef( sp, AST::MetaItems(), mv$(params), make_spanned(sp, debug_trait), type ) );
        rv.add_function(false, false, "fmt", mv$(fcn));
        return mv$(rv);
    }
    
public:
    AST::Impl handle_item(Span sp, const ::std::string& core_name, const AST::GenericParams& p, const TypeRef& type, const AST::Struct& str) const override
    {
        // TODO: be correct herhe and use "core" as the crate name
        // - Requires handling the crate_name crate attribute correctly
        const ::std::string& name = type.path().nodes().back().name();
        
        // Generate code for Debug
        AST::ExprNodeP  node;
        TU_MATCH(AST::StructData, (str.m_data), (e),
        (Struct,
            node = NEWNODE(NamedValue, AST::Path("f"));
            node = NEWNODE(CallMethod,
                mv$(node), AST::PathNode("debug_struct",{}),
                vec$( NEWNODE(String, name) )
                );
            for( const auto& fld : e.ents )
            {
                node = NEWNODE(CallMethod,
                    mv$(node), AST::PathNode("field",{}),
                    vec$(
                        NEWNODE(String, fld.m_name),
                        NEWNODE(UniOp, AST::ExprNode_UniOp::REF,
                            NEWNODE(Field,
                                NEWNODE(NamedValue, AST::Path("self")),
                                fld.m_name
                                )
                            )
                    )
                    );
            }
            node = NEWNODE(CallMethod, mv$(node), AST::PathNode("finish",{}), {});
            ),
        (Tuple,
            node = NEWNODE(NamedValue, AST::Path("f"));
            node = NEWNODE(CallMethod,
                mv$(node), AST::PathNode("debug_tuple",{}),
                vec$( NEWNODE(String, name) )
                );
            for( unsigned int idx = 0; idx < e.ents.size(); idx ++ )
            {
                node = NEWNODE(CallMethod,
                    mv$(node), AST::PathNode("field",{}),
                    vec$(
                        NEWNODE(UniOp, AST::ExprNode_UniOp::REF,
                            NEWNODE(Field,
                                NEWNODE(NamedValue, AST::Path("self")),
                                FMT(idx)
                                )
                            )
                        )
                    );
            }
            node = NEWNODE(CallMethod, mv$(node), AST::PathNode("finish",{}), {});
            )
        )
        
        return this->make_ret(sp, core_name, p, type, this->get_field_bounds(str), mv$(node));
    }
    AST::Impl handle_item(Span sp, const ::std::string& core_name, const AST::GenericParams& p, const TypeRef& type, const AST::Enum& enm) const override
    {
        AST::Path base_path = type.m_data.as_Path().path;
        base_path.nodes().back() = base_path.nodes().back().name();
        
        ::std::vector< AST::ExprNode_Match_Arm> arms;
        for(const auto& v : enm.variants())
        {
            AST::ExprNodeP  code;
            AST::Pattern    pat_a;
            
            TU_MATCH(::AST::EnumVariantData, (v.m_data), (e),
            (Value,
                code = NEWNODE(CallMethod,
                    NEWNODE(NamedValue, AST::Path("f")),
                    AST::PathNode("write_str",{}),
                    vec$( NEWNODE(String, v.m_name) )
                    );
                pat_a = AST::Pattern(AST::Pattern::TagValue(), AST::Pattern::Value::make_Named(base_path + v.m_name));
                ),
            (Tuple,
                if( e.m_sub_types.size() == 0 )
                {
                    code = NEWNODE(CallMethod,
                        NEWNODE(NamedValue, AST::Path("f")),
                        AST::PathNode("write_str",{}),
                        vec$( NEWNODE(String, v.m_name + "()") )
                        );
                    pat_a = AST::Pattern(AST::Pattern::TagValue(), AST::Pattern::Value::make_Named(base_path + v.m_name));
                }
                else
                {
                    // TODO: Complete this.
                    ::std::vector<AST::Pattern>    pats_a;
                    //::std::vector<AST::ExprNodeP>   nodes;
                    
                    for( unsigned int idx = 0; idx < e.m_sub_types.size(); idx ++ )
                    {
                        auto name_a = FMT("a" << idx);
                        pats_a.push_back( ::AST::Pattern(::AST::Pattern::TagBind(), name_a, ::AST::PatternBinding::Type::REF) );
                        //nodes.push_back( this->assert_is_eq(assert_method_path, NEWNODE(NamedValue, AST::Path(name_a))) );
                    }
                    
                    //code = NEWNODE(Block, mv$(nodes));
                    code = NEWNODE(CallMethod,
                        NEWNODE(NamedValue, AST::Path("f")),
                        AST::PathNode("write_str",{}),
                        vec$( NEWNODE(String, v.m_name + "(...)") )
                        );
                    
                    pat_a = AST::Pattern(AST::Pattern::TagNamedTuple(), base_path + v.m_name, mv$(pats_a));
                }
                ),
            (Struct,
                ::std::vector< ::std::pair<std::string, AST::Pattern> > pats_a;
                //::std::vector<AST::ExprNodeP>   nodes;
                
                for( const auto& fld : e.m_fields )
                {
                    auto name_a = FMT("a" << fld.m_name);
                    pats_a.push_back( ::std::make_pair(fld.m_name, ::AST::Pattern(::AST::Pattern::TagBind(), name_a, ::AST::PatternBinding::Type::REF)) );
                    //nodes.push_back( this->assert_is_eq(assert_method_path, NEWNODE(NamedValue, AST::Path(name_a))) );
                }
                
                //code = NEWNODE(Block, mv$(nodes) );
                code = NEWNODE(CallMethod,
                    NEWNODE(NamedValue, AST::Path("f")),
                    AST::PathNode("write_str",{}),
                    vec$( NEWNODE(String, v.m_name + "{...}") )
                    );
                pat_a = AST::Pattern(AST::Pattern::TagStruct(), base_path + v.m_name, mv$(pats_a), true);
                )
            )
            
            ::std::vector< AST::Pattern>    pats;
            pats.push_back( AST::Pattern(AST::Pattern::TagReference(), false, mv$(pat_a)) );
            
            arms.push_back(AST::ExprNode_Match_Arm(
                mv$(pats),
                nullptr,
                mv$(code)
                ));
        }
        AST::ExprNodeP  node = NEWNODE(Match,
            NEWNODE(NamedValue, AST::Path("self")),
            mv$(arms)
            );
        
        return this->make_ret(sp, core_name, p, type, this->get_field_bounds(enm), mv$(node));
    }
} g_derive_debug;

class Deriver_PartialEq:
    public Deriver
{
    AST::Impl make_ret(Span sp, const ::std::string& core_name, const AST::GenericParams& p, const TypeRef& type, ::std::vector<TypeRef> types_to_bound, AST::ExprNodeP node) const
    {
        const AST::Path    trait_path(core_name, { AST::PathNode("cmp", {}), AST::PathNode("PartialEq", {}) });
        
        AST::Function fcn(
            sp,
            AST::GenericParams(),
            "rust", false, false, false,
            TypeRef(sp, CORETYPE_BOOL),
            vec$(
                ::std::make_pair( AST::Pattern(AST::Pattern::TagBind(), "self"), TypeRef(TypeRef::TagReference(), sp, false, TypeRef("Self", 0xFFFF)) ),
                ::std::make_pair( AST::Pattern(AST::Pattern::TagBind(), "v"), TypeRef(TypeRef::TagReference(), sp, false, TypeRef("Self", 0xFFFF)) )
                )
            );
        fcn.set_code( NEWNODE(Block, vec$(mv$(node))) );
        
        AST::GenericParams  params = get_params_with_bounds(p, trait_path, mv$(types_to_bound));
        
        AST::Impl   rv( AST::ImplDef( sp, AST::MetaItems(), mv$(params), make_spanned(sp, trait_path), type ) );
        rv.add_function(false, false, "eq", mv$(fcn));
        return mv$(rv);
    }
public:
    AST::Impl handle_item(Span sp, const ::std::string& core_name, const AST::GenericParams& p, const TypeRef& type, const AST::Struct& str) const override
    {
        ::std::vector<AST::ExprNodeP>   nodes;
        
        TU_MATCH(AST::StructData, (str.m_data), (e),
        (Struct,
            for( const auto& fld : e.ents )
            {
                nodes.push_back(NEWNODE(If,
                    NEWNODE(BinOp, AST::ExprNode_BinOp::CMPNEQU,
                        NEWNODE(Field, NEWNODE(NamedValue, AST::Path("self")), fld.m_name),
                        NEWNODE(Field, NEWNODE(NamedValue, AST::Path("v")), fld.m_name)
                        ),
                    NEWNODE(Flow, AST::ExprNode_Flow::RETURN, "", NEWNODE(Bool, false)),
                    nullptr
                    ));
            }
            ),
        (Tuple,
            for( unsigned int idx = 0; idx < e.ents.size(); idx ++ )
            {
                auto fld_name = FMT(idx);
                nodes.push_back(NEWNODE(If,
                    NEWNODE(BinOp, AST::ExprNode_BinOp::CMPNEQU,
                        NEWNODE(Field, NEWNODE(NamedValue, AST::Path("self")), fld_name),
                        NEWNODE(Field, NEWNODE(NamedValue, AST::Path("v")), fld_name)
                        ),
                    NEWNODE(Flow, AST::ExprNode_Flow::RETURN, "", NEWNODE(Bool, false)),
                    nullptr
                    ));
            }
            )
        )
        nodes.push_back( NEWNODE(Bool, true) );
        
        return this->make_ret(sp, core_name, p, type, this->get_field_bounds(str), NEWNODE(Block, mv$(nodes)));
    }
    
    AST::Impl handle_item(Span sp, const ::std::string& core_name, const AST::GenericParams& p, const TypeRef& type, const AST::Enum& enm) const override
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
                code = NEWNODE(Bool, true);
                pat_a = AST::Pattern(AST::Pattern::TagValue(), AST::Pattern::Value::make_Named(base_path + v.m_name));
                pat_b = AST::Pattern(AST::Pattern::TagValue(), AST::Pattern::Value::make_Named(base_path + v.m_name));
                ),
            (Tuple,
                if( e.m_sub_types.size() == 0 )
                {
                    code = NEWNODE(Bool, true);
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
                        pats_a.push_back( ::AST::Pattern(::AST::Pattern::TagBind(), name_a, ::AST::PatternBinding::Type::REF) );
                        pats_b.push_back( ::AST::Pattern(::AST::Pattern::TagBind(), name_b, ::AST::PatternBinding::Type::REF) );
                        nodes.push_back(NEWNODE(If,
                            NEWNODE(BinOp, AST::ExprNode_BinOp::CMPNEQU,
                                NEWNODE(NamedValue, AST::Path(name_a)),
                                NEWNODE(NamedValue, AST::Path(name_b))
                                ),
                            NEWNODE(Flow, AST::ExprNode_Flow::RETURN, "", NEWNODE(Bool, false)),
                            nullptr
                            ));
                    }
                    
                    nodes.push_back( NEWNODE(Bool, true) );
                    pat_a = AST::Pattern(AST::Pattern::TagNamedTuple(), base_path + v.m_name, mv$(pats_a));
                    pat_b = AST::Pattern(AST::Pattern::TagNamedTuple(), base_path + v.m_name, mv$(pats_b));
                    code = NEWNODE(Block, mv$(nodes));
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
                    pats_a.push_back( ::std::make_pair(fld.m_name, ::AST::Pattern(::AST::Pattern::TagBind(), name_a, ::AST::PatternBinding::Type::REF)) );
                    pats_b.push_back( ::std::make_pair(fld.m_name, ::AST::Pattern(::AST::Pattern::TagBind(), name_b, ::AST::PatternBinding::Type::REF)) );
                    nodes.push_back(NEWNODE(If,
                        NEWNODE(BinOp, AST::ExprNode_BinOp::CMPNEQU,
                            NEWNODE(NamedValue, AST::Path(name_a)),
                            NEWNODE(NamedValue, AST::Path(name_b))
                            ),
                        NEWNODE(Flow, AST::ExprNode_Flow::RETURN, "", NEWNODE(Bool, false)),
                        nullptr
                        ));
                }
                
                nodes.push_back( NEWNODE(Bool, true) );
                pat_a = AST::Pattern(AST::Pattern::TagStruct(), base_path + v.m_name, mv$(pats_a), true);
                pat_b = AST::Pattern(AST::Pattern::TagStruct(), base_path + v.m_name, mv$(pats_b), true);
                code = NEWNODE(Block, mv$(nodes));
                )
            )
            
            ::std::vector< AST::Pattern>    pats;
            {
                ::std::vector< AST::Pattern>    tuple_pats;
                tuple_pats.push_back( AST::Pattern(AST::Pattern::TagReference(), false, mv$(pat_a)) );
                tuple_pats.push_back( AST::Pattern(AST::Pattern::TagReference(), false, mv$(pat_b)) );
                pats.push_back( AST::Pattern(AST::Pattern::TagTuple(), mv$(tuple_pats)) );
            }
            
            arms.push_back(AST::ExprNode_Match_Arm(
                mv$(pats),
                nullptr,
                mv$(code)
                ));
        }
        
        // Default arm
        {
            arms.push_back(AST::ExprNode_Match_Arm(
                ::make_vec1( AST::Pattern() ),
                nullptr,
                NEWNODE(Bool, false)
                ));
        }

        ::std::vector<AST::ExprNodeP>   vals;
        vals.push_back( NEWNODE(NamedValue, AST::Path("self")) );
        vals.push_back( NEWNODE(NamedValue, AST::Path("v")) );
        return this->make_ret(sp, core_name, p, type, this->get_field_bounds(enm), NEWNODE(Match,
            NEWNODE(Tuple, mv$(vals)),
            mv$(arms)
            ));
    }
} g_derive_partialeq;

class Deriver_Eq:
    public Deriver
{
    AST::Path get_trait_path(const ::std::string& core_name) const {
        return AST::Path(core_name, { AST::PathNode("cmp", {}), AST::PathNode("Eq", {}) });
    }
    
    AST::Impl make_ret(Span sp, const ::std::string& core_name, const AST::GenericParams& p, const TypeRef& type, ::std::vector<TypeRef> types_to_bound, AST::ExprNodeP node) const
    {
        const AST::Path    trait_path = this->get_trait_path(core_name);
        
        AST::Function fcn(
            sp,
            AST::GenericParams(),
            "rust", false, false, false,
            TypeRef(TypeRef::TagUnit(), sp),
            vec$(
                ::std::make_pair( AST::Pattern(AST::Pattern::TagBind(), "self"), TypeRef(TypeRef::TagReference(), sp, false, TypeRef("Self", 0xFFFF)) )
                )
            );
        fcn.set_code( NEWNODE(Block, vec$(mv$(node))) );
        
        AST::GenericParams  params = get_params_with_bounds(p, trait_path, mv$(types_to_bound));
        
        AST::Impl   rv( AST::ImplDef( sp, AST::MetaItems(), mv$(params), make_spanned(sp, trait_path), type ) );
        rv.add_function(false, false, "assert_receiver_is_total_eq", mv$(fcn));
        return mv$(rv);
    }
    AST::ExprNodeP assert_is_eq(const AST::Path& method_path, AST::ExprNodeP val) const {
        return NEWNODE(CallPath,
            AST::Path(method_path),
            vec$( NEWNODE(UniOp, AST::ExprNode_UniOp::REF, mv$(val) ) )
            );
    }
    AST::ExprNodeP field(const ::std::string& name) const {
        return NEWNODE(Field, NEWNODE(NamedValue, AST::Path("self")), name);
    }
    
public:
    AST::Impl handle_item(Span sp, const ::std::string& core_name, const AST::GenericParams& p, const TypeRef& type, const AST::Struct& str) const override
    {
        const AST::Path    assert_method_path = this->get_trait_path(core_name) + "assert_receiver_is_total_eq";
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
        
        return this->make_ret(sp, core_name, p, type, this->get_field_bounds(str), NEWNODE(Block, mv$(nodes)));
    }
    
    AST::Impl handle_item(Span sp, const ::std::string& core_name, const AST::GenericParams& p, const TypeRef& type, const AST::Enum& enm) const override
    {
        const AST::Path    assert_method_path = this->get_trait_path(core_name) + "assert_receiver_is_total_eq";

        AST::Path base_path = type.m_data.as_Path().path;
        base_path.nodes().back().args() = ::AST::PathParams();
        ::std::vector<AST::ExprNode_Match_Arm>   arms;
        
        for(const auto& v : enm.variants())
        {
            AST::ExprNodeP  code;
            AST::Pattern    pat_a;
            
            TU_MATCH(::AST::EnumVariantData, (v.m_data), (e),
            (Value,
                code = NEWNODE(Block);
                pat_a = AST::Pattern(AST::Pattern::TagValue(), AST::Pattern::Value::make_Named(base_path + v.m_name));
                ),
            (Tuple,
                if( e.m_sub_types.size() == 0 )
                {
                    code = NEWNODE(Block);
                    pat_a = AST::Pattern(AST::Pattern::TagValue(), AST::Pattern::Value::make_Named(base_path + v.m_name));
                }
                else
                {
                    ::std::vector<AST::Pattern>    pats_a;
                    ::std::vector<AST::ExprNodeP>   nodes;
                    
                    for( unsigned int idx = 0; idx < e.m_sub_types.size(); idx ++ )
                    {
                        auto name_a = FMT("a" << idx);
                        pats_a.push_back( ::AST::Pattern(::AST::Pattern::TagBind(), name_a, ::AST::PatternBinding::Type::REF) );
                        nodes.push_back( this->assert_is_eq(assert_method_path, NEWNODE(NamedValue, AST::Path(name_a))) );
                    }
                    
                    pat_a = AST::Pattern(AST::Pattern::TagNamedTuple(), base_path + v.m_name, mv$(pats_a));
                    code = NEWNODE(Block, mv$(nodes));
                }
                ),
            (Struct,
                ::std::vector< ::std::pair<std::string, AST::Pattern> > pats_a;
                ::std::vector<AST::ExprNodeP>   nodes;
                
                for( const auto& fld : e.m_fields )
                {
                    auto name_a = FMT("a" << fld.m_name);
                    pats_a.push_back( ::std::make_pair(fld.m_name, ::AST::Pattern(::AST::Pattern::TagBind(), name_a, ::AST::PatternBinding::Type::REF)) );
                    nodes.push_back( this->assert_is_eq(assert_method_path, NEWNODE(NamedValue, AST::Path(name_a))) );
                }
                
                pat_a = AST::Pattern(AST::Pattern::TagStruct(), base_path + v.m_name, mv$(pats_a), true);
                code = NEWNODE(Block, mv$(nodes));
                )
            )
            
            ::std::vector< AST::Pattern>    pats;
            pats.push_back( AST::Pattern(AST::Pattern::TagReference(), false, mv$(pat_a)) );
            
            arms.push_back(AST::ExprNode_Match_Arm(
                mv$(pats),
                nullptr,
                mv$(code)
                ));
        }

        return this->make_ret(sp, core_name, p, type, this->get_field_bounds(enm), NEWNODE(Match,
            NEWNODE(NamedValue, AST::Path("self")),
            mv$(arms)
            ));
    }
} g_derive_eq;

class Deriver_Clone:
    public Deriver
{
    AST::Path get_trait_path(const ::std::string& core_name) const {
        return AST::Path(core_name, { AST::PathNode("clone", {}), AST::PathNode("Clone", {}) });
    }
    AST::Path get_method_path(const ::std::string& core_name) const {
        return get_trait_path(core_name) + "clone";
    }
    
    AST::Impl make_ret(Span sp, const ::std::string& core_name, const AST::GenericParams& p, const TypeRef& type, ::std::vector<TypeRef> types_to_bound, AST::ExprNodeP node) const
    {
        const AST::Path    trait_path = this->get_trait_path(core_name);
        
        AST::Function fcn(
            sp,
            AST::GenericParams(),
            "rust", false, false, false,
            TypeRef("Self", 0xFFFF),
            vec$(
                ::std::make_pair( AST::Pattern(AST::Pattern::TagBind(), "self"), TypeRef(TypeRef::TagReference(), sp, false, TypeRef("Self", 0xFFFF)) )
                )
            );
        fcn.set_code( NEWNODE(Block, vec$(mv$(node))) );
        
        AST::GenericParams  params = get_params_with_bounds(p, trait_path, mv$(types_to_bound));
        
        AST::Impl   rv( AST::ImplDef( sp, AST::MetaItems(), mv$(params), make_spanned(sp, trait_path), type ) );
        rv.add_function(false, false, "clone", mv$(fcn));
        return mv$(rv);
    }
    AST::ExprNodeP clone_val_ref(const ::std::string& core_name, AST::ExprNodeP val) const {
        return NEWNODE(CallPath,
            this->get_method_path(core_name),
            vec$( NEWNODE(UniOp, AST::ExprNode_UniOp::REF, mv$(val) ) )
            );
    }
    AST::ExprNodeP clone_val_direct(const ::std::string& core_name, AST::ExprNodeP val) const {
        return NEWNODE(CallPath,
            this->get_method_path(core_name),
            vec$( mv$(val) )
            );
    }
    AST::ExprNodeP field(const ::std::string& name) const {
        return NEWNODE(Field, NEWNODE(NamedValue, AST::Path("self")), name);
    }
    
public:
    AST::Impl handle_item(Span sp, const ::std::string& core_name, const AST::GenericParams& p, const TypeRef& type, const AST::Struct& str) const override
    {
        const AST::Path& ty_path = type.m_data.as_Path().path;
        ::std::vector<AST::ExprNodeP>   nodes;
        
        TU_MATCH(AST::StructData, (str.m_data), (e),
        (Struct,
            ::std::vector< ::std::pair< ::std::string, AST::ExprNodeP> >    vals;
            for( const auto& fld : e.ents )
            {
                vals.push_back( ::std::make_pair(fld.m_name, this->clone_val_ref(core_name, this->field(fld.m_name)) ) );
            }
            nodes.push_back( NEWNODE(StructLiteral, ty_path, nullptr, mv$(vals)) );
            ),
        (Tuple,
            if( e.ents.size() == 0 )
            {
                nodes.push_back( NEWNODE(NamedValue, AST::Path(ty_path)) );
            }
            else
            {
                ::std::vector<AST::ExprNodeP>   vals;
                for( unsigned int idx = 0; idx < e.ents.size(); idx ++ )
                {
                    vals.push_back( this->clone_val_ref(core_name, this->field(FMT(idx))) );
                }
                nodes.push_back( NEWNODE(CallPath, AST::Path(ty_path), mv$(vals)) );
            }
            )
        )
        
        return this->make_ret(sp, core_name, p, type, this->get_field_bounds(str), NEWNODE(Block, mv$(nodes)));
    }
    
    AST::Impl handle_item(Span sp, const ::std::string& core_name, const AST::GenericParams& p, const TypeRef& type, const AST::Enum& enm) const override
    {
        const AST::Path    assert_method_path = this->get_trait_path(core_name) + "assert_receiver_is_total_eq";

        AST::Path base_path = type.m_data.as_Path().path;
        base_path.nodes().back().args() = ::AST::PathParams();
        ::std::vector<AST::ExprNode_Match_Arm>   arms;
        
        for(const auto& v : enm.variants())
        {
            AST::ExprNodeP  code;
            AST::Pattern    pat_a;
            
            TU_MATCH(::AST::EnumVariantData, (v.m_data), (e),
            (Value,
                code = NEWNODE(NamedValue, base_path + v.m_name);
                pat_a = AST::Pattern(AST::Pattern::TagValue(), AST::Pattern::Value::make_Named(base_path + v.m_name));
                ),
            (Tuple,
                if( e.m_sub_types.size() == 0 )
                {
                    code = NEWNODE(NamedValue, base_path + v.m_name);
                    pat_a = AST::Pattern(AST::Pattern::TagValue(), AST::Pattern::Value::make_Named(base_path + v.m_name));
                }
                else
                {
                    ::std::vector<AST::Pattern>    pats_a;
                    ::std::vector<AST::ExprNodeP>   nodes;
                    
                    for( unsigned int idx = 0; idx < e.m_sub_types.size(); idx ++ )
                    {
                        auto name_a = FMT("a" << idx);
                        pats_a.push_back( ::AST::Pattern(::AST::Pattern::TagBind(), name_a, ::AST::PatternBinding::Type::REF) );
                        nodes.push_back( this->clone_val_direct(core_name, NEWNODE(NamedValue, AST::Path(name_a))) );
                    }
                    
                    pat_a = AST::Pattern(AST::Pattern::TagNamedTuple(), base_path + v.m_name, mv$(pats_a));
                    code = NEWNODE(CallPath, base_path + v.m_name, mv$(nodes));
                }
                ),
            (Struct,
                ::std::vector< ::std::pair<std::string, AST::Pattern> > pats_a;
                ::std::vector< ::std::pair<std::string, AST::ExprNodeP> >   vals;
                
                for( const auto& fld : e.m_fields )
                {
                    auto name_a = FMT("a" << fld.m_name);
                    pats_a.push_back( ::std::make_pair(fld.m_name, ::AST::Pattern(::AST::Pattern::TagBind(), name_a, ::AST::PatternBinding::Type::REF)) );
                    vals.push_back( ::std::make_pair( fld.m_name, this->clone_val_direct(core_name, NEWNODE(NamedValue, AST::Path(name_a))) ) );
                }
                
                pat_a = AST::Pattern(AST::Pattern::TagStruct(), base_path + v.m_name, mv$(pats_a), true);
                code = NEWNODE(StructLiteral, base_path + v.m_name, nullptr, mv$(vals));
                )
            )
            
            ::std::vector< AST::Pattern>    pats;
            pats.push_back( AST::Pattern(AST::Pattern::TagReference(), false, mv$(pat_a)) );
            
            arms.push_back(AST::ExprNode_Match_Arm(
                mv$(pats),
                nullptr,
                mv$(code)
                ));
        }

        return this->make_ret(sp, core_name, p, type, this->get_field_bounds(enm), NEWNODE(Match,
            NEWNODE(NamedValue, AST::Path("self")),
            mv$(arms)
            ));
    }
} g_derive_clone;

class Deriver_Copy:
    public Deriver
{
    AST::Path get_trait_path(const ::std::string& core_name) const {
        return AST::Path(core_name, { AST::PathNode("marker", {}), AST::PathNode("Copy", {}) });
    }
    
    AST::Impl make_ret(Span sp, const ::std::string& core_name, const AST::GenericParams& p, const TypeRef& type, ::std::vector<TypeRef> types_to_bound, AST::ExprNodeP node) const
    {
        const AST::Path    trait_path = this->get_trait_path(core_name);
        
        AST::GenericParams params = get_params_with_bounds(p, trait_path, mv$(types_to_bound));
        
        AST::Impl   rv( AST::ImplDef( sp, AST::MetaItems(), mv$(params), make_spanned(sp, trait_path), type ) );
        return mv$(rv);
    }
    
public:
    AST::Impl handle_item(Span sp, const ::std::string& core_name, const AST::GenericParams& p, const TypeRef& type, const AST::Struct& str) const override
    {
        return this->make_ret(sp, core_name, p, type, this->get_field_bounds(str), nullptr);
    }
    
    AST::Impl handle_item(Span sp, const ::std::string& core_name, const AST::GenericParams& p, const TypeRef& type, const AST::Enum& enm) const override
    {
        return this->make_ret(sp, core_name, p, type, this->get_field_bounds(enm), nullptr);
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
static void derive_item(const Span& sp, const AST::Crate& crate, AST::Module& mod, const AST::MetaItem& attr, const AST::Path& path, const T& item)
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
        
        mod.add_impl( dp->handle_item(sp, (crate.m_load_std == ::AST::Crate::LOAD_NONE ? "" : "core"), params, type, item) );
    }
    
    if( fail ) {
        //ERROR(sp, E0000, "Failed to apply #[derive] - Missing handlers for " << missing_handlers);
    }
}

class Decorator_Derive:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::Post; }
    void handle(const Span& sp, const AST::MetaItem& attr, ::AST::Crate& crate, const AST::Path& path, AST::Module& mod, AST::Item& i) const override
    {
        TU_MATCH_DEF(::AST::Item, (i), (e),
        (
            TODO(sp, "Handle #[derive] for other item types - " << i.tag_str());
            ),
        (None,
            //
            ),
        (Enum,
            derive_item(sp, crate, mod, attr, path, e);
            ),
        (Struct,
            derive_item(sp, crate, mod, attr, path, e);
            )
        )
    }
};

STATIC_DECORATOR("derive", Decorator_Derive)

