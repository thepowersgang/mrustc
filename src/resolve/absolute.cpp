/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * resolve/absolute.cpp
 * - Convert all paths in AST into absolute form (or to the relevant local item)
 *
 * NOTE: This is the core of the 'resolve' pass.
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

#define FLAG_CONST_GENERIC  (1u << 31)

namespace
{
    struct GenericSlot
    {
        enum class Level
        {
            Top,
            Method,
            Hrb,
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
        RcString    name;
        Val value;
    };
    template<typename Val>
    struct NamedI
    {
        const Ident& name;
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
            GenericSlot::Level level;
            ::AST::GenericParams*   params_def; // TODO: What if it's HRBs?, they have a different type
            //::AST::HigherRankedBounds*  hrbs_def;
            ::std::vector< Named< GenericSlot > > types;
            ::std::vector< NamedI< GenericSlot > > constants;
            ::std::vector< NamedI< GenericSlot > > lifetimes;
            })
        );

        const ::AST::Crate&     m_crate;
        const ::AST::Module&    m_mod;
        ::std::vector<Ent>  m_name_context;
        struct PatternStackEnt {
            unsigned    first_arm_done = false;
            std::set<Ident> created_variables;
            std::set<Ident> first_arm_variables;
        };
        ::std::vector<PatternStackEnt>    m_pattern_stack;
        unsigned int m_var_count;
        unsigned int m_block_level;

        // Destination `GenericParams` for in_band_lifetimes
        ::AST::GenericParams* m_ibl_target_generics;

        Context(const ::AST::Crate& crate, const ::AST::Module& mod):
            m_crate(crate),
            m_mod(mod),
            m_var_count(~0u),
            m_block_level(0),
            m_ibl_target_generics(nullptr)
        {}

        void push(const ::AST::HigherRankedBounds& params) {
            auto e = Ent::make_Generic({ GenericSlot::Level::Hrb, nullptr /*, &params*/ });
            auto& data = e.as_Generic();

            for(size_t i = 0; i < params.m_lifetimes.size(); i ++)
            {
                data.lifetimes.push_back( NamedI<GenericSlot> { params.m_lifetimes[i].name(), GenericSlot { GenericSlot::Level::Hrb, static_cast<unsigned short>(i) } } );
            }

            m_name_context.push_back(mv$(e));
        }
        void push(/*const */::AST::GenericParams& params, GenericSlot::Level level, bool has_self=false) {
            auto   e = Ent::make_Generic({ level, &params });
            auto& data = e.as_Generic();

            if( has_self ) {
                //assert( level == GenericSlot::Level::Top );
                data.types.push_back( Named<GenericSlot> { "Self", GenericSlot { level, 0xFFFF } } );
                m_name_context.push_back( Ent::make_ConcreteSelf(nullptr) );
            }
            if( !params.m_params.empty() ) {
                unsigned short lft_idx = 0;
                unsigned short ty_idx = 0;
                unsigned short val_idx = 0;
                for(const auto& e : params.m_params)
                {
                    TU_MATCH_HDRA( (e), {)
                    TU_ARMA(None, param) {
                        }
                    TU_ARMA(Lifetime, lft) {
                        data.lifetimes.push_back( NamedI<GenericSlot> { lft.name(), GenericSlot { level, lft_idx } } );
                        lft_idx += 1;
                        }
                    TU_ARMA(Type, ty_def) {
                        data.types.push_back( Named<GenericSlot> { ty_def.name(), GenericSlot { level, ty_idx } } );
                        ty_idx += 1;
                        }
                    TU_ARMA(Value, val_def) {
                        data.constants.push_back( NamedI<GenericSlot> { val_def.name(), GenericSlot { level, val_idx } } );
                        val_idx += 1;
                        }
                    }
                }
            }

            m_name_context.push_back(mv$(e));
        }
        void pop(const ::AST::HigherRankedBounds& ) {
            if( !m_name_context.back().is_Generic() )
                BUG(Span(), "resolve/absolute.cpp - Context::pop(GenericParams) - Mismatched pop");
            m_name_context.pop_back();
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
                    if( false && e ) {
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

        const ::TypeRef* get_self_opt() const {
            for(auto it = m_name_context.rbegin(); it != m_name_context.rend(); ++ it)
            {
                if( const auto* e = it->opt_ConcreteSelf() )
                {
                    return *e;
                }
            }
            return nullptr;
        }

        void push_block() {
            m_block_level += 1;
            DEBUG("Push block to " << m_block_level);
        }
        unsigned int push_var(const Span& sp, const Ident& name) {
            if( m_var_count == ~0u ) {
                BUG(sp, "Assigning local when there's no variable context");
            }
            // If this variable is defined within a stack entry, then use it
            assert(!m_pattern_stack.empty());
            bool already_defined = m_pattern_stack.back().first_arm_done;
            for(auto it = m_pattern_stack.rbegin(); it != m_pattern_stack.rend(); ++it) {
                if( it->first_arm_variables.count(name) ) {
                    already_defined = true;
                    break;
                }
            }
            if( !m_pattern_stack.back().created_variables.insert(name).second ) {
                ERROR(sp, E0000, "Duplicate definition of `" << name << "` in pattern arm");
            }
            // Are we currently in the second (or later) arm of a split pattern
            if( already_defined )
            {
                if( !m_name_context.back().is_VarBlock() ) {
                    BUG(sp, "resolve/absolute.cpp - Context::push_var - No block");
                }
                auto& vb = m_name_context.back().as_VarBlock();
                for( const auto& v : vb.variables )
                {
                    if( v.first == name ) {
                        DEBUG("Arm defined var @ " << m_block_level << ": #" << v.second << " " << name);
                        return v.second;
                    }
                }
                ERROR(sp, E0000, "Mismatched bindings in pattern (`" << name << "` wasn't in the first arm)");
            }
            else
            {
                assert( m_block_level > 0 );
                if( m_name_context.empty() || !m_name_context.back().is_VarBlock() || m_name_context.back().as_VarBlock().level < m_block_level ) {
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
            m_pattern_stack.push_back(PatternStackEnt());
        }
        /// Freeze the set of pattern bindings
        void end_patbind_arm(const Span& sp) {
            auto& e = m_pattern_stack.back();
            if(e.first_arm_done)
            {
                if( e.first_arm_variables != e.created_variables ) {
                    ERROR(sp, E0000, "Mismatched bindings in pattern - [" << e.first_arm_variables << "] != [" << e.created_variables << "]");
                }
            }
            else
            {
                e.first_arm_variables = std::move(e.created_variables);
                e.first_arm_done = true;
            }
            e.created_variables.clear();
        }
        /// End a multiple-pattern binding state (unfreeze really)
        void end_patbind() {
            assert(!m_pattern_stack.empty());
            // Propagate the created variables to the next level up.
            if( m_pattern_stack.size() > 1 ) {
                const auto& cur = m_pattern_stack[m_pattern_stack.size() - 1];
                auto& next = m_pattern_stack[m_pattern_stack.size() - 2];
                for(auto& var : cur.first_arm_variables) {
                    next.created_variables.insert(std::move(var));
                }
            }
            m_pattern_stack.pop_back();
        }


        enum class LookupMode {
            Namespace,
            Type,
            Constant,
            PatternValue,
            //PatternAny,
            Variable,
        };
        static const char* lookup_mode_msg(LookupMode mode) {
            switch(mode)
            {
            case LookupMode::Namespace: return "path component";
            case LookupMode::Type:      return "type name";
            case LookupMode::PatternValue: return "pattern constant";
            case LookupMode::Constant:  return "constant name";
            case LookupMode::Variable:  return "variable name";
            }
            return "";
        }
        AST::Path lookup(const Span& sp, const RcString& name, const Ident::Hygiene& src_context, LookupMode mode) const {
            auto rv = this->lookup_opt(name, src_context, mode);
            if( !rv.is_valid() ) {
                switch(mode)
                {
                case LookupMode::Namespace: ERROR(sp, E0000, "Couldn't find path component '" << name << "'");
                case LookupMode::Type:      ERROR(sp, E0000, "Couldn't find type name '" << name << "'");
                case LookupMode::PatternValue:   ERROR(sp, E0000, "Couldn't find pattern value '" << name << "'");
                case LookupMode::Constant:  ERROR(sp, E0000, "Couldn't find constant name '" << name << "'");
                case LookupMode::Variable:  ERROR(sp, E0000, "Couldn't find variable name '" << name << "'");
                }
            }
            return rv;
        }
        static bool lookup_in_mod(const ::AST::Module& mod, const RcString& name, LookupMode mode,  ::AST::Path& path) {
            switch(mode)
            {
            case LookupMode::Namespace:
                {
                    auto v = mod.m_namespace_items.find(name);
                    if( v != mod.m_namespace_items.end() ) {
                        DEBUG("- " << mod.path() << " NS: Namespace " << v->second.path);
                        path = ::AST::Path( v->second.path );
                        return true;
                    }
                }
                {
                    auto v = mod.m_type_items.find(name);
                    if( v != mod.m_type_items.end() ) {
                        DEBUG("- " << mod.path() << " NS: Type " << v->second.path);
                        path = ::AST::Path( v->second.path );
                        return true;
                    }
                }
                break;

            case LookupMode::Type:
                {
                    auto v = mod.m_type_items.find(name);
                    if( v != mod.m_type_items.end() ) {
                        DEBUG("- " << mod.path() << " TY: Type " << v->second.path);
                        path = ::AST::Path( v->second.path );
                        return true;
                    }
                }
                // HACK: For `Enum::Var { .. }` patterns matching value variants
                {
                    auto v = mod.m_value_items.find(name);
                    if( v != mod.m_value_items.end() ) {
                        const auto& b = v->second.path.m_bindings.value;
                        if( /*const auto* be =*/ b.binding.opt_EnumVar() ) {
                            DEBUG("- " << mod.path() << " TY: Enum variant " << b.path);
                            path = ::AST::Path(b);
                            return true;
                        }
                    }
                }
                break;
            //case LookupMode::PatternAny:
            //    {
            //        auto v = mod.m_type_items.find(name);
            //        if( v != mod.m_type_items.end() ) {
            //            DEBUG("- TY: Type " << v->second.path);
            //            path = ::AST::Path( v->second.path );
            //            return true;
            //        }
            //        auto v2 = mod.m_value_items.find(name);
            //        if( v2 != mod.m_value_items.end() ) {
            //            const auto& b = v2->second.path.m_bindings.value;
            //            if( b.is_EnumVar() ) {
            //                DEBUG("- TY: Enum variant " << v2->second.path);
            //                path = ::AST::Path( v2->second.path );
            //                return true;
            //            }
            //        }
            //    }
            //    break;
            case LookupMode::PatternValue:
                {
                    auto v = mod.m_value_items.find(name);
                    if( v != mod.m_value_items.end() ) {
                        const auto& b = v->second.path.m_bindings.value;
                        switch( b.binding.tag() )
                        {
                        case ::AST::PathBinding_Value::TAG_EnumVar:
                        case ::AST::PathBinding_Value::TAG_Static:
                            DEBUG("- PV: Value " << v->second.path);
                            path = ::AST::Path( v->second.path );
                            return true;
                        case ::AST::PathBinding_Value::TAG_Struct: {
                            const auto& be = b.binding.as_Struct();
                            // TODO: Restrict this to unit-like structs
                            if( be.struct_ && !be.struct_->m_data.is_Unit() )
                                ;
                            else if( be.hir && !be.hir->m_data.is_Unit() )
                                ;
                            else
                            {
                                DEBUG("- " << mod.path() << " PV: Value " << b.path);
                                path = ::AST::Path(b);
                                return true;
                            }
                            break; }
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
                        DEBUG("- " << mod.path() << " C/V: Value " << v->second.path);
                        path = ::AST::Path( v->second.path );
                        return true;
                    }
                }
                break;
            }
            return false;
        }
        AST::Path lookup_opt(const RcString& name, const Ident::Hygiene& src_context, LookupMode mode) const {
            DEBUG("name=" << name <<", src_context=" << src_context);
            // NOTE: src_context may provide a module to search
            // TODO: This should be checked AFTER locals
            if( src_context.has_mod_path() )
            {
                const auto& mp = src_context.mod_path();
                DEBUG(mp);
                if(mp.crate != "")
                {
                    static Span sp;
                    // External crate path
                    ASSERT_BUG(sp, m_crate.m_extern_crates.count(mp.crate), "Crate not loaded for " << mp);
                    const auto& crate = m_crate.m_extern_crates.at(mp.crate);
                    const HIR::Module*  mod = &crate.m_hir->m_root_module;
                    for(const auto& n : mp.ents)
                    {
                        ASSERT_BUG(sp, mod->m_mod_items.count(n), "Node `" << n << "` missing in path " << mp);
                        const auto& i = *mod->m_mod_items.at(n);
                        ASSERT_BUG(sp, i.ent.is_Module(), "Node `" << n << "` not a module in path " << mp);
                        mod = &i.ent.as_Module();
                    }
                    AST::Path::Bindings bindings;
                    const HIR::SimplePath* true_path = nullptr;
                    switch(mode)
                    {
                    case LookupMode::Constant:
                    case LookupMode::PatternValue:
                    case LookupMode::Variable: {
                        auto it = mod->m_value_items.find(name);
                        if(it != mod->m_value_items.end()) {
                            const auto* item = &it->second->ent;
                            auto item_path = AST::AbsolutePath(mp.crate, mp.ents) + name;
                            if( item->is_Import() ) {
                                const auto& imp = item->as_Import();
                                // Set the true path (so the returned path is canonical)
                                true_path = &imp.path;
                                auto item_path = AST::AbsolutePath(imp.path.m_crate_name, imp.path.m_components) + name;
                                if(imp.is_variant) {
                                    const auto& enm = m_crate.m_extern_crates.at(imp.path.m_crate_name).m_hir
                                        ->get_enum_by_path(sp, imp.path, /*ignore_crate_name*/true, /*ignore_last*/true);
                                    bindings.value.set( item_path, AST::PathBinding_Value::make_EnumVar({nullptr, imp.idx, &enm}) );
                                    break;  // Break out of the switch
                                }
                                else {
                                    item = &m_crate.m_extern_crates.at(imp.path.m_crate_name).m_hir->get_valitem_by_path(sp, imp.path, true);
                                }
                            }
                            TU_MATCH_HDRA( (*item), {)
                            TU_ARMA(Function, e) {
                                bindings.value.set( item_path, AST::PathBinding_Value::make_Function({nullptr}) );
                                }
                            TU_ARMA(Static, e) {
                                bindings.value.set( item_path, AST::PathBinding_Value::make_Static({nullptr}) );
                                }
                            default:
                                TODO(sp, "Found value '" << name << "' for module path " << mp << " : " << it->second->ent.tag_str());
                            }
                        }
                        } break;
                    case LookupMode::Namespace:
                    case LookupMode::Type: {
                        auto it = mod->m_mod_items.find(name);
                        if(it != mod->m_mod_items.end()) {
                            TODO(sp, "Found type/mod '" << name << "' for module path " << mp);
                        }
                        } break;
                    }
                    // If any bindings were populated, then generate a path
                    if(bindings.has_binding()) {
                        auto rv = AST::Path(mp.crate, {});
                        if( true_path ) {
                            rv.m_class.as_Absolute().crate = true_path->m_crate_name;
                            for(const auto& e : true_path->m_components) {
                                rv.nodes().push_back( e );
                            }
                        }
                        else {
                            for(const auto& e : mp.ents) {
                                rv.nodes().push_back( e );
                            }
                        }
                        rv.m_bindings = std::move(bindings);
                        return rv;
                    }
                    // Fall through
                }
                else
                {
                    const AST::Module*  mod = &m_crate.root_module();
                    for(const auto& node : mp.ents)
                    {
                        const AST::Module* next = nullptr;
                        if( node.c_str()[0] == '#' ) {
                            char c;
                            unsigned int idx;
                            ::std::stringstream ss( node.c_str() );
                            ss >> c;
                            ss >> idx;
                            assert( idx < mod->anon_mods().size() );
                            assert( mod->anon_mods()[idx] );
                            next = mod->anon_mods()[idx].get();
                        }
                        else {
                            for(const auto& i : mod->m_items)
                            {
                                if( i->name == node ) {
                                    next = &i->data.as_Module();
                                    break;
                                }
                            }
                        }
                        ASSERT_BUG(Span(), next, "Failed to find module `" << node << "` in " << mod->path() << " for " << mp);
                        mod = next;
                    }
                    ::AST::Path rv;
                    if( this->lookup_in_mod(*mod, name, mode, rv) ) {
                        return rv;
                    }
                }
            }
            for(auto it = m_name_context.rbegin(); it != m_name_context.rend(); ++ it)
            {
                TU_MATCH_HDRA( (*it), {)
                TU_ARMA(Module, e) {
                    DEBUG("- Module " << e.mod->path());
                    ::AST::Path rv;
                    if( this->lookup_in_mod(*e.mod, name, mode,  rv) ) {
                        return rv;
                    }
                    }
                TU_ARMA(ConcreteSelf, e) {
                    DEBUG("- ConcreteSelf");
                    if( name == "Self" )
                    {
                        switch(mode)
                        {
                        case LookupMode::Type:
                        case LookupMode::Namespace:
                            // TODO: Want to return the type if handling a struct literal
                            if( false ) {
                                return ::AST::Path::new_ufcs_ty( e->clone(), ::std::vector< ::AST::PathNode>() );
                            }
                            else {
                                ::AST::Path rv(name);
                                rv.m_bindings.type.set(AST::AbsolutePath(), ::AST::PathBinding_Type::make_TypeParameter({ 0xFFFF }));
                                return rv;
                            }
                            break;
                        case LookupMode::Constant:
                        case LookupMode::Variable:
                            // TODO: Ensure validity? (I.e. that `Self` is a unit or tuple struct
                            if( e->m_data.is_Path() )
                            {
                                return *e->m_data.as_Path();
                            }
                        default:
                            break;
                        }
                    }
                    }
                TU_ARMA(VarBlock, e) {
                    DEBUG("- VarBlock");
                    assert(e.level <= m_block_level);
                    if( mode != LookupMode::Variable ) {
                        // ignore
                    }
                    else {
                        for( auto it2 = e.variables.rbegin(); it2 != e.variables.rend(); ++ it2 )
                        {
                            if( it2->first.name == name ) {
                                DEBUG("> Match: Hygiene " << it2->first.hygiene << " check against src_context");
                            }
                            if( it2->first.name == name && it2->first.hygiene.is_visible(src_context) ) {
                                ::AST::Path rv(name);
                                rv.bind_variable( it2->second );
                                return rv;
                            }
                        }
                    }
                    }
                TU_ARMA(Generic, e) {
                    DEBUG("- Generic");
                    switch(mode)
                    {
                    case LookupMode::Type:
                    case LookupMode::Namespace:
                        for( auto it2 = e.types.rbegin(); it2 != e.types.rend(); ++ it2 )
                        {
                            if( it2->name == name ) {
                                ::AST::Path rv(name);
                                rv.m_bindings.type.set(AST::AbsolutePath(), AST::PathBinding_Type::make_TypeParameter({ it2->value.to_binding() }));
                                return rv;
                            }
                        }
                        break;
                    case LookupMode::Variable:
                    case LookupMode::Constant:
                        for(auto it2 = e.constants.rbegin(); it2 != e.constants.rend(); ++it2)
                        {
                            if( it2->name == name ) {
                                ::AST::Path rv(name);
                                rv.m_bindings.value.set(AST::AbsolutePath(), AST::PathBinding_Value::make_Generic({ it2->value.to_binding() }));
                                return rv;
                            }
                        }
                        break;
                    default:
                        // ignore.
                        // TODO: Integer generics
                        break;
                    }
                    }
                }
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
                auto ct = coretype_fromstring(name.c_str());
                if( ct != CORETYPE_INVAL )
                {
                    return ::AST::Path::new_ufcs_ty( TypeRef(Span(), ct), ::std::vector< ::AST::PathNode>() );
                }
                } break;
            default:
                break;
            }

            // #![feature(extern_prelude)] - 2018-style extern paths
            if( mode == LookupMode::Namespace && TARGETVER_LEAST_1_29 /*&& m_crate.has_feature("extern_prelude")*/ )
            {
                DEBUG("Extern crates - " << AST::g_implicit_crates);
                auto it = AST::g_implicit_crates.find(name);
                if(it != AST::g_implicit_crates.end() )
                {
                    DEBUG("- Found '" << name << "' (= " << it->second << ")");
                    return AST::Path(it->second, {});
                }
            }

            return AST::Path();
        }

        unsigned int lookup_local(const Span& sp, const RcString name, LookupMode mode) {
            for(auto it = m_name_context.rbegin(); it != m_name_context.rend(); ++ it)
            {
                TU_MATCH_HDRA( (*it), {)
                TU_ARMA(Module, e) {
                    }
                TU_ARMA(ConcreteSelf, e) {
                    }
                TU_ARMA(VarBlock, e) {
                    if( mode == LookupMode::Variable ) {
                        DEBUG("- VarBlock lvl" << e.level);
                        for( auto it2 = e.variables.rbegin(); it2 != e.variables.rend(); ++ it2 )
                        {
                            // TODO: Hyginic lookup?
                            DEBUG(" > " << it2->first.name);
                            if( it2->first.name == name ) {
                                return it2->second;
                            }
                        }
                    }
                    }
                TU_ARMA(Generic, e) {
                    DEBUG("- Generic");
                    switch(mode)
                    {
                    case LookupMode::Type:
                        for( auto it2 = e.types.rbegin(); it2 != e.types.rend(); ++ it2 )
                        {
                            if( it2->name == name ) {
                                return it2->value.to_binding();
                            }
                        }
                        break;
                    case LookupMode::Variable:
                        for( auto it2 = e.constants.rbegin(); it2 != e.constants.rend(); ++ it2 )
                        {
                            if( it2->name == name ) {
                                //TODO(sp, "Return a reference to a constant generic '" << name << "'");
                                // Need to disambiguate it... could set a high bit
                                return it2->value.to_binding() | FLAG_CONST_GENERIC;
                            }
                        }
                        break;
                    default:
                        // ignore.
                        // TODO: Integer generics
                        break;
                    }
                    }
                }
            }

            ERROR(sp, E0000, "Unable to find local " << (mode == LookupMode::Variable ? "variable" : "type") << " '" << name << "'");
        }

        /// Clones the context, including only the module-level items (i.e. just the Module entries)
        Context clone_mod() const {
            auto rv = Context(this->m_crate, this->m_mod);
            for(const auto& v : m_name_context)
            {
                if(const auto* e = v.opt_Module())
                {
                    rv.m_name_context.push_back( Ent::make_Module(*e) );
                }
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
    case Context::LookupMode::PatternValue:  os << "PatternValue";    break;
    case Context::LookupMode::Constant: os << "Constant";   break;
    case Context::LookupMode::Variable: os << "Variable";   break;
    }
    return os;
}



void Resolve_Absolute_Path_BindAbsolute(Context& context, const Span& sp, Context::LookupMode& mode, ::AST::Path& path);
void Resolve_Absolute_Path(/*const*/ Context& context, const Span& sp, Context::LookupMode mode,  ::AST::Path& path);
void Resolve_Absolute_Lifetime(Context& context, const Span& sp, AST::LifetimeRef& type);
void Resolve_Absolute_Type(Context& context,  TypeRef& type);
void Resolve_Absolute_Expr(Context& context,  ::AST::Expr& expr);
void Resolve_Absolute_ExprNode(Context& context,  ::AST::ExprNode& node);
void Resolve_Absolute_Pattern(Context& context, bool allow_refutable, ::AST::Pattern& pat);
void Resolve_Absolute_Mod(const ::AST::Crate& crate, ::AST::Module& mod);
void Resolve_Absolute_Mod( Context item_context, ::AST::Module& mod );

void Resolve_Absolute_Function(Context& item_context, ::AST::Function& fcn);

void Resolve_Absolute_PathParams(/*const*/ Context& context, const Span& sp, ::AST::PathParams& args)
{
    for(auto& ent : args.m_entries)
    {
        TU_MATCH_HDRA( (ent), {)
        TU_ARMA(Null, _) {}
        TU_ARMA(Lifetime, l) {
            Resolve_Absolute_Lifetime(context, sp, l);
            }
        TU_ARMA(Type, t) {
            // A trivial path type might be refering to a generic value (e.g. `Foo<T,N>` where `N` is a const generic)
            if(t.m_data.is_Path() && t.m_data.as_Path()->is_trivial() )
            {
                auto p = t.m_data.as_Path()->m_class.as_Relative();
                // If type lookup fails
                auto new_path = context.lookup_opt(p.nodes[0].name(), p.hygiene, Context::LookupMode::Type);
                if(new_path == AST::Path()) {
                    // Try (constant) value lookup
                    auto new_path = context.lookup_opt(p.nodes[0].name(), p.hygiene, Context::LookupMode::Constant);
                    if(new_path != AST::Path()) {
                        // If that lookup succeeds, then create a value (and visit it - just in case)
                        ent = AST::PathParamEnt::make_Value(new AST::ExprNode_NamedValue(std::move(new_path)));
                        Resolve_Absolute_ExprNode(context, *ent.as_Value());
                    }
                    else {
                        // Otherwise, visit (which will most likely fail)
                        Resolve_Absolute_Type(context, t);
                    }
                }
                else {
                    // Normal type, update it then visit
                    *t.m_data.as_Path() = std::move(new_path);
                    Resolve_Absolute_Type(context, t);
                }
            }
            else
            {
                Resolve_Absolute_Type(context, t);
            }
            }
        TU_ARMA(Value, n) {
            Resolve_Absolute_ExprNode(context, *n);
            }
        TU_ARMA(AssociatedTyEqual, a) {
            Resolve_Absolute_Type(context, a.second);
            }
        TU_ARMA(AssociatedTyBound, a) {
            Resolve_Absolute_Path(context, sp, Context::LookupMode::Type, a.second);
            }
        }
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
        path = ::AST::Path::new_ufcs_ty( TypeRef(span, mv$(inner_path)), mv$(nodes) );
    }

    if(path.m_class.as_UFCS().type) {
        Resolve_Absolute_Type(context, *path.m_class.as_UFCS().type);
    }

    const auto& ufcs = path.m_class.as_UFCS();
    if( ufcs.nodes.size() == 0 ) {

        if( mode == Context::LookupMode::Type && (!ufcs.trait || *ufcs.trait == ::AST::Path()) ) {
            return ;
        }

        BUG(sp, "UFCS with no nodes encountered - " << path);
    }
    const auto& node = ufcs.nodes.at(0);

    if( ufcs.trait && ufcs.trait->is_valid() )
    {
        // Trait is specified, definitely a trait item
        // - Must resolve here
        const auto& pb = ufcs.trait->m_bindings.type.binding;
        if( ! pb.is_Trait() ) {
            ERROR(sp, E0000, "UFCS trait was not a trait - " << *ufcs.trait);
        }
        if( !pb.as_Trait().trait_ )
            return ;
        assert( pb.as_Trait().trait_ );
        const auto& tr = *pb.as_Trait().trait_;

        switch(mode)
        {
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
                TU_MATCH_HDRA( (item.data), {)
                default:
                    // TODO: Error
                TU_ARMA(Function, e) {
                    // Bind as trait method
                    path.m_bindings.value.set(ufcs.trait->m_bindings.type.path + item.name, AST::PathBinding_Value::make_Function({&e}));
                    }
                TU_ARMA(Static, e) {
                    // Resolve to asociated static
                    }
                }
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
    AST::Path split_into_crate(const Span& sp, AST::Path path, unsigned int start, const RcString& crate_name)
    {
        auto& nodes = path.nodes();
        AST::Path   np = AST::Path(crate_name, {});
        for(unsigned int i = start; i < nodes.size(); i ++)
        {
            np.nodes().push_back( mv$(nodes[i]) );
        }
        np.m_bindings = path.m_bindings.clone();
        return np;
    }
    AST::Path split_into_ufcs_ty(const Span& sp, const AST::Path& path, unsigned int i /*item_name_idx*/)
    {
        const auto& path_abs = path.m_class.as_Absolute();
        auto type_path = ::AST::Path( path );
        type_path.m_class.as_Absolute().nodes.resize( i+1 );
        //Resolve_Absolute_Path(

        auto new_path = ::AST::Path::new_ufcs_ty( ::TypeRef(sp, mv$(type_path)) );
        for( unsigned int j = i+1; j < path_abs.nodes.size(); j ++ )
            new_path.nodes().push_back( mv$(path_abs.nodes[j]) );

        DEBUG(path << " -> " << new_path);

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
        auto new_path = ::AST::Path::new_ufcs_ty( ::TypeRef(sp, mv$(type_path)) );
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

            TU_MATCH_HDRA( (it->second->ent), {)
            default:
                TODO(sp, "Unknown item type in path - " << i << " " << p << " - " << it->second->ent.tag_str());
            TU_ARMA(Enum, e) {
                if( i != p.m_components.size() - 2 ) {
                    ERROR(sp, E0000, "Enum as path component in unexpected location - " << p);
                }
                const auto& varname = p.m_components.back();
                auto var_idx = e.find_variant(varname);
                ASSERT_BUG(sp, var_idx != SIZE_MAX, "Extern crate import path points to non-present variant - " << p);

                // Construct output path (with same set of parameters)
                AST::Path   rv( p.m_crate_name, {} );
                rv.nodes().reserve( p.m_components.size() );
                for(const auto& c : p.m_components)
                    rv.nodes().push_back( AST::PathNode(c) );
                rv.nodes().back().args() = mv$( path.nodes().back().args() );
                auto ap = AST::AbsolutePath(p.m_crate_name, p.m_components);
                if( e.m_data.is_Data() && e.m_data.as_Data()[var_idx].is_struct ) {
                    rv.m_bindings.type.set( ap, ::AST::PathBinding_Type::make_EnumVar({nullptr, static_cast<unsigned>(var_idx), &e}) );
                }
                else {
                    rv.m_bindings.value.set( ap, ::AST::PathBinding_Value::make_EnumVar({nullptr, static_cast<unsigned>(var_idx), &e}) );
                }
                path = mv$(rv);

                return ;
                }
            TU_ARMA(Module, e) {
                hmod = &e;
                }
            }
        }

        ::AST::Path::Bindings   pb;

        const auto& name = p.m_components.back();
        auto ap = ::AST::AbsolutePath(p.m_crate_name, p.m_components);
        if( is_value )
        {
            auto it = hmod->m_value_items.find(name);
            if( it == hmod->m_value_items.end() )
                ERROR(sp, E0000, "Couldn't find final component of " << p);
            AST::PathBinding_Value  pbv;
            TU_MATCH_HDRA( (it->second->ent), {)
            TU_ARMA(Import, e) {
                // Wait? is this even valid?
                BUG(sp, "HIR Import item pointed to an import");
                }
            TU_ARMA(Constant, e) {
                pbv = ::AST::PathBinding_Value::make_Static({nullptr, nullptr});
                }
            TU_ARMA(Static, e) {
                pbv = ::AST::PathBinding_Value::make_Static({nullptr, &e});
                }
            TU_ARMA(StructConstant, e) {
                pbv = ::AST::PathBinding_Value::make_Struct({nullptr, &ext_crate.m_hir->get_typeitem_by_path(sp, e.ty, true).as_Struct()});
                }
            TU_ARMA(Function, e) {
                pbv = ::AST::PathBinding_Value::make_Function({nullptr/*, &e*/});
                }
            TU_ARMA(StructConstructor, e) {
                pbv = ::AST::PathBinding_Value::make_Struct({nullptr, &ext_crate.m_hir->get_typeitem_by_path(sp, e.ty, true).as_Struct()});
                }
            }
            pb.value.set( ::std::move(ap), ::std::move(pbv) );
        }
        else
        {
            auto it = hmod->m_mod_items.find(name);
            if( it == hmod->m_mod_items.end() )
                ERROR(sp, E0000, "Couldn't find final component of " << p);
            AST::PathBinding_Type   pbt;
            TU_MATCH_HDRA( (it->second->ent), {)
            TU_ARMA(Import, e) {
                // Wait? is this even valid?
                BUG(sp, "HIR Import item pointed to an import");
                }
            TU_ARMA(Module, e) {
                pbt = ::AST::PathBinding_Type::make_Module({nullptr, {&ext_crate, &e}});
                }
            TU_ARMA(Trait, e) {
                pbt = ::AST::PathBinding_Type::make_Trait({nullptr, &e});
                }
            TU_ARMA(TraitAlias, e) {
                pbt = ::AST::PathBinding_Type::make_TraitAlias({nullptr, &e});
                }
            TU_ARMA(TypeAlias, e) {
                pbt = ::AST::PathBinding_Type::make_TypeAlias({nullptr/*, &e*/});
                }
            TU_ARMA(ExternType, e) {
                pbt = ::AST::PathBinding_Type::make_TypeAlias({nullptr/*, &e*/});
                }
            TU_ARMA(Struct, e) {
                pbt = ::AST::PathBinding_Type::make_Struct({nullptr, &e});
                }
            TU_ARMA(Union, e) {
                pbt = ::AST::PathBinding_Type::make_Union({nullptr, &e});
                }
            TU_ARMA(Enum, e) {
                pbt = ::AST::PathBinding_Type::make_Enum({nullptr, &e});
                }
            }
            pb.type.set( ::std::move(ap), ::std::move(pbt) );
        }

        // Construct output path (with same set of parameters)
        AST::Path   rv( p.m_crate_name, {} );
        rv.nodes().reserve( p.m_components.size() );
        for(const auto& c : p.m_components)
            rv.nodes().push_back( AST::PathNode(c) );
        rv.nodes().back().args() = mv$( path.nodes().back().args() );
        rv.m_bindings = mv$(pb);
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
                path.m_bindings.type.set( {crate.m_name, {}}, ::AST::PathBinding_Type::make_Module({nullptr, {&crate, &crate.m_hir->m_root_module}}) );
                return ;
            default:
                TODO(sp, "Looking up a non-namespace, but pointed to crate root");
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

            TU_MATCH_HDRA( (it->second->ent), {)
            TU_ARMA(Import, e) {
                // - Update path then restart
                auto newpath = AST::Path(e.path.m_crate_name, {});
                for(const auto& n : e.path.m_components)
                    newpath.nodes().push_back( AST::PathNode(n) );
                if( newpath.nodes().empty() ) {
                    ASSERT_BUG(sp, n.args().is_empty(), "Params present, but name resolves to a crate root - " << path << " #" << i << " -> " << newpath);
                }
                else {
                    newpath.nodes().back().args() = mv$(path.nodes()[i].args());
                }
                for(unsigned int j = i + 1; j < path.nodes().size(); j ++)
                    newpath.nodes().push_back( mv$(path.nodes()[j]) );
                DEBUG("> Recurse with " << newpath);
                path = mv$(newpath);
                // TODO: Recursion limit
                Resolve_Absolute_Path_BindAbsolute(context, sp, mode, path);
                return ;
                }
            TU_ARMA(Module, e) {
                hmod = &e;
                }
            TU_ARMA(TraitAlias, e) {
                //for(const auto& trait_path_hir : e.m_traits)
                //{
                //}
                TODO(sp, "Path referring to a trait alias - " << path);
                }
            TU_ARMA(Trait, e) {
                AST::AbsolutePath   ap( crate.m_name, {} );
                for(unsigned int j = start; j <= i; j ++)
                    ap.nodes.push_back( path_abs.nodes[j].name() );
                AST::PathParams pp;
                if( !n.args().is_empty() ) {
                    pp = mv$(n.args());
                }
                else {
                    for(const auto& typ : e.m_params.m_types)
                    {
                        (void)typ;
                        pp.m_entries.push_back( ::TypeRef(sp) );
                    }
                }
                AST::Path   trait_path(ap, std::move(pp));
                trait_path.m_bindings.type.set( ::std::move(ap), ::AST::PathBinding_Type::make_Trait({nullptr, &e}) );

                ::AST::Path new_path;
                const auto& next_node = path_abs.nodes[i+1];
                // If the named item can't be found in the trait, fall back to it being a type binding
                // - What if this item is from a nested trait?
                bool found = false;
                switch( i+1 < path_abs.nodes.size() ? Context::LookupMode::Namespace : mode )
                {
                case Context::LookupMode::Namespace:
                case Context::LookupMode::Type:
                    found = (e.m_types.find( next_node.name() ) != e.m_types.end());
                case Context::LookupMode::PatternValue:
                case Context::LookupMode::Constant:
                case Context::LookupMode::Variable:
                    found = (e.m_values.find( next_node.name() ) != e.m_values.end());
                    break;
                }

                if( !found ) {
                    new_path = ::AST::Path::new_ufcs_ty( ::TypeRef(sp, mv$(trait_path)) );
                }
                else {
                    new_path = ::AST::Path::new_ufcs_trait( ::TypeRef(sp), mv$(trait_path) );
                }
                for( unsigned int j = i+1; j < path_abs.nodes.size(); j ++ )
                    new_path.nodes().push_back( mv$(path_abs.nodes[j]) );

                path = mv$(new_path);
                return Resolve_Absolute_Path_BindUFCS(context, sp, mode,  path);
                }
            case ::HIR::TypeItem::TAG_ExternType:
            case ::HIR::TypeItem::TAG_TypeAlias:
            case ::HIR::TypeItem::TAG_Struct:
            case ::HIR::TypeItem::TAG_Union:
                path = split_into_crate(sp, mv$(path), start,  crate.m_name);
                path = split_into_ufcs_ty(sp, mv$(path), i-start);
                return Resolve_Absolute_Path_BindUFCS(context, sp, mode,  path);
            TU_ARMA(Enum, e) {
                if( i+1 < path_abs.nodes.size() )
                {
                    auto& next_node = path_abs.nodes[i+1];
                    // If this refers to an enum variant, return the full path
                    // - Otherwise, assume it's an associated type?
                    auto idx = e.find_variant(next_node.name());
                    if( idx != SIZE_MAX )
                    {
                        if( i != path_abs.nodes.size() - 2 ) {
                            ERROR(sp, E0000, "Unexpected enum in path " << path);
                        }

                        AST::AbsolutePath   ap( crate.m_name, {} );
                        auto trait_path = ::AST::Path( crate.m_name, {} );
                        for(unsigned int j = start; j < path_abs.nodes.size(); j ++)
                            ap.nodes.push_back( path_abs.nodes[j].name() );

                        // NOTE: Type parameters for enums go after the _variant_
                        if( ! n.args().is_empty() ) {
                            if( next_node.args().is_empty() ) {
                                DEBUG("Moving type params from on the enum to the variant");
                                next_node.args() = std::move(n.args());
                            }
                            else {
                                ERROR(sp, E0000, "Type parameters were not expected here (enum params go on the variant)");
                            }
                        }

                        if( e.m_data.is_Data() && e.m_data.as_Data()[idx].is_struct ) {
                            path.m_bindings.type.set(ap, ::AST::PathBinding_Type::make_EnumVar({nullptr, static_cast<unsigned int>(idx), &e}));
                        }
                        else {
                            path.m_bindings.value.set(ap, ::AST::PathBinding_Value::make_EnumVar({nullptr, static_cast<unsigned int>(idx), &e}));
                        }
                        path = split_into_crate(sp, mv$(path), start,  crate.m_name);
                        return;
                    }
                }
                path = split_into_crate(sp, mv$(path), start,  crate.m_name);
                path = split_into_ufcs_ty(sp, mv$(path), i-start);
                return Resolve_Absolute_Path_BindUFCS(context, sp, mode,  path);
                }
            }
        }

        AST::AbsolutePath   ap( crate.m_name, {} );
        auto trait_path = ::AST::Path( crate.m_name, {} );
        for(unsigned int j = start; j < path_abs.nodes.size(); j ++)
            ap.nodes.push_back( path_abs.nodes[j].name() );

        const auto& name = path_abs.nodes.back().name();
        switch(mode)
        {
        // TODO: Don't bind to a Module if LookupMode::Type
        case Context::LookupMode::Namespace:
        case Context::LookupMode::Type:
            {
                auto v = hmod->m_mod_items.find(name);
                if( v != hmod->m_mod_items.end() ) {

                    ::AST::PathBinding_Type pbt;
                    TU_MATCH_HDRA( (v->second->ent), {)
                    TU_ARMA(Import, e) {
                        DEBUG("= Import " << e.path);
                        Resolve_Absolute_Path_BindAbsolute__hir_from_import(context, sp, false,  path, e.path);
                        return ;
                        }
                    TU_ARMA(Trait, e) {
                        pbt = ::AST::PathBinding_Type::make_Trait({nullptr, &e});
                        }
                    TU_ARMA(TraitAlias, e) {
                        pbt = ::AST::PathBinding_Type::make_TraitAlias({nullptr, &e});
                        }
                    TU_ARMA(Module, e) {
                        pbt = ::AST::PathBinding_Type::make_Module({nullptr, {&crate, &e}});
                        }
                    TU_ARMA(ExternType, e) {
                        pbt = ::AST::PathBinding_Type::make_TypeAlias({nullptr/*, &e*/});
                        }
                    TU_ARMA(TypeAlias, e) {
                        pbt = ::AST::PathBinding_Type::make_TypeAlias({nullptr/*, &e*/});
                        }
                    TU_ARMA(Enum, e) {
                        pbt = ::AST::PathBinding_Type::make_Enum({nullptr, &e});
                        }
                    TU_ARMA(Struct, e) {
                        pbt = ::AST::PathBinding_Type::make_Struct({nullptr, &e});
                        }
                    TU_ARMA(Union, e) {
                        pbt = ::AST::PathBinding_Type::make_Union({nullptr, &e});
                        }
                    }
                    path.m_bindings.type.set(::std::move(ap), ::std::move(pbt));
                    // Update path (trim down to `start` and set crate name)
                    path = split_into_crate(sp, mv$(path), start,  crate.m_name);
                    return ;
                }
            }
            break;

        case Context::LookupMode::PatternValue:
            {
                auto v = hmod->m_value_items.find(name);
                if( v != hmod->m_value_items.end() ) {
                    TU_MATCH_HDRA( (v->second->ent), {)
                    default:
                        DEBUG("Ignore - " << v->second->ent.tag_str());
                    TU_ARMA(StructConstant, e) {
                        auto ty_path = e.ty;
                        path.m_bindings.value.set( ::std::move(ap), ::AST::PathBinding_Value::make_Struct({nullptr, &crate.m_hir->get_struct_by_path(sp, ty_path)}) );
                        path = split_into_crate(sp, mv$(path), start,  crate.m_name);
                        return ;
                        }
                    TU_ARMA(Import, e) {
                        Resolve_Absolute_Path_BindAbsolute__hir_from_import(context, sp, true,  path, e.path);
                        return ;
                        }
                    TU_ARMA(Constant, e) {
                        // Bind and update path
                        path.m_bindings.value.set( ::std::move(ap), ::AST::PathBinding_Value::make_Static({nullptr, nullptr}) );
                        path = split_into_crate(sp, mv$(path), start,  crate.m_name);
                        return ;
                        }
                    }
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
                    ::AST::PathBinding_Value pbv;
                    TU_MATCH_HDRA( (v->second->ent), {)
                    TU_ARMA(Import, e) {
                        Resolve_Absolute_Path_BindAbsolute__hir_from_import(context, sp, true,  path, e.path);
                        return ;
                        }
                    TU_ARMA(Function, e) {
                        pbv = ::AST::PathBinding_Value::make_Function({nullptr/*, &e*/});
                        }
                    TU_ARMA(StructConstructor, e) {
                        auto ty_path = e.ty;
                        pbv = ::AST::PathBinding_Value::make_Struct({nullptr, &crate.m_hir->get_struct_by_path(sp, ty_path)});
                        }
                    TU_ARMA(StructConstant, e) {
                        auto ty_path = e.ty;
                        pbv = ::AST::PathBinding_Value::make_Struct({nullptr, &crate.m_hir->get_struct_by_path(sp, ty_path)});
                        }
                    TU_ARMA(Static, e) {
                        pbv = ::AST::PathBinding_Value::make_Static({nullptr, &e});
                        }
                    TU_ARMA(Constant, e) {
                        // Bind
                        pbv = ::AST::PathBinding_Value::make_Static({nullptr, nullptr});
                        }
                    }
                    path.m_bindings.value.set(::std::move(ap), ::std::move(pbv));
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

    if( path_abs.crate != "" && path_abs.crate != context.m_crate.m_crate_name_real ) {
        // TODO: Handle items from other crates (back-converting HIR paths)
        Resolve_Absolute_Path_BindAbsolute__hir_from(context, sp, mode, path,  context.m_crate.m_extern_crates.at(path_abs.crate), 0);
        return ;
    }


    const ::AST::Module*    mod = &context.m_crate.m_root_module;
    for(unsigned int i = 0; i < path_abs.nodes.size() - 1; i ++ )
    {
        auto& n = path_abs.nodes[i];

        if( n.name().c_str()[0] == '#' ) {
            if( ! n.args().is_empty() ) {
                ERROR(sp, E0000, "Type parameters were not expected here");
            }

            if( n.name() == "#" ) {
                TODO(sp, "magic module");
            }

            char c;
            unsigned int idx;
            ::std::stringstream ss( n.name().c_str() );
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

            TU_MATCH_HDRA( (name_ref.path.m_bindings.type.binding), {)
            default:
                ERROR(sp, E0000, "Encountered non-namespace item '" << n.name() << "' ("<<name_ref.path<<") in path " << path);
            TU_ARMA(TypeAlias, e) {
                path = split_replace_into_ufcs_path(sp, mv$(path), i,  name_ref.path);
                return Resolve_Absolute_Path_BindUFCS(context, sp, mode,  path);
                }
            TU_ARMA(Crate, e) {
                Resolve_Absolute_Path_BindAbsolute__hir_from(context, sp, mode, path,  *e.crate_, i+1);
                return ;
                }
            TU_ARMA(Trait, e) {
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
                        for(const auto& param : e.trait_->params().m_params)
                        {
                            TU_MATCH_HDRA( (param), {)
                            TU_ARMA(None, e) {
                                }
                            TU_ARMA(Lifetime, e) {
                                }
                            TU_ARMA(Type, typ) {
                                trait_path.nodes().back().args().m_entries.push_back( ::TypeRef(sp) );
                                }
                            TU_ARMA(Value, val) {
                                //trait_path.nodes().back().args().m_entries.push_back( ::TypeRef(sp) );
                                }
                            }
                        }
                    }
                    else {
                        for(const auto& typ : e.hir->m_params.m_types)
                        {
                            (void)typ;
                            trait_path.nodes().back().args().m_entries.push_back( ::TypeRef(sp) );
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
                    new_path = ::AST::Path::new_ufcs_ty( ::TypeRef(sp, mv$(trait_path)) );
                }
                else {
                    new_path = ::AST::Path::new_ufcs_trait( ::TypeRef(sp), mv$(trait_path) );
                }
                for( unsigned int j = i+1; j < path_abs.nodes.size(); j ++ )
                    new_path.nodes().push_back( mv$(path_abs.nodes[j]) );

                path = mv$(new_path);
                return Resolve_Absolute_Path_BindUFCS(context, sp, mode,  path);
                }
            TU_ARMA(Enum, e) {
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
                    auto& last_node = path_abs.nodes.back();
                    for( const auto& var : e.enum_->variants() ) {
                        if( var.m_name == last_node.name() ) {

                            if( i != path_abs.nodes.size() - 2 ) {
                                ERROR(sp, E0000, "Unexpected enum in path " << path);
                            }
                            // NOTE: Type parameters for enums go after the _variant_
                            if( ! n.args().is_empty() ) {
                                if( last_node.args().is_empty() ) {
                                    DEBUG("Moving type params from on the enum to the variant");
                                    last_node.args() = std::move(n.args());
                                }
                                else {
                                    ERROR(sp, E0000, "Type parameters were not expected here (enum params go on the variant)");
                                }
                            }

                            unsigned int idx = &var - &e.enum_->variants().front();

                            DEBUG("Bound to enum variant '" << var.m_name << "' (#" << idx << ")");
                            auto ap = name_ref.path.m_bindings.type.path + var.m_name;
                            if( var.m_data.is_Struct() ) {
                                path.m_bindings.type.set( ap, AST::PathBinding_Type::make_EnumVar({ e.enum_, idx }) );
                            }
                            else {
                                path.m_bindings.value.set( ap, AST::PathBinding_Value::make_EnumVar({ e.enum_, idx }) );
                            }
                            return;
                        }
                    }

                    path = split_replace_into_ufcs_path(sp, mv$(path), i,  name_ref.path);
                    return Resolve_Absolute_Path_BindUFCS(context, sp, mode,  path);
                }
                }
            TU_ARMA(Struct, e) {
                path = split_replace_into_ufcs_path(sp, mv$(path), i,  name_ref.path);
                return Resolve_Absolute_Path_BindUFCS(context, sp, mode,  path);
                }
            TU_ARMA(Union, e) {
                path = split_replace_into_ufcs_path(sp, mv$(path), i,  name_ref.path);
                return Resolve_Absolute_Path_BindUFCS(context, sp, mode,  path);
                }
            TU_ARMA(Module, e) {
                if( name_ref.is_import ) {
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
                }
            }
        }
    }

    // Set binding to binding of node in last module
    ::AST::Path tmp;
    if( ! Context::lookup_in_mod(*mod, path_abs.nodes.back().name(), mode,  tmp) ) {
        ERROR(sp, E0000, "Couldn't find " << Context::lookup_mode_msg(mode) << " '" << path_abs.nodes.back().name() << "' of " << path);
    }
    ASSERT_BUG(sp, tmp.m_bindings.has_binding(), "Lookup for " << path << " succeeded, but had no binding");

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

    TU_MATCH_HDRA( (path.m_class), {)
    TU_ARMA(Invalid, e) {
        BUG(sp, "Attempted resolution of invalid path");
        }
    TU_ARMA(Local, e) {
        // Nothing to do (TODO: Check that it's valid?)
        if( mode == Context::LookupMode::Variable ) {
            auto idx = context.lookup_local(sp, e.name, mode);
            if( idx >= FLAG_CONST_GENERIC )
            {
                path.m_bindings.value.set( {}, ::AST::PathBinding_Value::make_Generic({idx - FLAG_CONST_GENERIC}) );
            }
            else
            {
                path.m_bindings.value.set( {}, ::AST::PathBinding_Value::make_Variable({idx}) );
            }
        }
        else if( mode == Context::LookupMode::Type ) {
            path.bind_variable( context.lookup_local(sp, e.name, mode) );
        }
        else {
        }
        }
    TU_ARMA(Relative, e) {
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
            if( ! p.m_class.is_Local() && coretype_fromstring(e.nodes[0].name().c_str()) != CORETYPE_INVAL ) {
                if( const auto* pep = p.m_bindings.type.binding.opt_Module() ) {
                    const auto& pe = *pep;
                    bool found = false;
                    const auto& name = e.nodes[1].name();
                    if( !pe.module_ ) {
                        assert( pe.hir.mod );
                        const auto& mod = *pe.hir.mod;

                        switch( e.nodes.size() == 2 ? mode : Context::LookupMode::Namespace )
                        {
                        case Context::LookupMode::Namespace:
                        case Context::LookupMode::Type:
                            // TODO: Restrict if ::Type
                            if( mod.m_mod_items.find(name) != mod.m_mod_items.end() ) {
                                found = true;
                            }
                            break;
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
                        auto ct = coretype_fromstring(e.nodes[0].name().c_str());
                        p = ::AST::Path::new_ufcs_ty( TypeRef(Span(), ct), ::std::vector< ::AST::PathNode>() );
                    }

                    DEBUG("Primitive module hack yeilded " << p);
                }
            }

            if( e.nodes.size() > 1 )
            {
                // Only primitive types turn `Local` paths
                if( p.m_class.is_Local() ) {
                    p = ::AST::Path::new_ufcs_ty( TypeRef(sp, mv$(p)) );
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
                p.m_bindings = ::AST::Path::Bindings {};
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
        }
    TU_ARMA(Self, e) {
        DEBUG("- Self");
        const auto& mp_nodes = context.m_mod.path().nodes;
        // Ignore any leading anon modules
        unsigned int start_len = mp_nodes.size();
        while( start_len > 0 && mp_nodes[start_len-1].c_str()[0] == '#' )
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
        }
    TU_ARMA(Super, e) {
        DEBUG("- Super");
        // - Determine how many components of the `self` path to use
        const auto& mp_nodes = context.m_mod.path().nodes;
        assert( e.count >= 1 );
        // TODO: The first super should ignore any anon modules.
        unsigned int start_len = e.count > mp_nodes.size() ? 0 : mp_nodes.size() - e.count;
        while( start_len > 0 && mp_nodes[start_len-1].c_str()[0] == '#' )
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
        }
    TU_ARMA(Absolute, e) {
        DEBUG("- Absolute");
        // HACK: if the crate name starts with `=` it's a 2018 absolute path (references a crate loaded with `--extern`)
        if( /*context.m_crate.m_edition >= AST::Edition::Rust2018 &&*/ e.crate.c_str()[0] == '=' ) {
            // Absolute paths in 2018 edition are crate-prefixed?
            auto ec_it = AST::g_implicit_crates.find(e.crate.c_str() + 1);
            if(ec_it == AST::g_implicit_crates.end())
                ERROR(sp, E0000, "Unable to find external crate for path " << path);
            e.crate = ec_it->second;
        }
        // Nothing to do (TODO: Bind?)
        Resolve_Absolute_PathNodes(context, sp,  e.nodes);
        }
    TU_ARMA(UFCS, e) {
        DEBUG("- UFCS");
        Resolve_Absolute_Type(context, *e.type);
        if( e.trait && *e.trait != ::AST::Path() ) {
            Resolve_Absolute_Path(context, sp, Context::LookupMode::Type, *e.trait);
        }

        Resolve_Absolute_PathNodes(context, sp,  e.nodes);
        }
    }

    DEBUG("path = " << path);
    // TODO: Should this be deferred until the HIR?
    // - Doing it here so the HIR lowering has a bit more information
    // - Also handles splitting "absolute" paths into UFCS
    TU_MATCH_HDRA((path.m_class), {)
    default:
        BUG(sp, "Path wasn't absolutised correctly");
    TU_ARMA(Local, e) {
        if( !path.m_bindings.has_binding() )
        {
            TODO(sp, "Bind unbound local path - " << path);
        }
        }
    TU_ARMA(Absolute, e) {
        Resolve_Absolute_Path_BindAbsolute(context, sp, mode,  path);
        }
    TU_ARMA(UFCS, e) {
        Resolve_Absolute_Path_BindUFCS(context, sp, mode,  path);
        }
    }

    // TODO: Expand default type parameters?
    // - Helps with cases like PartialOrd<Self>, but hinders when the default is a hint (in expressions)

    // 
    if(const auto* e = path.m_class.opt_UFCS())
    {
        if( !e->nodes.empty() && (!e->trait || !e->trait->is_valid()) && e->type->m_data.is_Generic() && e->type->m_data.as_Generic().index == GENERIC_Self )
        {
            const auto& name = e->nodes.front().name();

            if(const auto* self_ty = context.get_self_opt())
            {
                // Check if we're in an enum
                if( const auto* ty_path = self_ty->m_data.opt_Path() )
                {
                    const auto& p = **ty_path;
                    if( const auto* pbe = p.m_bindings.type.binding.opt_Enum() )
                    {
                        if(pbe->enum_)
                        {
                            const auto& enm = *pbe->enum_;
                            auto it = std::find_if(enm.variants().begin(), enm.variants().end(), [&](const AST::EnumVariant& v){ return v.m_name == name; });
                            if( it != enm.variants().end() )
                            {
                                unsigned idx = it - enm.variants().begin();
                                auto p2 = p.m_bindings.type.path + name;
                                auto new_path = std::move(p);
                                new_path.append(name);
                                if( it->m_data.is_Struct() ) {
                                    new_path.m_bindings.type.set(p2, AST::PathBinding_Type::make_EnumVar({ &enm, idx }));
                                }
                                else {
                                    new_path.m_bindings.value.set(p2, AST::PathBinding_Value::make_EnumVar({ &enm, idx }));
                                }
                                DEBUG("UFCS of enum variant converted to Generic: " << new_path);
                                path = std::move(new_path);
                            }
                        }
                        else if(pbe->hir)
                        {
                            // TODO: Could be in an `impl Trait for Foo`
                        }
                        else
                        {
                        }
                    }
                }
            }
        }
    }
}

void Resolve_Absolute_Lifetime(Context& context, const Span& sp, AST::LifetimeRef& lft)
{
    TRACE_FUNCTION_FR("lft = " << lft, "lft = " << lft);
    if( lft.is_unbound() )
    {
        if( lft.name() == "static" )
        {
            lft = AST::LifetimeRef::new_static();
            return ;
        }

        if( lft.name() == "_" )
        {
            if( TARGETVER_MOST_1_19 )
            {
                ERROR(sp, E0000, "'_ is not a valid lifetime name in 1.19 mode");
            }
            // Note: '_ is just an explicit elided lifetime
            lft.set_binding(AST::LifetimeRef::BINDING_INFER);
            return ;
        }

        for(auto it = context.m_name_context.rbegin(); it != context.m_name_context.rend(); ++ it)
        {
            if( const auto* e = it->opt_Generic() )
            {
                for(const auto& l : e->lifetimes)
                {
                    // NOTE: Hygiene doesn't apply to lifetime params!
                    if( l.name.name == lft.name().name /*&& l.name.hygiene.is_visible(lft.name().hygiene)*/ )
                    {
                        lft.set_binding( l.value.index | (static_cast<int>(l.value.level) << 8) );
                        return ;
                    }
                }
            }
        }

        if( TARGETVER_LEAST_1_29 )
        {
            // If parsing a function header, add a new lifetime param to the function
            // - Does the same apply to impl headers? Yes it does.
            if( context.m_ibl_target_generics )
            {
                DEBUG("Considering in-band-lifetimes");
                ASSERT_BUG(sp, !context.m_name_context.empty(), "Name context stack is empty");
                auto it = context.m_name_context.rbegin();
                ASSERT_BUG(sp, it->is_Generic(), "Name context stack end not Generic, instead " << it->tag_str());
                while( it->as_Generic().level == GenericSlot::Level::Hrb ) {
                    it ++;
                    ASSERT_BUG(sp, it != context.m_name_context.rend(), "");
                    ASSERT_BUG(sp, it->is_Generic(), "Name context stack end not Generic, instead " << it->tag_str());
                }
                if( it->as_Generic().level != GenericSlot::Level::Hrb )
                {
                    auto& context_gen = it->as_Generic();
                    auto& def_gen = *context.m_ibl_target_generics;
                    auto level = context_gen.level;
                    // 1. Assert that the last item of `context.m_name_context` is Generic, and matches `m_ibl_target_generics`
                    ASSERT_BUG(sp, context_gen.lifetimes.size() + context_gen.types.size() + context_gen.constants.size() == def_gen.m_params.size(), "");
                    // 2. Add the new lifetime to both `m_ibl_target_generics` and the last entry in m_name_context
                    size_t idx = context_gen.lifetimes.size();
                    def_gen.add_lft_param(AST::LifetimeParam(sp, {}, lft.name()));
                    context_gen.lifetimes.push_back( NamedI<GenericSlot> { lft.name(), GenericSlot { level, static_cast<unsigned short>(idx) } } );
                    lft.set_binding( idx | (static_cast<int>(level) << 8) );
                    return ;
                }
            }
        }
        ERROR(sp, E0000, "Couldn't find lifetime " << lft);
    }
}

void Resolve_Absolute_Type(Context& context,  TypeRef& type)
{
    TRACE_FUNCTION_FR("type = " << type, "type = " << type);
    const auto& sp = type.span();

    if( type.m_data.is_Path() && type.m_data.as_Path()->m_bindings.type.binding.is_TypeParameter() ) {
        auto& e = type.m_data.as_Path()->m_bindings.type.binding.as_TypeParameter();
        type.m_data = TypeData::make_Generic({ type.m_data.as_Path()->as_trivial(), e.slot });
    }

    TU_MATCH_HDRA( (type.m_data), {)
    TU_ARMA(None, e) {
        // invalid type
        }
    TU_ARMA(Any, e) {
        // _ type
        }
    TU_ARMA(Unit, e) {
        }
    TU_ARMA(Bang, e) {
        // ! type
        }
    TU_ARMA(Macro, e) {
        BUG(sp, "Resolve_Absolute_Type - Encountered an unexpanded macro in type - " << type);
        }
    TU_ARMA(Primitive, e) {
        }
    TU_ARMA(Function, e) {
        context.push( e.info.hrbs );
        Resolve_Absolute_Type(context,  *e.info.m_rettype);
        for(auto& t : e.info.m_arg_types) {
            Resolve_Absolute_Type(context,  t);
        }
        context.pop( e.info.hrbs );
        }
    TU_ARMA(Tuple, e) {
        for(auto& t : e.inner_types)
            Resolve_Absolute_Type(context,  t);
        }
    TU_ARMA(Borrow, e) {
        Resolve_Absolute_Lifetime(context, type.span(), e.lifetime);
        Resolve_Absolute_Type(context,  *e.inner);
        }
    TU_ARMA(Pointer, e) {
        Resolve_Absolute_Type(context,  *e.inner);
        }
    TU_ARMA(Array, e) {
        Resolve_Absolute_Type(context,  *e.inner);
        if( e.size ) {
            auto _h = context.enter_rootblock();
            Resolve_Absolute_ExprNode(context,  *e.size);
        }
        }
    TU_ARMA(Generic, e) {
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
        }
    TU_ARMA(Path, e) {
        Resolve_Absolute_Path(context, type.span(), Context::LookupMode::Type, *e);
        if(auto* ufcs = e->m_class.opt_UFCS())
        {
            if( ufcs->nodes.size() == 0 /*&& ufcs->trait && *ufcs->trait == ::AST::Path()*/ ) {
                auto ty = mv$(*ufcs->type);
                type = mv$(ty);
                return ;
            }
            assert( ufcs->nodes.size() == 1);
        }

        if(e->m_bindings.type.binding.opt_Trait())
        {
            auto tp = Type_TraitPath();
            tp.path = std::move(e);
            auto ty = ::TypeRef( type.span(), ::make_vec1(mv$(tp)), {} );
            type = mv$(ty);
            return ;
        }
        //else if(auto* be = e->m_bindings.type.binding.opt_TypeParameter())
        //{
        //}
        }
    TU_ARMA(TraitObject, e) {
        for(auto& trait : e.traits) {
            context.push( trait.hrbs );
            Resolve_Absolute_Path(context, type.span(), Context::LookupMode::Type, *trait.path);
            context.pop(trait.hrbs);
        }
        for(auto& lft : e.lifetimes)
            Resolve_Absolute_Lifetime(context, type.span(), lft);
        }
    TU_ARMA(ErasedType, e) {
        for(auto& trait : e.traits) {
            context.push( trait.hrbs );
            Resolve_Absolute_Path(context, type.span(), Context::LookupMode::Type, *trait.path);
            context.pop(trait.hrbs);
        }
        for(auto& trait : e.maybe_traits) {
            context.push( trait.hrbs );
            Resolve_Absolute_Path(context, type.span(), Context::LookupMode::Type, *trait.path);
            context.pop(trait.hrbs);
        }
        for(auto& lft : e.lifetimes)
            Resolve_Absolute_Lifetime(context, type.span(), lft);
        }
    }
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

                this->context.start_patbind();
                // TODO: Save the context, ensure that each arm results in the same state.
                // - Or just an equivalent state
                // OR! Have a mode in the context that handles multiple bindings.
                for( auto& pat : arm.m_patterns )
                {
                    // If this isn't the first pattern, save the newly created bindings, roll back entire state, and check afterwards
                    Resolve_Absolute_Pattern(this->context, true,  pat);
                    this->context.end_patbind_arm(pat.span());
                }
                this->context.end_patbind();

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
                this->context.start_patbind();
                Resolve_Absolute_Pattern(this->context, true, node.m_pattern);
                this->context.end_patbind();
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
            this->context.start_patbind();
            Resolve_Absolute_Pattern(this->context, false, node.m_pat);
            this->context.end_patbind();
        }
        void visit(AST::ExprNode_IfLet& node) override {
            DEBUG("ExprNode_IfLet");
            node.m_value->visit( *this );

            this->context.push_block();

            this->context.start_patbind();
            for(auto& pat : node.m_patterns)
            {
                Resolve_Absolute_Pattern(this->context, true, pat);
                this->context.end_patbind_arm(pat.span());
            }
            this->context.end_patbind();

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
        void visit(AST::ExprNode_TypeAnnotation& node) override {
            DEBUG("ExprNode_TypeAnnotation");
            Resolve_Absolute_Type(this->context,  node.m_type);
            AST::NodeVisitorDef::visit(node);
        }
        void visit(AST::ExprNode_Closure& node) override {
            DEBUG("ExprNode_Closure");

            Resolve_Absolute_Type(this->context,  node.m_return);

            this->context.push_block();
            for( auto& arg : node.m_args ) {
                Resolve_Absolute_Type(this->context,  arg.second);
                this->context.start_patbind();
                Resolve_Absolute_Pattern(this->context, false,  arg.first);
                this->context.end_patbind();
            }

            node.m_code->visit(*this);

            this->context.pop_block();
        }
    } expr_iter(context);

    node.visit( expr_iter );
}

void Resolve_Absolute_Generic(Context& context, ::AST::GenericParams& params)
{
    for( auto& param : params.m_params )
    {
        TU_MATCH_HDRA( (param), {)
        TU_ARMA(None, _) {
            }
        TU_ARMA(Lifetime, param) {
            }
        TU_ARMA(Type, param) {
            Resolve_Absolute_Type(context, param.get_default());
            }
        TU_ARMA(Value, param) {
            Resolve_Absolute_Type(context, param.type());
            }
        }
    }
    for( auto& bound : params.m_bounds )
    {
        TU_MATCH(::AST::GenericBound, (bound), (e),
        (None,
            ),
        (Lifetime,
            Resolve_Absolute_Lifetime(context, bound.span, e.test);
            Resolve_Absolute_Lifetime(context, bound.span,e.bound);
            ),
        (TypeLifetime,
            Resolve_Absolute_Type(context, e.type);
            Resolve_Absolute_Lifetime(context, bound.span, e.bound);
            ),
        (IsTrait,
            context.push( e.outer_hrbs );
            Resolve_Absolute_Type(context, e.type);
            context.push( e.inner_hrbs );
            Resolve_Absolute_Path(context, bound.span, Context::LookupMode::Type, e.trait);
            context.pop( e.inner_hrbs );
            context.pop( e.outer_hrbs );
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
    TRACE_FUNCTION_FR("allow_refutable = " << allow_refutable << ", pat = " << pat, pat);
    for(auto& pb : pat.bindings()) {
        //if( !pat.data().is_Any() && ! allow_refutable )
        //    TODO(pat.span(), "Resolve_Absolute_Pattern - Encountered bound destructuring pattern");
        pb.m_slot = context.push_var( pat.span(), pb.m_name );
        DEBUG("- Binding #" << pb.m_slot << " '" << pb.m_name << "'");
    }

    TU_MATCH_HDRA( (pat.data()), {)
    TU_ARMA(MaybeBind, e) {
        auto name = mv$( e.name );
        // Attempt to resolve the name in the current namespace, and if it fails, it's a binding
        auto p = context.lookup_opt( name.name, name.hygiene, Context::LookupMode::PatternValue );
        if( p.is_valid() ) {
            Resolve_Absolute_Path(context, pat.span(), Context::LookupMode::PatternValue, p);
            pat.data() = AST::Pattern::Data::make_Value({ ::AST::Pattern::Value::make_Named(mv$(p)), AST::Pattern::Value() });
            DEBUG("MaybeBind resolved to " << pat);
        }
        else {
            pat.bindings().push_back(AST::PatternBinding(mv$(name), AST::PatternBinding::Type::MOVE, false));
            pat.bindings().back().m_slot = context.push_var( pat.span(), pat.bindings().back().m_name );
            pat.data() = AST::Pattern::Data::make_Any({});
            DEBUG("- Binding #" << pat.bindings().back().m_slot << " '" << pat.bindings().back().m_name << "' (was MaybeBind)");
        }
        }
    TU_ARMA(Macro, e) {
        BUG(pat.span(), "Resolve_Absolute_Pattern - Encountered Macro - " << pat);
        }
    TU_ARMA(Any, e) {
        // Ignore '_'
        }
    TU_ARMA(Box, e) {
        Resolve_Absolute_Pattern(context, allow_refutable,  *e.sub);
        }
    TU_ARMA(Ref, e) {
        Resolve_Absolute_Pattern(context, allow_refutable,  *e.sub);
        }
    TU_ARMA(Value, e) {
        if( ! allow_refutable )
        {
            // TODO: If this is a single value of a unit-like struct, accept
            BUG(pat.span(), "Resolve_Absolute_Pattern - Encountered refutable pattern where only irrefutable allowed - " << pat);
        }
        Resolve_Absolute_PatternValue(context, pat.span(), e.start);
        Resolve_Absolute_PatternValue(context, pat.span(), e.end);
        }
    TU_ARMA(ValueLeftInc, e) {
        if( ! allow_refutable )
        {
            // TODO: If this is a single value of a unit-like struct, accept
            BUG(pat.span(), "Resolve_Absolute_Pattern - Encountered refutable pattern where only irrefutable allowed - " << pat);
        }
        Resolve_Absolute_PatternValue(context, pat.span(), e.start);
        Resolve_Absolute_PatternValue(context, pat.span(), e.end);
        }
    TU_ARMA(Tuple, e) {
        for(auto& sp : e.start)
            Resolve_Absolute_Pattern(context, allow_refutable,  sp);
        for(auto& sp : e.end)
            Resolve_Absolute_Pattern(context, allow_refutable,  sp);
        }
    TU_ARMA(StructTuple, e) {
        Resolve_Absolute_Path(context, pat.span(), Context::LookupMode::Constant, e.path);
        for(auto& sp : e.tup_pat.start)
            Resolve_Absolute_Pattern(context, allow_refutable,  sp);
        for(auto& sp : e.tup_pat.end)
            Resolve_Absolute_Pattern(context, allow_refutable,  sp);
        }
    TU_ARMA(Struct, e) {
        // TODO: `Struct { .. }` patterns can match anything
        //if( e.sub_patterns.empty() && !e.is_exhaustive ) {
        //    auto rv = this->lookup_opt(name, src_context, mode);
        //}
        Resolve_Absolute_Path(context, pat.span(), Context::LookupMode::Type, e.path);
        for(auto& sp : e.sub_patterns)
            Resolve_Absolute_Pattern(context, allow_refutable,  sp.pat);
        }
    TU_ARMA(Slice, e) {
        // NOTE: Can be irrefutable (if the type is array)
        for(auto& sp : e.sub_pats)
            Resolve_Absolute_Pattern(context, allow_refutable,  sp);
        }
    TU_ARMA(SplitSlice, e) {
        // NOTE: Can be irrefutable (if the type is array)
        for(auto& sp : e.leading)
            Resolve_Absolute_Pattern(context, allow_refutable,  sp);
        if( e.extra_bind.is_valid() ) {
            e.extra_bind.m_slot = context.push_var( pat.span(), e.extra_bind.m_name );
        }
        for(auto& sp : e.trailing)
            Resolve_Absolute_Pattern(context, allow_refutable,  sp);
        }
    TU_ARMA(Or, e) {
        // TODO: Need to ensure that all arms bind the same set of variables
        context.start_patbind();
        for(auto& sp : e) {
            Resolve_Absolute_Pattern(context, allow_refutable,  sp);
            context.end_patbind_arm(sp.span());
        }
        context.end_patbind();
        }
    }
}

// - For traits
void Resolve_Absolute_ImplItems(Context& item_context,  ::AST::NamedList< ::AST::Item >& items)
{
    TRACE_FUNCTION_F("");
    for(auto& i : items)
    {
        TU_MATCH(AST::Item, (i.data), (e),
        (None, ),
        (MacroInv,
            //BUG(i.span, "Resolve_Absolute_ImplItems - MacroInv");
            ),
        (ExternBlock, BUG(i.span, "Resolve_Absolute_ImplItems - " << i.data.tag_str());),
        (Impl,        BUG(i.span, "Resolve_Absolute_ImplItems - " << i.data.tag_str());),
        (NegImpl,     BUG(i.span, "Resolve_Absolute_ImplItems - " << i.data.tag_str());),
        (Macro,    BUG(i.span, "Resolve_Absolute_ImplItems - " << i.data.tag_str());),
        (Use,    BUG(i.span, "Resolve_Absolute_ImplItems - Use");),
        (Module, BUG(i.span, "Resolve_Absolute_ImplItems - Module");),
        (Crate , BUG(i.span, "Resolve_Absolute_ImplItems - Crate");),
        (Enum  , BUG(i.span, "Resolve_Absolute_ImplItems - Enum");),
        (Trait , BUG(i.span, "Resolve_Absolute_ImplItems - " << i.data.tag_str());),
        (TraitAlias, BUG(i.span, "Resolve_Absolute_ImplItems - " << i.data.tag_str());),
        (Struct, BUG(i.span, "Resolve_Absolute_ImplItems - Struct");),
        (Union , BUG(i.span, "Resolve_Absolute_ImplItems - Union");),
        (Type,
            DEBUG("Type - " << i.name);
            assert( e.params().m_params.size() == 0 );
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
            auto _h = item_context.enter_rootblock();
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

        (Impl  , BUG(i.sp, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (NegImpl, BUG(i.sp, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (ExternBlock, BUG(i.sp, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (Macro , BUG(i.sp, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (Use   , BUG(i.sp, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (Module, BUG(i.sp, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (Crate , BUG(i.sp, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (Enum  , BUG(i.sp, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (Trait , BUG(i.sp, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (TraitAlias, BUG(i.sp, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (Struct, BUG(i.sp, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (Union , BUG(i.sp, "Resolve_Absolute_ImplItems - " << i.data->tag_str());),
        (Type,
            DEBUG("Type - " << i.name);
            assert( e.params().m_params.size() == 0 );
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
            auto _h = item_context.enter_rootblock();
            Resolve_Absolute_Expr( item_context, e.value() );
            )
        )
    }
}

void Resolve_Absolute_Function(Context& item_context, ::AST::Function& fcn)
{
    TRACE_FUNCTION_F("");
    item_context.push( fcn.params(), GenericSlot::Level::Method );
    item_context.m_ibl_target_generics = &fcn.params();
    DEBUG("- Generics");
    Resolve_Absolute_Generic(item_context,  fcn.params());

    DEBUG("- Prototype types");
    Resolve_Absolute_Type( item_context, fcn.rettype() );
    for(auto& arg : fcn.args())
        Resolve_Absolute_Type( item_context, arg.ty );
    item_context.m_ibl_target_generics = nullptr;

    DEBUG("- Body");
    {
        auto _h = item_context.enter_rootblock();
        item_context.push_block();
        for(auto& arg : fcn.args()) {
            item_context.start_patbind();
            Resolve_Absolute_Pattern( item_context, false, arg.pat );
            item_context.end_patbind();
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
        if( !st.ent.path->is_valid() ) {
            DEBUG("- ST 'static");
        }
        else {
            DEBUG("- ST " << st.ent.hrbs << *st.ent.path);
            item_context.push(st.ent.hrbs);
            Resolve_Absolute_Path(item_context, st.sp, Context::LookupMode::Type,  *st.ent.path);
            item_context.pop(st.ent.hrbs);
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

    for( auto& i : mod.m_items )
    {
        TU_MATCH_HDRA( (i->data), {)
        TU_ARMA(None, e) {
            }
        TU_ARMA(MacroInv, e) {
            }
        TU_ARMA(Use, e) {
            }
        TU_ARMA(Macro, e) {
            }
        TU_ARMA(ExternBlock, e) {
            for(auto& i2 : e.items())
            {
                TU_MATCH_DEF(AST::Item, (i2.data), (e2),
                (
                    BUG(i->span, "Unexpected item in ExternBlock - " << i2.data.tag_str());
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
            }
        TU_ARMA(Impl, e) {
            auto& def = e.def();
            if( !def.type().is_valid() )
            {
                TRACE_FUNCTION_F("impl " << def.trait().ent << " for ..");
                item_context.push(def.params(), GenericSlot::Level::Top);

                item_context.m_ibl_target_generics = &def.params();
                assert( def.trait().ent.is_valid() );
                Resolve_Absolute_Path(item_context, def.trait().sp, Context::LookupMode::Type, def.trait().ent);
                item_context.m_ibl_target_generics = nullptr;

                Resolve_Absolute_Generic(item_context,  def.params());

                if( e.items().size() != 0 ) {
                    ERROR(i->span, E0000, "impl Trait for .. with methods");
                }

                item_context.pop(def.params());

                // HACK: Mutate the source to indicate that it's an auto trait
                const_cast< ::AST::Trait*>(def.trait().ent.m_bindings.type.binding.as_Trait().trait_)->set_is_marker();
            }
            else
            {
                TRACE_FUNCTION_F("impl " << def.trait().ent << " for " << def.type());
                item_context.push_self( def.type() );
                item_context.push(def.params(), GenericSlot::Level::Top);

                item_context.m_ibl_target_generics = &def.params();
                Resolve_Absolute_Type(item_context, def.type());
                if( def.trait().ent.is_valid() ) {
                    Resolve_Absolute_Path(item_context, def.trait().sp, Context::LookupMode::Type, def.trait().ent);
                }
                item_context.m_ibl_target_generics = nullptr;

                Resolve_Absolute_Generic(item_context,  def.params());

                Resolve_Absolute_ImplItems(item_context,  e.items());

                item_context.pop(def.params());
                item_context.pop_self( def.type() );
            }
            }
        TU_ARMA(NegImpl, e) {
            auto& impl_def = e;
            TRACE_FUNCTION_F("impl ! " << impl_def.trait().ent << " for " << impl_def.type());
            item_context.push_self( impl_def.type() );
            item_context.push(impl_def.params(), GenericSlot::Level::Top);

            item_context.m_ibl_target_generics = &impl_def.params();
            Resolve_Absolute_Type(item_context, impl_def.type());
            if( !impl_def.trait().ent.is_valid() )
                BUG(i->span, "Encountered negative impl with no trait");
            Resolve_Absolute_Path(item_context, impl_def.trait().sp, Context::LookupMode::Type, impl_def.trait().ent);
            item_context.m_ibl_target_generics = nullptr;

            Resolve_Absolute_Generic(item_context,  impl_def.params());

            // No items

            item_context.pop(impl_def.params());
            item_context.pop_self( impl_def.type() );
            }
        TU_ARMA(Module, e) {
            DEBUG("Module - " << i->name);
            Resolve_Absolute_Mod(item_context.m_crate, e);
            }
        TU_ARMA(Crate, e) {
            // - Nothing
            }
        TU_ARMA(Enum, e) {
            DEBUG("Enum - " << i->name);
            Resolve_Absolute_Enum(item_context, e);
            }
        TU_ARMA(Trait, e) {
            DEBUG("Trait - " << i->name);
            Resolve_Absolute_Trait(item_context, e);
            }
        TU_ARMA(TraitAlias, e) {
            DEBUG("TraitAlias - " << i->name);
            item_context.push( e.params, GenericSlot::Level::Top, true );
            Resolve_Absolute_Generic(item_context,  e.params);

            for(auto& st : e.traits) {
                item_context.push(st.ent.hrbs);
                Resolve_Absolute_Path(item_context, st.sp, Context::LookupMode::Type,  *st.ent.path);
                item_context.pop(st.ent.hrbs);
            }

            item_context.pop(e.params, true);
            }
        TU_ARMA(Type, e) {
            DEBUG("Type - " << i->name);
            item_context.push( e.params(), GenericSlot::Level::Top, true );
            Resolve_Absolute_Generic(item_context,  e.params());

            Resolve_Absolute_Type( item_context, e.type() );

            item_context.pop( e.params(), true );
            }
        TU_ARMA(Struct, e) {
            DEBUG("Struct - " << i->name);
            Resolve_Absolute_Struct(item_context, e);
            }
        TU_ARMA(Union, e) {
            DEBUG("Union - " << i->name);
            Resolve_Absolute_Union(item_context, e);
            }
        TU_ARMA(Function, e) {
            DEBUG("Function - " << i->name);
            Resolve_Absolute_Function(item_context, e);
            }
        TU_ARMA(Static, e) {
            DEBUG("Static - " << i->name);
            Resolve_Absolute_Static(item_context, e);
            }
        }
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


