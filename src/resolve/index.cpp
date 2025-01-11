/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * resolve/index.cpp
 * - Build up a name index in all modules (optimising lookups in later stages)
 */
#include <ast/ast.hpp>
#include <ast/crate.hpp>
#include <main_bindings.hpp>
#include <hir/hir.hpp>
#include <macro_rules/macro_rules.hpp>

enum class IndexName
{
    Namespace,
    Type,
    Value,
    Macro,
};

void Resolve_Index_Module_Wildcard__use_stmt(AST::Crate& crate, AST::Module& dst_mod, const AST::UseItem::Ent& i_data, const AST::Visibility& vis);

::std::ostream& operator<<(::std::ostream& os, const IndexName& loc)
{
    switch(loc)
    {
    case IndexName::Namespace:
        return os << "namespace";
    case IndexName::Type:
        return os << "type";
    case IndexName::Value:
        return os << "value";
    case IndexName::Macro:
        return os << "macro";
    }
    throw "";
}
::std::unordered_map< RcString, ::AST::Module::IndexEnt >& get_mod_index(::AST::Module& mod, IndexName location) {
    switch(location)
    {
    case IndexName::Namespace:
        return mod.m_namespace_items;
    case IndexName::Type:
        return mod.m_type_items;
    case IndexName::Value:
        return mod.m_value_items;
    case IndexName::Macro:
        return mod.m_macro_items;
    }
    throw "";
}

namespace {
    AST::Path hir_to_ast(const HIR::SimplePath& p) {
        // The crate name here has to be non-empty, because it's external.
        assert( p.crate_name() != "" );
        AST::Path   rv( p.crate_name(), {} );
        rv.nodes().reserve( p.components().size() );
        for(const auto& n : p.components())
            rv.nodes().push_back( AST::PathNode(n) );
        return rv;
    }
}   // namespace

void _add_item(const Span& sp, AST::Module& mod, IndexName location, const RcString& name, const AST::Visibility& vis, ::AST::Path ir, bool error_on_collision=true)
{
    ASSERT_BUG(sp, ir.m_bindings.has_binding(), "Adding item with no binding - " << ir);
    auto& list = get_mod_index(mod, location);

    if(location != IndexName::Namespace)
    {
        ASSERT_BUG(sp, ir.m_class.as_Absolute().nodes.size() > 0, "Non-namespace path must have nodes - " << location << " " << name << " = " << ir);
    }

    // Add traits to a separate list
    if( ir.m_bindings.type.binding.is_Trait() ) {
        auto it = std::find(mod.m_traits.begin(), mod.m_traits.end(), ir.m_bindings.type.path);
        if(it == mod.m_traits.end() ) {
            mod.m_traits.push_back( ir.m_bindings.type.path );
        }
    }

    bool was_import = (ir != mod.path() + name);
    if( list.count(name) > 0 )
    {
        auto& e = list.at(name);
        if( e.path == ir )
        {
            // Ignore, re-import of the same thing

            // Update the visibility, if this new visibility adds anything
            if( !e.vis.contains(vis) )
            {
                e.vis.inplace_union( vis );
                DEBUG("### Import " << location << " item " << mod.path() << " :: " << name << " = " << ir << " (update to " << e.vis << ")");
            }
        }
        else if( error_on_collision )
        {
            ERROR(sp, E0000, "Duplicate definition of name '" << name << "' in " << location << " scope (" << mod.path() << ") " << ir << ", and " << e.path);
        }
        else
        {
            DEBUG(location << " name collision - '" << name << "' = " << ir << ", ignored (mod=" << mod.path() << ", was " << e.path << ")");
        }
    }
    else
    {
        DEBUG("### " << (was_import ? "Import" : "Add") << location << " item " << mod.path() << " :: " << name << " = " << ir << vis);
        auto rec = list.insert(::std::make_pair(name, ::AST::Module::IndexEnt { was_import, mv$(vis), mv$(ir) } ));
        assert(rec.second);
    }
}
void _add_item_type(const Span& sp, AST::Module& mod, const RcString& name, const AST::Visibility& vis, ::AST::Path ir, bool error_on_collision=true)
{
    _add_item(sp, mod, IndexName::Namespace, name, vis, ::AST::Path(ir), error_on_collision);
    _add_item(sp, mod, IndexName::Type     , name, vis, ::std::move(ir), error_on_collision);
}
void _add_item_value(const Span& sp, AST::Module& mod, const RcString& name, const AST::Visibility& vis, ::AST::Path ir, bool error_on_collision=true)
{
    _add_item(sp, mod, IndexName::Value, name, vis, mv$(ir), error_on_collision);
}

void Resolve_Index_Module_Base(const AST::Crate& crate, AST::Module& mod)
{
    TRACE_FUNCTION_F("mod = " << mod.path());
    for( const auto& i : mod.m_items )
    {
        auto ap = mod.path() + i->name;
        auto p = ::AST::Path(ap);
        //DEBUG("- p = " << p << " : " << ::AST::Item::tag_to_str(i.data.tag()));

        TU_MATCH_HDRA( (i->data), {)
        TU_ARMA(None, e) {
            }
        TU_ARMA(MacroInv, e) {
            }
        // Unnamed
        TU_ARMA(ExternBlock, e) {
            }
        TU_ARMA(Impl, e) {
            }
        TU_ARMA(NegImpl, e) {
            }
        TU_ARMA(GlobalAsm, e) {
            }

        TU_ARMA(Macro, e) {
            // Handled by `for(const auto& item : mod.macros())` below
            //p.m_bindings.macro = ::AST::PathBinding_Macro::make_MacroRules({nullptr, e ? &*e : nullptr});
            //_add_item(i->span, mod, IndexName::Macro, i->name, i->vis, mv$(p));
            }

        TU_ARMA(Use, e) {
            // Skip for now
            }
        // - Types/modules only
        TU_ARMA(Module, e) {
            p.m_bindings.type.set( ap, ::AST::PathBinding_Type::make_Module({&e}) );
            _add_item(i->span, mod, IndexName::Namespace, i->name, i->vis,  mv$(p));
            }
        TU_ARMA(Crate, e) {
            if( e.name != "" )
            {
                ASSERT_BUG(i->span, crate.m_extern_crates.count(e.name) > 0, "Referenced crate '" << e.name << "' isn't loaded for `extern crate`");
                p.m_bindings.type.set( ap, ::AST::PathBinding_Type::make_Crate({ &crate.m_extern_crates.at(e.name) }) );
            }
            else
            {
                p.m_bindings.type.set( ap, ::AST::PathBinding_Type::make_Module({ &crate.m_root_module }) );
            }
            _add_item(i->span, mod, IndexName::Namespace, i->name, i->vis,  mv$(p));
            }
        TU_ARMA(Enum, e) {
            p.m_bindings.type.set( ap, ::AST::PathBinding_Type::make_Enum({&e}) );
            _add_item_type(i->span, mod, i->name, i->vis,  mv$(p));
            }
        TU_ARMA(Union, e) {
            p.m_bindings.type.set( ap, ::AST::PathBinding_Type::make_Union({&e}) );
            _add_item_type(i->span, mod, i->name, i->vis,  mv$(p));
            }
        TU_ARMA(Trait, e) {
            p.m_bindings.type.set( ap, ::AST::PathBinding_Type::make_Trait({&e}) );
            _add_item_type(i->span, mod, i->name, i->vis,  mv$(p));
            }
        TU_ARMA(TraitAlias, e) {
            p.m_bindings.type.set( ap, ::AST::PathBinding_Type::make_TraitAlias({&e}) );
            _add_item_type(i->span, mod, i->name, i->vis,  mv$(p));
            }
        TU_ARMA(Type, e) {
            p.m_bindings.type.set( ap, ::AST::PathBinding_Type::make_TypeAlias({&e}) );
            _add_item_type(i->span, mod, i->name, i->vis,  mv$(p));
            }
        // - Mixed
        TU_ARMA(Struct, e) {
            p.m_bindings.type.set( ap, ::AST::PathBinding_Type::make_Struct({&e}) );
            // - If the struct is a tuple-like struct (or unit-like), it presents in the value namespace
            if( ! e.m_data.is_Struct() ) {
                p.m_bindings.value.set( ap, ::AST::PathBinding_Value::make_Struct({&e}) );
                _add_item_value(i->span, mod, i->name, i->vis,  p);
            }
            _add_item_type(i->span, mod, i->name, i->vis,  mv$(p));
            }
        // - Values only
        TU_ARMA(Function, e) {
            p.m_bindings.value.set( ap, ::AST::PathBinding_Value::make_Function({&e}) );
            _add_item_value(i->span, mod, i->name, i->vis,  mv$(p));
            }
        TU_ARMA(Static, e) {
            p.m_bindings.value.set( ap, ::AST::PathBinding_Value::make_Static({&e}) );
            _add_item_value(i->span, mod, i->name, i->vis,  mv$(p));
            }
        }
    }

    for(const auto& item : mod.macros())
    {
        ::AST::Path p = mod.path() + item.name;
        p.m_bindings.macro.set( mod.path() + item.name, ::AST::PathBinding_Macro::make_MacroRules({nullptr, &*item.data}) );
        // NOTE: Macros can be freely duplicated, BUT the last entry takes precedence (TODO)
        _add_item(item.span, mod, IndexName::Macro, item.name, item.vis, mv$(p), /*error_on_collision=*/false);
    }

    bool has_pub_wildcard = false;
    // Named imports
    for( const auto& ip : mod.m_items )
    {
        const auto& i = *ip;
        if( ! i.data.is_Use() )
            continue ;
        for(const auto& i_data : i.data.as_Use().entries)
        if( i_data.name != "" )
        {
            DEBUG("Use " << i_data.name << " = " << i_data.path);
            // TODO: Ensure that the path is canonical?

            const auto& sp = i_data.sp;
            ASSERT_BUG(sp, i_data.path.m_bindings.has_binding(), "`use " << i_data.path << "` left unbound in module " << mod.path());
            const auto& pb = i_data.path.m_bindings;

            bool allow_collide = true;  // Allow collisions (`use` can import mutliple namespaces, local gets priority)
            // - Types
            TU_MATCH_HDRA( (pb.type.binding), {)
            TU_ARMA(Unbound, _e) {
                DEBUG(i_data.name << " - Not a type/module");
                }
            TU_ARMA(TypeParameter, e)
                BUG(sp, "Import was bound to type parameter");
            TU_ARMA(Crate , e)
                _add_item(sp, mod, IndexName::Namespace, i_data.name, i.vis,  pb.type, !allow_collide);
            TU_ARMA(Module, e)
                _add_item(sp, mod, IndexName::Namespace, i_data.name, i.vis,  pb.type, !allow_collide);
            TU_ARMA(Enum, e)
                _add_item_type(sp, mod, i_data.name, i.vis,  pb.type, !allow_collide);
            TU_ARMA(Union, e)
                _add_item_type(sp, mod, i_data.name, i.vis,  pb.type, !allow_collide);
            TU_ARMA(Trait, e)
                _add_item_type(sp, mod, i_data.name, i.vis,  pb.type, !allow_collide);
            TU_ARMA(TraitAlias, e)
                _add_item_type(sp, mod, i_data.name, i.vis,  pb.type, !allow_collide);
            TU_ARMA(TypeAlias, e)
                _add_item_type(sp, mod, i_data.name, i.vis,  pb.type, !allow_collide);
            TU_ARMA(Struct, e)
                _add_item_type(sp, mod, i_data.name, i.vis,  pb.type, !allow_collide);
            TU_ARMA(EnumVar, e)
                _add_item_type(sp, mod, i_data.name, i.vis,  pb.type, !allow_collide);
            }
            // - Values
            TU_MATCH_HDRA( (pb.value.binding), {)
            TU_ARMA(Unbound, _e) {
                DEBUG(i_data.name << " - Not a value");
                }
            TU_ARMA(Variable, e)
                BUG(sp, "Import was bound to a variable");
            TU_ARMA(Generic, e)
                BUG(sp, "Import was bound to a generic value");
            TU_ARMA(Struct, e)
                _add_item_value(sp, mod, i_data.name, i.vis,  pb.value, !allow_collide);
            TU_ARMA(EnumVar, e)
                _add_item_value(sp, mod, i_data.name, i.vis,  pb.value, !allow_collide);
            TU_ARMA(Static  , e)
                _add_item_value(sp, mod, i_data.name, i.vis,  pb.value, !allow_collide);
            TU_ARMA(Function, e)
                _add_item_value(sp, mod, i_data.name, i.vis,  pb.value, !allow_collide);
            }
            // - Macros
            TU_MATCH_HDRA( (pb.macro.binding), {)
            TU_ARMA(Unbound, _e) {
                DEBUG(i_data.name << " - Not a macro");
                }
            TU_ARMA(MacroRules, e) {
                _add_item(sp, mod, IndexName::Macro, i_data.name, i.vis, pb.macro, !allow_collide);
                }
            TU_ARMA(ProcMacro, e) {
                _add_item(sp, mod, IndexName::Macro, i_data.name, i.vis, pb.macro, !allow_collide);
                }
            TU_ARMA(ProcMacroAttribute, e) {
                TODO(sp, "ProcMacroAttribute import");
                }
            TU_ARMA(ProcMacroDerive, e) {
                TODO(sp, "ProcMacroDerive import");
                }
            }
        }
        else
        {
            if( i.vis.ty() != AST::Visibility::Ty::Private )
            {
                has_pub_wildcard = true;
            }
        }
    }

    mod.m_index_populated = (has_pub_wildcard ? 1 : 2);

    // Handle child modules
    for( auto& i : mod.m_items )
    {
        if(auto* e = i->data.opt_Module())
        {
            Resolve_Index_Module_Base(crate, *e);
        }
    }
    for(auto& mp : mod.anon_mods())
    {
        if( mp ) {
            Resolve_Index_Module_Base(crate, *mp);
        }
    }
}

void Resolve_Index_Module_Wildcard__glob_in_hir_mod(
    const Span& sp, const AST::Crate& crate, AST::Module& dst_mod,
    /*const AST::ExternCrate& hcrate,*/ const ::HIR::Module& hmod,
    const ::AST::Path& path, const ::AST::Visibility& vis,
    AST::AbsolutePath mod_ap
    )
{
    TRACE_FUNCTION_F(dst_mod.path() << " <= " << mod_ap);
    for(const auto& it : hmod.m_mod_items) {
        const auto& ve = *it.second;
        if( ve.publicity.is_global() ) {
            const auto* vep = &ve.ent;

            ::AST::PathBinding<::AST::PathBinding_Type> pb;
            if( vep->is_Import() ) {
                const auto& spath = vep->as_Import().path;
                pb.path.crate = spath.crate_name();
                pb.path.nodes = spath.components().to_vec();

                ASSERT_BUG(sp, crate.m_extern_crates.count(spath.crate_name()) == 1, "Crate " << spath.crate_name() << " is not loaded");
                const auto* hmod = &crate.m_extern_crates.at(spath.crate_name()).m_hir->m_root_module;
                // Import of the crate root
                if( spath.components().size() == 0 ) {
                    pb.binding = ::AST::PathBinding_Type::make_Module({nullptr, {nullptr, hmod}});
                    _add_item(sp, dst_mod, IndexName::Namespace, it.first, vis, ::AST::Path(pb), false);
                    continue ;
                }
                for(unsigned int i = 0; i < spath.components().size()-1; i ++) {
                    const auto& hit = hmod->m_mod_items.at( spath.components()[i] );
                    // Only support enums on the penultimate component
                    if( i == spath.components().size()-2 && hit->ent.is_Enum() ) {
                        pb.binding = ::AST::PathBinding_Type::make_EnumVar({nullptr, 0});
                        _add_item_type( sp, dst_mod, it.first, vis, mv$(pb), false );
                        hmod = nullptr;
                        break ;
                    }
                    ASSERT_BUG(sp, hit->ent.is_Module(), "Path component " << spath.components()[i] << " of " << spath << " is not a module, instead " << hit->ent.tag_str());
                    hmod = &hit->ent.as_Module();
                }
                if( !hmod )
                    continue ;
                vep = &hmod->m_mod_items.at( spath.components().back() )->ent;
            }
            else {
                pb.path = mod_ap + it.first;
            }
            TU_MATCH_HDRA( (*vep), {)
            TU_ARMA(Import, e) {
                //throw "";
                TODO(sp, "Get binding for HIR import? " << e.path);
                }
            TU_ARMA(Module, e) {
                pb.binding = ::AST::PathBinding_Type::make_Module({nullptr, {nullptr, &e}});
                }
            TU_ARMA(Trait, e) {
                pb.binding = ::AST::PathBinding_Type::make_Trait({nullptr, &e});
                }
            TU_ARMA(Struct, e) {
                pb.binding = ::AST::PathBinding_Type::make_Struct({nullptr, &e});
                }
            TU_ARMA(TraitAlias, e) {
                pb.binding = ::AST::PathBinding_Type::make_TraitAlias({nullptr, &e});
                }
            TU_ARMA(Union, e) {
                pb.binding = ::AST::PathBinding_Type::make_Union({nullptr, &e});
                }
            TU_ARMA(Enum, e) {
                pb.binding = ::AST::PathBinding_Type::make_Enum({nullptr});
                }
            TU_ARMA(TypeAlias, e) {
                pb.binding = ::AST::PathBinding_Type::make_TypeAlias({nullptr});
                }
            TU_ARMA(ExternType, e) {
                pb.binding = ::AST::PathBinding_Type::make_TypeAlias({nullptr});
                }
            }
            _add_item_type( sp, dst_mod, it.first, vis, mv$(pb), false );
        }
    }
    for(const auto& it : hmod.m_value_items) {
        const auto& ve = *it.second;
        if( ve.publicity.is_global() ) {
            const auto* vep = &ve.ent;

            ::AST::PathBinding<::AST::PathBinding_Value> pb;
            if( ve.ent.is_Import() ) {
                const auto& spath = ve.ent.as_Import().path;
                pb.path.crate = spath.crate_name();
                pb.path.nodes = spath.components().to_vec();

                ASSERT_BUG(sp, crate.m_extern_crates.count(spath.crate_name()) == 1, "Crate " << spath.crate_name() << " is not loaded");
                const auto* hmod = &crate.m_extern_crates.at(spath.crate_name()).m_hir->m_root_module;
                for(unsigned int i = 0; i < spath.components().size()-1; i ++) {
                    const auto& hit = hmod->m_mod_items.at( spath.components()[i] );
                    if( i == spath.components().size()-2 && hit->ent.is_Enum() ) {
                        auto idx = hit->ent.as_Enum().find_variant(spath.components().back());
                        ASSERT_BUG(sp, idx != SIZE_MAX, spath);
                        pb.binding = ::AST::PathBinding_Value::make_EnumVar({nullptr, static_cast<unsigned>(idx)});
                        _add_item_value( sp, dst_mod, it.first, vis, mv$(pb), false );
                        hmod = nullptr;
                        break ;
                    }
                    ASSERT_BUG(sp, hit->ent.is_Module(), "Path component " << spath.components()[i] << " of " << spath << " is not a module, instead " << hit->ent.tag_str());
                    hmod = &hit->ent.as_Module();
                }
                if( !hmod )
                    continue ;
                vep = &hmod->m_value_items.at( spath.components().back() )->ent;
            }
            else {
                pb.path = mod_ap + it.first;
            }
            assert(vep);
            TU_MATCH_HDRA( (*vep), {)
            TU_ARMA(Import, e) {
                throw "";
                }
            TU_ARMA(Constant, e) {
                pb.binding = ::AST::PathBinding_Value::make_Static({nullptr});
                }
            TU_ARMA(Static, e) {
                pb.binding = ::AST::PathBinding_Value::make_Static({nullptr});
                }
            // TODO: What if these refer to an enum variant?
            TU_ARMA(StructConstant, e) {
                pb.binding = ::AST::PathBinding_Value::make_Struct({ nullptr, &crate.m_extern_crates.at(e.ty.crate_name()).m_hir->get_typeitem_by_path(sp, e.ty, true).as_Struct() });
                }
            TU_ARMA(StructConstructor, e) {
                pb.binding = ::AST::PathBinding_Value::make_Struct({ nullptr, &crate.m_extern_crates.at(e.ty.crate_name()).m_hir->get_typeitem_by_path(sp, e.ty, true).as_Struct() });
                }
            TU_ARMA(Function, e) {
                pb.binding = ::AST::PathBinding_Value::make_Function({nullptr});
                }
            }
            _add_item_value( sp, dst_mod, it.first, vis, mv$(pb), false );
        }
    }
    for(const auto& it : hmod.m_macro_items) {
        const auto& e = *it.second;
        if( e.publicity.is_global() ) {
            ::AST::PathBinding<::AST::PathBinding_Macro>    pb;
            if(const auto* ep = e.ent.opt_Import()) {
                pb.path.crate = ep->path.crate_name();
                pb.path.nodes = ep->path.components().to_vec();
                // NOTE: This shouldn't ever be pointing at an import, and no other handling needed
            }
            else {
                pb.path = mod_ap + it.first;
            }

            TU_MATCH_HDRA( (e.ent), {)
            TU_ARMA(Import, _) {
                pb.binding = ::AST::PathBinding_Macro::make_MacroRules({ nullptr, nullptr });
                }
            TU_ARMA(ProcMacro, me) {
                pb.binding = ::AST::PathBinding_Macro::make_ProcMacro({ nullptr, me.name });
                }
            TU_ARMA(MacroRules, me) {
                pb.binding = ::AST::PathBinding_Macro::make_MacroRules({ nullptr, &*me });
                }
            }
            _add_item(sp, dst_mod, IndexName::Macro, it.first, vis, mv$(pb), false );
        }
    }
}

void Resolve_Index_Module_Wildcard__submod(AST::Crate& crate, AST::Module& dst_mod, const AST::Module& src_mod, const AST::Visibility& dst_vis)
{
    static Span sp;
    TRACE_FUNCTION_F(dst_mod.path() << " <= " << src_mod.path());
    static ::std::set<const AST::Module*>   stack;
    if( ! stack.insert( &src_mod ).second )
    {
        DEBUG("- Avoided recursion");
        return ;
    }

    for(const auto& vi : src_mod.m_namespace_items) {
        if( vi.second.vis.is_visible(dst_mod.path()/*, src_mod.path()*/) ) {
            _add_item( sp, dst_mod, IndexName::Namespace, vi.first, dst_vis, vi.second.path, false );
        }
    }
    for(const auto& vi : src_mod.m_type_items) {
        if( vi.second.vis.is_visible(dst_mod.path()/*, src_mod.path()*/) ) {
            _add_item( sp, dst_mod, IndexName::Type     , vi.first, dst_vis, vi.second.path, false );
        }
    }
    for(const auto& vi : src_mod.m_value_items) {
        if( vi.second.vis.is_visible(dst_mod.path()/*, src_mod.path()*/) ) {
            _add_item( sp, dst_mod, IndexName::Value    , vi.first, dst_vis, vi.second.path, false );
        }
    }
    for(const auto& vi : src_mod.m_macro_items) {
        if( vi.second.vis.is_visible(dst_mod.path()/*, src_mod.path()*/) ) {
            _add_item( sp, dst_mod, IndexName::Macro    , vi.first, dst_vis, vi.second.path, false );
        }
    }

    if( src_mod.m_index_populated != 2 )
    {
        for( const auto& i : src_mod.m_items )
        {
            if( ! i->data.is_Use() )
                continue ;
            if( !i->vis.is_visible(dst_mod.path()/*, src_mod.path()*/) )
                continue ;
            for(const auto& e : i->data.as_Use().entries)
            {
                if( e.name != "" )
                    continue ;
                Resolve_Index_Module_Wildcard__use_stmt(crate, dst_mod, e, dst_vis);
            }
        }
    }

    stack.erase(&src_mod);
}

void Resolve_Index_Module_Wildcard__use_stmt(AST::Crate& crate, AST::Module& dst_mod, const AST::UseItem::Ent& i_data, const AST::Visibility& vis)
{
    const auto& sp = i_data.sp;
    const auto& b = i_data.path.m_bindings.type;

    if(const auto* e = b.binding.opt_Crate())
    {
        DEBUG("Glob crate " << i_data.path);
        const auto& hmod = e->crate_->m_hir->m_root_module;
        Resolve_Index_Module_Wildcard__glob_in_hir_mod(sp, crate, dst_mod, hmod, i_data.path, vis, b.path);
    }
    else if(const auto* e = b.binding.opt_Module() )
    {
        DEBUG("Glob mod " << i_data.path);
        if( !e->module_ )
        {
            ASSERT_BUG(sp, e->hir.mod, "Glob import where HIR module pointer not set - " << i_data.path);
            const auto& hmod = *e->hir.mod;
            Resolve_Index_Module_Wildcard__glob_in_hir_mod(sp, crate, dst_mod, hmod, i_data.path, vis, b.path);
        }
        else
        {
            Resolve_Index_Module_Wildcard__submod(crate, dst_mod, *e->module_, vis);
        }
    }
    else if( const auto* ep = b.binding.opt_Enum() )
    {
        const auto& e = *ep;
        ASSERT_BUG(sp, e.enum_ || e.hir, "Glob import but enum pointer not set - " << i_data.path);
        if( e.enum_ )
        {
            DEBUG("Glob enum " << i_data.path << " (AST)");
            unsigned int idx = 0;
            for( const auto& ev : e.enum_->variants() ) {
                if( ev.m_data.is_Struct() ) {
                    AST::PathBinding<AST::PathBinding_Type>    pb;
                    pb.path = b.path + ev.m_name;
                    pb.binding = ::AST::PathBinding_Type::make_EnumVar({e.enum_, idx});
                    _add_item_type( sp, dst_mod, ev.m_name, vis, mv$(pb), false );
                }
                else {
                    AST::PathBinding<AST::PathBinding_Value>    pb;
                    pb.path = b.path + ev.m_name;
                    pb.binding = ::AST::PathBinding_Value::make_EnumVar({e.enum_, idx});
                    _add_item_value( sp, dst_mod, ev.m_name, vis, mv$(pb), false );
                }

                idx += 1;
            }
        }
        else
        {
            DEBUG("Glob enum " << i_data.path << " (HIR)");
            unsigned int idx = 0;
            if( e.hir->m_data.is_Value() )
            {
                const auto* de = e.hir->m_data.opt_Value();
                for(const auto& ev : de->variants)
                {
                    AST::PathBinding<AST::PathBinding_Value>    pb;
                    pb.path = b.path + ev.name;
                    pb.binding = ::AST::PathBinding_Value::make_EnumVar({nullptr, idx, e.hir});
                    _add_item_value( sp, dst_mod, ev.name, vis, mv$(pb), false );

                    idx += 1;
                }
            }
            else
            {
                const auto* de = &e.hir->m_data.as_Data();
                for(const auto& ev : *de)
                {
                    if( ev.is_struct ) {
                        AST::PathBinding<AST::PathBinding_Type>    pb;
                        pb.path = b.path + ev.name;
                        pb.binding = ::AST::PathBinding_Type::make_EnumVar({nullptr, idx, e.hir});
                        _add_item_type ( sp, dst_mod, ev.name, vis, mv$(pb), false );
                    }
                    else {
                        AST::PathBinding<AST::PathBinding_Value>    pb;
                        pb.path = b.path + ev.name;
                        pb.binding = ::AST::PathBinding_Value::make_EnumVar({nullptr, idx, e.hir});
                        _add_item_value( sp, dst_mod, ev.name, vis, mv$(pb), false );
                    }

                    idx += 1;
                }
            }
        }
    }
    else
    {
        BUG(sp, "Invalid path binding for glob import: " << b.binding.tag_str() << " - "<< i_data.path);
    }
}

// Wildcard (aka glob) import resolution
//
// Strategy:
// - HIR just imports the items
// - Enums import all variants
// - AST modules: (See Resolve_Index_Module_Wildcard__submod)
//  - Clone index in (marked as publicity and weak)
//  - Recurse into globs
void Resolve_Index_Module_Wildcard(AST::Crate& crate, AST::Module& mod)
{
    TRACE_FUNCTION_F("mod = " << mod.path());
    for( const auto& i : mod.m_items )
    {
        if( ! i->data.is_Use() )
            continue ;
        for(const auto& e : i->data.as_Use().entries )
        {
            if( e.name != "" )
                continue ;
            Resolve_Index_Module_Wildcard__use_stmt(crate, mod, e, i->vis);
        }
    }

    // Mark this as having all the items it ever will.
    mod.m_index_populated = 2;

    // Handle child modules
    for( auto& i : mod.m_items )
    {
        if( auto* e = i->data.opt_Module() )
        {
            Resolve_Index_Module_Wildcard(crate, *e);
        }
    }
    for(auto& mp : mod.anon_mods())
    {
        if( mp ) {
            Resolve_Index_Module_Wildcard(crate, *mp);
        }
    }
}


void Resolve_Index_Module_Normalise_Path_ext(const ::AST::Crate& crate, const Span& sp, ::AST::Path& path, IndexName loc,  const ::AST::ExternCrate& ext, unsigned int start)
{
    auto& info = path.m_class.as_Absolute();
    const ::HIR::Module* hmod = &ext.m_hir->m_root_module;

    // TODO: Mangle path into being absolute into the crate
    //info.crate = ext.m_name;
    //do {
    //    path.nodes().erase( path.nodes().begin() + i );
    //} while( --i > 0 );

    info.crate = ext.m_name;
    info.nodes.erase(info.nodes.begin(), info.nodes.begin() + start);

    if(info.nodes.empty()) {
        return ;
    }

    for(unsigned int i = 0; i < info.nodes.size() - 1; i ++)
    {
        auto it = hmod->m_mod_items.find( info.nodes[i].name() );
        if( it == hmod->m_mod_items.end() ) {
            ERROR(sp, E0000,  "Couldn't find node " << i << " of path " << path);
        }
        const auto* item_ptr = &it->second->ent;
        if( item_ptr->is_Import() ) {
            const auto& e = item_ptr->as_Import();
            const auto& ec = crate.m_extern_crates.at( e.path.crate_name() );

            // TODO: Update the path (and update `i` while there)

            if( e.path.components().empty() ) {
                hmod = &ec.m_hir->m_root_module;
                continue ;
            }
            item_ptr = &ec.m_hir->get_typeitem_by_path(sp, e.path, /*ignore_crate_name=*/true);
        }
        TU_MATCH_DEF(::HIR::TypeItem, (*item_ptr), (e),
        (
            BUG(sp, "Path " << path << " pointed to non-module in component " << i);
            ),
        (Import,
            BUG(sp, "Recursive import in " << path << " - " << it->second->ent.as_Import().path << " -> " << e.path);
            ),
        (Enum,
            if( i != info.nodes.size() - 2 ) {
                BUG(sp, "Path " << path << " pointed to non-module in component " << i);
            }
            // Lazy, not checking
            return ;
            ),
        (Module,
            hmod = &e;
            )
        )
    }
    const auto& lastnode = info.nodes.back();

    switch(loc)
    {
    case IndexName::Type:
    case IndexName::Namespace: {
        auto it_m = hmod->m_mod_items.find( lastnode.name() );
        if( it_m != hmod->m_mod_items.end() )
        {
            TU_IFLET( ::HIR::TypeItem, it_m->second->ent, Import, e,
                // Replace the path with this path (maintaining binding)
                auto bindings = path.m_bindings.clone();
                path = hir_to_ast(e.path);
                path.m_bindings = mv$(bindings);
            )
            return ;
        }
        } break;
    case IndexName::Value: {
        auto it_v = hmod->m_value_items.find( lastnode.name() );
        if( it_v != hmod->m_value_items.end() )
        {
            TU_IFLET( ::HIR::ValueItem, it_v->second->ent, Import, e,
                // Replace the path with this path (maintaining binding)
                auto bindings = path.m_bindings.clone();
                path = hir_to_ast(e.path);
                path.m_bindings = mv$(bindings);
            )
            return ;
        }
        } break;
    case IndexName::Macro: {
        auto it_v = hmod->m_macro_items.find( lastnode.name() );
        if( it_v != hmod->m_macro_items.end() )
        {
            if(const auto* e = it_v->second->ent.opt_Import())
            {
                // Replace the path with this path (maintaining binding)
                auto bindings = path.m_bindings.clone();
                path = hir_to_ast(e->path);
                path.m_bindings = mv$(bindings);
            }
            return ;
        }
        } break;
    }

    ERROR(sp, E0000,  "Couldn't find final node of path " << path);
}

// Returns true if a change was made
bool Resolve_Index_Module_Normalise_Path(const ::AST::Crate& crate, const Span& sp, ::AST::Path& path, IndexName loc)
{
    const auto& info = path.m_class.as_Absolute();
    if( info.crate != "" )
    {
        if( info.crate == CRATE_BUILTINS )
        {
            //TODO(sp, "Normalise builtin paths");
            return false;
        }
        Resolve_Index_Module_Normalise_Path_ext(crate, sp, path, loc,  crate.m_extern_crates.at(info.crate), 0);
        return false;
    }

    const ::AST::Module* mod = &crate.m_root_module;
    for( unsigned int i = 0; i < info.nodes.size() - 1; i ++ )
    {
        const auto& node = info.nodes[i];

        auto it = mod->m_namespace_items.find( node.name() );
        if( it == mod->m_namespace_items.end() )
            ERROR(sp, E0000,  "Couldn't find node " << i << " of path " << path);
        const auto& ie = it->second;

        if( ie.is_import ) {
            // Need to replace all nodes up to and including the current with the import path
            auto new_path = ie.path;
            for(unsigned int j = i+1; j < info.nodes.size(); j ++)
                new_path.nodes().push_back( mv$(info.nodes[j]) );
            new_path.m_bindings = path.m_bindings.clone();
            path = mv$(new_path);
            return Resolve_Index_Module_Normalise_Path(crate, sp, path, loc);
        }
        else {
            TU_MATCH_HDRA( (ie.path.m_bindings.type.binding), {)
            default:
                BUG(sp, "Path " << path << " pointed to non-module " << ie.path);
            TU_ARMA(Module, e) {
                mod = e.module_;
                }
            TU_ARMA(Crate, e) {
                Resolve_Index_Module_Normalise_Path_ext(crate, sp, path, loc, *e.crate_, i+1);
                return false;
                }
            TU_ARMA(Enum, e) {
                // NOTE: Just assuming that if an Enum is hit, it's sane
                return false;
                }
            }
        }
    }

    const auto& node = info.nodes.back();


    // TODO: Use get_mod_index instead.
    const ::AST::Module::IndexEnt* ie_p = nullptr;
    switch(loc)
    {
    case IndexName::Namespace: {
        auto it = mod->m_namespace_items.find( node.name() );
        if( it != mod->m_namespace_items.end() )
            ie_p = &it->second;
        } break;
    case IndexName::Value: {
        auto it = mod->m_value_items.find( node.name() );
        if( it != mod->m_value_items.end() )
            ie_p = &it->second;
        } break;
    case IndexName::Type: {
        auto it = mod->m_type_items.find( node.name() );
        if( it != mod->m_type_items.end() )
            ie_p = &it->second;
        } break;
    case IndexName::Macro: {
        auto it = mod->m_macro_items.find( node.name() );
        if( it != mod->m_macro_items.end() )
            ie_p = &it->second;
        } break;
    }
    if( !ie_p )
        ERROR(sp, E0000,  "Couldn't find final node of path " << path);
    const auto& ie = *ie_p;

    if( ie.is_import ) {
        // TODO: Prevent infinite recursion if the user does something dumb
        path = ::AST::Path(ie.path);
        Resolve_Index_Module_Normalise_Path(crate, sp, path, loc);
        return true;
    }
    else {
        // All good
        return false;
    }
}
void Resolve_Index_Module_Normalise(const ::AST::Crate& crate, const Span& mod_span, ::AST::Module& mod)
{
    TRACE_FUNCTION_F("mod = " << mod.path());
    for( auto& item : mod.m_items )
    {
        if(auto* e = item->data.opt_Module())
        {
            Resolve_Index_Module_Normalise(crate, item->span, *e);
        }
    }

    DEBUG("Index for " << mod.path());
    for( auto& ent : mod.m_namespace_items ) {
        Resolve_Index_Module_Normalise_Path(crate, mod_span, ent.second.path, IndexName::Namespace);
        DEBUG("NS " << ent.first << " = " << ent.second.path);
    }
    for( auto& ent : mod.m_type_items ) {
        Resolve_Index_Module_Normalise_Path(crate, mod_span, ent.second.path, IndexName::Type);
        DEBUG("Ty " << ent.first << " = " << ent.second.path);
    }
    for( auto& ent : mod.m_value_items ) {
        Resolve_Index_Module_Normalise_Path(crate, mod_span, ent.second.path, IndexName::Value);
        DEBUG("Val " << ent.first << " = " << ent.second.path);
    }
    for( auto& ent : mod.m_macro_items ) {
        Resolve_Index_Module_Normalise_Path(crate, mod_span, ent.second.path, IndexName::Macro);
        DEBUG("Macro " << ent.first << " = " << ent.second.path);
    }
}

void Resolve_Index_Module_ExportedMacros(::AST::Crate& crate, const Span& mod_span, ::AST::Module& mod)
{
    TRACE_FUNCTION_F("mod = " << mod.path());

    if(&mod != &crate.m_root_module)
    {
        for(const auto& item : mod.macros())
        {
            if( item.data->m_exported )
            {
                ASSERT_BUG(item.span, mod.m_macro_items.count(item.name), "Missing " << item.name << " in " << mod.path());
                _add_item(item.span, crate.m_root_module, IndexName::Macro, item.name, AST::Visibility::make_global(), mod.m_macro_items.at(item.name).path);
            }
        }
    }

    for( auto& item : mod.m_items )
    {
        if(auto* e = item->data.opt_Module())
        {
            Resolve_Index_Module_ExportedMacros(crate, item->span, *e);
        }
    }
}

void Resolve_Index(AST::Crate& crate)
{
    // - Index all explicitly named items
    Resolve_Index_Module_Base(crate, crate.m_root_module);
    // - Index wildcard imports
    Resolve_Index_Module_Wildcard(crate, crate.m_root_module);

    // Macros marked with `#[macro_export]` actually live in the root
    Resolve_Index_Module_ExportedMacros(crate, Span(), crate.m_root_module);

    // - Normalise the index (ensuring all paths point directly to the item)
    Resolve_Index_Module_Normalise(crate, Span(), crate.m_root_module);
}
