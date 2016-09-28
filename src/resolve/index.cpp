/*
 * Build up a name index in all modules (optimising lookups in later stages)
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
::std::unordered_map< ::std::string, ::AST::Module::IndexEnt >& get_mod_index(::AST::Module& mod, IndexName location) {
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

void _add_item(const Span& sp, AST::Module& mod, IndexName location, const ::std::string& name, bool is_pub, ::AST::Path ir, bool error_on_collision=true)
{
    auto& list = get_mod_index(mod, location);
    
    bool was_import = (ir != mod.path() + name);
    if( was_import ) {
        DEBUG("### Import " << location << " item " << name << " = " << ir); 
    }
    else {
        DEBUG("Add " << location << " item '" << name << "': " << ir);
    }
    if( false == list.insert(::std::make_pair(name, ::AST::Module::IndexEnt { is_pub, was_import, mv$(ir) } )).second )
    {
        if( error_on_collision ) 
        {
            ERROR(sp, E0000, "Duplicate definition of name '" << name << "' in " << location << " scope (" << mod.path() << ")");
        }
        else
        {
            DEBUG("Name collision in " << mod.path() << " - '" << name << "', ignored");
        }
    }
}
void _add_item_type(const Span& sp, AST::Module& mod, const ::std::string& name, bool is_pub, ::AST::Path ir, bool error_on_collision=true)
{
    _add_item(sp, mod, IndexName::Namespace, name, is_pub, ::AST::Path(ir), error_on_collision);
    _add_item(sp, mod, IndexName::Type, name, is_pub, mv$(ir), error_on_collision);
}
void _add_item_value(const Span& sp, AST::Module& mod, const ::std::string& name, bool is_pub, ::AST::Path ir, bool error_on_collision=true)
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
        
        (Use,
            // Skip for now
            ),
        // - Types/modules only
        (Module,
            p.bind( ::AST::PathBinding::make_Module({&e}) );
            _add_item(i.data.span, mod, IndexName::Namespace, i.name, i.is_pub,  mv$(p));
            ),
        (Crate,
            p.bind( ::AST::PathBinding::make_Crate({ &crate.m_extern_crates.at(e.name) }) );
            _add_item(i.data.span, mod, IndexName::Namespace, i.name, i.is_pub,  mv$(p));
            ),
        (Enum,
            p.bind( ::AST::PathBinding::make_Enum({&e}) );
            _add_item_type(i.data.span, mod, i.name, i.is_pub,  mv$(p));
            ),
        (Trait,
            p.bind( ::AST::PathBinding::make_Trait({&e}) );
            _add_item_type(i.data.span, mod, i.name, i.is_pub,  mv$(p));
            ),
        (Type,
            p.bind( ::AST::PathBinding::make_TypeAlias({&e}) );
            _add_item_type(i.data.span, mod, i.name, i.is_pub,  mv$(p));
            ),
        // - Mixed
        (Struct,
            p.bind( ::AST::PathBinding::make_Struct({&e}) );
            // - If the struct is a tuple-like struct (or unit-like), it presents in the value namespace
            if( e.m_data.is_Tuple() ) {
                _add_item_value(i.data.span, mod, i.name, i.is_pub,  p);
            }
            _add_item_type(i.data.span, mod, i.name, i.is_pub,  mv$(p));
            ),
        // - Values only
        (Function,
            p.bind( ::AST::PathBinding::make_Function({&e}) );
            _add_item_value(i.data.span, mod, i.name, i.is_pub,  mv$(p));
            ),
        (Static,
            p.bind( ::AST::PathBinding::make_Static({&e}) );
            _add_item_value(i.data.span, mod, i.name, i.is_pub,  mv$(p));
            )
        )
    }
    
    bool has_pub_wildcard = false;
    // Named imports
    for( const auto& i : mod.items() )
    {
        if( ! i.data.is_Use() )
            continue ;
        const auto& i_data = i.data.as_Use();
        if( i.name != "" )
        {
            // TODO: Ensure that the path is canonical?
            
            const auto& sp = i_data.sp;
            struct H {
                static void handle_pb(const Span& sp, AST::Module& mod, const AST::Named<AST::Item>& i, const AST::PathBinding& pb, bool allow_collide)
                {
                    const auto& i_data = i.data.as_Use();
                    TU_MATCH(::AST::PathBinding, (pb), (e),
                    (Unbound,
                        ),
                    (Variable,
                        BUG(sp, "Import was bound to variable");
                        ),
                    (TypeParameter,
                        BUG(sp, "Import was bound to type parameter");
                        ),
                    (TraitMethod,
                        BUG(sp, "Import was bound to trait method");
                        ),
                    (StructMethod,
                        BUG(sp, "Import was bound to struct method");
                        ),
                    
                    (Crate ,  _add_item(sp, mod, IndexName::Namespace, i.name, i.is_pub,  i_data.path, !allow_collide); ),
                    (Module,  _add_item(sp, mod, IndexName::Namespace, i.name, i.is_pub,  i_data.path, !allow_collide); ),
                    (Enum,     _add_item_type(sp, mod, i.name, i.is_pub,  i_data.path, !allow_collide); ),
                    (Trait,    _add_item_type(sp, mod, i.name, i.is_pub,  i_data.path, !allow_collide); ),
                    (TypeAlias,_add_item_type(sp, mod, i.name, i.is_pub,  i_data.path, !allow_collide); ),
                    
                    (Struct,
                        _add_item_type(sp, mod, i.name, i.is_pub,  i_data.path, !allow_collide);
                        // - If the struct is a tuple-like struct, it presents in the value namespace
                        assert(e.struct_ || e.hir);
                        if( e.struct_ ) {
                            if( e.struct_->m_data.is_Tuple() ) {
                                _add_item_value(sp, mod, i.name, i.is_pub,  i_data.path, !allow_collide);
                            }
                        }
                        else {
                            if( ! e.hir->m_data.is_Named() ) {
                                _add_item_value(sp, mod, i.name, i.is_pub,  i_data.path, !allow_collide);
                            }
                        }
                        ),
                    (Static  , _add_item_value(sp, mod, i.name, i.is_pub,  i_data.path, !allow_collide); ),
                    (Function, _add_item_value(sp, mod, i.name, i.is_pub,  i_data.path, !allow_collide); ),
                    (EnumVar , _add_item_value(sp, mod, i.name, i.is_pub,  i_data.path, !allow_collide); )
                    )
                }
            };
            if( i_data.path.binding().is_Unbound() ) {
                BUG(sp, "Import left unbound ("<<i_data.path<<")");
            }
            else {
                H::handle_pb(sp, mod, i, i_data.path.binding(), false);
            }
            if( i_data.alt_binding.is_Unbound() ) {
                // Doesn't matter
            }
            else {
                H::handle_pb(sp, mod, i, i_data.alt_binding, true);
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
        if( ve.is_public ) {
            AST::Path   p;
            if( ve.ent.is_Import() ) {
                p = hir_to_ast( ve.ent.as_Import() );
            }
            else {
                p = path + it.first;
            }
            TU_MATCHA( (ve.ent), (e),
            (Import,
                //throw "";
                ),
            (Module,
                p.bind( ::AST::PathBinding::make_Module({nullptr, &e}) );
                ),
            (Trait,
                p.bind( ::AST::PathBinding::make_Trait({nullptr, &e}) );
                ),
            (Struct,
                p.bind( ::AST::PathBinding::make_Struct({nullptr, &e}) );
                ),
            (Enum,
                p.bind( ::AST::PathBinding::make_Enum({nullptr}) );
                ),
            (TypeAlias,
                p.bind( ::AST::PathBinding::make_TypeAlias({nullptr}) );
                )
            )
            _add_item_type( sp, dst_mod, it.first, is_pub, mv$(p), false );
        }
    }
    for(const auto& it : hmod.m_value_items) {
        const auto& ve = *it.second;
        if( ve.is_public ) {
            AST::Path   p;
            const auto* vep = &ve.ent;
            if( ve.ent.is_Import() ) {
                const auto& spath = ve.ent.as_Import();
                p = hir_to_ast( spath );
                
                const auto* hmod = &crate.m_extern_crates.at(spath.m_crate_name).m_hir->m_root_module;
                for(unsigned int i = 0; i < spath.m_components.size()-1; i ++) {
                    const auto& it = hmod->m_mod_items.at( spath.m_components[i] );
                    if(it->ent.is_Enum()) {
                        ASSERT_BUG(sp, i + 1 == spath.m_components.size() - 1, "");
                        p.bind( ::AST::PathBinding::make_EnumVar({nullptr, 0}) );
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
                    p.bind( ::AST::PathBinding::make_Static({nullptr}) );
                    ),
                (Static,
                    p.bind( ::AST::PathBinding::make_Static({nullptr}) );
                    ),
                // TODO: What if these refer to an enum variant?
                (StructConstant,
                    p.bind( ::AST::PathBinding::make_Struct({ nullptr, &crate.m_extern_crates.at(e.ty.m_crate_name).m_hir->get_typeitem_by_path(sp, e.ty, true).as_Struct() }) );
                    ),
                (StructConstructor,
                    p.bind( ::AST::PathBinding::make_Struct({ nullptr, &crate.m_extern_crates.at(e.ty.m_crate_name).m_hir->get_typeitem_by_path(sp, e.ty, true).as_Struct() }) );
                    ),
                (Function,
                    p.bind( ::AST::PathBinding::make_Function({nullptr}) );
                    )
                )
            }
            _add_item_value( sp, dst_mod, it.first, is_pub, mv$(p), false );
        }
    }
}

void Resolve_Index_Module_Wildcard(AST::Crate& crate, AST::Module& mod, bool handle_pub)
{
    TRACE_FUNCTION_F("mod = " << mod.path() << ", handle_pub=" << handle_pub);
    //if( mod.m_index_populated == 2 ) {
    //    DEBUG("- Index pre-populated")
    //    return ;
    //}
    // Glob/wildcard imports
    for( const auto& i : mod.items() )
    {
        if( ! i.data.is_Use() )
            continue ;
        const auto& i_data = i.data.as_Use();
        
        if( i.name == "" && i.is_pub == handle_pub )
        {
            const auto& sp = i_data.sp;
            const auto& b = i_data.path.binding();
            TU_MATCH_DEF(::AST::PathBinding, (b), (e),
            (
                BUG(sp, "Glob import of invalid type encountered");
                ),
            (Unbound,
                BUG(sp, "Import left unbound ("<<i_data.path<<")");
                ),
            (Variable,
                BUG(sp, "Import was bound to variable");
                ),
            (TypeParameter,
                BUG(sp, "Import was bound to type parameter");
                ),
            (TraitMethod,
                BUG(sp, "Import was bound to trait method");
                ),
            (StructMethod,
                BUG(sp, "Import was bound to struct method");
                ),
            
            (Crate,
                TODO(sp, "Glob import of crate");
                ),
            (Module,
                DEBUG("Glob mod " << i_data.path);
                if( !e.module_ )
                {
                    ASSERT_BUG(sp, e.hir, "Glob import where HIR module pointer not set - " << i_data.path);
                    const auto& hmod = *e.hir;
                    Resolve_Index_Module_Wildcard__glob_in_hir_mod(sp, crate, mod, hmod, i_data.path, i.is_pub);
                }
                else
                {
                    if( e.module_ == &mod ) {
                        ERROR(sp, E0000, "Glob import of self");
                    }
                    // 1. Begin indexing of this module if it is not already indexed
                    assert( e.module_->m_index_populated != 0 );
                    if( e.module_->m_index_populated == 1 )
                    {
                        // TODO: Handle wildcard import of a module with a public wildcard import
                        // TODO XXX: Huge chance of infinite recursion here (if the code recursively references)
                        Resolve_Index_Module_Wildcard(crate, *const_cast<AST::Module*>(e.module_), true);
                        assert(e.module_->m_index_populated == 2);
                        DEBUG("- Globbing in " << i_data.path);
                    }
                    for(const auto& vi : e.module_->m_namespace_items) {
                        if( vi.second.is_pub ) {
                            _add_item( sp, mod, IndexName::Namespace, vi.first, i.is_pub, vi.second.path, false );
                        }
                    }
                    for(const auto& vi : e.module_->m_type_items) {
                        if( vi.second.is_pub ) {
                            _add_item( sp, mod, IndexName::Type, vi.first, i.is_pub, vi.second.path, false );
                        }
                    }
                    for(const auto& vi : e.module_->m_value_items) {
                        if( vi.second.is_pub ) {
                            _add_item( sp, mod, IndexName::Value, vi.first, i.is_pub, vi.second.path, false );
                        }
                    }
                }
                ),
            (Enum,
                ASSERT_BUG(sp, e.enum_, "Glob import but enum pointer not set - " << i_data.path);
                DEBUG("Glob enum " << i_data.path);
                unsigned int idx = 0;
                for( const auto& ev : e.enum_->variants() ) {
                    ::AST::Path p = i_data.path + ev.m_name;
                    p.bind( ::AST::PathBinding::make_EnumVar({e.enum_, idx}) );
                    if( ev.m_data.is_Struct() ) {
                        _add_item_type ( sp, mod, ev.m_name, i.is_pub, mv$(p), false );
                    }
                    else {
                        _add_item_value( sp, mod, ev.m_name, i.is_pub, mv$(p), false );
                    }
                    
                    idx += 1;
                }
                )
            )
        }
    }
    
    // handle_pub == true first, leading to full resoltion no matter what
    mod.m_index_populated = 2;
    
    // Handle child modules
    for( auto& i : mod.items() )
    {
        TU_MATCH_DEF(AST::Item, (i.data), (e),
        (
            ),
        (Module,
            Resolve_Index_Module_Wildcard(crate, e, handle_pub);
            )
        )
    }
    for(auto& mp : mod.anon_mods())
    {
        if( mp ) {
            Resolve_Index_Module_Wildcard(crate, *mp, handle_pub);
        }
    }
}


void Resolve_Index_Module_Normalise_Path_ext(const ::AST::Crate& crate, const Span& sp, ::AST::Path& path,  const ::AST::ExternCrate& ext, unsigned int start)
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
            const auto& ec = crate.m_extern_crates.at( e.m_crate_name );
            item_ptr = &ec.m_hir->get_typeitem_by_path(sp, e, true);    // ignore_crate_name=true
        }
        TU_MATCH_DEF(::HIR::TypeItem, (*item_ptr), (e),
        (
            BUG(sp, "Path " << path << " pointed to non-module in component " << i);
            ),
        (Import,
            BUG(sp, "Recursive import in " << path << " - " << it->second->ent.as_Import() << " -> " << e);
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
    
    auto it_m = hmod->m_mod_items.find( lastnode.name() );
    if( it_m != hmod->m_mod_items.end() )
    {
        TU_IFLET( ::HIR::TypeItem, it_m->second->ent, Import, e,
            // Replace the path with this path (maintaining binding)
            auto binding = path.binding().clone();
            path = hir_to_ast(e);
            path.bind( mv$(binding) );
        )
        return ;
    }
    auto it_v = hmod->m_value_items.find( lastnode.name() );
    if( it_v != hmod->m_value_items.end() )
    {
        TU_IFLET( ::HIR::ValueItem, it_v->second->ent, Import, e,
            // Replace the path with this path (maintaining binding)
            auto binding = path.binding().clone();
            path = hir_to_ast(e);
            path.bind( mv$(binding) );
        )
        return ;
    }
    
    ERROR(sp, E0000,  "Couldn't find final node of path " << path);
}

// Returns true if a change was made
bool Resolve_Index_Module_Normalise_Path(const ::AST::Crate& crate, const Span& sp, ::AST::Path& path, IndexName loc)
{
    const auto& info = path.m_class.as_Absolute();
    if( info.crate != "" )
    {
        Resolve_Index_Module_Normalise_Path_ext(crate, sp, path,  crate.m_extern_crates.at(info.crate), 0);
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
            new_path.bind( path.binding().clone() );
            path = mv$(new_path);
            return Resolve_Index_Module_Normalise_Path(crate, sp, path, loc);
        }
        else {
            TU_MATCH_DEF(::AST::PathBinding, (ie.path.binding()), (e),
            (
                BUG(sp, "Path " << path << " pointed to non-module " << ie.path);
                ),
            (Module,
                mod = e.module_;
                ),
            (Crate,
                Resolve_Index_Module_Normalise_Path_ext(crate, sp, path, *e.crate_, i+1);
                return false;
                ),
            (Enum,
                // NOTE: Just assuming that if an Enum is hit, it's sane
                return false;
                )
            )
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
            Resolve_Index_Module_Normalise(crate, item.data.span, e);
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
    // - Add all public glob imported items - `pub use module::*`
    Resolve_Index_Module_Wildcard(crate, crate.m_root_module, true);
    // - Add all private glob imported items
    Resolve_Index_Module_Wildcard(crate, crate.m_root_module, false);
    
    // - Normalise the index (ensuring all paths point directly to the item)
    Resolve_Index_Module_Normalise(crate, Span(), crate.m_root_module);
}
