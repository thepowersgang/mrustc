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

void _add_item(const Span& sp, AST::Module& mod, IndexName location, const ::std::string& name, bool is_pub, ::AST::Path ir, bool error_on_collision=true)
{
    DEBUG("Add " << location << " item '" << name << "': " << ir);
    auto& list = get_mod_index(mod, location);
    
    bool was_import = (ir != mod.path() + name);
    if( was_import ) {
        DEBUG("### Import " << name << " = " << ir); 
    }
    if( false == list.insert(::std::make_pair(name, ::AST::Module::IndexEnt { is_pub, was_import, mv$(ir) } )).second )
    {
        if( error_on_collision ) 
        {
            ERROR(sp, E0000, "Duplicate definition of name '" << name << "' in " << location << " scope (" << mod.path() << ")");
        }
        else
        {
            DEBUG("Name collision " << mod.path() << ", ignored");
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
        DEBUG("- p = " << p << " : " << ::AST::Item::tag_to_str(i.data.tag()));
        
        TU_MATCH(AST::Item, (i.data), (e),
        (None,
            ),
        (MacroInv,
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
    for( const auto& i : mod.imports() )
    {
        if( i.name != "" )
        {
            const auto& sp = i.data.sp;
            const auto& b = i.data.path.binding();
            TU_MATCH(::AST::PathBinding, (b), (e),
            (Unbound,
                BUG(sp, "Import left unbound ("<<i.data.path<<")");
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
            
            (Crate ,  _add_item(sp, mod, IndexName::Namespace, i.name, i.is_pub,  i.data.path); ),
            (Module,  _add_item(sp, mod, IndexName::Namespace, i.name, i.is_pub,  i.data.path); ),
            (Enum,    _add_item_type(sp, mod, i.name, i.is_pub,  i.data.path); ),
            (Trait,   _add_item_type(sp, mod, i.name, i.is_pub,  i.data.path); ),
            (TypeAlias, _add_item_type(sp, mod, i.name, i.is_pub,  i.data.path); ),
            
            (Struct,
                _add_item_type(sp, mod, i.name, i.is_pub,  i.data.path);
                // TODO: Items from extern crates don't populate e.struct_ correctly
                // - If the struct is a tuple-like struct, it presents in the value namespace
                if( e.struct_ && e.struct_->m_data.is_Tuple() ) {
                    _add_item_value(sp, mod, i.name, i.is_pub,  i.data.path);
                }
                ),
            (Static  , _add_item_value(sp, mod, i.name, i.is_pub,  i.data.path); ),
            (Function, _add_item_value(sp, mod, i.name, i.is_pub,  i.data.path); ),
            (EnumVar , _add_item_value(sp, mod, i.name, i.is_pub,  i.data.path); )
            )
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
        Resolve_Index_Module_Base(crate, *mp);
    }
}

void Resolve_Index_Module_Wildcard(AST::Module& mod, bool handle_pub)
{
    TRACE_FUNCTION_F("mod = " << mod.path());
    // Glob/wildcard imports
    for( const auto& i : mod.imports() )
    {
        if( i.name == "" && i.is_pub == handle_pub )
        {
            const auto& sp = i.data.sp;
            const auto& b = i.data.path.binding();
            TU_MATCH_DEF(::AST::PathBinding, (b), (e),
            (
                BUG(sp, "Glob import of invalid type encountered");
                ),
            (Unbound,
                BUG(sp, "Import left unbound ("<<i.data.path<<")");
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
                DEBUG("Glob mod " << i.data.path);
                if( !e.module_ )
                {
                    ASSERT_BUG(sp, e.hir, "Glob import where HIR module pointer not set - " << i.data.path);
                    const auto& hmod = *e.hir;
                    struct H {
                        static AST::Path hir_to_ast(const HIR::SimplePath& p) {
                            // The crate name here has to be non-empty, because it's external.
                            assert( p.m_crate_name != "" );
                            AST::Path   rv( p.m_crate_name, {} );
                            rv.nodes().reserve( p.m_components.size() );
                            for(const auto& n : p.m_components)
                                rv.nodes().push_back( AST::PathNode(n) );
                            return rv;
                        }
                    };
                    for(const auto& it : hmod.m_mod_items) {
                        const auto& ve = *it.second;
                        if( ve.is_public ) {
                            AST::Path   p;
                            if( ve.ent.is_Import() ) {
                                p = H::hir_to_ast( ve.ent.as_Import() );
                            }
                            else {
                                p = i.data.path + it.first;
                            }
                            _add_item_type( sp, mod, it.first, i.is_pub, mv$(p), false );
                        }
                    }
                    for(const auto& it : hmod.m_value_items) {
                        const auto& ve = *it.second;
                        if( ve.is_public ) {
                            AST::Path   p;
                            if( ve.ent.is_Import() ) {
                                p = H::hir_to_ast( ve.ent.as_Import() );
                            }
                            else {
                                p = i.data.path + it.first;
                            }
                            _add_item_value( sp, mod, it.first, i.is_pub, mv$(p), false );
                        }
                    }
                }
                else
                {
                    if( e.module_ == &mod ) {
                        ERROR(sp, E0000, "Glob import of self");
                    }
                    // 1. Begin indexing of this module if it is not already indexed
                    if( e.module_->m_index_populated == 1 )
                    {
                        // TODO: Handle wildcard import of a module with a public wildcard import
                        TODO(sp, "Handle wildcard import of module with a wildcard (" << e.module_->path() << " by " << mod.path() << ")");
                        //Resolve_Index_Module( *e.module_ );
                    }
                    for(const auto& vi : e.module_->m_type_items) {
                        if( vi.second.is_pub ) {
                            _add_item_type( sp, mod, vi.first, i.is_pub, vi.second.path, false );
                        }
                    }
                    for(const auto& vi : e.module_->m_value_items) {
                        if( vi.second.is_pub ) {
                            _add_item_value( sp, mod, vi.first, i.is_pub, vi.second.path, false );
                        }
                    }
                }
                ),
            (Enum,
                ASSERT_BUG(sp, e.enum_, "Glob import but enum pointer not set - " << i.data.path);
                DEBUG("Glob enum " << i.data.path);
                unsigned int idx = 0;
                for( const auto& ev : e.enum_->variants() ) {
                    ::AST::Path p = i.data.path + ev.m_name;
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
            Resolve_Index_Module_Wildcard(e, handle_pub);
            )
        )
    }
    for(auto& mp : mod.anon_mods())
    {
        Resolve_Index_Module_Wildcard(*mp, handle_pub);
    }
}


void Resolve_Index_Module_Normalise_Path(const ::AST::Crate& crate, const Span& sp, ::AST::Path& path)
{
    const auto& info = path.m_class.as_Absolute();
    if( info.crate != "" )
    {
        TODO(sp, "Resolve_Index_Module_Normalise_Path - Paths referring to extern crates - " << path);
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
            TODO(sp, "Replace imports");
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
                //const ::HIR::Module* hmod = &crate.m_extern_crates.at( e.name ).m_hir->m_root_module;
                const ::HIR::Module* hmod = &e.crate_->m_hir->m_root_module;
                for( i ++ ; i < info.nodes.size() - 1; i ++)
                {
                    auto it = hmod->m_mod_items.find( info.nodes[i].name() );
                    if( it == hmod->m_mod_items.end() ) {
                        ERROR(sp, E0000,  "Couldn't find node " << i << " of path " << path);
                    }
                    TU_MATCH_DEF(::HIR::TypeItem, (it->second->ent), (e),
                    (
                        BUG(sp, "Path " << path << " pointed to non-module in component " << i);
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
                        ),
                    //(Crate,
                    //    TODO(sp, "Crates within HIR");
                    //    ),
                    (Import,
                        TODO(sp, "Imports in HIR - Module");
                        )
                    )
                }
                
                auto it_m = hmod->m_mod_items.find( info.nodes[i].name() );
                if( it_m != hmod->m_mod_items.end() )
                {
                    TU_IFLET( ::HIR::TypeItem, it_m->second->ent, Import, e,
                        TODO(sp, "Imports in HIR - TypeItem");
                    )
                    else {
                        return ;
                    }
                }
                auto it_v = hmod->m_value_items.find( info.nodes[i].name() );
                if( it_v != hmod->m_value_items.end() )
                {
                    TU_IFLET( ::HIR::ValueItem, it_v->second->ent, Import, e,
                        TODO(sp, "Imports in HIR - ValueItem");
                    )
                    else {
                        return ;
                    }
                }
                
                ERROR(sp, E0000,  "Couldn't find final node of path " << path);
                ),
            (Enum,
                // NOTE: Just assuming that if an Enum is hit, it's sane
                return ;
                )
            )
        }
    }
    
    const auto& node = info.nodes.back();
    
    auto it = mod->m_namespace_items.find( node.name() );
    if( it == mod->m_namespace_items.end() )
        it = mod->m_value_items.find( node.name() );
    if( it == mod->m_value_items.end() )
        ERROR(sp, E0000,  "Couldn't find final node of path " << path);
    const auto& ie = it->second;
    
    if( ie.is_import ) {
        // TODO: Prevent infinite recursion if the user does something dumb
        path = ::AST::Path(ie.path);
        Resolve_Index_Module_Normalise_Path(crate, sp, path);
    }
    else {
        // All good
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
    
    for( auto& ent : mod.m_namespace_items ) {
        Resolve_Index_Module_Normalise_Path(crate, mod_span, ent.second.path);
    }
    for( auto& ent : mod.m_type_items ) {
        Resolve_Index_Module_Normalise_Path(crate, mod_span, ent.second.path);
    }
    for( auto& ent : mod.m_value_items ) {
        Resolve_Index_Module_Normalise_Path(crate, mod_span, ent.second.path);
    }
}

void Resolve_Index(AST::Crate& crate)
{
    // - Index all explicitly named items
    Resolve_Index_Module_Base(crate, crate.m_root_module);
    // - Add all public glob imported items - `pub use module::*`
    Resolve_Index_Module_Wildcard(crate.m_root_module, true);
    // - Add all private glob imported items
    Resolve_Index_Module_Wildcard(crate.m_root_module, false);
    
    // - Normalise the index (ensuring all paths point directly to the item)
    Resolve_Index_Module_Normalise(crate, Span(), crate.m_root_module);
}
