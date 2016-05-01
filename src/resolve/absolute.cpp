/*
 * Convert all paths in AST into absolute form (or to the relevant local item)
 * - NOTE: This is the core of the 'resolve' pass.
 *
 * After complete there should be no:
 * - Relative/super/self paths
 * - MaybeBind patterns
 */
#include <ast/crate.hpp>
#include <ast/ast.hpp>
#include <main_bindings.hpp>

struct GenericSlot
{
    enum class Level
    {
        Impl,
        Function,
    } level;
    unsigned short  index;
};
template<typename Val>
struct Named
{
    ::std::string   name;
    Val value;
};

struct Context
{
    TAGGED_UNION(Ent, Module,
    (Module, struct {
        const ::AST::Module* mod;
        }),
    (VarBlock, struct {
        // "Map" of names to function-level variable slots
        ::std::vector< Named< unsigned int > > variables;
        }),
    (Generic, struct {
        // Map of names to slots
        ::std::vector< Named< GenericSlot > > types;
        ::std::vector< Named< GenericSlot > > constants;
        ::std::vector< Named< GenericSlot > > lifetimes;
        })
    );

    const ::AST::Crate&     m_crate;
    const ::AST::Module&    m_mod;
    ::std::vector<Ent>  m_name_context;
    
    Context(const ::AST::Crate& crate, const::AST::Module& mod):
        m_crate(crate),
        m_mod(mod)
    {}
    
    AST::Path lookup_type(const Span& sp, const ::std::string& name) const {
        return this->lookup(sp, name, true);
    }
    AST::Path lookup_value(const Span& sp, const ::std::string& name) const {
        return this->lookup(sp, name, false);
    }
    AST::Path lookup(const Span& sp, const ::std::string& name, bool is_type) const {
        for(auto it = m_name_context.rbegin(); it != m_name_context.rend(); ++ it)
        {
            TU_MATCH(Ent, (*it), (e),
            (Module,
                // TODO: m_type_items/m_value_items should store the path
                if( is_type ) {
                    auto v = e.mod->m_type_items.find(name);
                    if( v != e.mod->m_type_items.end() ) {
                        return e.mod->path() + name;
                    }
                }
                else {
                    auto v = e.mod->m_value_items.find(name);
                    if( v != e.mod->m_value_items.end() ) {
                        return e.mod->path() + name;
                    }
                }
                ),
            (VarBlock,
                if( is_type ) {
                    // ignore
                }
                else {
                    TODO(sp, "resolve/absolute.cpp - Context::lookup - Handle VarBlock");
                }
                ),
            (Generic,
                if( is_type ) {
                    TODO(sp, "resolve/absolute.cpp - Context::lookup - Handle Generic");
                }
                else {
                    // ignore
                }
                )
            )
        }
        
        // Top-level module
        if( is_type ) {
            auto v = m_mod.m_type_items.find(name);
            if( v != m_mod.m_type_items.end() ) {
                return m_mod.path() + name;
            }
        }
        else {
            auto v = m_mod.m_value_items.find(name);
            if( v != m_mod.m_value_items.end() ) {
                return m_mod.path() + name;
            }
        }
        
        ERROR(sp, E0000, "Couldn't find name '" << name << "'");
    }
};



void Resolve_Absolute_Path(const Context& context, const Span& sp, bool is_type,  ::AST::Path& path);
void Resolve_Absolute_Type(const Context& context,  TypeRef& type);
void Resolve_Absolute_Expr(const Context& context,  ::AST::Expr& expr);
void Resolve_Absolute_Expr(const Context& context,  ::AST::ExprNode& node);
void Resolve_Absolute_Mod(const ::AST::Crate& crate, ::AST::Module& mod);


void Resolve_Absolute_Path(const Context& context, const Span& sp, bool is_type,  ::AST::Path& path)
{
    TU_MATCH(::AST::Path::Class, (path.m_class), (e),
    (Invalid,
        BUG(sp, "Encountered invalid path");
        ),
    (Local,
        // Nothing to do (TODO: Check that it's valid?)
        ),
    (Relative,
        if(e.nodes.size() == 0)
            BUG(sp, "Resolve_Absolute_Path - Relative path with no nodes");
        if(e.nodes.size() > 1 || is_type) {
            // Look up type
            auto p = context.lookup_type(sp, e.nodes[0].name());
            DEBUG("Found - " << p);
            path = mv$(p);
        }
        else {
            // Look up value
            auto p = context.lookup_value(sp, e.nodes[0].name());
            DEBUG("Found val - " << p);
            path = mv$(p);
        }
        ),
    (Self,
        TODO(sp, "Resolve_Absolute_Path - Self-relative paths - " << path);
        ),
    (Super,
        TODO(sp, "Resolve_Absolute_Path - Super-relative paths - " << path);
        ),
    (Absolute,
        // Nothing to do (TODO: Bind?)
        ),
    (UFCS,
        TODO(sp, "Resolve_Absolute_Path - UFCS");
        )
    )
}

void Resolve_Absolute_Type(const Context& context,  TypeRef& type)
{
    const auto& sp = type.span();
    TU_MATCH(TypeData, (type.m_data), (e),
    (None,
        // ! type
        ),
    (Any,
        // _ type
        ),
    (Unit,
        ),
    (Macro,
        BUG(sp, "Resolve_Absolute_Type - Encountered an unexpanded macro");
        ),
    (Primitive,
        ),
    (Function,
        TODO(sp, "Resolve_Absolute_Type - Function - " << type);
        ),
    (Tuple,
        for(auto& t : e.inner_types)
            Resolve_Absolute_Type(context,  t);
        ),
    (Borrow,
        Resolve_Absolute_Type(context,  *e.inner);
        ),
    (Pointer,
        Resolve_Absolute_Type(context,  *e.inner);
        ),
    (Array,
        Resolve_Absolute_Type(context,  *e.inner);
        Resolve_Absolute_Expr(context,  *e.size);
        ),
    (Generic,
        TODO(sp, "Resolve_Absolute_Type - Encountered generic");
        ),
    (Path,
        TODO(sp, "Resolve_Absolute_Type - Path");
        ),
    (TraitObject,
        TODO(sp, "Resolve_Absolute_Type - TraitObject");
        )
    )
}

void Resolve_Absolute_Expr(const Context& context,  ::AST::Expr& expr)
{
    if( expr.is_valid() )
    {
        Resolve_Absolute_Expr(context, expr.node());
    }
}
void Resolve_Absolute_Expr(const Context& context,  ::AST::ExprNode& node)
{
    TRACE_FUNCTION_F("");
    struct NV:
        public AST::NodeVisitorDef
    {
        const Context& context;
        
        NV(const Context& context):
            context(context)
        {
        }
        
        void visit(AST::ExprNode_Block& node) override {
            if( node.m_local_mod ) {
                Resolve_Absolute_Mod(this->context.m_crate, *node.m_local_mod);
                
                //this->context.push( *node.m_local_mod );
            }
            AST::NodeVisitorDef::visit(node);
            if( node.m_local_mod ) {
                //this->context.pop();
            }
        }
        
        void visit(AST::ExprNode_Match& node) override {
            TODO(node.get_pos(), "Resolve_Absolute_Expr - ExprNode_Match");
        }
        
        void visit(AST::ExprNode_LetBinding& node) override {
            TODO(Span(), "Resolve_Absolute_Expr - ExprNode_LetBinding");
        }
        void visit(AST::ExprNode_StructLiteral& node) override {
            TODO(Span(), "Resolve_Absolute_Expr - ExprNode_StructLiteral");
        }
        void visit(AST::ExprNode_CallPath& node) override {
            TODO(Span(), "Resolve_Absolute_Expr - ExprNode_CallPath");
        }
        void visit(AST::ExprNode_NamedValue& node) override {
            Resolve_Absolute_Path(this->context, Span(node.get_pos()), false,  node.m_path);
        }
        void visit(AST::ExprNode_Cast& node) override {
            Resolve_Absolute_Type(this->context,  node.m_type);
            AST::NodeVisitorDef::visit(node);
        }
    } expr_iter(context);

    node.visit( expr_iter );
}

void Resolve_Absolute_Mod(const ::AST::Crate& crate, ::AST::Module& mod)
{
    TRACE_FUNCTION_F("(mod="<<mod.path()<<")");
    for( auto& i : mod.items() )
    {
        TU_MATCH(AST::Item, (i.data), (e),
        (None,
            ),
        (Module,
            Resolve_Absolute_Mod(crate, e);
            ),
        (Crate,
            // - Nothing
            ),
        (Enum,
            TODO(Span(), "Resolve_Absolute_Mod - Enum");
            ),
        (Trait,
            TODO(Span(), "Resolve_Absolute_Mod - Trait");
            ),
        (Type,
            TODO(Span(), "Resolve_Absolute_Mod - Type");
            ),
        (Struct,
            TODO(Span(), "Resolve_Absolute_Mod - Struct");
            ),
        (Function,
            TODO(Span(), "Resolve_Absolute_Mod - Function");
            ),
        (Static,
            Resolve_Absolute_Type( Context(crate, mod), e.type() );
            Resolve_Absolute_Expr( Context(crate, mod), e.value() );
            )
        )
    }
}

void Resolve_Absolutise(AST::Crate& crate)
{
    Resolve_Absolute_Mod(crate, crate.root_module());
}


