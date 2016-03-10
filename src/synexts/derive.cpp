
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
    virtual AST::Impl handle_item(const AST::GenericParams& params, const TypeRef& type, const AST::Struct& str) const = 0;
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
    
public:
    AST::Impl handle_item(const AST::GenericParams& p, const TypeRef& type, const AST::Struct& str) const override
    {
        // TODO: be correct herhe and use "core" as the crate name
        // - Requires handling the crate_name crate attribute correctly
        const AST::Path    debug_trait("", { AST::PathNode("fmt", {}), AST::PathNode("Debug", {}) });
        const TypeRef  ret_type(AST::Path("", {AST::PathNode("fmt",{}), AST::PathNode("Result",{})}) );
        const TypeRef  f_type(TypeRef::TagReference(), true,
            TypeRef(AST::Path("", {AST::PathNode("fmt",{}), AST::PathNode("Formatter", {})}))
            );
        const ::std::string& name = type.path().nodes().back().name();
        
        // Generate code for Debug
        AST::ExprNodeP  node;
        node = NEWNODE(AST::ExprNode_NamedValue, AST::Path("f"));
        node = NEWNODE(AST::ExprNode_CallMethod,
            mv$(node), AST::PathNode("debug_struct",{}),
            vec$( NEWNODE(AST::ExprNode_String, name) )
            );
        for( const auto& fld : str.fields() )
        {
            node = NEWNODE(AST::ExprNode_CallMethod,
                mv$(node), AST::PathNode("field",{}),
                vec$(
                    NEWNODE(AST::ExprNode_String, fld.name),
                    NEWNODE(AST::ExprNode_UniOp, AST::ExprNode_UniOp::REF,
                        NEWNODE(AST::ExprNode_Field,
                            NEWNODE(AST::ExprNode_NamedValue, AST::Path("self")),
                            fld.name
                            )
                        )
                )
                );
        }
        node = NEWNODE(AST::ExprNode_CallMethod, mv$(node), AST::PathNode("finish",{}), {});
        
        DEBUG("node = " << *node);
        
        AST::Function fcn(
            AST::GenericParams(),
            ret_type,
            vec$(
                ::std::make_pair( AST::Pattern(AST::Pattern::TagBind(), "self"), TypeRef(TypeRef::TagReference(), false, TypeRef("Self")) ),
                ::std::make_pair( AST::Pattern(AST::Pattern::TagBind(), "f"), f_type )
                )
            );
        fcn.set_code( NEWNODE(AST::ExprNode_Block, vec$(mv$(node)), ::std::unique_ptr<AST::Module>()) );
        
        AST::Impl   rv( AST::MetaItems(), p, type, debug_trait );
        rv.add_function(false, "fmt", mv$(fcn));
        return mv$(rv);
    }
} g_derive_debug;


// --------------------------------------------------------------------
// Select and dispatch the correct derive() handler
// --------------------------------------------------------------------
static const Deriver* find_impl(const ::std::string& trait_name)
{
    if( trait_name == "Debug" )
        return &g_derive_debug;
    else
        return nullptr;
}

template<typename T>
static void derive_item(AST::Module& mod, const AST::MetaItem& attr, const AST::Path& path, const T& item)
{
    if( !attr.has_sub_items() ) {
        //throw CompileError::Generic("#[derive()] requires a list of known traits to derive");
        return ;
    }
    
    DEBUG("path = " << path);
    bool    fail = false;
    
    const auto& params = item.params();
    TypeRef type(path);
    for( const auto& param : params.ty_params() )
        type.path().nodes().back().args().push_back( TypeRef(TypeRef::TagArg(), param.name()) );
    
    for( const auto& trait : attr.items() )
    {
        DEBUG("- " << trait.name());
        auto dp = find_impl(trait.name());
        if( !dp ) {
            DEBUG("> No handler for " << trait.name());
            fail = true;
            continue ;
        }
        
        mod.add_impl( dp->handle_item(params, type, item) );
    }
    
    if( fail ) {
        //throw CompileError::Generic("Failed to #[dervie]");
    }
}

class Decorator_Derive:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::LatePost; }
    void handle(const AST::MetaItem& attr, AST::Crate& crate, const AST::Path& path, AST::Module& mod, AST::Item& i) const override
    {
        TU_MATCH_DEF(::AST::Item, (i), (e),
        (
            ),
        (Struct,
            derive_item(mod, attr, path, e.e);
            )
        )
    }
};

STATIC_DECORATOR("derive", Decorator_Derive)

