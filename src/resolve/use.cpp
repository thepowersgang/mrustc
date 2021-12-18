/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * resolve/use.cpp
 * - Absolutise and check all 'use' statements
 */
#include <main_bindings.hpp>
#include <ast/crate.hpp>
#include <ast/ast.hpp>
#include <ast/expr.hpp>
#include <hir/hir.hpp>
#include <stdspan.hpp>  // std::span

enum class Lookup
{
    Any,    // Allow binding to anything
    AnyOpt, // Allow binding to anything, but don't error on failure
    Type,   // Allow only type bindings
    Value,  // Allow only value bindings
};


void Resolve_Use_Mod(const ::AST::Crate& crate, ::AST::Module& mod, ::AST::Path path, ::std::span< const ::AST::Module* > parent_modules={});
::AST::Path::Bindings Resolve_Use_GetBinding(
    const Span& span, const ::AST::Crate& crate, const ::AST::AbsolutePath& source_mod_path,
    const ::AST::Path& path, ::std::span< const ::AST::Module* > parent_modules,
    bool types_only=false
    );

::AST::Path::Bindings Resolve_Use_GetBinding_Mod(
    const Span& span,
    const ::AST::Crate& crate, const ::AST::AbsolutePath& source_mod_path, const ::AST::Module& mod,
    const RcString& des_item_name,
    ::std::span< const ::AST::Module* > parent_modules,
    bool types_only = false
    );
::AST::Path::Bindings Resolve_Use_GetBinding__ext(const Span& span, const ::AST::Crate& crate, const AST::ExternCrate& ec, const ::HIR::Module& hmodr, const ::AST::Path& path, unsigned int start, AST::AbsolutePath ap={});
::AST::Path::Bindings Resolve_Use_GetBinding__ext(const Span& span, const ::AST::Crate& crate, const ::AST::Path& path,  const AST::ExternCrate& ec, unsigned int start);

void Resolve_Use(::AST::Crate& crate)
{
    Resolve_Use_Mod(crate, crate.m_root_module, ::AST::Path("", {}));
}

// - Convert self::/super:: paths into non-canonical absolute forms
::AST::Path Resolve_Use_AbsolutisePath(const Span& span, const AST::Crate& crate, const ::AST::Path& base_path, ::AST::Path path)
{
    TU_MATCH_HDRA( (path.m_class), {)
    TU_ARMA(Invalid, e) {
        // Should never happen
        BUG(span, "Invalid path class encountered");
        }
    TU_ARMA(Local, e) {
        // Wait, how is this already known?
        BUG(span, "Local path class in use statement");
        }
    TU_ARMA(UFCS, e) {
        // Wait, how is this already known?
        BUG(span, "UFCS path class in use statement");
        }
    TU_ARMA(Relative, e) {
        // How can this happen?
        DEBUG("Relative " << path);

        // 2018 edition and later: all extern crates are implicitly in the namespace.
        // - Fun fact: The equivalent logic for non-use is gated on TARGETVER_LEAST_1_29 (but use is still special until 2018)
        if( crate.m_edition >= AST::Edition::Rust2018 ) {
            const auto& name = e.nodes.at(0).name();
            auto ec_it = AST::g_implicit_crates.find(name);
            if(ec_it != AST::g_implicit_crates.end())
            {
                DEBUG("Found implict crate " << name);
                e.nodes.erase(e.nodes.begin());
                return AST::Path( ec_it->second, e.nodes);
            }
            else
            {
                DEBUG("No implict crate " << name);
            }
        }

        // If there's only one node, then check for primitives.
        if(path.nodes().size() == 1) {
            auto ct = coretype_fromstring(path.nodes()[0].name().c_str());
            if( ct != CORETYPE_INVAL ) {
                DEBUG("Found builtin type for `use` - " << path);
                // TODO: only if the item doesn't already exist?
                return AST::Path(CRATE_BUILTINS, path.nodes());
            }
        }

        // EVIL HACK: If the current module is an anon module, refer to the parent
        // TODO: Check if the desired item is in this module, 
        if( base_path.nodes().size() > 0 && base_path.nodes().back().name().c_str()[0] == '#' ) {

            std::vector<const AST::Module*>   parent_mods;
            const AST::Module* cur_mod = &crate.m_root_module;
            parent_mods.push_back(cur_mod);
            for( unsigned int i = 0; i < base_path.nodes().size(); i ++ )
            {
                const auto& name = base_path.nodes()[i].name();
                const AST::Module* next_mod = nullptr;

                // If the desired item is an anon module (starts with #) then parse and index
                if( name.size() > 0 && name.c_str()[0] == '#' ) {
                    unsigned int idx = 0;
                    if( ::std::sscanf(name.c_str(), "#%u", &idx) != 1 ) {
                        BUG(span, "Invalid anon path segment '" << name << "'");
                    }
                    ASSERT_BUG(span, idx < cur_mod->anon_mods().size(), "Invalid anon path segment '" << name << "'");
                    assert( cur_mod->anon_mods()[idx] );
                    next_mod = &*cur_mod->anon_mods()[idx];
                }
                else {
                    for(const auto& item : cur_mod->m_items) {
                        if( item->name == name && item->data.is_Module() ) {
                            next_mod = &item->data.as_Module();
                            break;
                        }
                    }
                }
                assert(next_mod);
                cur_mod = next_mod;
                if( name.c_str()[0] != '#' ) {
                    parent_mods.clear();
                }
                parent_mods.push_back(cur_mod);
            }
            parent_mods.pop_back();
            DEBUG("parent_mods.size() = " << parent_mods.size());

            while( !parent_mods.empty() && !Resolve_Use_GetBinding_Mod(span, crate, parent_mods[0]->path(), *cur_mod, e.nodes.front().name(), parent_mods, /*types_only*/e.nodes.size() > 1).has_binding() )
            {
                cur_mod = parent_mods.back();
                parent_mods.pop_back();
            }
            DEBUG("Found item in " << cur_mod->path());

            AST::Path   np("", {});
            for( unsigned int i = 0; i < cur_mod->path().nodes.size(); i ++ )
                np.nodes().push_back( cur_mod->path().nodes[i] );
            np += path;
            return np;
        }
        else {
            return base_path + path;
        }
        }
    TU_ARMA(Self, e) {
        DEBUG("Self " << path);
        // EVIL HACK: If the current module is an anon module, refer to the parent
        if( base_path.nodes().size() > 0 && base_path.nodes().back().name().c_str()[0] == '#' ) {
            AST::Path   np("", {});
            for( unsigned int i = 0; i < base_path.nodes().size() - 1; i ++ )
                np.nodes().push_back( base_path.nodes()[i] );
            np += path;
            return np;
        }
        else {
            return base_path + path;
        }
        }
    TU_ARMA(Super, e) {
        DEBUG("Super " << path);
        assert(e.count >= 1);
        AST::Path   np("", {});
        if( e.count > base_path.nodes().size() ) {
            ERROR(span, E0000, "Too many `super` components");
        }
        // TODO: Do this in a cleaner manner.
        unsigned int n_anon = 0;
        // Skip any anon modules in the way (i.e. if the current module is an anon, go to the parent)
        while( base_path.nodes().size() > n_anon && base_path.nodes()[ base_path.nodes().size()-1-n_anon ].name().c_str()[0] == '#' )
            n_anon ++;
        for( unsigned int i = 0; i < base_path.nodes().size() - e.count - n_anon; i ++ )
            np.nodes().push_back( base_path.nodes()[i] );
        np += path;
        return np;
        }
    TU_ARMA(Absolute, e) {
        DEBUG("Absolute " << path);
        // HACK: if the crate name starts with `=` it's a 2018 absolute path (references a crate loaded with `--extern`)
        if( crate.m_edition >= AST::Edition::Rust2018 && e.crate.c_str()[0] == '=' ) {
            // Absolute paths in 2018 edition are crate-prefixed?
            auto ec_it = AST::g_implicit_crates.find(e.crate.c_str() + 1);
            if(ec_it == AST::g_implicit_crates.end())
                ERROR(span, E0000, "Unable to find external crate for path " << path);
            e.crate = ec_it->second;
        }
        // Leave as is
        return path;
        }
    }
    throw "BUG: Reached end of Resolve_Use_AbsolutisePath";
}

void Resolve_Use_Mod(const ::AST::Crate& crate, ::AST::Module& mod, ::AST::Path path, ::std::span< const ::AST::Module* > parent_modules)
{
    TRACE_FUNCTION_F("path = " << path);

    for(auto& use_stmt : mod.m_items)
    {
        if( ! use_stmt->data.is_Use() )
            continue ;
        auto& use_stmt_data = use_stmt->data.as_Use();

        const Span& span = use_stmt_data.sp;
        for(auto& use_ent : use_stmt_data.entries)
        {
            TRACE_FUNCTION_F(use_ent);

            use_ent.path = Resolve_Use_AbsolutisePath(span, crate, path, mv$(use_ent.path));
            if( !use_ent.path.m_class.is_Absolute() )
                BUG(span, "Use path is not absolute after absolutisation");

            // NOTE: Use statements can refer to _three_ different items
            // - types/modules ("type namespace")
            // - values ("value namespace")
            // - macros ("macro namespace")
            // TODO: Have Resolve_Use_GetBinding return the actual path
            use_ent.path.m_bindings = Resolve_Use_GetBinding(span, crate, mod.path(), use_ent.path, parent_modules);
            if( !use_ent.path.m_bindings.has_binding() )
            {
                ERROR(span, E0000, "Unable to resolve `use` target " << use_ent.path);
            }
            DEBUG("'" << use_ent.name << "' = " << use_ent.path);

            // - If doing a glob, ensure the item type is valid
            if( use_ent.name == "" )
            {
                TU_MATCH_DEF(::AST::PathBinding_Type, (use_ent.path.m_bindings.type.binding), (e),
                (
                    ERROR(span, E0000, "Wildcard import of invalid item type - " << use_ent.path);
                    ),
                (Enum,
                    ),
                (Crate,
                    ),
                (Module,
                    )
                )
            }
            else
            {
            }
        }
    }

    struct NV:
        public AST::NodeVisitorDef
    {
        const AST::Crate& crate;
        ::std::vector< const AST::Module* >   parent_modules;

        NV(const AST::Crate& crate, const AST::Module& cur_module, ::std::span< const AST::Module* > parent_modules):
            crate(crate),
            parent_modules(parent_modules.begin(), parent_modules.end())
        {
            this->parent_modules.push_back( &cur_module );
        }

        void visit(AST::ExprNode_Block& node) override {
            if( node.m_local_mod ) {
                Resolve_Use_Mod(this->crate, *node.m_local_mod, node.m_local_mod->path(), this->parent_modules);

                parent_modules.push_back(&*node.m_local_mod);
            }
            AST::NodeVisitorDef::visit(node);
            if( node.m_local_mod ) {
                parent_modules.pop_back();
            }
        }
    } expr_iter(crate, mod, parent_modules);

    // TODO: Check that all code blocks are covered by these
    // - NOTE: Handle anon modules by iterating code (allowing correct item mappings)
    for(auto& ip : mod.m_items)
    {
        auto& i = *ip;
        TU_MATCH_HDRA( (i.data),  {)
        default:
            break;
        TU_ARMA(Module, e) {
            Resolve_Use_Mod(crate, e, path + i.name);
            }
        TU_ARMA(Impl, e) {
            for(auto& i : e.items())
            {
                TU_MATCH_DEF( AST::Item, (*i.data), (e),
                (
                    ),
                (Function,
                    if( e.code().is_valid() ) {
                        e.code().node().visit( expr_iter );
                    }
                    ),
                (Static,
                    if( e.value().is_valid() ) {
                        e.value().node().visit( expr_iter );
                    }
                    )
                )
            }
            }
        TU_ARMA(Trait, e) {
            for(auto& ti : e.items())
            {
                TU_MATCH_DEF( AST::Item, (ti.data), (e),
                (
                    BUG(Span(), "Unexpected item in trait - " << ti.data.tag_str());
                    ),
                (None,
                    // Deleted, ignore
                    ),
                (MacroInv,
                    // TODO: Should this already be deleted?
                    ),
                (Type,
                    ),
                (Function,
                    if(e.code().is_valid()) {
                        e.code().node().visit( expr_iter );
                    }
                    ),
                (Static,
                    if( e.value().is_valid() ) {
                        e.value().node().visit( expr_iter );
                    }
                    )
                )
            }
            }
        TU_ARMA(Function, e) {
            if( e.code().is_valid() ) {
                e.code().node().visit( expr_iter );
            }
            }
        TU_ARMA(Static, e) {
            if( e.value().is_valid() ) {
                e.value().node().visit( expr_iter );
            }
            }
        }
    }
}

::AST::Path::Bindings Resolve_Use_GetBinding_Mod(
        const Span& span,
        const ::AST::Crate& crate, const ::AST::AbsolutePath& source_mod_path, const ::AST::Module& mod,
        const RcString& des_item_name,
        ::std::span< const ::AST::Module* > parent_modules,
        bool types_only// = false
    )
{
    ::AST::Path::Bindings   rv;
    TRACE_FUNCTION_F(mod.path() << ", des_item_name=" << des_item_name);
    // If the desired item is an anon module (starts with #) then parse and index
    if( des_item_name.size() > 0 && des_item_name.c_str()[0] == '#' ) {
        unsigned int idx = 0;
        if( ::std::sscanf(des_item_name.c_str(), "#%u", &idx) != 1 ) {
            BUG(span, "Invalid anon path segment '" << des_item_name << "'");
        }
        ASSERT_BUG(span, idx < mod.anon_mods().size(), "Invalid anon path segment '" << des_item_name << "'");
        assert( mod.anon_mods()[idx] );
        const auto& m = *mod.anon_mods()[idx];
        rv.type.set(m.path(), ::AST::PathBinding_Type::make_Module({&m, nullptr}));
        return rv;
    }

    // Seach for the name defined in the module.
    for( const auto& ip : mod.m_items )
    {
        const auto& item = *ip;
        if( item.data.is_None() )
            continue ;

        if( item.name == des_item_name ) {
            auto p = mod.path() + item.name;
            DEBUG("Matching item: " << item.data.tag_str());
            TU_MATCH_HDRA( (item.data), {)
            TU_ARMA(None, _e) {
                // IMPOSSIBLE - Handled above
                }
            TU_ARMA(MacroInv, e) {
                BUG(span, "Hit MacroInv in use resolution");
                }
            TU_ARMA(Macro, e) {
                //rv.macro = ::AST::PathBinding_Macro::make_MacroRules({nullptr, e.get()});
                }
            TU_ARMA(Use, e) {
                continue; // Skip for now
                }
            TU_ARMA(Impl, e) {
                BUG(span, "Hit Impl in use resolution");
                }
            TU_ARMA(NegImpl, e) {
                BUG(span, "Hit NegImpl in use resolution");
                }
            TU_ARMA(ExternBlock, e) {
                BUG(span, "Hit Extern in use resolution");
                }
            TU_ARMA(Crate, e) {
                if( e.name != "" )
                {
                    ASSERT_BUG(span, crate.m_extern_crates.count(e.name), "Crate '" << e.name << "' not loaded");
                    rv.type.set( AST::AbsolutePath(e.name, {}), ::AST::PathBinding_Type::make_Crate({ &crate.m_extern_crates.at(e.name) }) );
                }
                else
                {
                    rv.type.set( AST::AbsolutePath(e.name, {}), ::AST::PathBinding_Type::make_Module({ &crate.m_root_module }) );
                }
                }
            TU_ARMA(Type, e) {
                rv.type.set(p, ::AST::PathBinding_Type::make_TypeAlias({&e}));
                }
            TU_ARMA(Trait, e) {
                rv.type.set(p, ::AST::PathBinding_Type::make_Trait({&e}));
                }
            TU_ARMA(TraitAlias, e) {
                rv.type.set(p, ::AST::PathBinding_Type::make_TraitAlias({&e}));
                }

            TU_ARMA(Function, e) {
                rv.value.set(p, ::AST::PathBinding_Value::make_Function({&e}));
                }
            TU_ARMA(Static, e) {
                rv.value.set(p, ::AST::PathBinding_Value::make_Static({&e}));
                }
            TU_ARMA(Struct, e) {
                // TODO: What happens with name collisions?
                if( !e.m_data.is_Struct() )
                    rv.value.set(p, ::AST::PathBinding_Value::make_Struct({&e}));
                rv.type.set(p, ::AST::PathBinding_Type::make_Struct({&e}));
                }
            TU_ARMA(Enum, e) {
                rv.type.set(p, ::AST::PathBinding_Type::make_Enum({&e}));
                }
            TU_ARMA(Union, e) {
                rv.type.set(p, ::AST::PathBinding_Type::make_Union({&e}));
                }
            TU_ARMA(Module, e) {
                rv.type.set(p, ::AST::PathBinding_Type::make_Module({&e}));
                }
            }
        }
    }
    // TODO: macros
    for(const auto& mac : mod.macros())
    {
        if( mac.name == des_item_name ) {
            rv.macro.set( mod.path() + mac.name, ::AST::PathBinding_Macro::make_MacroRules({ nullptr, &*mac.data }) );
            break;
        }
    }
    // TODO: If target is the crate root AND the crate exports macros with `macro_export`
    if( rv.macro.is_Unbound() && &mod == &crate.m_root_module )
    {
        auto it = crate.m_exported_macros.find(des_item_name);
        if(it != crate.m_exported_macros.end())
        {
            rv.macro.set( mod.path() + des_item_name, ::AST::PathBinding_Macro::make_MacroRules({ nullptr, &*it->second }) );
        }
    }

    if( types_only && !rv.type.is_Unbound() ) {
        return rv;
    }

    // Imports
    // - Explicitly named imports first (they take priority over anon imports)
    for( const auto& imp : mod.m_items )
    {
        if( ! imp->data.is_Use() )
            continue ;
        const auto& imp_data = imp->data.as_Use();
        for( const auto& imp_e : imp_data.entries )
        {
            const Span& sp2 = imp_e.sp;
            if( imp_e.name == des_item_name ) {
                DEBUG("- Named import " << imp_e.name << " = " << imp_e.path);
                if( !imp_e.path.m_bindings.has_binding() )
                {
                    DEBUG(" > Needs resolve p=" << &imp_e.path);
                    static ::std::vector<const ::AST::Path*> s_mods;
                    if( ::std::find(s_mods.begin(), s_mods.end(), &imp_e.path) == s_mods.end() )
                    {
                        s_mods.push_back(&imp_e.path);
                        rv.merge_from( Resolve_Use_GetBinding(sp2, crate, mod.path(), Resolve_Use_AbsolutisePath(sp2, crate, mod.path(), imp_e.path), parent_modules) );
                        s_mods.pop_back();
                    }
                    else
                    {
                        DEBUG("Recursion on path " << &imp_e.path << " " << imp_e.path);
                    }
                }
                else {
                    //out_path = imp_e.path;
                    rv.merge_from( imp_e.path.m_bindings.clone() );
                }
                continue ;
            }
        }
    }

    for( const auto& imp : mod.m_items )
    {
        if( ! imp->data.is_Use() )
            continue ;
        const auto& imp_data = imp->data.as_Use();
        for( const auto& imp_e : imp_data.entries )
        {
            const Span& sp2 = imp_e.sp;
            if( imp_e.name != "" )
                continue ;

            // TODO: Correct privacy rules (if the origin of this lookup can see this item)
            if( (imp->is_pub || mod.path().is_parent_of(source_mod_path)) )
            {
                DEBUG("- Search glob of " << imp_e.path << " in " << mod.path());
                // INEFFICIENT! Resolves and throws away the result (because we can't/shouldn't mutate here)
                ::AST::Path::Bindings   bindings_;
                const auto* bindings = &imp_e.path.m_bindings;
                if( bindings->type.is_Unbound() ) {
                    DEBUG("Temp resolving wildcard " << imp_e.path);
                    // Handle possibility of recursion
                    static ::std::vector<const ::AST::UseItem*>    resolve_stack_ptrs;
                    if( ::std::find(resolve_stack_ptrs.begin(), resolve_stack_ptrs.end(), &imp_data) == resolve_stack_ptrs.end() )
                    {
                        resolve_stack_ptrs.push_back( &imp_data );
                        bindings_ = Resolve_Use_GetBinding(sp2, crate, mod.path(), Resolve_Use_AbsolutisePath(sp2, crate, mod.path(), imp_e.path), parent_modules, /*type_only=*/true);
                        if( bindings_.type.is_Unbound() ) {
                            DEBUG("Recursion detected, skipping " << imp_e.path);
                            resolve_stack_ptrs.pop_back();
                            continue ;
                        }
                        // *waves hand* I'm not evil.
                        const_cast< ::AST::Path::Bindings&>( imp_e.path.m_bindings ) = bindings_.clone();
                        bindings = &bindings_;
                        resolve_stack_ptrs.pop_back();
                    }
                    else {
                        DEBUG("Recursion detected (resolve_stack_ptrs), skipping " << imp_e.path);
                        continue ;
                    }
                }
                else {
                    //out_path = imp_e.path;
                }

                TU_MATCH_HDRA( (bindings->type.binding), {)
                TU_ARMA(Crate, e) {
                    assert(e.crate_);
                    //const ::HIR::Module& hmod = e.crate_->m_hir->m_root_module;
                    rv.merge_from( Resolve_Use_GetBinding__ext(sp2, crate, AST::Path("", { AST::PathNode(des_item_name,{}) }), *e.crate_, 0) );
                    }
                TU_ARMA(Module, e) {
                    if( e.module_ ) {
                        // TODO: Prevent infinite recursion?
                        static ::std::vector<const AST::Module*>  s_use_glob_mod_stack;
                        if( ::std::find(s_use_glob_mod_stack.begin(), s_use_glob_mod_stack.end(), &*e.module_) == s_use_glob_mod_stack.end() )
                        {
                            s_use_glob_mod_stack.push_back( &*e.module_ );
                            rv.merge_from( Resolve_Use_GetBinding_Mod(span, crate, mod.path(), *e.module_, des_item_name, {}) );
                            s_use_glob_mod_stack.pop_back();
                        }
                        else
                        {
                            DEBUG("Recursion prevented of " << e.module_->path());
                        }
                    }
                    else if( e.hir.mod ) {
                        rv.merge_from( Resolve_Use_GetBinding__ext(sp2, crate, *e.hir.crate, *e.hir.mod, AST::Path("", { AST::PathNode(des_item_name,{}) }), 0, bindings->type.path) );
                    }
                    else {
                        BUG(span, "NULL module for binding on glob of " << imp_e.path);
                    }
                    }
                TU_ARMA(Enum, e) {
                    assert(e.enum_ || e.hir);
                    if( e.enum_ ) {
                        const auto& enm = *e.enum_;
                        unsigned int i = 0;
                        for(const auto& var : enm.variants())
                        {
                            if( var.m_name == des_item_name ) {
                                ::AST::Path::Bindings   tmp_rv;
                                if( var.m_data.is_Struct() )
                                    tmp_rv.type.set( bindings->type.path + des_item_name, ::AST::PathBinding_Type::make_EnumVar({ &enm, i }) );
                                else
                                    tmp_rv.value.set( bindings->type.path + des_item_name, ::AST::PathBinding_Value::make_EnumVar({ &enm, i }) );
                                rv.merge_from(tmp_rv);
                                break;
                            }
                            i ++;
                        }
                    }
                    else {
                        const auto& enm = *e.hir;
                        auto idx = enm.find_variant(des_item_name);
                        if( idx != SIZE_MAX )
                        {
                            ::AST::Path::Bindings   tmp_rv;
                            if( enm.m_data.is_Data() && enm.m_data.as_Data()[idx].is_struct ) {
                                tmp_rv.type.set( bindings->type.path + des_item_name, ::AST::PathBinding_Type::make_EnumVar({ nullptr, static_cast<unsigned>(idx), &enm }) );
                            }
                            else {
                                tmp_rv.value.set( bindings->type.path + des_item_name, ::AST::PathBinding_Value::make_EnumVar({ nullptr, static_cast<unsigned>(idx), &enm }) );
                            }
                            rv.merge_from(tmp_rv);
                            break;
                        }
                    }
                    } break;
                default:
                    BUG(sp2, "Wildcard import expanded to an invalid item class - " << bindings->type.binding.tag_str());
                    break;
                }
            }
        }
    }
    if( rv.has_binding() )
    {
        return rv;
    }

    if( mod.path().nodes.size() > 0 && mod.path().nodes.back().c_str()[0] == '#' ) {
        ASSERT_BUG(span, parent_modules.size() > 0, "Anon module with no parent modules - " << mod.path() );
        return Resolve_Use_GetBinding_Mod(span, crate, source_mod_path, *parent_modules.back(), des_item_name, parent_modules.subspan(0, parent_modules.size()-1));
    }
    else {
        //if( allow == Lookup::Any )
        //    ERROR(span, E0000, "Could not find node '" << des_item_name << "' in module " << mod.path());
        return ::AST::Path::Bindings();
    }
}

namespace {
    const ::HIR::Module* get_hir_mod_by_path(const Span& sp, const ::AST::Crate& crate, const ::HIR::SimplePath& path);

    const void* get_hir_modenum_by_path(const Span& sp, const ::AST::Crate& crate, const ::HIR::SimplePath& path, bool& is_enum)
    {
        const auto* hmod = &crate.m_extern_crates.at( path.m_crate_name ).m_hir->m_root_module;
        for(const auto& node : path.m_components)
        {
            auto it = hmod->m_mod_items.find(node);
            if( it == hmod->m_mod_items.end() )
                BUG(sp, "");
            TU_IFLET( ::HIR::TypeItem, (it->second->ent), Module, mod,
                hmod = &mod;
            )
            else TU_IFLET( ::HIR::TypeItem, (it->second->ent), Import, import,
                hmod = get_hir_mod_by_path(sp, crate, import.path);
                if( !hmod )
                    BUG(sp, "Import in module position didn't resolve as a module - " << import.path);
            )
            else TU_IFLET( ::HIR::TypeItem, (it->second->ent), Enum, enm,
                if( &node == &path.m_components.back() ) {
                    is_enum = true;
                    return &enm;
                }
                BUG(sp, "");
            )
            else {
                if( &node == &path.m_components.back() )
                    return nullptr;
                BUG(sp, "");
            }
        }
        is_enum = false;
        return hmod;
    }
    const ::HIR::Module* get_hir_mod_by_path(const Span& sp, const ::AST::Crate& crate, const ::HIR::SimplePath& path)
    {
        bool is_enum = false;
        auto rv = get_hir_modenum_by_path(sp, crate, path, is_enum);
        if( !rv )   return nullptr;
        ASSERT_BUG(sp, !is_enum, "");
        return reinterpret_cast< const ::HIR::Module*>(rv);
    }
}

::AST::Path::Bindings Resolve_Use_GetBinding__ext(
    const Span& span, const ::AST::Crate& crate,
    const AST::ExternCrate& hcrate, const ::HIR::Module& hmodr,
    const ::AST::Path& path, unsigned int start,
    AST::AbsolutePath ap
    )
{
    if(ap.crate == "")
        ap.crate = hcrate.m_name;

    ::AST::Path::Bindings   rv;
    //TRACE_FUNCTION_FR(path << " offset " << start, rv.value << rv.type << rv.macro);
    TRACE_FUNCTION_F(path << " offset " << start << " [" << ap << "]");
    const auto& nodes = path.nodes();
    const ::HIR::Module* hmod = &hmodr;

    for(unsigned int i = start; i < nodes.size(); i ++)
        ap.nodes.push_back( nodes[i].name() );

    if(nodes.size() == start) {
        rv.type.set( ap, ::AST::PathBinding_Type::make_Module({nullptr, { &hcrate, hmod } }) );
        return rv;
    }
    for(unsigned int i = start; i < nodes.size() - 1; i ++)
    {
        DEBUG("m_mod_items = {" << FMT_CB(ss, for(const auto& e : hmod->m_mod_items) ss << e.first << ", ";) << "}");
        auto it = hmod->m_mod_items.find(nodes[i].name());
        if( it == hmod->m_mod_items.end() ) {
            // BZZT!
            ERROR(span, E0000, "Unable to find path component " << nodes[i].name() << " in " << path << " (" << ap << ")");
        }
        DEBUG(i << " : " << nodes[i].name() << " = " << it->second->ent.tag_str());
        TU_MATCH_HDRA( (it->second->ent), {)
        default:
            ERROR(span, E0000, "Unexpected item type in import " << path << " @ " << i << " - " << it->second->ent.tag_str());
        TU_ARMA(Import, e) {
            // TODO: This is kinda like a duplicate of Resolve_Absolute_Path_BindAbsolute__hir_from ?
            bool is_enum = false;
            auto ptr = get_hir_modenum_by_path(span, crate, e.path, is_enum);
            if( !ptr )
                BUG(span, "Path component " << nodes[i].name() << " pointed to non-module (" << path << ")");
            if( is_enum ) {
                const auto& enm = *reinterpret_cast<const ::HIR::Enum*>(ptr);
                i += 1;
                if( i != nodes.size() - 1 ) {
                    ERROR(span, E0000, "Encountered enum at unexpected location in import");
                }
                const auto& name = nodes[i].name();

                auto idx = enm.find_variant(name);
                if( idx == SIZE_MAX ) {
                    ERROR(span, E0000, "Unable to find variant " << path);
                }
                ap.crate = e.path.m_crate_name;
                ap.nodes = e.path.m_components;
                ap.nodes.push_back(name);
                if( enm.m_data.is_Data() && enm.m_data.as_Data()[idx].is_struct ) {
                    rv.type.set(ap, ::AST::PathBinding_Type::make_EnumVar({ nullptr, static_cast<unsigned int>(idx), &enm }));
                }
                else {
                    rv.value.set(ap, ::AST::PathBinding_Value::make_EnumVar({ nullptr, static_cast<unsigned int>(idx), &enm }));
                }
                return rv;
            }
            else {
                hmod = reinterpret_cast<const ::HIR::Module*>(ptr);
            }
            }
        TU_ARMA(Module, e) {
            hmod = &e;
            }
        TU_ARMA(Enum, e) {
            i += 1;
            if( i != nodes.size() - 1 ) {
                ERROR(span, E0000, "Encountered enum at unexpected location in import");
            }
            const auto& name = nodes[i].name();

            auto idx = e.find_variant(name);
            if(idx == SIZE_MAX) {
                ERROR(span, E0000, "Unable to find variant " << path);
            }
            if( e.m_data.is_Data() && e.m_data.as_Data()[idx].is_struct ) {
                rv.type.set(ap, ::AST::PathBinding_Type::make_EnumVar({ nullptr, static_cast<unsigned int>(idx), &e }) );
            }
            else {
                rv.value.set(ap, ::AST::PathBinding_Value::make_EnumVar({ nullptr, static_cast<unsigned int>(idx), &e }) );
            }
            return rv;
            }
        }
    }
    // > Found the target module
    
    // - namespace/type items
    {
        auto it = hmod->m_mod_items.find(nodes.back().name());
        if( it == hmod->m_mod_items.end() )
        {
            DEBUG("E: : Types = " << FMT_CB(ss, for(const auto& e : hmod->m_mod_items){ ss << e.first << ":" << e.second->ent.tag_str() << ","; }));
        }
        else if( !it->second->publicity.is_global() )
        {
            DEBUG("E : Mod " << nodes.back().name() << " = " << it->second->ent.tag_str() << " [private]");
        }
        else
        {
            const auto* item_ptr = &it->second->ent;
            auto ap2 = ap; auto ap = ap2;
            DEBUG("E : Mod " << nodes.back().name() << " = " << item_ptr->tag_str());
            if( item_ptr->is_Import() ) {
                const auto& e = item_ptr->as_Import();
                ap = AST::AbsolutePath(e.path.m_crate_name, e.path.m_components);
                const auto& ec = crate.m_extern_crates.at( e.path.m_crate_name );
                // This doesn't need to recurse - it can just do a single layer (as no Import should refer to another)
                if( e.is_variant ) {
                    auto p = e.path;
                    p.m_components.pop_back();
                    const auto& enm = ec.m_hir->get_typeitem_by_path(span, p, true).as_Enum();
                    assert(e.idx < enm.num_variants());
                    rv.type.set( ap, ::AST::PathBinding_Type::make_EnumVar({ nullptr, e.idx, &enm }) );
                }
                else if( e.path.m_components.empty() )
                {
                    rv.type.set( ap, ::AST::PathBinding_Type::make_Module({nullptr, {&ec, &ec.m_hir->m_root_module}}) );
                }
                else
                {
                    item_ptr = &ec.m_hir->get_typeitem_by_path(span, e.path, true);    // ignore_crate_name=true
                }
            }
            else {
            }
            if( rv.type.is_Unbound() )
            {
                TU_MATCHA( (*item_ptr), (e),
                (Import,
                    BUG(span, "Recursive import in " << path << " - " << it->second->ent.as_Import().path << " -> " << e.path);
                    ),
                (Module,
                    rv.type.set( ap, ::AST::PathBinding_Type::make_Module({nullptr, {&hcrate, &e}}) );
                    ),
                (TypeAlias,
                    rv.type.set( ap, ::AST::PathBinding_Type::make_TypeAlias({nullptr}) );
                    ),
                (ExternType,
                    rv.type.set( ap, ::AST::PathBinding_Type::make_TypeAlias({nullptr}) );   // Lazy.
                    ),
                (Enum,
                    rv.type.set( ap, ::AST::PathBinding_Type::make_Enum({nullptr, &e}) );
                    ),
                (Struct,
                    rv.type.set( ap, ::AST::PathBinding_Type::make_Struct({nullptr, &e}) );
                    ),
                (Union,
                    rv.type.set( ap, ::AST::PathBinding_Type::make_Union({nullptr, &e}) );
                    ),
                (Trait,
                    rv.type.set( ap, ::AST::PathBinding_Type::make_Trait({nullptr, &e}) );
                    ),
                (TraitAlias,
                    rv.type.set( ap, ::AST::PathBinding_Type::make_TraitAlias({nullptr, &e}) );
                    )
                )
            }
        }
    }
    // - Values
    {
        auto it = hmod->m_value_items.find(nodes.back().name());
        if( it ==  hmod->m_value_items.end() )
        {
            DEBUG("E : Values = " << FMT_CB(ss, for(const auto& e : hmod->m_value_items){ ss << e.first << ":" << e.second->ent.tag_str() << ","; }));
        }
        else if( !it->second->publicity.is_global() )
        {
            DEBUG("E : Value " << nodes.back().name() << " = " << it->second->ent.tag_str() << " [private]");
        }
        else
        {
            const auto* item_ptr = &it->second->ent;
            auto ap2 = ap; auto ap = ap2;
            DEBUG("E : Value " << nodes.back().name() << " = " << item_ptr->tag_str());
            if( item_ptr->is_Import() ) {
                const auto& e = item_ptr->as_Import();
                ap = AST::AbsolutePath(e.path.m_crate_name, e.path.m_components);
                // This doesn't need to recurse - it can just do a single layer (as no Import should refer to another)
                const auto& ec = crate.m_extern_crates.at( e.path.m_crate_name );
                if( e.is_variant )
                {
                    auto p = e.path;
                    p.m_components.pop_back();
                    const auto& enm = ec.m_hir->get_typeitem_by_path(span, p, true).as_Enum();
                    assert(e.idx < enm.num_variants());
                    rv.value.set( ap, ::AST::PathBinding_Value::make_EnumVar({ nullptr, e.idx, &enm }) );
                }
                else
                {
                    item_ptr = &ec.m_hir->get_valitem_by_path(span, e.path, true);    // ignore_crate_name=true
                }
            }
            if( rv.value.is_Unbound() )
            {
                TU_MATCH_HDRA( (*item_ptr), {)
                TU_ARMA(Import, e) {
                    BUG(span, "Recursive import in " << path << " - " << it->second->ent.as_Import().path << " -> " << e.path);
                    }
                TU_ARMA(Constant, e) {
                    rv.value.set( ap, ::AST::PathBinding_Value::make_Static({ nullptr }) );
                    }
                TU_ARMA(Static, e) {
                    rv.value.set( ap, ::AST::PathBinding_Value::make_Static({ nullptr }) );
                    }
                // TODO: What happens if these two refer to an enum constructor?
                TU_ARMA(StructConstant, e) {
                    ASSERT_BUG(span, crate.m_extern_crates.count(e.ty.m_crate_name), "Crate '" << e.ty.m_crate_name << "' not loaded for " << e.ty);
                    rv.value.set( ap, ::AST::PathBinding_Value::make_Struct({ nullptr, &crate.m_extern_crates.at(e.ty.m_crate_name).m_hir->get_typeitem_by_path(span, e.ty, true).as_Struct() }) );
                    }
                TU_ARMA(StructConstructor, e) {
                    ASSERT_BUG(span, crate.m_extern_crates.count(e.ty.m_crate_name), "Crate '" << e.ty.m_crate_name << "' not loaded for " << e.ty);
                    rv.value.set( ap, ::AST::PathBinding_Value::make_Struct({ nullptr, &crate.m_extern_crates.at(e.ty.m_crate_name).m_hir->get_typeitem_by_path(span, e.ty, true).as_Struct() }) );
                    }
                TU_ARMA(Function, e) {
                    rv.value.set( ap, ::AST::PathBinding_Value::make_Function({ nullptr }) );
                    }
                }
            }
        }
    }
    // - Macros
    {
        auto it = hmod->m_macro_items.find(nodes.back().name());
        if( it == hmod->m_macro_items.end() )
        {
            DEBUG("E : Macros = " << FMT_CB(ss, for(const auto& e : hmod->m_macro_items){ ss << e.first << ":" << e.second->ent.tag_str() << ","; }));
        }
        else if( !it->second->publicity.is_global() )
        {
            DEBUG("E : Macro " << nodes.back().name() << " = " << it->second->ent.tag_str() << " [private]");
        }
        else
        {
            const auto* item_ptr = &it->second->ent;
            auto ap2 = ap; auto ap = ap2;
            DEBUG("E : Macro " << nodes.back().name() << " = " << item_ptr->tag_str());

            if( const auto* imp = item_ptr->opt_Import() ) {
                if( imp->path.m_crate_name == CRATE_BUILTINS )
                {
                    rv.macro.set( AST::AbsolutePath(CRATE_BUILTINS, {nodes.back().name()}), AST::PathBinding_Macro::make_MacroRules({ nullptr }) );
                    return rv;
                }
                ASSERT_BUG(span, crate.m_extern_crates.count(imp->path.m_crate_name) > 0, "Unable to find crate for " << imp->path);
                const auto& c = *crate.m_extern_crates.at(imp->path.m_crate_name).m_hir;    // Have to manually look up, AST doesn't have a `get_mod_by_path`
                const auto& mod = c.get_mod_by_path(span, imp->path, /*ignore_last=*/true, /*ignore_crate=*/true);
                item_ptr = &mod.m_macro_items.at(imp->path.m_components.back())->ent;
                ap = AST::AbsolutePath(imp->path.m_crate_name, imp->path.m_components);
            }
            else {
            }
            
            if( rv.macro.is_Unbound() )
            {
                TU_MATCH_HDRA( (*item_ptr), {)
                TU_ARMA(Import, e) {
                    if( e.path.m_crate_name == CRATE_BUILTINS )
                        ;
                    else
                        BUG(span, "Recursive import in " << path << " - " << it->second->ent.as_Import().path << " -> " << e.path);
                    rv.macro.set( ap, ::AST::PathBinding_Macro::make_MacroRules({ nullptr, nullptr }) );
                    }
                TU_ARMA(ProcMacro, e) {
                    rv.macro.set( ap, ::AST::PathBinding_Macro::make_ProcMacro({ &hcrate, e.name }) );
                    }
                TU_ARMA(MacroRules, e) {
                    rv.macro.set( ap, ::AST::PathBinding_Macro::make_MacroRules({ nullptr, &*e } ) );
                    }
                }
            }
        }
    }

    if( rv.type.is_Unbound() && rv.value.is_Unbound() && rv.macro.is_Unbound() )
    {
        DEBUG("E : None");
    }
    else
    {
        DEBUG(rv.type << rv.value << rv.macro);
    }
    return rv;
}
::AST::Path::Bindings Resolve_Use_GetBinding__ext(const Span& span, const ::AST::Crate& crate, const ::AST::Path& path,  const AST::ExternCrate& ec, unsigned int start)
{
    DEBUG("Crate " << ec.m_name);
    auto rv = Resolve_Use_GetBinding__ext(span, crate, ec, ec.m_hir->m_root_module, path, start);
    if( auto* e = rv.macro.binding.opt_MacroRules() )
    {
        if( e->crate_ == nullptr )
        {
            e->crate_ = &ec;
        }
    }
    return rv;
}

::AST::Path::Bindings Resolve_Use_GetBinding(
    const Span& span, const ::AST::Crate& crate, const ::AST::AbsolutePath& source_mod_path,
    const ::AST::Path& path, ::std::span< const ::AST::Module* > parent_modules,
    bool types_only/*=false*/
    )
{
    TRACE_FUNCTION_F(path);
    //::AST::Path rv;

    // If the path is directly referring to an external crate - call __ext
    if( path.m_class.is_Absolute() && (path.m_class.as_Absolute().crate != "" && path.m_class.as_Absolute().crate != crate.m_crate_name_real) ) {
        const auto& path_abs = path.m_class.as_Absolute();
        // Builtin macro imports
        if(path_abs.crate == CRATE_BUILTINS)
        {
            ::AST::Path::Bindings   rv;
            ASSERT_BUG(span, !path_abs.nodes.empty(), "");
            rv.macro.set( AST::AbsolutePath(CRATE_BUILTINS, {path_abs.nodes.back().name()}), AST::PathBinding_Macro::make_MacroRules({ nullptr }) );
            return rv;
        }

        ASSERT_BUG(span, crate.m_extern_crates.count(path_abs.crate.c_str()), "Crate '" << path_abs.crate << "' not loaded");
        return Resolve_Use_GetBinding__ext(span, crate, path,  crate.m_extern_crates.at( path_abs.crate.c_str() ), 0);
    }

    ::AST::Path::Bindings   rv;

    const AST::Module* mod = &crate.m_root_module;
    const auto& nodes = path.nodes();
    if( nodes.size() == 0 ) {
        // An import of the root.
        rv.type.set( mod->path(), ::AST::PathBinding_Type::make_Module({ mod, nullptr }) );
        return rv;
    }

    std::vector<const AST::Module*>   inner_parent_modules;
    for( unsigned int i = 0; i < nodes.size()-1; i ++ )
    {
        DEBUG("Component " << nodes.at(i).name());
        // TODO: If this came from an import, return the real path?

        //rv = Resolve_Use_CanoniseAndBind_Mod(span, crate, *mod, mv$(rv), nodes[i].name(), parent_modules, Lookup::Type);
        //const auto& b = rv.binding();
        assert(mod);
        auto b = Resolve_Use_GetBinding_Mod(span, crate, source_mod_path, *mod, nodes.at(i).name(), inner_parent_modules, /*types_only=*/true);
        TU_MATCH_HDRA( (b.type.binding), {)
        default:
            ERROR(span, E0000, "Unexpected item type " << b.type.binding.tag_str() << " in import of " << path);
        TU_ARMA(Unbound, e) {
            ERROR(span, E0000, "Cannot find component " << i << " of " << path << " (" << b.type.binding << ")");
            }
        TU_ARMA(Crate, e) {
            // TODO: Mangle the original path (or return a new path somehow)
            DEBUG("Extern - Call _ext with remainder");
            return Resolve_Use_GetBinding__ext(span, crate, path,  *e.crate_, i+1);
            }
        TU_ARMA(Enum, e) {
            ASSERT_BUG(span, e.enum_ || e.hir, "nullptr enum pointer in node " << i << " of " << path);
            ASSERT_BUG(span, e.enum_ == nullptr || e.hir == nullptr, "both AST and HIR pointers set in node " << i << " of " << path);
            i += 1;
            if( i != nodes.size() - 1 ) {
                ERROR(span, E0000, "Encountered enum at unexpected location in import");
            }
            ASSERT_BUG(span, i < nodes.size(), "Enum import position error, " << i << " >= " << nodes.size() << " - " << path);

            const auto& node2 = nodes[i];

            unsigned    variant_index = 0;
            bool    is_value = false;
            if(e.hir)
            {
                const auto& enum_ = *e.hir;
                size_t idx = enum_.find_variant(node2.name());
                if( idx == ~0u ) {
                    ERROR(span, E0000, "Unknown enum variant " << path);
                }
                TU_MATCH_HDRA( (enum_.m_data), {)
                TU_ARMA(Value, ve) {
                    is_value = true;
                    }
                TU_ARMA(Data, ve) {
                    is_value = !ve[idx].is_struct;
                    }
                }
                DEBUG("HIR Enum variant - " << variant_index << ", is_value=" << is_value);
            }
            else
            {
                const auto& enum_ = *e.enum_;
                for( const auto& var : enum_.variants() )
                {
                    if( var.m_name == node2.name() ) {
                        is_value = !var.m_data.is_Struct();
                        break ;
                    }
                    variant_index ++;
                }
                if( variant_index == enum_.variants().size() ) {
                    ERROR(span, E0000, "Unknown enum variant '" << node2.name() << "'");
                }

                DEBUG("AST Enum variant - " << variant_index << ", is_value=" << is_value << " " << enum_.variants()[variant_index].m_data.tag_str());
            }
            if( is_value ) {
                rv.value.set( b.type.path + node2.name(), ::AST::PathBinding_Value::make_EnumVar({e.enum_, variant_index, e.hir}) );
            }
            else {
                rv.type.set( b.type.path + node2.name(), ::AST::PathBinding_Type::make_EnumVar({e.enum_, variant_index, e.hir}) );
            }
            return rv;
            }
        TU_ARMA(Module, e) {
            ASSERT_BUG(span, e.module_ || e.hir.mod, "nullptr module pointer in node " << i << " of " << path);
            if( !e.module_ )
            {
                assert(e.hir.crate);
                assert(e.hir.mod);
                return Resolve_Use_GetBinding__ext(span, crate, *e.hir.crate, *e.hir.mod, path, i+1, b.type.path);
            }
            inner_parent_modules.push_back(mod);
            mod = e.module_;
            }
        }
    }

    assert(mod);
    return Resolve_Use_GetBinding_Mod(span, crate, source_mod_path, *mod, nodes.back().name(), parent_modules, types_only);
}

//::AST::PathBinding_Macro Resolve_Use_GetBinding_Macro(const Span& span, const ::AST::Crate& crate, const ::AST::Path& path, ::std::span< const ::AST::Module* > parent_modules)
//{
//    throw "";
//}
