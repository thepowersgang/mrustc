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
        Top,
        Method,
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
        unsigned int level;
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
    unsigned int m_var_count;
    unsigned int m_block_level;
    
    Context(const ::AST::Crate& crate, const ::AST::Module& mod):
        m_crate(crate),
        m_mod(mod),
        m_var_count(0),
        m_block_level(0)
    {}
    
    void push(const ::AST::GenericParams& params, GenericSlot::Level level) {
        auto   e = Ent::make_Generic({});
        auto& data = e.as_Generic();
        
        if( params.ty_params().size() > 0 ) {
            const auto& typs = params.ty_params();
            for(unsigned int i = 0; i < typs.size(); i ++ ) {
                data.types.push_back( Named<GenericSlot> { typs[i].name(), GenericSlot { level, static_cast<unsigned short>(i) } } );
            }
        }
        if( params.lft_params().size() > 0 ) {
            TODO(Span(), "resolve/absolute.cpp - Context::push(GenericParams) - " << params);
        }
        
        m_name_context.push_back(mv$(e));
    }
    void pop(const ::AST::GenericParams& ) {
        if( !m_name_context.back().is_Generic() )
            BUG(Span(), "resolve/absolute.cpp - Context::pop(GenericParams) - Mismatched pop");
        m_name_context.pop_back();
    }
    void push(const ::AST::Module& mod) {
        m_name_context.push_back( Ent::make_Module({ &mod }) );
    }
    void pop(const ::AST::Module& mod) {
        if( !m_name_context.back().is_Module() )
            BUG(Span(), "resolve/absolute.cpp - Context::pop(GenericParams) - Mismatched pop");
        m_name_context.pop_back();
    }
    
    void push_block() {
        m_block_level += 1;
    }
    void push_var(const ::std::string& name) {
        assert( m_block_level > 0 );
        if( !m_name_context.back().is_VarBlock() ) {
            m_name_context.push_back( Ent::make_VarBlock({ m_block_level, {} }) );
        }
        m_name_context.back().as_VarBlock().variables.push_back( Named<unsigned int> { name, m_var_count } );
        m_var_count += 1;
    }
    void pop_block() {
        assert( m_block_level > 0 );
        if( m_name_context.back().is_VarBlock() ) {
            if( m_name_context.back().as_VarBlock().level == m_block_level ) {
                m_name_context.pop_back();
            }
        }
        m_block_level -= 1;
    }
   

    bool lookup_in_mod(const ::AST::Module& mod, const ::std::string& name, bool is_type,  ::AST::Path& path) const {
        // TODO: m_type_items/m_value_items should store the path
        if( is_type ) {
            auto v = mod.m_type_items.find(name);
            if( v != mod.m_type_items.end() ) {
                path = mod.path() + name;
                return true;
            }
        }
        else {
            auto v = mod.m_value_items.find(name);
            if( v != mod.m_value_items.end() ) {
                path = mod.path() + name;
                return true;
            }
        }
        return false;
    }
    
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
                ::AST::Path rv;
                if( this->lookup_in_mod(*e.mod, name, is_type,  rv) ) {
                    return rv;
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
        ::AST::Path rv;
        if( this->lookup_in_mod(m_mod, name, is_type,  rv) ) {
            return rv;
        }
        
        ERROR(sp, E0000, "Couldn't find name '" << name << "'");
    }
};



void Resolve_Absolute_Path(const Context& context, const Span& sp, bool is_type,  ::AST::Path& path);
void Resolve_Absolute_Type(const Context& context,  TypeRef& type);
void Resolve_Absolute_Expr(const Context& context,  ::AST::Expr& expr);
void Resolve_Absolute_Expr(const Context& context,  ::AST::ExprNode& node);
void Resolve_Absolute_Pattern(Context& context, bool allow_refutable, ::AST::Pattern& pat);
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
        // - Determine how many components of the `self` path to use
        const auto& mp_nodes = context.m_mod.path().nodes();
        assert( e.count >= 1 );
        unsigned int start_len = e.count > mp_nodes.size() ? 0 : mp_nodes.size() - e.count;
        
        // - Create a new path
        ::AST::Path np("", {});
        auto& np_nodes = np.nodes();
        np_nodes.reserve( start_len + e.nodes.size() );
        for(unsigned int i = 0; i < start_len; i ++ )
            np_nodes.push_back( mp_nodes[i] );
        for(auto& en : e.nodes)
            np_nodes.push_back( mv$(en) );
        
        // TODO: Resolve to the actual item?
        
        path = mv$(np);
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

void Resolve_Absolute_Generic(const Context& context, ::AST::GenericParams& params)
{
    for( auto& bound : params.bounds() )
    {
        TODO(Span(), "Resolve_Absolute_Generic - " << bound);
    }
}

void Resolve_Absolute_Pattern(Context& context, bool allow_refutable,  ::AST::Pattern& pat)
{
    if( pat.binding() != "" ) {
        if( !pat.data().is_Any() && ! allow_refutable )
            TODO(Span(), "Resolve_Absolute_Pattern - Encountered bound destructuring pattern");
        context.push_var( pat.binding() );
    }

    TU_MATCH( ::AST::Pattern::Data, (pat.data()), (e),
    (MaybeBind,
        BUG(Span(), "Resolve_Absolute_Pattern - Encountered MaybeBind");
        ),
    (Macro,
        BUG(Span(), "Resolve_Absolute_Pattern - Encountered Macro");
        ),
    (Any,
        // Ignore '_'
        ),
    (Box,
        Resolve_Absolute_Pattern(context, allow_refutable,  *e.sub);
        ),
    (Ref,
        Resolve_Absolute_Pattern(context, allow_refutable,  *e.sub);
        ),
    (Value,
        if( ! allow_refutable )
            BUG(Span(), "Resolve_Absolute_Pattern - Enountered refutable pattern where only irrefutable allowed");
        // TODO: Value patterns should be resolved with a different context (disallowing locals)
        Resolve_Absolute_Expr(context, *e.start);
        if( e.end )
            Resolve_Absolute_Expr(context, *e.end);
        ),
    (Tuple,
        for(auto& sp : e.sub_patterns)
            Resolve_Absolute_Pattern(context, allow_refutable,  sp);
        ),
    (StructTuple,
        Resolve_Absolute_Path(context, Span(), true, e.path);
        for(auto& sp : e.sub_patterns)
            Resolve_Absolute_Pattern(context, allow_refutable,  sp);
        ),
    (Struct,
        Resolve_Absolute_Path(context, Span(), true, e.path);
        for(auto& sp : e.sub_patterns)
            Resolve_Absolute_Pattern(context, allow_refutable,  sp.second);
        ),
    (Slice,
        if( !allow_refutable )
            BUG(Span(), "Resolve_Absolute_Pattern - Enountered refutable pattern where only irrefutable allowed");
        for(auto& sp : e.leading)
            Resolve_Absolute_Pattern(context, allow_refutable,  sp);
        if( e.extra_bind != "" && e.extra_bind != "_" ) {
            context.push_var( e.extra_bind );
        }
        for(auto& sp : e.trailing)
            Resolve_Absolute_Pattern(context, allow_refutable,  sp);
        )
    )
}

void Resolve_Absolute_Mod(const ::AST::Crate& crate, ::AST::Module& mod)
{
    TRACE_FUNCTION_F("(mod="<<mod.path()<<")");
    for( auto& i : mod.items() )
    {
        Context item_context { crate, mod };
        
        TU_MATCH(AST::Item, (i.data), (e),
        (None,
            ),
        (Module,
            DEBUG("Module - " << i.name);
            Resolve_Absolute_Mod(crate, e);
            ),
        (Crate,
            // - Nothing
            ),
        (Enum,
            DEBUG("Enum - " << i.name);
            item_context.push( e.params(), GenericSlot::Level::Top );
            Resolve_Absolute_Generic(item_context,  e.params());
            TODO(Span(), "Resolve_Absolute_Mod - Enum");
            item_context.pop( e.params() );
            ),
        (Trait,
            DEBUG("Trait - " << i.name);
            TODO(Span(), "Resolve_Absolute_Mod - Trait");
            ),
        (Type,
            DEBUG("Type - " << i.name);
            TODO(Span(), "Resolve_Absolute_Mod - Type");
            ),
        (Struct,
            DEBUG("Struct - " << i.name);
            item_context.push( e.params(), GenericSlot::Level::Top );
            Resolve_Absolute_Generic(item_context,  e.params());
            
            TU_MATCH(::AST::StructData, (e.m_data), (s),
            (Tuple,
                for(auto& field : s.ents) {
                    Resolve_Absolute_Type(item_context,  field.m_type);
                }
                ),
            (Struct,
                for(auto& field : s.ents) {
                    Resolve_Absolute_Type(item_context,  field.m_type);
                }
                )
            )
            
            item_context.pop( e.params() );
            ),
        (Function,
            DEBUG("Function - " << i.name);
            item_context.push( e.params(), GenericSlot::Level::Top );
            
            Resolve_Absolute_Type( item_context, e.rettype() );
            for(auto& arg : e.args())
                Resolve_Absolute_Type( item_context, arg.second );
            
            item_context.push_block();
            for(auto& arg : e.args()) {
                Resolve_Absolute_Pattern( item_context, false, arg.first );
            }
            
            Resolve_Absolute_Expr( item_context, e.code() );
            
            item_context.pop_block();
            
            item_context.pop( e.params() );
            ),
        (Static,
            DEBUG("Static - " << i.name);
            Resolve_Absolute_Type( item_context, e.type() );
            Resolve_Absolute_Expr( item_context, e.value() );
            )
        )
    }
}

void Resolve_Absolutise(AST::Crate& crate)
{
    Resolve_Absolute_Mod(crate, crate.root_module());
}


