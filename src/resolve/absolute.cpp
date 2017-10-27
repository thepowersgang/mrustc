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
#include <ast/expr.hpp>
#include <main_bindings.hpp>
#include <hir/hir.hpp>

namespace
{
    struct GenericSlot
    {
        enum class Level
        {
            Top,
            Method,
        } level;
        unsigned short  index;

        unsigned int to_binding() const {
            if(level == Level::Method && index != 0xFFFF) {
                return (unsigned int)index + 256;
            }
            else {
                return (unsigned int)index;
            }
        }
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
        (ConcreteSelf, const TypeRef* ),
        (VarBlock, struct {
            unsigned int level;
            // "Map" of names to function-level variable slots
            ::std::vector< ::std::pair<Ident, unsigned int> > variables;
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
        bool m_frozen_bind_set;

        Context(const ::AST::Crate& crate, const ::AST::Module& mod):
            m_crate(crate),
            m_mod(mod),
            m_var_count(~0u),
            m_block_level(0),
            m_frozen_bind_set( false )
        {}

        void push(const ::AST::GenericParams& params, GenericSlot::Level level, bool has_self=false) {
            auto   e = Ent::make_Generic({});
            auto& data = e.as_Generic();

            if( has_self ) {
                //assert( level == GenericSlot::Level::Top );
                data.types.push_back( Named<GenericSlot> { "Self", GenericSlot { level, 0xFFFF } } );
                m_name_context.push_back( Ent::make_ConcreteSelf(nullptr) );
            }
            if( params.ty_params().size() > 0 ) {
                const auto& typs = params.ty_params();
                for(unsigned int i = 0; i < typs.size(); i ++ ) {
                    data.types.push_back( Named<GenericSlot> { typs[i].name(), GenericSlot { level, static_cast<unsigned short>(i) } } );
                }
            }
            if( params.lft_params().size() > 0 ) {
                //TODO(Span(), "resolve/absolute.cpp - Context::push(GenericParams) - Lifetime params - " << params);
            }

            m_name_context.push_back(mv$(e));
        }
        void pop(const ::AST::GenericParams& , bool has_self=false) {
            if( !m_name_context.back().is_Generic() )
                BUG(Span(), "resolve/absolute.cpp - Context::pop(GenericParams) - Mismatched pop");
            m_name_context.pop_back();
            if(has_self) {
                if( !m_name_context.back().is_ConcreteSelf() )
                    BUG(Span(), "resolve/absolute.cpp - Context::pop(GenericParams) - Mismatched pop");
                m_name_context.pop_back();
            }
        }
        void push(const ::AST::Module& mod) {
            m_name_context.push_back( Ent::make_Module({ &mod }) );
        }
        void pop(const ::AST::Module& mod) {
            if( !m_name_context.back().is_Module() )
                BUG(Span(), "resolve/absolute.cpp - Context::pop(GenericParams) - Mismatched pop");
            m_name_context.pop_back();
        }

        class RootBlockScope {
            friend struct Context;
            Context& ctxt;
            unsigned int old_varcount;
            RootBlockScope(Context& ctxt, unsigned int val):
                ctxt(ctxt),
                old_varcount(ctxt.m_var_count)
            {
                ctxt.m_var_count = val;
            }
        public:
            ~RootBlockScope() {
                ctxt.m_var_count = old_varcount;
            }
        };
        RootBlockScope enter_rootblock() {
            return RootBlockScope(*this, 0);
        }
        RootBlockScope clear_rootblock() {
            return RootBlockScope(*this, ~0u);
        }

        void push_self(const TypeRef& tr) {
            m_name_context.push_back( Ent::make_ConcreteSelf(&tr) );
        }
        void pop_self(const TypeRef& tr) {
            TU_IFLET(Ent, m_name_context.back(), ConcreteSelf, e,
                m_name_context.pop_back();
            )
            else {
                BUG(Span(), "resolve/absolute.cpp - Context::pop(TypeRef) - Mismatched pop");
            }
        }
        ::TypeRef get_self() const {
            for(auto it = m_name_context.rbegin(); it != m_name_context.rend(); ++ it)
            {
                TU_MATCH_DEF(Ent, (*it), (e),
                (
                    ),
                (ConcreteSelf,
                    if( e ) {
                        return e->clone();
                    }
                    else {
                        return ::TypeRef(Span(), "Self", 0xFFFF);
                    }
                    )
                )
            }

            TODO(Span(), "Error when get_self called with no self");
        }

        void push_block() {
            m_block_level += 1;
            DEBUG("Push block to " << m_block_level);
        }
        unsigned int push_var(const Span& sp, const Ident& name) {
            if( m_var_count == ~0u ) {
                BUG(sp, "Assigning local when there's no variable context");
            }
            // TODO: Handle avoiding duplicate bindings in a pattern
            if( m_frozen_bind_set )
            {
                if( !m_name_context.back().is_VarBlock() ) {
                    BUG(sp, "resolve/absolute.cpp - Context::push_var - No block");
                }
                auto& vb = m_name_context.back().as_VarBlock();
                for( const auto& v : vb.variables )
                {
                    // TODO: Error when a binding is used twice or not at all
                    if( v.first == name ) {
                        return v.second;
                    }
                }
                ERROR(sp, E0000, "Mismatched bindings in pattern");
            }
            else
            {
                assert( m_block_level > 0 );
                if( !m_name_context.back().is_VarBlock() || m_name_context.back().as_VarBlock().level < m_block_level ) {
                    m_name_context.push_back( Ent::make_VarBlock({ m_block_level, {} }) );
                }
                DEBUG("New var @ " << m_block_level << ": #" << m_var_count << " " << name);
                auto& vb = m_name_context.back().as_VarBlock();
                assert(vb.level == m_block_level);
                vb.variables.push_back( ::std::make_pair(mv$(name), m_var_count) );
                m_var_count += 1;
                assert( m_var_count >= vb.variables.size() );
                return m_var_count - 1;
            }
        }
        void pop_block() {
            assert( m_block_level > 0 );
            if( m_name_context.size() > 0 && m_name_context.back().is_VarBlock() && m_name_context.back().as_VarBlock().level == m_block_level ) {
                DEBUG("Pop block from " << m_block_level << " with vars:" << FMT_CB(os,
                    for(const auto& v : m_name_context.back().as_VarBlock().variables)
                        os << " " << v.first << "#" << v.second;
                    ));
                m_name_context.pop_back();
            }
            else {
                DEBUG("Pop block from " << m_block_level << " - no vars");
                for(const auto& ent : ::reverse(m_name_context)) {
                    TU_IFLET(Ent, ent, VarBlock, e,
                        //DEBUG("Block @" << e.level << ": " << e.variables.size() << " vars");
                        assert( e.level < m_block_level );
                    )
                }
            }
            m_block_level -= 1;
        }

        /// Indicate that a multiple-pattern binding is started
        void start_patbind() {
            assert( m_block_level > 0 );
            m_name_context.push_back( Ent::make_VarBlock({ m_block_level, {} }) );
            assert( m_frozen_bind_set == false );
        }
        /// Freeze the set of pattern bindings
        void freeze_patbind() {
            m_frozen_bind_set = true;
        }
        /// End a multiple-pattern binding state (unfreeze really)
        void end_patbind() {
            m_frozen_bind_set = false;
        }


        enum class LookupMode {
            Namespace,
            Type,
            Constant,
            Pattern,
            PatternValue,
            Variable,
        };
        static const char* lookup_mode_msg(LookupMode mode) {
            switch(mode)
            {
            case LookupMode::Namespace: return "path component";
            case LookupMode::Type:      return "type name";
            case LookupMode::Pattern:   return "pattern name";
            case LookupMode::PatternValue: return "pattern constant";
            case LookupMode::Constant:  return "constant name";
            case LookupMode::Variable:  return "variable name";
            }
            return "";
        }
        AST::Path lookup(const Span& sp, const ::std::string& name, const Ident::Hygiene& src_context, LookupMode mode) const {
            auto rv = this->lookup_opt(name, src_context, mode);
            if( !rv.is_valid() ) {
                switch(mode)
                {
                case LookupMode::Namespace: ERROR(sp, E0000, "Couldn't find path component '" << name << "'");
                case LookupMode::Type:      ERROR(sp, E0000, "Couldn't find type name '" << name << "'");
                case LookupMode::Pattern:   ERROR(sp, E0000, "Couldn't find pattern name '" << name << "'");
                case LookupMode::PatternValue:   ERROR(sp, E0000, "Couldn't find pattern value '" << name << "'");
                case LookupMode::Constant:  ERROR(sp, E0000, "Couldn't find constant name '" << name << "'");
                case LookupMode::Variable:  ERROR(sp, E0000, "Couldn't find variable name '" << name << "'");
                }
            }
            return rv;
        }
        static bool lookup_in_mod(const ::AST::Module& mod, const ::std::string& name, LookupMode mode,  ::AST::Path& path) {
            switch(mode)
            {
            case LookupMode::Namespace:
                {
                    auto v = mod.m_namespace_items.find(name);
                    if( v != mod.m_namespace_items.end() ) {
                        path = ::AST::Path( v->second.path );
                        return true;
                    }
                }
                {
                    auto v = mod.m_type_items.find(name);
                    if( v != mod.m_type_items.end() ) {
                        path = ::AST::Path( v->second.path );
                        return true;
                    }
                }
                break;

            case LookupMode::Type:
                //if( name == "IntoIterator" ) {
                //    DEBUG("lookup_in_mod(mod="<<mod.path()<<")");
                //    for(const auto& v : mod.m_type_items) {
                //        DEBUG("- " << v.first << " = " << (v.second.is_pub ? "pub " : "") << v.second.path);
                //    }
                //}
                {
                    auto v = mod.m_type_items.find(name);
                    if( v != mod.m_type_items.end() ) {
                        path = ::AST::Path( v->second.path );
                        return true;
                    }
                }
                break;
            case LookupMode::Pattern:
                {
                    auto v = mod.m_type_items.find(name);
                    if( v != mod.m_type_items.end() ) {
                        const auto& b = v->second.path.binding();
                        switch( b.tag() )
                        {
                        case ::AST::PathBinding::TAG_Struct:
                            path = ::AST::Path( v->second.path );
                            return true;
                        default:
                            break;
                        }
                    }
                }
            case LookupMode::PatternValue:
                {
                    auto v = mod.m_value_items.find(name);
                    if( v != mod.m_value_items.end() ) {
                        const auto& b = v->second.path.binding();
                        switch( b.tag() )
                        {
                        case ::AST::PathBinding::TAG_EnumVar:
                        case ::AST::PathBinding::TAG_Static:
                            path = ::AST::Path( v->second.path );
                            return true;
                        case ::AST::PathBinding::TAG_Struct:
                            // TODO: Restrict this to unit-like structs
                            if( b.as_Struct().struct_ && !b.as_Struct().struct_->m_data.is_Unit() )
                                ;
                            else if( b.as_Struct().hir && !b.as_Struct().hir->m_data.is_Unit() )
                                ;
                            else
                            {
                                path = ::AST::Path( v->second.path );
                                return true;
                            }
                            break;
                        default:
                            break;
                        }
                    }
                }
                break;
            case LookupMode::Constant:
            case LookupMode::Variable:
                {
                    auto v = mod.m_value_items.find(name);
                    if( v != mod.m_value_items.end() ) {
                        path = ::AST::Path( v->second.path );
                        return true;
                    }
                }
                break;
            }
            return false;
        }
        AST::Path lookup_opt(const ::std::string& name, const Ident::Hygiene& src_context, LookupMode mode) const {
            DEBUG("name=" << name <<", src_context=" << src_context);
            for(auto it = m_name_context.rbegin(); it != m_name_context.rend(); ++ it)
            {
                TU_MATCH(Ent, (*it), (e),
                (Module,
                    DEBUG("- Module");
                    ::AST::Path rv;
                    if( this->lookup_in_mod(*e.mod, name, mode,  rv) ) {
                        return rv;
                    }
                    ),
                (ConcreteSelf,
                    DEBUG("- ConcreteSelf");
                    if( ( mode == LookupMode::Type || mode == LookupMode::Namespace ) && name == "Self" ) {
                        return ::AST::Path( ::AST::Path::TagUfcs(), e->clone(), ::AST::Path(), ::std::vector< ::AST::PathNode>() );
                    }
                    ),
                (VarBlock,
                    DEBUG("- VarBlock");
                    assert(e.level <= m_block_level);
                    if( mode != LookupMode::Variable ) {
                        // ignore
                    }
                    else {
                        for( auto it2 = e.variables.rbegin(); it2 != e.variables.rend(); ++ it2 )
                        {
                            if( it2->first.name == name && it2->first.hygiene.is_visible(src_context) ) {
                                ::AST::Path rv(name);
                                rv.bind_variable( it2->second );
                                return rv;
                            }
                        }
                    }
                    ),
                (Generic,
                    DEBUG("- Generic");
                    if( mode == LookupMode::Type || mode == LookupMode::Namespace ) {
                        for( auto it2 = e.types.rbegin(); it2 != e.types.rend(); ++ it2 )
                        {
                            if( it2->name == name ) {
                                ::AST::Path rv(name);
                                rv.bind_variable( it2->value.to_binding() );
                                return rv;
                            }
                        }
                    }
                    else {
                        // ignore.
                        // TODO: Integer generics
                    }
                    )
                )
            }

            // Top-level module
            DEBUG("- Top module (" << m_mod.path() << ")");
            ::AST::Path rv;
            if( this->lookup_in_mod(m_mod, name, mode,  rv) ) {
                return rv;
            }

            DEBUG("- Primitives");
            switch(mode)
            {
            case LookupMode::Namespace:
            case LookupMode::Type: {
                // Look up primitive types
                auto ct = coretype_fromstring(name);
                if( ct != CORETYPE_INVAL )
                {
                    return ::AST::Path( ::AST::Path::TagUfcs(), TypeRef(Span("-",0,0,0,0), ct), ::AST::Path(), ::std::vector< ::AST::PathNode>() );
                }
                } break;
            default:
                break;
            }

            return AST::Path();
        }

        unsigned int lookup_local(const Span& sp, const ::std::string name, LookupMode mode) {
            for(auto it = m_name_context.rbegin(); it != m_name_context.rend(); ++ it)
            {
                TU_MATCH(Ent, (*it), (e),
                (Module,
                    ),
                (ConcreteSelf,
                    ),
                (VarBlock,
                    if( mode == LookupMode::Variable ) {
                        for( auto it2 = e.variables.rbegin(); it2 != e.variables.rend(); ++ it2 )
                        {
                            // TODO: Hyginic lookup?
                            if( it2->first.name == name ) {
                                return it2->second;
                            }
                        }
                    }
                    ),
                (Generic,
                    if( mode == LookupMode::Type ) {
                        for( auto it2 = e.types.rbegin(); it2 != e.types.rend(); ++ it2 )
                        {
                            if( it2->name == name ) {
                                return it2->value.to_binding();
                            }
                        }
                    }
                    else {
                        // ignore.
                        // TODO: Integer generics
                    }
                    )
                )
            }

            ERROR(sp, E0000, "Unable to find local " << (mode == LookupMode::Variable ? "variable" : "type") << " '" << name << "'");
        }

        /// Clones the context, including only the module-level items (i.e. just the Module entries)
        Context clone_mod() const {
            auto rv = Context(this->m_crate, this->m_mod);
            for(const auto& v : m_name_context) {
                TU_IFLET(Ent, v, Module, e,
                    rv.m_name_context.push_back( Ent::make_Module(e) );
                )
            }
            return rv;
        }
    };
}   // Namespace

::std::ostream& operator<<(::std::ostream& os, const Context::LookupMode& v) {
    switch(v)
    {
    case Context::LookupMode::Namespace:os << "Namespace";  break;
    case Context::LookupMode::Type:     os << "Type";       break;
    case Context::LookupMode::Pattern:  os << "Pattern";    break;
    case Context::LookupMode::PatternValue:  os << "PatternValue";    break;
    case Context::LookupMode::Constant: os << "Constant";   break;
    case Context::LookupMode::Variable: os << "Variable";   break;
    }
    return os;
}



void Resolve_Absolute_Path_BindAbsolute(Context& context, const Span& sp, Context::LookupMode& mode, ::AST::Path& path);
void Resolve_Absolute_Path(/*const*/ Context& context, const Span& sp, Context::LookupMode mode,  ::AST::Path& path);
void Resolve_Absolute_Type(Context& context,  TypeRef& type);
void Resolve_Absolute_Expr(Context& context,  ::AST::Expr& expr);
void Resolve_Absolute_ExprNode(Context& context,  ::AST::ExprNode& node);
void Resolve_Absolute_Pattern(Context& context, bool allow_refutable, ::AST::Pattern& pat);
void Resolve_Absolute_Mod(const ::AST::Crate& crate, ::AST::Module& mod);
void Resolve_Absolute_Mod( Context item_context, ::AST::Module& mod );

void Resolve_Absolute_Function(Context& item_context, ::AST::Function& fcn);

void Resolve_Absolute_PathParams(/*const*/ Context& context, const Span& sp, ::AST::PathParams& args)
{
    for(auto& arg : args.m_types)
    {
        Resolve_Absolute_Type(context, arg);
    }
    for(auto& arg : args.m_assoc)
    {
        Resolve_Absolute_Type(context, arg.second);
    }
}

void Resolve_Absolute_PathNodes(/*const*/ Context& context, const Span& sp, ::std::vector< ::AST::PathNode >& nodes)
{
    for(auto& node : nodes)
    {
        Resolve_Absolute_PathParams(context, sp, node.args());
    }
}

void Resolve_Absolute_Path_BindUFCS(Context& context, const Span& sp, Context::LookupMode mode, ::AST::Path& path)
{
    while( path.m_class.as_UFCS().nodes.size() > 1 )
    {
        // More than one node, break into inner UFCS
        // - Since traits can't be associated items, this will always be the same form

        auto span = path.m_class.as_UFCS().type->span();
        auto nodes = mv$(path.m_class.as_UFCS().nodes);
        auto inner_path = mv$(path);
        inner_path.m_class.as_UFCS().nodes.push_back( mv$(nodes.front()) );
        nodes.erase( nodes.begin() );
        path = ::AST::Path( ::AST::Path::TagUfcs(), TypeRef(span, mv$(inner_path)), ::AST::Path(), mv$(nodes) );
    }

    const auto& ufcs = path.m_class.as_UFCS();
    if( ufcs.nodes.size() == 0 ) {

        if( mode == Context::LookupMode::Type && ufcs.trait && *ufcs.trait == ::AST::Path() ) {
            return ;
        }

        BUG(sp, "UFCS with no nodes encountered - " << path);
    }
    const auto& node = ufcs.nodes.at(0);

    if( ufcs.trait && ufcs.trait->is_valid() )
    {
        // Trait is specified, definitely a trait item
        // - Must resolve here
        const auto& pb = ufcs.trait->binding();
        if( ! pb.is_Trait() ) {
            ERROR(sp, E0000, "UFCS trait was not a trait - " << *ufcs.trait);
        }
        if( !pb.as_Trait().trait_ )
            return ;
        assert( pb.as_Trait().trait_ );
        const auto& tr = *pb.as_Trait().trait_;

        switch(mode)
        {
        case Context::LookupMode::Pattern:
        case Context::LookupMode::PatternValue:
            ERROR(sp, E0000, "Invalid use of UFCS in pattern");
            break;
        case Context::LookupMode::Namespace:
        case Context::LookupMode::Type:
            for( const auto& item : tr.items() )
            {
                if( item.name != node.name() ) {
                    continue;
                }
                TU_MATCH_DEF(::AST::Item, (item.data), (e),
                (
                    // TODO: Error
                    ),
                (Type,
                    // Resolve to asociated type
                    )
                )
            }
            break;
        case Context::LookupMode::Constant:
        case Context::LookupMode::Variable:
            for( const auto& item : tr.items() )
            {
                if( item.name != node.name() ) {
                    continue;
                }
                TU_MATCH_DEF(::AST::Item, (item.data), (e),
                (
                    // TODO: Error
                    ),
                (Function,
                    // Bind as trait method
                    path.bind_function(e);
                    ),
                (Static,
                    // Resolve to asociated static
                    )
                )
            }
            break;
        }
    }
    else
    {
        // Trait is unknown or inherent, search for items on the type (if known) otherwise leave it until type resolution
        // - Methods can't be known until typeck (after the impl map is created)
    }
}

namespace {
    AST::Path split_into_crate(const Span& sp, AST::Path path, unsigned int start, const ::std::string& crate_name)
    {
        auto& nodes = path.nodes();
        AST::Path   np = AST::Path(crate_name, {});
        for(unsigned int i = start; i < nodes.size(); i ++)
        {
            np.nodes().push_back( mv$(nodes[i]) );
        }
        np.bind( path.binding().clone() );
        return np;
    }
    AST::Path split_into_ufcs_ty(const Span& sp, AST::Path path, unsigned int i /*item_name_idx*/)
    {
        const auto& path_abs = path.m_class.as_Absolute();
        auto type_path = ::AST::Path( path );
        type_path.m_class.as_Absolute().nodes.resize( i+1 );

        auto new_path = ::AST::Path(::AST::Path::TagUfcs(), ::TypeRef(sp, mv$(type_path)), ::AST::Path());
        for( unsigned int j = i+1; j < path_abs.nodes.size(); j ++ )
            new_path.nodes().push_back( mv$(path_abs.nodes[j]) );

        return new_path;
    }
    AST::Path split_replace_into_ufcs_path(const Span& sp, AST::Path path, unsigned int i, const AST::Path& ty_path_tpl)
    {
        auto& path_abs = path.m_class.as_Absolute();
        auto& n = path_abs.nodes[i];

        auto type_path = ::AST::Path(ty_path_tpl);
        if( ! n.args().is_empty() ) {
            type_path.nodes().back().args() = mv$(n.args());
        }
        auto new_path = ::AST::Path(::AST::Path::TagUfcs(), ::TypeRef(sp, mv$(type_path)), ::AST::Path());
        for( unsigned int j = i+1; j < path_abs.nodes.size(); j ++ )
            new_path.nodes().push_back( mv$(path_abs.nodes[j]) );

        return new_path;
    }

    void Resolve_Absolute_Path_BindAbsolute__hir_from_import(Context& context, const Span& sp, bool is_value, AST::Path& path, const ::HIR::SimplePath& p)
    {
        TRACE_FUNCTION_FR("path="<<path<<", p="<<p, path);
        const auto& ext_crate = context.m_crate.m_extern_crates.at(p.m_crate_name);
        const ::HIR::Module* hmod = &ext_crate.m_hir->m_root_module;
        for(unsigned int i = 0; i < p.m_components.size() - 1; i ++)
        {
            const auto& name = p.m_components[i];
            auto it = hmod->m_mod_items.find(name);
            if( it == hmod->m_mod_items.end() )
                ERROR(sp, E0000, "Couldn't find path component '" << name << "' of " << p);

            TU_MATCH_DEF(::HIR::TypeItem, (it->second->ent), (e),
            (
                TODO(sp, "Unknown item type in path - " << i << " " << p << " - " << it->second->ent.tag_str());
                ),
            (Enum,
                if( i != p.m_components.size() - 2 ) {
                    ERROR(sp, E0000, "Enum as path component in unexpected location - " << p);
                }
                const auto& varname = p.m_components.back();
                auto it = ::std::find_if( e.m_variants.begin(), e.m_variants.end(), [&](const auto&x){return x.first == varname;} );
                ASSERT_BUG(sp, it != e.m_variants.end(), "Extern crate import path points to non-present variant - " << p);
                unsigned int var_idx = it - e.m_variants.begin();
                auto pb = ::AST::PathBinding::make_EnumVar({nullptr, var_idx, &e});

                // Construct output path (with same set of parameters)
                AST::Path   rv( p.m_crate_name, {} );
                rv.nodes().reserve( p.m_components.size() );
                for(const auto& c : p.m_components)
                    rv.nodes().push_back( AST::PathNode(c) );
                rv.nodes().back().args() = mv$( path.nodes().back().args() );
                rv.bind( mv$(pb) );
                path = mv$(rv);

                return ;
                ),
            (Module,
                hmod = &e;
                )
            )
        }

        ::AST::PathBinding  pb;

        const auto& name = p.m_components.back();
        if( is_value )
        {
            auto it = hmod->m_value_items.find(name);
            if( it == hmod->m_value_items.end() )
                ERROR(sp, E0000, "Couldn't find final component of " << p);
            TU_MATCH(::HIR::ValueItem, (it->second->ent), (e),
            (Import,
                // Wait? is this even valid?
                TODO(sp, "HIR Import item pointed to an import");
                ),
            (Constant,
                pb = ::AST::PathBinding::make_Static({nullptr, nullptr});
                ),
            (Static,
                pb = ::AST::PathBinding::make_Static({nullptr, &e});
                ),
            (StructConstant,
                pb = ::AST::PathBinding::make_Struct({nullptr, &ext_crate.m_hir->get_typeitem_by_path(sp, e.ty, true).as_Struct()});
                ),
            (Function,
                pb = ::AST::PathBinding::make_Function({nullptr/*, &e*/});
                ),
            (StructConstructor,
                pb = ::AST::PathBinding::make_Struct({nullptr, &ext_crate.m_hir->get_typeitem_by_path(sp, e.ty, true).as_Struct()});
                )
            )
        }
        else
        {
            auto it = hmod->m_mod_items.find(name);
            if( it == hmod->m_mod_items.end() )
                ERROR(sp, E0000, "Couldn't find final component of " << p);
            TU_MATCH(::HIR::TypeItem, (it->second->ent), (e),
            (Import,
                // Wait? is this even valid?
                TODO(sp, "HIR Import item pointed to an import");
                ),
            (Module,
                pb = ::AST::PathBinding::make_Module({nullptr, &e});
                ),
            (Trait,
                pb = ::AST::PathBinding::make_Trait({nullptr, &e});
                ),
            (TypeAlias,
                pb = ::AST::PathBinding::make_TypeAlias({nullptr/*, &e*/});
                ),
            (Struct,
                pb = ::AST::PathBinding::make_Struct({nullptr, &e});
                ),
            (Union,
                pb = ::AST::PathBinding::make_Union({nullptr, &e});
                ),
            (Enum,
                pb = ::AST::PathBinding::make_Enum({nullptr, &e});
                )
            )
        }

        // Construct output path (with same set of parameters)
        AST::Path   rv( p.m_crate_name, {} );
        rv.nodes().reserve( p.m_components.size() );
        for(const auto& c : p.m_components)
            rv.nodes().push_back( AST::PathNode(c) );
        rv.nodes().back().args() = mv$( path.nodes().back().args() );
        rv.bind( mv$(pb) );
        path = mv$(rv);
    }

    void Resolve_Absolute_Path_BindAbsolute__hir_from(Context& context, const Span& sp, Context::LookupMode& mode, ::AST::Path& path, const AST::ExternCrate& crate, unsigned int start)
    {
        assert(crate.m_hir->m_crate_name == crate.m_name);
        TRACE_FUNCTION_FR( crate.m_hir->m_crate_name << " - " << path << " start=" << start, path);
        auto& path_abs = path.m_class.as_Absolute();

        if( path_abs.nodes.empty() ) {
            switch(mode)
            {
            case Context::LookupMode::Namespace:
                path.bind( ::AST::PathBinding::make_Module({nullptr, &crate.m_hir->m_root_module}) );
                return ;
            default:
                TODO(sp, "");
            }
        }

        const ::HIR::Module* hmod = &crate.m_hir->m_root_module;
        for(unsigned int i = start; i < path_abs.nodes.size() - 1; i ++ )
        {
            auto& n = path_abs.nodes[i];
            assert(hmod);
            auto it = hmod->m_mod_items.find(n.name());
            if( it == hmod->m_mod_items.end() )
                ERROR(sp, E0000, "Couldn't find path component '" << n.name() << "' of " << path);

            TU_MATCH(::HIR::TypeItem, (it->second->ent), (e),
            (Import,
                // - Update path then restart
                auto newpath = AST::Path(e.path.m_crate_name, {});
                for(const auto& n : e.path.m_components)
                    newpath.nodes().push_back( AST::PathNode(n) );
                for(unsigned int j = i + 1; j < path.nodes().size(); j ++)
                    newpath.nodes().push_back( mv$(path.nodes()[j]) );
                path = mv$(newpath);
                // TODO: Recursion limit
                Resolve_Absolute_Path_BindAbsolute(context, sp, mode, path);
                return ;
                ),
            (Module,
                hmod = &e;
                ),
            (Trait,
                auto trait_path = ::AST::Path( crate.m_name, {} );
                for(unsigned int j = start; j <= i; j ++)
                    trait_path.nodes().push_back( path_abs.nodes[j].name() );
                if( !n.args().is_empty() ) {
                    trait_path.nodes().back().args() = mv$(n.args());
                }
                else {
                    for(const auto& typ : e.m_params.m_types)
                    {
                        (void)typ;
                        trait_path.nodes().back().args().m_types.push_back( ::TypeRef(sp) );
                    }
                }
                trait_path.bind( ::AST::PathBinding::make_Trait({nullptr, &e}) );

                ::AST::Path new_path;
                const auto& next_node = path_abs.nodes[i+1];
                // If the named item can't be found in the trait, fall back to it being a type binding
                // - What if this item is from a nested trait?
                bool found = false;
                switch( i+1 < path_abs.nodes.size() ? Context::LookupMode::Namespace : mode )
                {
                case Context::LookupMode::Namespace:
                case Context::LookupMode::Type:
                case Context::LookupMode::Pattern:
                    found = (e.m_types.find( next_node.name() ) != e.m_types.end());
                case Context::LookupMode::PatternValue:
                case Context::LookupMode::Constant:
                case Context::LookupMode::Variable:
                    found = (e.m_values.find( next_node.name() ) != e.m_values.end());
                    break;
                }

                if( !found ) {
                    new_path = ::AST::Path(::AST::Path::TagUfcs(), ::TypeRef(sp, mv$(trait_path)));
                }
                else {
                    new_path = ::AST::Path(::AST::Path::TagUfcs(), ::TypeRef(sp), mv$(trait_path));
                }
                for( unsigned int j = i+1; j < path_abs.nodes.size(); j ++ )
                    new_path.nodes().push_back( mv$(path_abs.nodes[j]) );

                path = mv$(new_path);
                return Resolve_Absolute_Path_BindUFCS(context, sp, mode,  path);
                ),
            (TypeAlias,
                path = split_into_crate(sp, mv$(path), start,  crate.m_name);
                path = split_into_ufcs_ty(sp, mv$(path), i-start);
                return Resolve_Absolute_Path_BindUFCS(context, sp, mode,  path);
                ),
            (Struct,
                path = split_into_crate(sp, mv$(path), start,  crate.m_name);
                path = split_into_ufcs_ty(sp, mv$(path), i-start);
                return Resolve_Absolute_Path_BindUFCS(context, sp, mode,  path);
                ),
            (Union,
                path = split_into_crate(sp, mv$(path), start,  crate.m_name);
                path = split_into_ufcs_ty(sp, mv$(path), i-start);
                return Resolve_Absolute_Path_BindUFCS(context, sp, mode,  path);
                ),
            (Enum,
                const auto& last_node = path_abs.nodes.back();
                // If this refers to an enum variant, return the full path
                for( const auto& var : e.m_variants )
                {
                    if( var.first == last_node.name() ) {

                        if( i != path_abs.nodes.size() - 2 ) {
                            ERROR(sp, E0000, "Unexpected enum in path " << path);
                        }
                        // NOTE: Type parameters for enums go after the _variant_
                        if( ! n.args().is_empty() ) {
                            ERROR(sp, E0000, "Type parameters were not expected here (enum params go on the variant)");
                        }

                        path.bind( ::AST::PathBinding::make_EnumVar({nullptr, static_cast<unsigned int>(&var - &*e.m_variants.begin()), &e}) );
                        path = split_into_crate(sp, mv$(path), start,  crate.m_name);
                        return;
                    }
                }
                path = split_into_crate(sp, mv$(path), start,  crate.m_name);
                path = split_into_ufcs_ty(sp, mv$(path), i-start);
                return Resolve_Absolute_Path_BindUFCS(context, sp, mode,  path);
                )
            )
        }

        const auto& name = path_abs.nodes.back().name();
        switch(mode)
        {
        // TODO: Don't bind to a Module if LookupMode::Type
        case Context::LookupMode::Namespace:
        case Context::LookupMode::Type:
            {
                auto v = hmod->m_mod_items.find(name);
                if( v != hmod->m_mod_items.end() ) {
                    TU_MATCH(::HIR::TypeItem, (v->second->ent), (e),
                    (Import,
                        DEBUG("= Import " << e.path);
                        Resolve_Absolute_Path_BindAbsolute__hir_from_import(context, sp, false,  path, e.path);
                        return ;
                        ),
                    (Trait,
                        path.bind( ::AST::PathBinding::make_Trait({nullptr, &e}) );
                        ),
                    (Module,
                        path.bind( ::AST::PathBinding::make_Module({nullptr, &e}) );
                        ),
                    (TypeAlias,
                        path.bind( ::AST::PathBinding::make_TypeAlias({nullptr/*, &e*/}) );
                        ),
                    (Enum,
                        path.bind( ::AST::PathBinding::make_Enum({nullptr, &e}) );
                        ),
                    (Struct,
                        path.bind( ::AST::PathBinding::make_Struct({nullptr, &e}) );
                        ),
                    (Union,
                        path.bind( ::AST::PathBinding::make_Union({nullptr, &e}) );
                        )
                    )
                    // Update path (trim down to `start` and set crate name)
                    path = split_into_crate(sp, mv$(path), start,  crate.m_name);
                    return ;
                }
            }
            break;

        case Context::LookupMode::Pattern:
            {
                auto v = hmod->m_mod_items.find(name);
                if( v != hmod->m_mod_items.end() ) {
                    TU_MATCH_DEF(::HIR::TypeItem, (v->second->ent), (e),
                    (
                        ),
                    (Import,
                        Resolve_Absolute_Path_BindAbsolute__hir_from_import(context, sp, false,  path, e.path);
                        return ;
                        ),
                    (Struct,
                        // Bind and update path
                        path.bind( ::AST::PathBinding::make_Struct({nullptr, &e}) );
                        path = split_into_crate(sp, mv$(path), start,  crate.m_name);
                        return ;
                        )
                    )
                }
            }
            break;
        case Context::LookupMode::PatternValue:
            {
                auto v = hmod->m_value_items.find(name);
                if( v != hmod->m_value_items.end() ) {
                    TU_MATCH_DEF(::HIR::ValueItem, (v->second->ent), (e),
                    (
                        DEBUG("Ignore - " << v->second->ent.tag_str());
                        ),
                    (StructConstant,
                        auto ty_path = e.ty;
                        path.bind( ::AST::PathBinding::make_Struct({nullptr, &crate.m_hir->get_struct_by_path(sp, ty_path)}) );
                        path = split_into_crate(sp, mv$(path), start,  crate.m_name);
                        return ;
                        ),
                    (Import,
                        Resolve_Absolute_Path_BindAbsolute__hir_from_import(context, sp, true,  path, e.path);
                        return ;
                        ),
                    (Constant,
                        // Bind and update path
                        path.bind( ::AST::PathBinding::make_Static({nullptr, nullptr}) );
                        path = split_into_crate(sp, mv$(path), start,  crate.m_name);
                        return ;
                        )
                    )
                }
                else {
                    DEBUG("No value item for " << name);
                }
            }
            break;
        case Context::LookupMode::Constant:
        case Context::LookupMode::Variable:
            {
                auto v = hmod->m_value_items.find(name);
                if( v != hmod->m_value_items.end() ) {
                    TU_MATCH(::HIR::ValueItem, (v->second->ent), (e),
                    (Import,
                        Resolve_Absolute_Path_BindAbsolute__hir_from_import(context, sp, true,  path, e.path);
                        return ;
                        ),
                    (Function,
                        path.bind( ::AST::PathBinding::make_Function({nullptr/*, &e*/}) );
                        ),
                    (StructConstructor,
                        auto ty_path = e.ty;
                        path.bind( ::AST::PathBinding::make_Struct({nullptr, &crate.m_hir->get_struct_by_path(sp, ty_path)}) );
                        ),
                    (StructConstant,
                        auto ty_path = e.ty;
                        path.bind( ::AST::PathBinding::make_Struct({nullptr, &crate.m_hir->get_struct_by_path(sp, ty_path)}) );
                        ),
                    (Static,
                        path.bind( ::AST::PathBinding::make_Static({nullptr, &e}) );
                        ),
                    (Constant,
                        // Bind
                        path.bind( ::AST::PathBinding::make_Static({nullptr, nullptr}) );
                        )
                    )
                    path = split_into_crate(sp, mv$(path), start,  crate.m_name);
                    return ;
                }
            }
            break;
        }
        ERROR(sp, E0000, "Couldn't find " << Context::lookup_mode_msg(mode) << " '" << path_abs.nodes.back().name() << "' of " << path);
    }
}

void Resolve_Absolute_Path_BindAbsolute(Context& context, const Span& sp, Context::LookupMode& mode, ::AST::Path& path)
{
    TRACE_FUNCTION_FR("path = " << path, path);
    auto& path_abs = path.m_class.as_Absolute();

    if( path_abs.crate != "" ) {
        // TODO: Handle items from other crates (back-converting HIR paths)
        Resolve_Absolute_Path_BindAbsolute__hir_from(context, sp, mode, path,  context.m_crate.m_extern_crates.at(path_abs.crate), 0);
        return ;
    }


    const ::AST::Module*    mod = &context.m_crate.m_root_module;
    for(unsigned int i = 0; i < path_abs.nodes.size() - 1; i ++ )
    {
        auto& n = path_abs.nodes[i];

        if( n.name()[0] == '#' ) {
            if( ! n.args().is_empty() ) {
                ERROR(sp, E0000, "Type parameters were not expected here");
            }

            if( n.name() == "#" ) {
                TODO(sp, "magic module");
            }

            char c;
            unsigned int idx;
            ::std::stringstream ss( n.name() );
            ss >> c;
            ss >> idx;
            assert( idx < mod->anon_mods().size() );
            assert( mod->anon_mods()[idx] );
            mod = mod->anon_mods()[idx].get();
        }
        else
        {
            auto it = mod->m_namespace_items.find( n.name() );
            if( it == mod->m_namespace_items.end() ) {
                ERROR(sp, E0000, "Couldn't find path component '" << n.name() << "' of " << path);
            }
            const auto& name_ref = it->second;
            DEBUG("#" << i << " \"" << n.name() << "\" = " << name_ref.path << (name_ref.is_import ? " (import)" : "") );

            TU_MATCH_DEF(::AST::PathBinding, (name_ref.path.binding()), (e),
            (
                ERROR(sp, E0000, "Encountered non-namespace item '" << n.name() << "' ("<<name_ref.path<<") in path " << path);
                ),
            (TypeAlias,
                path = split_replace_into_ufcs_path(sp, mv$(path), i,  name_ref.path);
                return Resolve_Absolute_Path_BindUFCS(context, sp, mode,  path);
                ),
            (Crate,
                Resolve_Absolute_Path_BindAbsolute__hir_from(context, sp, mode, path,  *e.crate_, i+1);
                return ;
                ),
            (Trait,
                assert( e.trait_ || e.hir );
                auto trait_path = ::AST::Path(name_ref.path);
                // HACK! If this was an import, recurse on it to fix paths. (Ideally, all index entries should have the canonical path, but don't currently)
                if( name_ref.is_import ) {
                    auto lm = Context::LookupMode::Type;
                    Resolve_Absolute_Path_BindAbsolute(context, sp, lm, trait_path);
                }
                if( !n.args().is_empty() ) {
                    trait_path.nodes().back().args() = mv$(n.args());
                }
                else {
                    if( e.trait_ ) {
                        for(const auto& typ : e.trait_->params().ty_params())
                        {
                            (void)typ;
                            trait_path.nodes().back().args().m_types.push_back( ::TypeRef(sp) );
                        }
                    }
                    else {
                        for(const auto& typ : e.hir->m_params.m_types)
                        {
                            (void)typ;
                            trait_path.nodes().back().args().m_types.push_back( ::TypeRef(sp) );
                        }
                    }
                }
                // TODO: If the named item can't be found in the trait, fall back to it being a type binding
                // - What if this item is from a nested trait?
                ::AST::Path new_path;
                bool found = false;
                assert(i+1 < path_abs.nodes.size());
                const auto& item_name = path_abs.nodes[i+1].name();
                if( e.trait_ )
                {
                    auto it = ::std::find_if( e.trait_->items().begin(), e.trait_->items().end(), [&](const auto& x){ return x.name == item_name; } );
                    if( it != e.trait_->items().end() ) {
                        found = true;
                    }
                }
                else
                {
                    switch(mode)
                    {
                    case Context::LookupMode::Constant:
                    case Context::LookupMode::Pattern:
                    case Context::LookupMode::Variable:
                    case Context::LookupMode::PatternValue:
                        found = (e.hir->m_values.count(item_name) != 0);
                        break;
                    case Context::LookupMode::Namespace:
                    case Context::LookupMode::Type:
                        found = (e.hir->m_types.count(item_name) != 0);
                        break;
                    }
                }
                if( !found ) {
                    new_path = ::AST::Path(::AST::Path::TagUfcs(), ::TypeRef(sp, mv$(trait_path)));
                }
                else {
                    new_path = ::AST::Path(::AST::Path::TagUfcs(), ::TypeRef(sp), mv$(trait_path));
                }
                for( unsigned int j = i+1; j < path_abs.nodes.size(); j ++ )
                    new_path.nodes().push_back( mv$(path_abs.nodes[j]) );

                path = mv$(new_path);
                return Resolve_Absolute_Path_BindUFCS(context, sp, mode,  path);
                ),
            (Enum,
                if( name_ref.is_import ) {
                    auto newpath = name_ref.path;
                    for(unsigned int j = i+1; j < path_abs.nodes.size(); j ++)
                    {
                        newpath.nodes().push_back( mv$(path_abs.nodes[j]) );
                    }
                    path = mv$(newpath);
                    //TOOD: Recursion limit
                    Resolve_Absolute_Path_BindAbsolute(context, sp, mode, path);
                    return ;
                }
                else {
                    assert( e.enum_ );
                    const auto& last_node = path_abs.nodes.back();
                    for( const auto& var : e.enum_->variants() ) {
                        if( var.m_name == last_node.name() ) {

                            if( i != path_abs.nodes.size() - 2 ) {
                                ERROR(sp, E0000, "Unexpected enum in path " << path);
                            }
                            // NOTE: Type parameters for enums go after the _variant_
                            if( ! n.args().is_empty() ) {
                                ERROR(sp, E0000, "Type parameters were not expected here (enum params go on the variant)");
                            }

                            path.bind_enum_var(*e.enum_, var.m_name);
                            return;
                        }
                    }

                    path = split_replace_into_ufcs_path(sp, mv$(path), i,  name_ref.path);
                    return Resolve_Absolute_Path_BindUFCS(context, sp, mode,  path);
                }
                ),
            (Struct,
                path = split_replace_into_ufcs_path(sp, mv$(path), i,  name_ref.path);
                return Resolve_Absolute_Path_BindUFCS(context, sp, mode,  path);
                ),
            (Union,
                path = split_replace_into_ufcs_path(sp, mv$(path), i,  name_ref.path);
                return Resolve_Absolute_Path_BindUFCS(context, sp, mode,  path);
                ),
            (Module,
                if( name_ref.is_import ) {
                    //TODO(sp, "Replace path component with new path - " << path << "[.."<<i+1<<"] with " << name_ref.path);
                    auto newpath = name_ref.path;
                    for(unsigned int j = i+1; j < path_abs.nodes.size(); j ++)
                    {
                        newpath.nodes().push_back( mv$(path_abs.nodes[j]) );
                    }
                    DEBUG("- Module import, " << path << " => " << newpath);
                    path = mv$(newpath);
                    Resolve_Absolute_Path_BindAbsolute(context, sp, mode, path);
                    return ;
                }
                else {
                    mod = e.module_;
                }
                )
            )
        }
    }

    // Set binding to binding of node in last module
    ::AST::Path tmp;
    if( ! Context::lookup_in_mod(*mod, path_abs.nodes.back().name(), mode,  tmp) ) {
        ERROR(sp, E0000, "Couldn't find " << Context::lookup_mode_msg(mode) << " '" << path_abs.nodes.back().name() << "' of " << path);
    }
    assert( ! tmp.binding().is_Unbound() );

    // Replaces the path with the one returned by `lookup_in_mod`, ensuring that `use` aliases are eliminated
    DEBUG("Replace " << path << " with " << tmp);
    auto args = mv$(path.nodes().back().args());
    if( tmp != path )
    {
        // If the paths mismatch (i.e. there was an import involved), pass through resolution again
        // - This works around cases where the index contains paths that refer to aliases.
        DEBUG("- Recurse");
        Resolve_Absolute_Path_BindAbsolute(context, sp, mode, tmp);
    }
    tmp.nodes().back().args() = mv$(args);
    path = mv$(tmp);
}

void Resolve_Absolute_Path(/*const*/ Context& context, const Span& sp, Context::LookupMode mode,  ::AST::Path& path)
{
    TRACE_FUNCTION_FR("mode = " << mode << ", path = " << path, path);

    TU_MATCH(::AST::Path::Class, (path.m_class), (e),
    (Invalid,
        BUG(sp, "Attempted resolution of invalid path");
        ),
    (Local,
        // Nothing to do (TODO: Check that it's valid?)
        if( mode == Context::LookupMode::Variable ) {
            path.bind_variable( context.lookup_local(sp, e.name, mode) );
        }
        else if( mode == Context::LookupMode::Type ) {
            path.bind_variable( context.lookup_local(sp, e.name, mode) );
        }
        else {
        }
        ),
    (Relative,
        DEBUG("- Relative");
        if(e.nodes.size() == 0)
            BUG(sp, "Resolve_Absolute_Path - Relative path with no nodes");
        if(e.nodes.size() > 1)
        {
            // Look up type/module name
            auto p = context.lookup(sp, e.nodes[0].name(), e.hygiene, Context::LookupMode::Namespace);
            DEBUG("Found type/mod - " << p);
            // HACK: If this is a primitive name, and resolved to a module.
            // - If the next component isn't found in the located module
            //  > Instead use the type name.
            if( ! p.m_class.is_Local() && coretype_fromstring(e.nodes[0].name()) != CORETYPE_INVAL ) {
                TU_IFLET( ::AST::PathBinding, p.binding(), Module, pe,
                    bool found = false;
                    const auto& name = e.nodes[1].name();
                    if( !pe.module_ ) {
                        assert( pe.hir );
                        const auto& mod = *pe.hir;

                        switch( e.nodes.size() == 2 ? mode : Context::LookupMode::Namespace )
                        {
                        case Context::LookupMode::Namespace:
                        case Context::LookupMode::Type:
                            // TODO: Restrict if ::Type
                            if( mod.m_mod_items.find(name) != mod.m_mod_items.end() ) {
                                found = true;
                            }
                            break;
                        case Context::LookupMode::Pattern:
                        case Context::LookupMode::PatternValue:
                            TODO(sp, "Check " << p << " for an item named " << name << " (Pattern)");
                        case Context::LookupMode::Constant:
                        case Context::LookupMode::Variable:
                            if( mod.m_value_items.find(name) != mod.m_value_items.end() ) {
                                found = true;
                            }
                            break;
                        }
                    }
                    else
                    {
                        const auto& mod = *pe.module_;
                        switch( e.nodes.size() == 2 ? mode : Context::LookupMode::Namespace )
                        {
                        case Context::LookupMode::Namespace:
                            if( mod.m_namespace_items.find(name) != mod.m_namespace_items.end() ) {
                                found = true;
                            }
                        case Context::LookupMode::Type:
                            if( mod.m_namespace_items.find(name) != mod.m_namespace_items.end() ) {
                                found = true;
                            }
                            break;
                        case Context::LookupMode::Pattern:
                        case Context::LookupMode::PatternValue:
                            TODO(sp, "Check " << p << " for an item named " << name << " (Pattern)");
                        case Context::LookupMode::Constant:
                        case Context::LookupMode::Variable:
                            if( mod.m_value_items.find(name) != mod.m_value_items.end() ) {
                                found = true;
                            }
                            break;
                        }
                    }
                    if( !found )
                    {
                        //TODO(sp, "Switch back to primitive from " << p << " for " << path);
                        //p = ::AST::Path( ::AST::Path::TagLocal(), e.nodes[0].name() );
                        auto ct = coretype_fromstring(e.nodes[0].name());
                        p = ::AST::Path( ::AST::Path::TagUfcs(), TypeRef(Span("-",0,0,0,0), ct), ::AST::Path(), ::std::vector< ::AST::PathNode>() );
                    }

                    DEBUG("Primitive module hack yeilded " << p);
                )
            }

            if( e.nodes.size() > 1 )
            {
                // Only primitive types turn `Local` paths
                if( p.m_class.is_Local() ) {
                    p = ::AST::Path( ::AST::Path::TagUfcs(), TypeRef(sp, mv$(p)), ::AST::Path() );
                }
                if( ! e.nodes[0].args().is_empty() )
                {
                    assert( p.nodes().size() > 0 );
                    assert( p.nodes().back().args().is_empty() );
                    p.nodes().back().args() = mv$( e.nodes[0].args() );
                }
                for( unsigned int i = 1; i < e.nodes.size(); i ++ )
                {
                    p.nodes().push_back( mv$(e.nodes[i]) );
                }
            }
            path = mv$(p);
        }
        else {
            // Look up value
            auto p = context.lookup(sp, e.nodes[0].name(), e.hygiene, mode);
            //DEBUG("Found path " << p << " for " << path);
            if( p.is_absolute() ) {
                assert( !p.nodes().empty() );
                p.nodes().back().args() = mv$(e.nodes.back().args());
            }
            path = mv$(p);
        }

        if( !path.is_trivial() )
            Resolve_Absolute_PathNodes(context, sp,  path.nodes());
        ),
    (Self,
        DEBUG("- Self");
        const auto& mp_nodes = context.m_mod.path().nodes();
        // Ignore any leading anon modules
        unsigned int start_len = mp_nodes.size();
        while( start_len > 0 && mp_nodes[start_len-1].name()[0] == '#' )
            start_len --;

        // - Create a new path
        ::AST::Path np("", {});
        auto& np_nodes = np.nodes();
        np_nodes.reserve( start_len + e.nodes.size() );
        for(unsigned int i = 0; i < start_len; i ++ )
            np_nodes.push_back( mp_nodes[i] );
        for(auto& en : e.nodes)
            np_nodes.push_back( mv$(en) );

        if( !path.is_trivial() )
            Resolve_Absolute_PathNodes(context, sp,  np_nodes);

        path = mv$(np);
        ),
    (Super,
        DEBUG("- Super");
        // - Determine how many components of the `self` path to use
        const auto& mp_nodes = context.m_mod.path().nodes();
        assert( e.count >= 1 );
        // TODO: The first super should ignore any anon modules.
        unsigned int start_len = e.count > mp_nodes.size() ? 0 : mp_nodes.size() - e.count;
        while( start_len > 0 && mp_nodes[start_len-1].name()[0] == '#' )
            start_len --;

        // - Create a new path
        ::AST::Path np("", {});
        auto& np_nodes = np.nodes();
        np_nodes.reserve( start_len + e.nodes.size() );
        for(unsigned int i = 0; i < start_len; i ++ )
            np_nodes.push_back( mp_nodes[i] );
        for(auto& en : e.nodes)
            np_nodes.push_back( mv$(en) );

        if( !path.is_trivial() )
            Resolve_Absolute_PathNodes(context, sp,  np_nodes);

        path = mv$(np);
        ),
    (Absolute,
        DEBUG("- Absolute");
        // Nothing to do (TODO: Bind?)
        Resolve_Absolute_PathNodes(context, sp,  e.nodes);
        ),
    (UFCS,
        DEBUG("- UFCS");
        Resolve_Absolute_Type(context, *e.type);
        if( e.trait && *e.trait != ::AST::Path() ) {
            Resolve_Absolute_Path(context, sp, Context::LookupMode::Type, *e.trait);
        }

        Resolve_Absolute_PathNodes(context, sp,  e.nodes);
        )
    )

    DEBUG("path = " << path);
    // TODO: Should this be deferred until the HIR?
    // - Doing it here so the HIR lowering has a bit more information
    // - Also handles splitting "absolute" paths into UFCS
    TU_MATCH_DEF(::AST::Path::Class, (path.m_class), (e),
    (
        BUG(sp, "Path wasn't absolutised correctly");
        ),
    (Local,
        if( path.binding().is_Unbound() )
        {
            TODO(sp, "Bind unbound local path - " << path);
        }
        ),
    (Absolute,
        Resolve_Absolute_Path_BindAbsolute(context, sp, mode,  path);
        ),
    (UFCS,
        Resolve_Absolute_Path_BindUFCS(context, sp, mode,  path);
        )
    )

    // TODO: Expand default type parameters?
    // - Helps with cases like PartialOrd<Self>, but hinders when the default is a hint (in expressions)
}

void Resolve_Absolute_Type(Context& context,  TypeRef& type)
{
    TRACE_FUNCTION_FR("type = " << type, "type = " << type);
    const auto& sp = type.span();
    TU_MATCH(TypeData, (type.m_data), (e),
    (None,
        // invalid type
        ),
    (Any,
        // _ type
        ),
    (Unit,
        ),
    (Bang,
        // ! type
        ),
    (Macro,
        BUG(sp, "Resolve_Absolute_Type - Encountered an unexpanded macro in type - " << type);
        ),
    (Primitive,
        ),
    (Function,
        Resolve_Absolute_Type(context,  *e.info.m_rettype);
        for(auto& t : e.info.m_arg_types) {
            Resolve_Absolute_Type(context,  t);
        }
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
        if( e.size ) {
            auto _h = context.enter_rootblock();
            Resolve_Absolute_ExprNode(context,  *e.size);
        }
        ),
    (Generic,
        if( e.name == "Self" )
        {
            type = context.get_self();
        }
        else
        {
            auto idx = context.lookup_local(type.span(), e.name, Context::LookupMode::Type);
            // TODO: Should this be bound to the relevant index, or just leave as-is?
            e.index = idx;
        }
        ),
    (Path,
        Resolve_Absolute_Path(context, type.span(), Context::LookupMode::Type, e.path);
        TU_IFLET(::AST::Path::Class, e.path.m_class, UFCS, ufcs,
            if( ufcs.nodes.size() == 0 /*&& ufcs.trait && *ufcs.trait == ::AST::Path()*/ ) {
                auto ty = mv$(*ufcs.type);
                type = mv$(ty);
                return ;
            }
            assert( ufcs.nodes.size() == 1);
        )

        TU_IFLET(::AST::PathBinding, e.path.binding(), Trait, be,
            auto ty = ::TypeRef( type.span(), {}, ::make_vec1(mv$(e.path)) );
            type = mv$(ty);
            return ;
        )
        ),
    (TraitObject,
        //context.push_lifetimes( e.hrls );
        for(auto& trait : e.traits) {
            Resolve_Absolute_Path(context, type.span(), Context::LookupMode::Type, trait);
        }
        //context.pop_lifetimes();
        ),
    (ErasedType,
        //context.push_lifetimes( e.hrls );
        for(auto& trait : e.traits) {
            Resolve_Absolute_Path(context, type.span(), Context::LookupMode::Type, trait);
        }
        //context.pop_lifetimes();
        )
    )
}

void Resolve_Absolute_Expr(Context& context,  ::AST::Expr& expr)
{
    if( expr.is_valid() )
    {
        Resolve_Absolute_ExprNode(context, expr.node());
    }
}
void Resolve_Absolute_ExprNode(Context& context,  ::AST::ExprNode& node)
{
    TRACE_FUNCTION_F("");

    struct NV:
        public AST::NodeVisitorDef
    {
        Context& context;

        NV(Context& context):
            context(context)
        {
        }

        void visit(AST::ExprNode_Block& node) override {
            DEBUG("ExprNode_Block");
            if( node.m_local_mod ) {
                auto _h = context.clear_rootblock();
                this->context.push( *node.m_local_mod );

                // Clone just the module stack part of the current context
                Resolve_Absolute_Mod(this->context.clone_mod(), *node.m_local_mod);
            }
            this->context.push_block();
            AST::NodeVisitorDef::visit(node);
            this->context.pop_block();
            if( node.m_local_mod ) {
                this->context.pop( *node.m_local_mod );
            }
        }

        void visit(AST::ExprNode_Match& node) override {
            DEBUG("ExprNode_Match");
            node.m_val->visit( *this );
            for( auto& arm : node.m_arms )
            {
                this->context.push_block();
                if( arm.m_patterns.size() > 1 ) {
                    // TODO: Save the context, ensure that each arm results in the same state.
                    // - Or just an equivalent state
                    // OR! Have a mode in the context that handles multiple bindings.
                    this->context.start_patbind();
                    for( auto& pat : arm.m_patterns )
                    {
                        Resolve_Absolute_Pattern(this->context, true,  pat);
                        this->context.freeze_patbind();
                    }
                    this->context.end_patbind();
                    // Requires ensuring that the binding set is the same.
                }
                else {
                    Resolve_Absolute_Pattern(this->context, true,  arm.m_patterns[0]);
                }

                if(arm.m_cond)
                    arm.m_cond->visit( *this );
                assert( arm.m_code );
                arm.m_code->visit( *this );

                this->context.pop_block();
            }
        }
        void visit(AST::ExprNode_Loop& node) override {
            AST::NodeVisitorDef::visit(node.m_cond);
            this->context.push_block();
            switch( node.m_type )
            {
            case ::AST::ExprNode_Loop::LOOP:
                break;
            case ::AST::ExprNode_Loop::WHILE:
                break;
            case ::AST::ExprNode_Loop::WHILELET:
                Resolve_Absolute_Pattern(this->context, true, node.m_pattern);
                break;
            case ::AST::ExprNode_Loop::FOR:
                BUG(node.span(), "`for` should be desugared");
            }
            node.m_code->visit( *this );
            this->context.pop_block();
        }

        void visit(AST::ExprNode_LetBinding& node) override {
            DEBUG("ExprNode_LetBinding");
            Resolve_Absolute_Type(this->context, node.m_type);
            AST::NodeVisitorDef::visit(node);
            Resolve_Absolute_Pattern(this->context, false, node.m_pat);
        }
        void visit(AST::ExprNode_IfLet& node) override {
            DEBUG("ExprNode_IfLet");
            node.m_value->visit( *this );

            this->context.push_block();
            Resolve_Absolute_Pattern(this->context, true, node.m_pattern);

            assert( node.m_true );
            node.m_true->visit( *this );
            this->context.pop_block();

            if(node.m_false)
                node.m_false->visit(*this);
        }
        void visit(AST::ExprNode_StructLiteral& node) override {
            DEBUG("ExprNode_StructLiteral");
            Resolve_Absolute_Path(this->context, node.span(), Context::LookupMode::Type, node.m_path);
            AST::NodeVisitorDef::visit(node);
        }
        void visit(AST::ExprNode_CallPath& node) override {
            DEBUG("ExprNode_CallPath");
            Resolve_Absolute_Path(this->context, node.span(), Context::LookupMode::Variable,  node.m_path);
            AST::NodeVisitorDef::visit(node);
        }
        void visit(AST::ExprNode_CallMethod& node) override {
            DEBUG("ExprNode_CallMethod");
            Resolve_Absolute_PathParams(this->context, node.span(),  node.m_method.args());
            AST::NodeVisitorDef::visit(node);
        }
        void visit(AST::ExprNode_NamedValue& node) override {
            DEBUG("(" << node.span() << ") ExprNode_NamedValue - " << node.m_path);
            Resolve_Absolute_Path(this->context, node.span(), Context::LookupMode::Variable,  node.m_path);
        }
        void visit(AST::ExprNode_Cast& node) override {
            DEBUG("ExprNode_Cast");
            Resolve_Absolute_Type(this->context,  node.m_type);
            AST::NodeVisitorDef::visit(node);
        }
        void visit(AST::ExprNode_Closure& node) override {
            DEBUG("ExprNode_Closure");

            Resolve_Absolute_Type(this->context,  node.m_return);

            this->context.push_block();
            for( auto& arg : node.m_args ) {
                Resolve_Absolute_Type(this->context,  arg.second);
                Resolve_Absolute_Pattern(this->context, false,  arg.first);
            }

            node.m_code->visit(*this);

            this->context.pop_block();
        }
    } expr_iter(context);

    node.visit( expr_iter );
}

void Resolve_Absolute_Generic(Context& context, ::AST::GenericParams& params)
{
    for( auto& param : params.ty_params() )
    {
        Resolve_Absolute_Type(context, param.get_default());
    }
    for( auto& bound : params.bounds() )
    {
        TU_MATCH(::AST::GenericBound, (bound), (e),
        (Lifetime,
            // TODO: Link lifetime names to params
            ),
        (TypeLifetime,
            Resolve_Absolute_Type(context, e.type);
            ),
        (IsTrait,
            Resolve_Absolute_Type(context, e.type);
            Resolve_Absolute_Path(context, bound.span, Context::LookupMode::Type, e.trait);
            ),
        (MaybeTrait,
            Resolve_Absolute_Type(context, e.type);
            Resolve_Absolute_Path(context, bound.span, Context::LookupMode::Type, e.trait);
            ),
        (NotTrait,
            Resolve_Absolute_Type(context, e.type);
            Resolve_Absolute_Path(context, bound.span, Context::LookupMode::Type, e.trait);
            ),
        (Equality,
            Resolve_Absolute_Type(context, e.type);
            Resolve_Absolute_Type(context, e.replacement);
            )
        )
    }
}

// Locals shouldn't be possible, as they'd end up as MaybeBind. Will assert the path class.
void Resolve_Absolute_PatternValue(/*const*/ Context& context, const Span& sp, ::AST::Pattern::Value& val)
{
    TU_IFLET(::AST::Pattern::Value, val, Named, e,
        //assert( ! e.is_trivial() );
        Resolve_Absolute_Path(context, sp, Context::LookupMode::Constant, e);
    )
}
void Resolve_Absolute_Pattern(Context& context, bool allow_refutable,  ::AST::Pattern& pat)
{
    TRACE_FUNCTION_F("allow_refutable = " << allow_refutable << ", pat = " << pat);
    if( pat.binding().is_valid() ) {
        if( !pat.data().is_Any() && ! allow_refutable )
            TODO(pat.span(), "Resolve_Absolute_Pattern - Encountered bound destructuring pattern");
        pat.binding().m_slot = context.push_var( pat.span(), pat.binding().m_name );
        DEBUG("- Binding #" << pat.binding().m_slot << " '" << pat.binding().m_name << "'");
    }

    TU_MATCH( ::AST::Pattern::Data, (pat.data()), (e),
    (MaybeBind,
        assert( pat.binding().is_valid() == false );
        if( allow_refutable ) {
            auto name = mv$( e.name );
            // Attempt to resolve the name in the current namespace, and if it fails, it's a binding
            auto p = context.lookup_opt( name.name, name.hygiene, Context::LookupMode::PatternValue );
            if( p.is_valid() ) {
                Resolve_Absolute_Path(context, pat.span(), Context::LookupMode::PatternValue, p);
                pat = ::AST::Pattern(::AST::Pattern::TagValue(), ::AST::Pattern::Value::make_Named(mv$(p)));
                DEBUG("MaybeBind resolved to " << pat);
            }
            else {
                pat = ::AST::Pattern(::AST::Pattern::TagBind(), mv$(name));
                pat.binding().m_slot = context.push_var( pat.span(), pat.binding().m_name );
                DEBUG("- Binding #" << pat.binding().m_slot << " '" << pat.binding().m_name << "' (was MaybeBind)");
            }
        }
        else {
            auto name = mv$( e.name );

            pat = ::AST::Pattern(::AST::Pattern::TagBind(), mv$(name));
            pat.binding().m_slot = context.push_var( pat.span(), pat.binding().m_name );
        }
        ),
    (Macro,
        BUG(pat.span(), "Resolve_Absolute_Pattern - Encountered Macro");
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
            BUG(pat.span(), "Resolve_Absolute_Pattern - Encountered refutable pattern where only irrefutable allowed - " << pat);
        Resolve_Absolute_PatternValue(context, pat.span(), e.start);
        Resolve_Absolute_PatternValue(context, pat.span(), e.end);
        ),
    (Tuple,
        for(auto& sp : e.start)
            Resolve_Absolute_Pattern(context, allow_refutable,  sp);
        for(auto& sp : e.end)
            Resolve_Absolute_Pattern(context, allow_refutable,  sp);
        ),
    (StructTuple,
        Resolve_Absolute_Path(context, pat.span(), Context::LookupMode::Constant, e.path);
        for(auto& sp : e.tup_pat.start)
            Resolve_Absolute_Pattern(context, allow_refutable,  sp);
        for(auto& sp : e.tup_pat.end)
            Resolve_Absolute_Pattern(context, allow_refutable,  sp);
        ),
    (Struct,
        Resolve_Absolute_Path(context, pat.span(), Context::LookupMode::Type, e.path);
        for(auto& sp : e.sub_patterns)
            Resolve_Absolute_Pattern(context, allow_refutable,  sp.second);
        ),
    (Slice,
        // NOTE: Can be irrefutable (if the type is array)
        for(auto& sp : e.sub_pats)
            Resolve_Absolute_Pattern(context, allow_refutable,  sp);
        ),
    (SplitSlice,
        // NOTE: Can be irrefutable (if the type is array)
        for(auto& sp : e.leading)
            Resolve_Absolute_Pattern(context, allow_refutable,  sp);
        if( e.extra_bind.is_valid() ) {
            e.extra_bind.m_slot = context.push_var( pat.span(), e.extra_bind.m_name );
        }
        for(auto& sp : e.trailing)
            Resolve_Absolute_Pattern(context, allow_refutable,  sp);
        )
    )
}

// - For traits
void Resolve_Absolute_ImplItems(Context& item_context,  ::AST::NamedList< ::AST::Item >& items)
{
    TRACE_FUNCTION_F("");
    for(auto& i : items)
    {
        TU_MATCH(AST::Item, (i.data), (e),
        (None, ),
        (MacroInv,    BUG(i.data.span, "Resolve_Absolute_ImplItems - MacroInv");),
        (ExternBlock, BUG(i.data.span, "Resolve_Absolute_ImplItems - " << i.data.tag_str());),
        (Impl,        BUG(i.data.span, "Resolve_Absolute_ImplItems - " << i.data.tag_str());),
        (NegImpl,     BUG(i.data.span, "Resolve_Absolute_ImplItems - " << i.data.tag_str());),
        (Use,    BUG(i.data.span, "Resolve_Absolute_ImplItems - Use");),
        (Module, BUG(i.data.span, "Resolve_Absolute_ImplItems - Module");),
        (Crate , BUG(i.data.span, "Resolve_Absolute_ImplItems - Crate");),
        (Enum  , BUG(i.data.span, "Resolve_Absolute_ImplItems - Enum");),
        (Trait , BUG(i.data.span, "Resolve_Absolute_ImplItems - Trait");),
        (Struct, BUG(i.data.span, "Resolve_Absolute_ImplItems - Struct");),
        (Union , BUG(i.data.span, "Resolve_Absolute_ImplItems - Union");),
        (Type,
            DEBUG("Type - " << i.name);
            assert( e.params().ty_params().size() == 0 );
            assert( e.params().lft_params().size() == 0 );
            item_context.push( e.params(), GenericSlot::Level::Method, true );
            Resolve_Absolute_Generic(item_context,  e.params());

            Resolve_Absolute_Type( item_context, e.type() );

            item_context.pop( e.params(), true );
            ),
        (Function,
            DEBUG("Function - " << i.name);
            item_context.push( e.params(), GenericSlot::Level::Method );
            Resolve_Absolute_Generic(item_context,  e.params());

            Resolve_Absolute_Type( item_context, e.rettype() );
            for(auto& arg : e.args())
                Resolve_Absolute_Type( item_context, arg.second );

            {
                auto _h = item_context.enter_rootblock();
                item_context.push_block();
                for(auto& arg : e.args()) {
                    Resolve_Absolute_Pattern( item_context, false, arg.first );
                }

                Resolve_Absolute_Expr( item_context, e.code() );

                item_context.pop_block();
            }

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

// - For impl blocks
void Resolve_Absolute_ImplItems(Context& item_context,  ::std::vector< ::AST::Impl::ImplItem >& items)
{
    TRACE_FUNCTION_F("");
    for(auto& i : items)
    {
        TU_MATCH(AST::Item, (*i.data), (e),
        (None, ),
        (MacroInv, ),

        (Impl  , BUG(i.data->span, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (NegImpl, BUG(i.data->span, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (ExternBlock, BUG(i.data->span, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (Use   , BUG(i.data->span, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (Module, BUG(i.data->span, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (Crate , BUG(i.data->span, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (Enum  , BUG(i.data->span, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (Trait , BUG(i.data->span, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (Struct, BUG(i.data->span, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (Union , BUG(i.data->span, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (Type,
            DEBUG("Type - " << i.name);
            assert( e.params().ty_params().size() == 0 );
            assert( e.params().lft_params().size() == 0 );
            item_context.push( e.params(), GenericSlot::Level::Method, true );
            Resolve_Absolute_Generic(item_context,  e.params());

            Resolve_Absolute_Type( item_context, e.type() );

            item_context.pop( e.params(), true );
            ),
        (Function,
            DEBUG("Function - " << i.name);
            Resolve_Absolute_Function(item_context, e);
            ),
        (Static,
            DEBUG("Static - " << i.name);
            Resolve_Absolute_Type( item_context, e.type() );
            Resolve_Absolute_Expr( item_context, e.value() );
            )
        )
    }
}

void Resolve_Absolute_Function(Context& item_context, ::AST::Function& fcn)
{
    TRACE_FUNCTION_F("");
    item_context.push( fcn.params(), GenericSlot::Level::Method );
    Resolve_Absolute_Generic(item_context,  fcn.params());

    Resolve_Absolute_Type( item_context, fcn.rettype() );
    for(auto& arg : fcn.args())
        Resolve_Absolute_Type( item_context, arg.second );

    {
        auto _h = item_context.enter_rootblock();
        item_context.push_block();
        for(auto& arg : fcn.args()) {
            Resolve_Absolute_Pattern( item_context, false, arg.first );
        }

        Resolve_Absolute_Expr( item_context, fcn.code() );

        item_context.pop_block();
    }

    item_context.pop( fcn.params() );
}
void Resolve_Absolute_Static(Context& item_context, ::AST::Static& e)
{
    Resolve_Absolute_Type( item_context, e.type() );
    auto _h = item_context.enter_rootblock();
    Resolve_Absolute_Expr( item_context, e.value() );
}

void Resolve_Absolute_Struct(Context& item_context, ::AST::Struct& e)
{
    item_context.push( e.params(), GenericSlot::Level::Top );
    Resolve_Absolute_Generic(item_context,  e.params());

    TU_MATCH(::AST::StructData, (e.m_data), (s),
    (Unit,
        ),
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
}
void Resolve_Absolute_Union(Context& item_context, ::AST::Union& e)
{
    item_context.push( e.m_params, GenericSlot::Level::Top );
    Resolve_Absolute_Generic(item_context,  e.m_params);

    for(auto& field : e.m_variants) {
        Resolve_Absolute_Type(item_context,  field.m_type);
    }

    item_context.pop( e.m_params );
}
void Resolve_Absolute_Trait(Context& item_context, ::AST::Trait& e)
{
    item_context.push( e.params(), GenericSlot::Level::Top, true );
    Resolve_Absolute_Generic(item_context,  e.params());

    for(auto& st : e.supertraits()) {
        if( !st.ent.is_valid() ) {
            DEBUG("- ST 'static");
        }
        else {
            DEBUG("- ST " << st.ent);
            Resolve_Absolute_Path(item_context, st.sp, Context::LookupMode::Type,  st.ent);
        }
    }

    Resolve_Absolute_ImplItems(item_context, e.items());

    item_context.pop( e.params(), true );
}
void Resolve_Absolute_Enum(Context& item_context, ::AST::Enum& e)
{
    item_context.push( e.params(), GenericSlot::Level::Top );
    Resolve_Absolute_Generic(item_context,  e.params());

    for(auto& variant : e.variants())
    {
        TU_MATCH(::AST::EnumVariantData, (variant.m_data), (s),
        (Value,
            auto _h = item_context.enter_rootblock();
            Resolve_Absolute_Expr(item_context,  s.m_value);
            ),
        (Tuple,
            for(auto& field : s.m_sub_types) {
                Resolve_Absolute_Type(item_context,  field);
            }
            ),
        (Struct,
            for(auto& field : s.m_fields) {
                Resolve_Absolute_Type(item_context,  field.m_type);
            }
            )
        )
    }

    item_context.pop( e.params() );
}

void Resolve_Absolute_Mod(const ::AST::Crate& crate, ::AST::Module& mod) {
    Resolve_Absolute_Mod( Context { crate, mod }, mod );
}
void Resolve_Absolute_Mod( Context item_context, ::AST::Module& mod )
{
    TRACE_FUNCTION_F("mod="<<mod.path());

    for( auto& i : mod.items() )
    {
        TU_MATCH(AST::Item, (i.data), (e),
        (None,
            ),
        (MacroInv,
            ),
        (Use,
            ),
        (ExternBlock,
            for(auto& i2 : e.items())
            {
                TU_MATCH_DEF(AST::Item, (i2.data), (e2),
                (
                    BUG(i2.data.span, "Unexpected item in ExternBlock - " << i2.data.tag_str());
                    ),
                (None,
                    ),
                (Function,
                    Resolve_Absolute_Function(item_context, e2);
                    ),
                (Static,
                    Resolve_Absolute_Static(item_context, e2);
                    )
                )
            }
            ),
        (Impl,
            auto& def = e.def();
            DEBUG("impl " << def.trait().ent << " for " << def.type());
            if( !def.type().is_valid() )
            {
                DEBUG("---- MARKER IMPL for " << def.trait().ent);
                item_context.push(def.params(), GenericSlot::Level::Top);
                Resolve_Absolute_Generic(item_context,  def.params());
                assert( def.trait().ent.is_valid() );
                Resolve_Absolute_Path(item_context, def.trait().sp, Context::LookupMode::Type, def.trait().ent);

                if( e.items().size() != 0 ) {
                    ERROR(def.span(), E0000, "impl Trait for .. with methods");
                }

                item_context.pop(def.params());

                const_cast< ::AST::Trait*>(def.trait().ent.binding().as_Trait().trait_)->set_is_marker();
            }
            else
            {
                item_context.push_self( def.type() );
                item_context.push(def.params(), GenericSlot::Level::Top);
                Resolve_Absolute_Generic(item_context,  def.params());

                Resolve_Absolute_Type(item_context, def.type());
                if( def.trait().ent.is_valid() ) {
                    Resolve_Absolute_Path(item_context, def.trait().sp, Context::LookupMode::Type, def.trait().ent);
                }

                Resolve_Absolute_ImplItems(item_context,  e.items());

                item_context.pop(def.params());
                item_context.pop_self( def.type() );
            }
            ),
        (NegImpl,
            auto& impl_def = e;
            DEBUG("impl ! " << impl_def.trait().ent << " for " << impl_def.type());
            item_context.push_self( impl_def.type() );
            item_context.push(impl_def.params(), GenericSlot::Level::Top);
            Resolve_Absolute_Generic(item_context,  impl_def.params());

            Resolve_Absolute_Type(item_context, impl_def.type());
            if( !impl_def.trait().ent.is_valid() )
                BUG(impl_def.span(), "Encountered negative impl with no trait");
            Resolve_Absolute_Path(item_context, impl_def.trait().sp, Context::LookupMode::Type, impl_def.trait().ent);

            // No items

            item_context.pop(impl_def.params());
            item_context.pop_self( impl_def.type() );
            ),
        (Module,
            DEBUG("Module - " << i.name);
            Resolve_Absolute_Mod(item_context.m_crate, e);
            ),
        (Crate,
            // - Nothing
            ),
        (Enum,
            DEBUG("Enum - " << i.name);
            Resolve_Absolute_Enum(item_context, e);
            ),
        (Trait,
            DEBUG("Trait - " << i.name);
            Resolve_Absolute_Trait(item_context, e);
            ),
        (Type,
            DEBUG("Type - " << i.name);
            item_context.push( e.params(), GenericSlot::Level::Top, true );
            Resolve_Absolute_Generic(item_context,  e.params());

            Resolve_Absolute_Type( item_context, e.type() );

            item_context.pop( e.params(), true );
            ),
        (Struct,
            DEBUG("Struct - " << i.name);
            Resolve_Absolute_Struct(item_context, e);
            ),
        (Union,
            DEBUG("Union - " << i.name);
            Resolve_Absolute_Union(item_context, e);
            ),
        (Function,
            DEBUG("Function - " << i.name);
            Resolve_Absolute_Function(item_context, e);
            ),
        (Static,
            DEBUG("Static - " << i.name);
            Resolve_Absolute_Static(item_context, e);
            )
        )
    }

    // - Run through the indexed items and fix up those paths
    static Span sp;
    DEBUG("Imports (mod = " << mod.path() << ")");
    for(auto& i : mod.m_namespace_items) {
        if( i.second.is_import ) {
            Resolve_Absolute_Path(item_context, sp, Context::LookupMode::Namespace, i.second.path);
        }
    }
    for(auto& i : mod.m_type_items) {
        if( i.second.is_import ) {
            Resolve_Absolute_Path(item_context, sp, Context::LookupMode::Type, i.second.path);
        }
    }
    for(auto& i : mod.m_value_items) {
        if( i.second.is_import ) {
            Resolve_Absolute_Path(item_context, sp, Context::LookupMode::Constant, i.second.path);
        }
    }
}

void Resolve_Absolutise(AST::Crate& crate)
{
    Resolve_Absolute_Mod(crate, crate.root_module());
}


