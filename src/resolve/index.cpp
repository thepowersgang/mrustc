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

enum class IndexName
{
    Namespace,
    Type,
    Value,
};

void Resolve_Index_Module_Wildcard__use_stmt(AST::Crate& crate, AST::Module& dst_mod, const AST::UseItem::Ent& i_data, bool is_pub);

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
    }
    throw "";
}

namespace {
    AST::Path hir_to_ast(const HIR::SimplePath& p) {
        // The crate name here has to be non-empty, because it's external.
        assert( p.m_crate_name != "" );
        AST::Path   rv( p.m_crate_name, {} );
        rv.nodes().reserve( p.m_components.size() );
        for(const auto& n : p.m_components)
            rv.nodes().push_back( AST::PathNode(n) );
        return rv;
    }
}   // namespace

void _add_item(const Span& sp, AST::Module& mod, IndexName location, const RcString& name, bool is_pub, ::AST::Path ir, bool error_on_collision=true)
{
    ASSERT_BUG(sp, ir.m_bindings.has_binding(), "");
    auto& list = get_mod_index(mod, location);

    bool was_import = (ir != mod.path() + name);
    if( list.count(name) > 0 )
    {
        if( error_on_collision )
        {
            ERROR(sp, E0000, "Duplicate definition of name '" << name << "' in " << location << " scope (" << mod.path() << ") " << ir << ", and " << list[name].path);
        }
        else if( list.at(name).path == ir )
        {
            // Ignore, re-import of the same thing
        }
        else
        {
            DEBUG(location << " name collision - '" << name << "' = " << ir << ", ignored (mod=" << mod.path() << ")");
        }
    }
    else
    {
        if( was_import ) {
            DEBUG("### Import " << location << " item " << name << " = " << ir);
        }
        else {
            DEBUG("### Add " << location << " item '" << name << "': " << ir);
        }
        auto rec = list.insert(::std::make_pair(name, ::AST::Module::IndexEnt { is_pub, was_import, mv$(ir) } ));
        assert(rec.second);
    }
}
void _add_item_type(const Span& sp, AST::Module& mod, const RcString& name, bool is_pub, ::AST::Path ir, bool error_on_collision=true)
{
    _add_item(sp, mod, IndexName::Namespace, name, is_pub, ::AST::Path(ir), error_on_collision);
    _add_item(sp, mod, IndexName::Type, name, is_pub, mv$(ir), error_on_collision);
}
void _add_item_value(const Span& sp, AST::Module& mod, const RcString& name, bool is_pub, ::AST::Path ir, bool error_on_collision=true)
{
    _add_item(sp, mod, IndexName::Value, name, is_pub, mv$(ir), error_on_collision);
}

void Resolve_Index_Module_Base(const AST::Crate& crate, AST::Module& mod)
{
    TRACE_FUNCTION_F("mod = " << mod.path());
    for( const auto& i : mod.items() )
    {
        ::AST::Path p = mod.path() + i.name;
        //DEBUG("- p = " << p << " : " << ::AST::Item::tag_to_str(i.data.tag()));

        TU_MATCH(AST::Item, (i.data), (e),
        (None,
            ),
        (MacroInv,
            ),
        // Unnamed
        (ExternBlock,
            ),
        (Impl,
            ),
        (NegImpl,
            ),

        (Macro,
            // TODO: Add to a macro list
            ),

        (Use,
            // Skip for now
            ),
        // - Types/modules only
        (Module,
            p.m_bindings.type = ::AST::PathBinding_Type::make_Module({&e});
            _add_item(i.span, mod, IndexName::Namespace, i.name, i.is_pub,  mv$(p));
            ),
        (Crate,
            ASSERT_BUG(i.span, crate.m_extern_crates.count(e.name) > 0, "Referenced crate '" << e.name << "' isn't loaded for `extern crate`");
            p.m_bindings.type = ::AST::PathBinding_Type::make_Crate({ &crate.m_extern_crates.at(e.name) });
            _add_item(i.span, mod, IndexName::Namespace, i.name, i.is_pub,  mv$(p));
            ),
        (Enum,
            p.m_bindings.type = ::AST::PathBinding_Type::make_Enum({&e});
            _add_item_type(i.span, mod, i.name, i.is_pub,  mv$(p));
            ),
        (Union,
            p.m_bindings.type = ::AST::PathBinding_Type::make_Union({&e});
            _add_item_type(i.span, mod, i.name, i.is_pub,  mv$(p));
            ),
        (Trait,
            p.m_bindings.type = ::AST::PathBinding_Type::make_Trait({&e});
            _add_item_type(i.span, mod, i.name, i.is_pub,  mv$(p));
            ),
        (Type,
            p.m_bindings.type = ::AST::PathBinding_Type::make_TypeAlias({&e});
            _add_item_type(i.span, mod, i.name, i.is_pub,  mv$(p));
            ),
        // - Mixed
        (Struct,
            p.m_bindings.type = ::AST::PathBinding_Type::make_Struct({&e});
            // - If the struct is a tuple-like struct (or unit-like), it presents in the value namespace
            if( ! e.m_data.is_Struct() ) {
                p.m_bindings.value = ::AST::PathBinding_Value::make_Struct({&e});
                _add_item_value(i.span, mod, i.name, i.is_pub,  p);
            }
            _add_item_type(i.span, mod, i.name, i.is_pub,  mv$(p));
            ),
        // - Values only
        (Function,
            p.m_bindings.value = ::AST::PathBinding_Value::make_Function({&e});
            _add_item_value(i.span, mod, i.name, i.is_pub,  mv$(p));
            ),
        (Static,
            p.m_bindings.value = ::AST::PathBinding_Value::make_Static({&e});
            _add_item_value(i.span, mod, i.name, i.is_pub,  mv$(p));
            )
        )
    }

    bool has_pub_wildcard = false;
    // Named imports
    for( const auto& i : mod.items() )
    {
        if( ! i.data.is_Use() )
            continue ;
        for(const auto& i_data : i.data.as_Use().entries)
        if( i_data.name != "" )
        {
            // TODO: Ensure that the path is canonical?

            const auto& sp = i_data.sp;
            ASSERT_BUG(sp, i_data.path.m_bindings.has_binding(), "`use " << i_data.path << "` left unbound in module " << mod.path());

            bool allow_collide = false;
            // - Types
            TU_MATCH_HDRA( (i_data.path.m_bindings.type), {)
            TU_ARMA(Unbound, _e) {
                DEBUG(i_data.name << " - Not a type/module");
                }
            TU_ARMA(TypeParameter, e)
                BUG(sp, "Import was bound to type parameter");
            TU_ARMA(Crate , e)
                _add_item(sp, mod, IndexName::Namespace, i_data.name, i.is_pub,  i_data.path, !allow_collide);
            TU_ARMA(Module, e)
                _add_item(sp, mod, IndexName::Namespace, i_data.name, i.is_pub,  i_data.path, !allow_collide);
            TU_ARMA(Enum, e)
                _add_item_type(sp, mod, i_data.name, i.is_pub,  i_data.path, !allow_collide);
            TU_ARMA(Union, e)
                _add_item_type(sp, mod, i_data.name, i.is_pub,  i_data.path, !allow_collide);
            TU_ARMA(Trait, e)
                _add_item_type(sp, mod, i_data.name, i.is_pub,  i_data.path, !allow_collide);
            TU_ARMA(TypeAlias, e)
                _add_item_type(sp, mod, i_data.name, i.is_pub,  i_data.path, !allow_collide);
            TU_ARMA(Struct, e)
                _add_item_type(sp, mod, i_data.name, i.is_pub,  i_data.path, !allow_collide);
            TU_ARMA(EnumVar, e)
                _add_item_type(sp, mod, i_data.name, i.is_pub,  i_data.path, !allow_collide);
            }
            // - Values
            TU_MATCH_HDRA( (i_data.path.m_bindings.value), {)
            TU_ARMA(Unbound, _e) {
                DEBUG(i_data.name << " - Not a value");
                }
            TU_ARMA(Variable, e)
                BUG(sp, "Import was bound to a variable");
            TU_ARMA(Generic, e)
                BUG(sp, "Import was bound to a generic value");
            TU_ARMA(Struct, e)
                _add_item_value(sp, mod, i_data.name, i.is_pub,  i_data.path, !allow_collide);
            TU_ARMA(EnumVar, e)
                _add_item_value(sp, mod, i_data.name, i.is_pub,  i_data.path, !allow_collide);
            TU_ARMA(Static  , e)
                _add_item_value(sp, mod, i_data.name, i.is_pub,  i_data.path, !allow_collide);
            TU_ARMA(Function, e)
                _add_item_value(sp, mod, i_data.name, i.is_pub,  i_data.path, !allow_collide);
            }
            // - Macros
            TU_MATCH_HDRA( (i_data.path.m_bindings.macro), {)
            TU_ARMA(Unbound, _e) {
                DEBUG(i_data.name << " - Not a macro");
                }
            TU_ARMA(MacroRules, e) {
                ::std::vector<RcString>    path;
                path.push_back( i_data.path.m_class.as_Absolute().crate );
                for(const auto& node : i_data.path.m_class.as_Absolute().nodes )
                    path.push_back( node.name() );
                mod.m_macro_imports.push_back({
                    i.is_pub, i_data.name, mv$(path), e.mac
                    });
                }
            TU_ARMA(ProcMacro, e) {
                TODO(sp, "ProcMacro import");
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
            if( i.is_pub )
            {
                has_pub_wildcard = true;
            }
        }
    }

    mod.m_index_populated = (has_pub_wildcard ? 1 : 2);

    // Handle child modules
    for( auto& i : mod.items() )
    {
        TU_MATCH_DEF(AST::Item, (i.data), (e),
        (
            ),
        (Module,
            Resolve_Index_Module_Base(crate, e);
            )
        )
    }
    for(auto& mp : mod.anon_mods())
    {
        if( mp ) {
            Resolve_Index_Module_Base(crate, *mp);
        }
    }
}

void Resolve_Index_Module_Wildcard__glob_in_hir_mod(const Span& sp, const AST::Crate& crate, AST::Module& dst_mod,  const ::HIR::Module& hmod, const ::AST::Path& path, bool is_pub)
{
    for(const auto& it : hmod.m_mod_items) {
        const auto& ve = *it.second;
        if( ve.publicity.is_global() ) {
            const auto* vep = &ve.ent;
            AST::Path   p;
            if( vep->is_Import() ) {
                const auto& spath = vep->as_Import().path;
                p = hir_to_ast( spath );

                ASSERT_BUG(sp, crate.m_extern_crates.count(spath.m_crate_name) == 1, "Crate " << spath.m_crate_name << " is not loaded");
                const auto* hmod = &crate.m_extern_crates.at(spath.m_crate_name).m_hir->m_root_module;
                for(unsigned int i = 0; i < spath.m_components.size()-1; i ++) {
                    const auto& hit = hmod->m_mod_items.at( spath.m_components[i] );
                    // Only support enums on the penultimate component
                    if( i == spath.m_components.size()-2 && hit->ent.is_Enum() ) {
                        p.m_bindings.type = ::AST::PathBinding_Type::make_EnumVar({nullptr, 0});
                        _add_item_type( sp, dst_mod, it.first, is_pub, mv$(p), false );
                        hmod = nullptr;
                        break ;
                    }
                    ASSERT_BUG(sp, hit->ent.is_Module(), "Path component " << spath.m_components[i] << " of " << spath << " is not a module, instead " << hit->ent.tag_str());
                    hmod = &hit->ent.as_Module();
                }
                if( !hmod )
                    continue ;
                vep = &hmod->m_mod_items.at( spath.m_components.back() )->ent;
            }
            else {
                p = path + it.first;
            }
            TU_MATCHA( (*vep), (e),
            (Import,
                //throw "";
                TODO(sp, "Get binding for HIR import? " << e.path);
                ),
            (Module,
                p.m_bindings.type = ::AST::PathBinding_Type::make_Module({nullptr, &e});
                ),
            (Trait,
                p.m_bindings.type = ::AST::PathBinding_Type::make_Trait({nullptr, &e});
                ),
            (Struct,
                p.m_bindings.type = ::AST::PathBinding_Type::make_Struct({nullptr, &e});
                ),
            (Union,
                p.m_bindings.type = ::AST::PathBinding_Type::make_Union({nullptr, &e});
                ),
            (Enum,
                p.m_bindings.type = ::AST::PathBinding_Type::make_Enum({nullptr});
                ),
            (TypeAlias,
                p.m_bindings.type = ::AST::PathBinding_Type::make_TypeAlias({nullptr});
                ),
            (ExternType,
                p.m_bindings.type = ::AST::PathBinding_Type::make_TypeAlias({nullptr});
                )
            )
            _add_item_type( sp, dst_mod, it.first, is_pub, mv$(p), false );
        }
    }
    for(const auto& it : hmod.m_value_items) {
        const auto& ve = *it.second;
        if( ve.publicity.is_global() ) {
            AST::Path   p;
            const auto* vep = &ve.ent;
            if( ve.ent.is_Import() ) {
                const auto& spath = ve.ent.as_Import().path;
                p = hir_to_ast( spath );

                ASSERT_BUG(sp, crate.m_extern_crates.count(spath.m_crate_name) == 1, "Crate " << spath.m_crate_name << " is not loaded");
                const auto* hmod = &crate.m_extern_crates.at(spath.m_crate_name).m_hir->m_root_module;
                for(unsigned int i = 0; i < spath.m_components.size()-1; i ++) {
                    const auto& it = hmod->m_mod_items.at( spath.m_components[i] );
                    if(it->ent.is_Enum()) {
                        ASSERT_BUG(sp, i + 1 == spath.m_components.size() - 1, "Found enum not at penultimate component of HIR import path");
                        p.m_bindings.value = ::AST::PathBinding_Value::make_EnumVar({nullptr, 0});  // TODO: What's the index?
                        vep = nullptr;
                        hmod = nullptr;
                        break ;
                    }
                    ASSERT_BUG(sp, it->ent.is_Module(), "");
                    hmod = &it->ent.as_Module();
                }
                if( hmod )
                    vep = &hmod->m_value_items.at( spath.m_components.back() )->ent;
            }
            else {
                p = path + it.first;
            }
            if( vep )
            {
                TU_MATCHA( (*vep), (e),
                (Import,
                    throw "";
                    ),
                (Constant,
                    p.m_bindings.value = ::AST::PathBinding_Value::make_Static({nullptr});
                    ),
                (Static,
                    p.m_bindings.value = ::AST::PathBinding_Value::make_Static({nullptr});
                    ),
                // TODO: What if these refer to an enum variant?
                (StructConstant,
                    p.m_bindings.value = ::AST::PathBinding_Value::make_Struct({ nullptr, &crate.m_extern_crates.at(e.ty.m_crate_name).m_hir->get_typeitem_by_path(sp, e.ty, true).as_Struct() });
                    ),
                (StructConstructor,
                    p.m_bindings.value = ::AST::PathBinding_Value::make_Struct({ nullptr, &crate.m_extern_crates.at(e.ty.m_crate_name).m_hir->get_typeitem_by_path(sp, e.ty, true).as_Struct() });
                    ),
                (Function,
                    p.m_bindings.value = ::AST::PathBinding_Value::make_Function({nullptr});
                    )
                )
            }
            _add_item_value( sp, dst_mod, it.first, is_pub, mv$(p), false );
        }
    }
}

void Resolve_Index_Module_Wildcard__submod(AST::Crate& crate, AST::Module& dst_mod, const AST::Module& src_mod, bool import_as_pub)
{
    static Span sp;
    TRACE_FUNCTION_F(src_mod.path());
    static ::std::set<const AST::Module*>   stack;
    if( ! stack.insert( &src_mod ).second )
    {
        DEBUG("- Avoided recursion");
        return ;
    }

    // TODO: Publicity of the source item shouldn't matter.
    // - Publicity should be a path, not a boolean.
    for(const auto& vi : src_mod.m_namespace_items) {
        _add_item( sp, dst_mod, IndexName::Namespace, vi.first, vi.second.is_pub && import_as_pub, vi.second.path, false );
    }
    for(const auto& vi : src_mod.m_type_items) {
        _add_item( sp, dst_mod, IndexName::Type     , vi.first, vi.second.is_pub && import_as_pub, vi.second.path, false );
    }
    for(const auto& vi : src_mod.m_value_items) {
        _add_item( sp, dst_mod, IndexName::Value    , vi.first, vi.second.is_pub && import_as_pub, vi.second.path, false );
    }

    if( src_mod.m_index_populated != 2 )
    {
        for( const auto& i : src_mod.items() )
        {
            if( ! i.data.is_Use() )
                continue ;
            for(const auto& e : i.data.as_Use().entries)
            {
                if( e.name != "" )
                    continue ;
                Resolve_Index_Module_Wildcard__use_stmt(crate, dst_mod, e, import_as_pub);
            }
        }
    }

    stack.erase(&src_mod);
}

void Resolve_Index_Module_Wildcard__use_stmt(AST::Crate& crate, AST::Module& dst_mod, const AST::UseItem::Ent& i_data, bool is_pub)
{
    const auto& sp = i_data.sp;
    const auto& b = i_data.path.m_bindings.type;

    TU_IFLET(::AST::PathBinding_Type, b, Crate, e,
        DEBUG("Glob crate " << i_data.path);
        const auto& hmod = e.crate_->m_hir->m_root_module;
        Resolve_Index_Module_Wildcard__glob_in_hir_mod(sp, crate, dst_mod, hmod, i_data.path, is_pub);
        // TODO: Macros too
        for(const auto& pm : e.crate_->m_hir->m_proc_macros)
        {
            dst_mod.m_macro_imports.push_back({
                is_pub, pm.name, make_vec2(e.crate_->m_hir->m_crate_name, pm.name), nullptr
                });
        }
    )
    else TU_IFLET(::AST::PathBinding_Type, b, Module, e,
        DEBUG("Glob mod " << i_data.path);
        if( !e.module_ )
        {
            ASSERT_BUG(sp, e.hir, "Glob import where HIR module pointer not set - " << i_data.path);
            const auto& hmod = *e.hir;
            Resolve_Index_Module_Wildcard__glob_in_hir_mod(sp, crate, dst_mod, hmod, i_data.path, is_pub);
        }
        else
        {
            Resolve_Index_Module_Wildcard__submod(crate, dst_mod, *e.module_, is_pub);
        }
    )
    else if( const auto* ep = b.opt_Enum() )
    {
        const auto& e = *ep;
        ASSERT_BUG(sp, e.enum_ || e.hir, "Glob import but enum pointer not set - " << i_data.path);
        if( e.enum_ )
        {
            DEBUG("Glob enum " << i_data.path << " (AST)");
            unsigned int idx = 0;
            for( const auto& ev : e.enum_->variants() ) {
                ::AST::Path p = i_data.path + ev.m_name;
                if( ev.m_data.is_Struct() ) {
                    p.m_bindings.type = ::AST::PathBinding_Type::make_EnumVar({e.enum_, idx});
                    _add_item_type ( sp, dst_mod, ev.m_name, is_pub, mv$(p), false );
                }
                else {
                    p.m_bindings.value = ::AST::PathBinding_Value::make_EnumVar({e.enum_, idx});
                    _add_item_value( sp, dst_mod, ev.m_name, is_pub, mv$(p), false );
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
                    ::AST::Path p = i_data.path + ev.name;
                    p.m_bindings.value = ::AST::PathBinding_Value::make_EnumVar({nullptr, idx, e.hir});

                    _add_item_value( sp, dst_mod, ev.name, is_pub, mv$(p), false );

                    idx += 1;
                }
            }
            else
            {
                const auto* de = &e.hir->m_data.as_Data();
                for(const auto& ev : *de)
                {
                    ::AST::Path p = i_data.path + ev.name;

                    if( ev.is_struct ) {
                        p.m_bindings.type = ::AST::PathBinding_Type::make_EnumVar({nullptr, idx, e.hir});
                        _add_item_type ( sp, dst_mod, ev.name, is_pub, mv$(p), false );
                    }
                    else {
                        p.m_bindings.value = ::AST::PathBinding_Value::make_EnumVar({nullptr, idx, e.hir});
                        _add_item_value( sp, dst_mod, ev.name, is_pub, mv$(p), false );
                    }

                    idx += 1;
                }
            }
        }
    }
    else
    {
        BUG(sp, "Invalid path binding for glob import: " << b.tag_str() << " - "<<i_data.path);
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
    for( const auto& i : mod.items() )
    {
        if( ! i.data.is_Use() )
            continue ;
        for(const auto& e : i.data.as_Use().entries )
        {
            if( e.name != "" )
                continue ;
            Resolve_Index_Module_Wildcard__use_stmt(crate, mod, e, i.is_pub);
        }
    }

    // Mark this as having all the items it ever will.
    mod.m_index_populated = 2;

    // Handle child modules
    for( auto& i : mod.items() )
    {
        if( auto* e = i.data.opt_Module() )
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
    const auto& info = path.m_class.as_Absolute();
    const ::HIR::Module* hmod = &ext.m_hir->m_root_module;

    // TODO: Mangle path into being absolute into the crate
    //info.crate = ext.m_name;
    //do {
    //    path.nodes().erase( path.nodes().begin() + i );
    //} while( --i > 0 );


    for(unsigned int i = start; i < info.nodes.size() - 1; i ++)
    {
        auto it = hmod->m_mod_items.find( info.nodes[i].name() );
        if( it == hmod->m_mod_items.end() ) {
            ERROR(sp, E0000,  "Couldn't find node " << i << " of path " << path);
        }
        const auto* item_ptr = &it->second->ent;
        if( item_ptr->is_Import() ) {
            const auto& e = item_ptr->as_Import();
            const auto& ec = crate.m_extern_crates.at( e.path.m_crate_name );
            if( e.path.m_components.size() == 0 ) {
                hmod = &ec.m_hir->m_root_module;
                continue ;
            }
            item_ptr = &ec.m_hir->get_typeitem_by_path(sp, e.path, true);    // ignore_crate_name=true
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
    }

    ERROR(sp, E0000,  "Couldn't find final node of path " << path);
}

// Returns true if a change was made
bool Resolve_Index_Module_Normalise_Path(const ::AST::Crate& crate, const Span& sp, ::AST::Path& path, IndexName loc)
{
    const auto& info = path.m_class.as_Absolute();
    if( info.crate != "" )
    {
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
            TU_MATCH_HDR( (ie.path.m_bindings.type), {)
            default:
                BUG(sp, "Path " << path << " pointed to non-module " << ie.path);
            TU_ARM( ie.path.m_bindings.type, Module, e) {
                mod = e.module_;
                }
            TU_ARM( ie.path.m_bindings.type, Crate, e) {
                Resolve_Index_Module_Normalise_Path_ext(crate, sp, path, loc, *e.crate_, i+1);
                return false;
                }
            TU_ARM( ie.path.m_bindings.type, Enum, e) {
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
    for( auto& item : mod.items() )
    {
        TU_IFLET(AST::Item, item.data, Module, e,
            Resolve_Index_Module_Normalise(crate, item.span, e);
        )
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
}

void Resolve_Index(AST::Crate& crate)
{
    // - Index all explicitly named items
    Resolve_Index_Module_Base(crate, crate.m_root_module);
    // - Index wildcard imports
    Resolve_Index_Module_Wildcard(crate, crate.m_root_module);

    // - Normalise the index (ensuring all paths point directly to the item)
    Resolve_Index_Module_Normalise(crate, Span(), crate.m_root_module);
}
