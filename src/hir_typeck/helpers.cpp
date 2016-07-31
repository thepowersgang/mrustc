
#include "helpers.hpp"
#include "expr_simple.hpp"


bool monomorphise_type_needed(const ::HIR::TypeRef& tpl);

bool monomorphise_pathparams_needed(const ::HIR::PathParams& tpl)
{
    for(const auto& ty : tpl.m_types)
        if( monomorphise_type_needed(ty) )
            return true;
    return false;
}
bool monomorphise_path_needed(const ::HIR::Path& tpl)
{
    TU_MATCH(::HIR::Path::Data, (tpl.m_data), (e),
    (Generic,
        return monomorphise_pathparams_needed(e.m_params);
        ),
    (UfcsInherent,
        return monomorphise_type_needed(*e.type) || monomorphise_pathparams_needed(e.params);
        ),
    (UfcsKnown,
        return monomorphise_type_needed(*e.type) || monomorphise_pathparams_needed(e.trait.m_params) || monomorphise_pathparams_needed(e.params);
        ),
    (UfcsUnknown,
        return monomorphise_type_needed(*e.type) || monomorphise_pathparams_needed(e.params);
        )
    )
    throw "";
}
bool monomorphise_traitpath_needed(const ::HIR::TraitPath& tpl)
{
    if( monomorphise_pathparams_needed(tpl.m_path.m_params) )    return true;
    for(const auto& assoc : tpl.m_type_bounds)
        if( monomorphise_type_needed(assoc.second) )
            return true;
    return false;
}
bool monomorphise_type_needed(const ::HIR::TypeRef& tpl)
{
    TU_MATCH(::HIR::TypeRef::Data, (tpl.m_data), (e),
    (Infer,
        assert(!"ERROR: _ type found in monomorphisation target");
        ),
    (Diverge,
        return false;
        ),
    (Primitive,
        return false;
        ),
    (Path,
        return monomorphise_path_needed(e.path);
        ),
    (Generic,
        return true;
        ),
    (TraitObject,
        if( monomorphise_traitpath_needed(e.m_trait) )
            return true;
        for(const auto& trait : e.m_markers)
            if( monomorphise_pathparams_needed(trait.m_params) )
                return true;
        return false;
        ),
    (Array,
        return monomorphise_type_needed(*e.inner);
        ),
    (Slice,
        return monomorphise_type_needed(*e.inner);
        ),
    (Tuple,
        for(const auto& ty : e) {
            if( monomorphise_type_needed(ty) )
                return true;
        }
        return false;
        ),
    (Borrow,
        return monomorphise_type_needed(*e.inner);
        ),
    (Pointer,
        return monomorphise_type_needed(*e.inner);
        ),
    (Function,
        for(const auto& ty : e.m_arg_types) {
            if( monomorphise_type_needed(ty) )
                return true;
        }
        return monomorphise_type_needed(*e.m_rettype);
        ),
    (Closure,
        for(const auto& ty : e.m_arg_types) {
            if( monomorphise_type_needed(ty) )
                return true;
        }
        return monomorphise_type_needed(*e.m_rettype);
        )
    )
    throw "";
}

::HIR::PathParams monomorphise_path_params_with(const Span& sp, const ::HIR::PathParams& tpl, t_cb_generic callback, bool allow_infer)
{
    ::HIR::PathParams   rv;
    for( const auto& ty : tpl.m_types) 
        rv.m_types.push_back( monomorphise_type_with(sp, ty, callback, allow_infer) );
    return rv;
}
::HIR::GenericPath monomorphise_genericpath_with(const Span& sp, const ::HIR::GenericPath& tpl, t_cb_generic callback, bool allow_infer)
{
    return ::HIR::GenericPath( tpl.m_path, monomorphise_path_params_with(sp, tpl.m_params, callback, allow_infer) );
}
::HIR::TraitPath monomorphise_traitpath_with(const Span& sp, const ::HIR::TraitPath& tpl, t_cb_generic callback, bool allow_infer)
{
    ::HIR::TraitPath    rv {
        monomorphise_genericpath_with(sp, tpl.m_path, callback, allow_infer),
        tpl.m_hrls,
        {},
        tpl.m_trait_ptr
        };
    
    for(const auto& assoc : tpl.m_type_bounds)
        rv.m_type_bounds.insert(::std::make_pair( assoc.first, monomorphise_type_with(sp, assoc.second, callback, allow_infer) ));
    
    return rv;
}
::HIR::TypeRef monomorphise_type_with(const Span& sp, const ::HIR::TypeRef& tpl, t_cb_generic callback, bool allow_infer)
{
    ::HIR::TypeRef  rv;
    TRACE_FUNCTION_FR("tpl = " << tpl, rv);
    TU_MATCH(::HIR::TypeRef::Data, (tpl.m_data), (e),
    (Infer,
        if( allow_infer ) {
            rv = ::HIR::TypeRef(e);
        }
        else {
           BUG(sp, "_ type found in monomorphisation target");
        }
        ),
    (Diverge,
        rv = ::HIR::TypeRef(e);
        ),
    (Primitive,
        rv = ::HIR::TypeRef(e);
        ),
    (Path,
        TU_MATCH(::HIR::Path::Data, (e.path.m_data), (e2),
        (Generic,
            rv = ::HIR::TypeRef( ::HIR::TypeRef::Data::Data_Path {
                    monomorphise_genericpath_with(sp, e2, callback, allow_infer),
                    e.binding.clone()
                    } );
            ),
        (UfcsKnown,
            rv = ::HIR::TypeRef( ::HIR::Path::Data::make_UfcsKnown({
                box$( monomorphise_type_with(sp, *e2.type, callback, allow_infer) ),
                monomorphise_genericpath_with(sp, e2.trait, callback, allow_infer),
                e2.item,
                monomorphise_path_params_with(sp, e2.params, callback, allow_infer)
                }) );
            ),
        (UfcsUnknown,
            TODO(sp, "UfcsUnknown - " << tpl);
            ),
        (UfcsInherent,
            TODO(sp, "UfcsInherent - " << tpl);
            )
        )
        ),
    (Generic,
        rv = callback(tpl).clone();
        ),
    (TraitObject,
        ::HIR::TypeRef::Data::Data_TraitObject  to;
        to.m_trait = monomorphise_traitpath_with(sp, e.m_trait, callback, allow_infer);
        for(const auto& trait : e.m_markers)
        {
            to.m_markers.push_back( monomorphise_genericpath_with(sp, trait, callback, allow_infer) ); 
        }
        to.m_lifetime = e.m_lifetime;
        rv = ::HIR::TypeRef( mv$(to) );
        ),
    (Array,
        if( e.size_val == ~0u ) {
            BUG(sp, "Attempting to clone array with unknown size - " << tpl);
        }
        rv = ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Array({
            box$( monomorphise_type_with(sp, *e.inner, callback) ),
            ::HIR::ExprPtr(),
            e.size_val
            }) );
        ),
    (Slice,
        rv = ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Slice({ box$(monomorphise_type_with(sp, *e.inner, callback)) }) );
        ),
    (Tuple,
        ::std::vector< ::HIR::TypeRef>  types;
        for(const auto& ty : e) {
            types.push_back( monomorphise_type_with(sp, ty, callback) );
        }
        rv = ::HIR::TypeRef( mv$(types) );
        ),
    (Borrow,
        rv = ::HIR::TypeRef::new_borrow(e.type, monomorphise_type_with(sp, *e.inner, callback));
        ),
    (Pointer,
        rv = ::HIR::TypeRef::new_pointer(e.type, monomorphise_type_with(sp, *e.inner, callback));
        ),
    (Function,
        ::HIR::FunctionType ft;
        ft.is_unsafe = e.is_unsafe;
        ft.m_abi = e.m_abi;
        ft.m_rettype = box$( monomorphise_type_with(sp, *e.m_rettype, callback) );
        for( const auto& arg : e.m_arg_types )
            ft.m_arg_types.push_back( monomorphise_type_with(sp, arg, callback) );
        rv = ::HIR::TypeRef( mv$(ft) );
        ),
    (Closure,
        ::HIR::TypeRef::Data::Data_Closure  oe;
        oe.node = e.node;
        oe.m_rettype = box$( monomorphise_type_with(sp, *e.m_rettype, callback) );
        for(const auto& a : e.m_arg_types)
            oe.m_arg_types.push_back( monomorphise_type_with(sp, a, callback) );
        rv = ::HIR::TypeRef(::HIR::TypeRef::Data::make_Closure( mv$(oe) ));
        )
    )
    return rv;
}

::HIR::TypeRef monomorphise_type(const Span& sp, const ::HIR::GenericParams& params_def, const ::HIR::PathParams& params,  const ::HIR::TypeRef& tpl)
{
    DEBUG("tpl = " << tpl);
    return monomorphise_type_with(sp, tpl, [&](const auto& gt)->const auto& {
        const auto& e = gt.m_data.as_Generic();
        if( e.name == "Self" )
            TODO(sp, "Handle 'Self' when monomorphising");
        //if( e.binding >= params_def.m_types.size() ) {
        //}
        if( e.binding >= params.m_types.size() ) {
            BUG(sp, "Generic param out of input range - " << e.binding << " '"<<e.name<<"' >= " << params.m_types.size());
        }
        return params.m_types[e.binding];
        }, false);
}



void HMTypeInferrence::dump() const
{
    unsigned int i = 0;
    for(const auto& v : m_ivars) {
        if(v.is_alias()) {
            //DEBUG("#" << i << " = " << v.alias);
        }
        else {
            DEBUG("#" << i << " = " << *v.type << FMT_CB(os,
                bool open = false;
                unsigned int i2 = 0;
                for(const auto& v2 : m_ivars) {
                    if( v2.is_alias() && v2.alias == i ) {
                        if( !open )
                            os << " { ";
                        open = true;
                        os << "#" << i2 << " ";
                    }
                    i2 ++;
                }
                if(open)
                    os << "}";
                ));
        }
        i ++ ;
    }
}
void HMTypeInferrence::compact_ivars()
{
    #if 1
    unsigned int i = 0;
    for(auto& v : m_ivars)
    {
        if( !v.is_alias() ) {
            //auto nt = this->expand_associated_types(Span(), v.type->clone());
            auto nt = v.type->clone();
            
            DEBUG("- " << i << " " << *v.type << " -> " << nt);
            *v.type = mv$(nt);
        }
        else {
            
            auto index = v.alias;
            unsigned int count = 0;
            assert(index < m_ivars.size());
            while( m_ivars.at(index).is_alias() ) {
                index = m_ivars.at(index).alias;
                
                if( count >= m_ivars.size() ) {
                    this->dump();
                    BUG(Span(), "Loop detected in ivar list when starting at " << v.alias << ", current is " << index);
                }
                count ++;
            }
            v.alias = index;
        }
        i ++;
    }
    #endif
}

bool HMTypeInferrence::apply_defaults()
{
    bool rv = false;
    for(auto& v : m_ivars)
    {
        if( !v.is_alias() ) {
            TU_IFLET(::HIR::TypeRef::Data, v.type->m_data, Infer, e,
                switch(e.ty_class)
                {
                case ::HIR::InferClass::None:
                    break;
                case ::HIR::InferClass::Integer:
                    rv = true;
                    DEBUG("- " << *v.type << " -> i32");
                    *v.type = ::HIR::TypeRef( ::HIR::CoreType::I32 );
                    break;
                case ::HIR::InferClass::Float:
                    rv = true;
                    DEBUG("- " << *v.type << " -> f64");
                    *v.type = ::HIR::TypeRef( ::HIR::CoreType::F64 );
                    break;
                }
            )
        }
    }
    return rv;
}   

void HMTypeInferrence::print_type(::std::ostream& os, const ::HIR::TypeRef& tr) const
{
    const auto& ty = this->get_type(tr);
    TU_MATCH(::HIR::TypeRef::Data, (ty.m_data), (e),
    (Infer,
        os << ty;
        ),
    (Primitive,
        os << ty;
        ),
    (Diverge, os << ty; ),
    (Generic, os << ty; ),
    (Path,
        TU_MATCH(::HIR::Path::Data, (e.path.m_data), (pe),
        (Generic,
            os << pe.m_path;
            this->print_pathparams(os, pe.m_params);
            ),
        (UfcsKnown,
            os << "<";
            this->print_type(os, *pe.type);
            os << " as " << pe.trait.m_path;
            this->print_pathparams(os, pe.trait.m_params);
            os << ">::" << pe.item;
            this->print_pathparams(os, pe.params);
            ),
        (UfcsInherent,
            os << "<";
            this->print_type(os, *pe.type);
            os << ">::" << pe.item;
            this->print_pathparams(os, pe.params);
            ),
        (UfcsUnknown,
            BUG(Span(), "UfcsUnknown");
            )
        )
        ),
    (Borrow,
        switch(e.type)
        {
        case ::HIR::BorrowType::Shared: os << "&";  break;
        case ::HIR::BorrowType::Unique: os << "&mut ";  break;
        case ::HIR::BorrowType::Owned:  os << "&move "; break;
        }
        this->print_type(os, *e.inner);
        ),
    (Pointer,
        switch(e.type)
        {
        case ::HIR::BorrowType::Shared: os << "*const ";  break;
        case ::HIR::BorrowType::Unique: os << "*mut ";  break;
        case ::HIR::BorrowType::Owned:  os << "*move "; break;
        }
        this->print_type(os, *e.inner);
        ),
    (Slice,
        os << "[";
        this->print_type(os, *e.inner);
        os << "]";
        ),
    (Array,
        os << "[";
        this->print_type(os, *e.inner);
        os << "; " << e.size_val << "]";
        ),
    (Closure,
        os << "{" << e.node << "}(";
        for(const auto& arg : e.m_arg_types) {
            this->print_type(os, arg);
            os << ",";
        }
        os << ")->";
        this->print_type(os, *e.m_rettype);
        ),
    (Function,
        if(e.is_unsafe)
            os << "unsafe ";
        if(e.m_abi != "")
            os << "extern \"" << e.m_abi << "\" ";
        os << "fn(";
        for(const auto& arg : e.m_arg_types) {
            this->print_type(os, arg);
            os << ",";
        }
        os << ")->";
        this->print_type(os, *e.m_rettype);
        ),
    (TraitObject,
        os << "(" << e.m_trait.m_path.m_path;
        this->print_pathparams(os, e.m_trait.m_path.m_params);
        for(const auto& marker : e.m_markers) {
            os << "+" << marker.m_path;
            this->print_pathparams(os, marker.m_params);
        }
        os << ")";
        ),
    (Tuple,
        os << "(";
        for(const auto& st : e) {
            this->print_type(os, st);
            os << ",";
        }
        os << ")";
        )
    )
}
void HMTypeInferrence::print_pathparams(::std::ostream& os, const ::HIR::PathParams& pps) const
{
    if( pps.m_types.size() > 0 ) {
        os << "<";
        for(const auto& pp_t : pps.m_types) {
            this->print_type(os, pp_t);
            os << ",";
        }
        os << ">";
    }
}

void HMTypeInferrence::expand_ivars(::HIR::TypeRef& type)
{
    TU_MATCH(::HIR::TypeRef::Data, (type.m_data), (e),
    (Infer,
        const auto& t = this->get_type(type);
        if( &t != &type ) {
            type = t.clone();
        }
        ),
    (Diverge,
        ),
    (Primitive,
        ),
    (Path,
        // Iterate all arguments
        TU_MATCH(::HIR::Path::Data, (e.path.m_data), (e2),
        (Generic,
            this->expand_ivars_params(e2.m_params);
            ),
        (UfcsKnown,
            this->expand_ivars(*e2.type);
            this->expand_ivars_params(e2.trait.m_params);
            this->expand_ivars_params(e2.params);
            ),
        (UfcsUnknown,
            this->expand_ivars(*e2.type);
            this->expand_ivars_params(e2.params);
            ),
        (UfcsInherent,
            this->expand_ivars(*e2.type);
            this->expand_ivars_params(e2.params);
            )
        )
        ),
    (Generic,
        ),
    (TraitObject,
        // TODO: Iterate all paths
        ),
    (Array,
        this->expand_ivars(*e.inner);
        ),
    (Slice,
        this->expand_ivars(*e.inner);
        ),
    (Tuple,
        for(auto& ty : e)
            this->expand_ivars(ty);
        ),
    (Borrow,
        this->expand_ivars(*e.inner);
        ),
    (Pointer,
        this->expand_ivars(*e.inner);
        ),
    (Function,
        // No ivars allowed?
        ),
    (Closure,
        this->expand_ivars(*e.m_rettype);
        for(auto& ty : e.m_arg_types)
            this->expand_ivars(ty);
        )
    )
}
void HMTypeInferrence::expand_ivars_params(::HIR::PathParams& params)
{
    for(auto& arg : params.m_types)
        expand_ivars(arg);
}

void HMTypeInferrence::add_ivars(::HIR::TypeRef& type)
{
    TU_MATCH(::HIR::TypeRef::Data, (type.m_data), (e),
    (Infer,
        if( e.index == ~0u ) {
            e.index = this->new_ivar();
            this->get_type(type).m_data.as_Infer().ty_class = e.ty_class;
            this->mark_change();
        }
        ),
    (Diverge,
        ),
    (Primitive,
        ),
    (Path,
        // Iterate all arguments
        TU_MATCH(::HIR::Path::Data, (e.path.m_data), (e2),
        (Generic,
            this->add_ivars_params(e2.m_params);
            ),
        (UfcsKnown,
            this->add_ivars(*e2.type);
            this->add_ivars_params(e2.trait.m_params);
            this->add_ivars_params(e2.params);
            ),
        (UfcsUnknown,
            this->add_ivars(*e2.type);
            this->add_ivars_params(e2.params);
            ),
        (UfcsInherent,
            this->add_ivars(*e2.type);
            this->add_ivars_params(e2.params);
            )
        )
        ),
    (Generic,
        ),
    (TraitObject,
        // Iterate all paths
        ),
    (Array,
        add_ivars(*e.inner);
        ),
    (Slice,
        add_ivars(*e.inner);
        ),
    (Tuple,
        for(auto& ty : e)
            add_ivars(ty);
        ),
    (Borrow,
        add_ivars(*e.inner);
        ),
    (Pointer,
        add_ivars(*e.inner);
        ),
    (Function,
        // No ivars allowed
        // TODO: Check?
        ),
    (Closure,
        // Shouldn't be possible
        )
    )
}
void HMTypeInferrence::add_ivars_params(::HIR::PathParams& params)
{
    for(auto& arg : params.m_types)
        add_ivars(arg);
}



unsigned int HMTypeInferrence::new_ivar()
{
    m_ivars.push_back( IVar() );
    m_ivars.back().type->m_data.as_Infer().index = m_ivars.size() - 1;
    return m_ivars.size() - 1;
}
::HIR::TypeRef HMTypeInferrence::new_ivar_tr()
{
    ::HIR::TypeRef rv;
    rv.m_data.as_Infer().index = this->new_ivar();
    return rv;
}

::HIR::TypeRef& HMTypeInferrence::get_type(::HIR::TypeRef& type)
{
    TU_IFLET(::HIR::TypeRef::Data, type.m_data, Infer, e,
        assert(e.index != ~0u);
        return *get_pointed_ivar(e.index).type;
    )
    else {
        return type;
    }
}

const ::HIR::TypeRef& HMTypeInferrence::get_type(const ::HIR::TypeRef& type) const
{
    TU_IFLET(::HIR::TypeRef::Data, type.m_data, Infer, e,
        assert(e.index != ~0u);
        return *get_pointed_ivar(e.index).type;
    )
    else {
        return type;
    }
}

void HMTypeInferrence::set_ivar_to(unsigned int slot, ::HIR::TypeRef type)
{
    auto sp = Span();
    auto& root_ivar = this->get_pointed_ivar(slot);
    DEBUG("set_ivar_to(" << slot << " { " << *root_ivar.type << " }, " << type << ")");
    
    // If the left type was '_', alias the right to it
    TU_IFLET(::HIR::TypeRef::Data, type.m_data, Infer, l_e,
        assert( l_e.index != slot );
        DEBUG("Set IVar " << slot << " = @" << l_e.index);
        
        if( l_e.ty_class != ::HIR::InferClass::None ) {
            TU_MATCH_DEF(::HIR::TypeRef::Data, (root_ivar.type->m_data), (e),
            (
                ERROR(sp, E0000, "Type unificiation of literal with invalid type - " << *root_ivar.type);
                ),
            (Primitive,
                typeck::check_type_class_primitive(sp, type, l_e.ty_class, e);
                ),
            (Infer,
                // TODO: Check for right having a ty_class
                if( e.ty_class != ::HIR::InferClass::None && e.ty_class != l_e.ty_class ) {
                    ERROR(sp, E0000, "Unifying types with mismatching literal classes");
                }
                )
            )
        }
        
        root_ivar.alias = l_e.index;
        root_ivar.type.reset();
    )
    else if( *root_ivar.type == type ) {
        return ;
    }
    else {
        // Otherwise, store left in right's slot
        DEBUG("Set IVar " << slot << " = " << type);
        TU_IFLET(::HIR::TypeRef::Data, root_ivar.type->m_data, Infer, e,
            switch(e.ty_class)
            {
            case ::HIR::InferClass::None:
                break;
            case ::HIR::InferClass::Integer:
            case ::HIR::InferClass::Float:
                // `type` can't be an ivar, so it has to be a primitive (or an associated?)
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (l_e),
                (
                    ),
                (Primitive,
                    typeck::check_type_class_primitive(sp, type, e.ty_class, l_e);
                    )
                )
                break;
            }
        )
        else {
            BUG(sp, "Overwriting ivar " << slot << " (" << *root_ivar.type << ") with " << type);
        }
        root_ivar.type = box$( mv$(type) );
    }
    
    this->mark_change();
}

void HMTypeInferrence::ivar_unify(unsigned int left_slot, unsigned int right_slot)
{
    auto sp = Span();
    if( left_slot != right_slot )
    {
        auto& left_ivar = this->get_pointed_ivar(left_slot);
        
        // TODO: Assert that setting this won't cause a loop.
        auto& root_ivar = this->get_pointed_ivar(right_slot);
        
        TU_IFLET(::HIR::TypeRef::Data, root_ivar.type->m_data, Infer, re,
            if(re.ty_class != ::HIR::InferClass::None) {
                TU_MATCH_DEF(::HIR::TypeRef::Data, (left_ivar.type->m_data), (le),
                (
                    ERROR(sp, E0000, "Type unificiation of literal with invalid type - " << *left_ivar.type);
                    ),
                (Infer,
                    if( le.ty_class != ::HIR::InferClass::None && le.ty_class != re.ty_class )
                    {
                        ERROR(sp, E0000, "Unifying types with mismatching literal classes");
                    }
                    le.ty_class = re.ty_class;
                    ),
                (Primitive,
                    typeck::check_type_class_primitive(sp, *left_ivar.type, re.ty_class, le);
                    )
                )
            }
        )
        else {
            BUG(sp, "Unifying over a concrete type - " << *root_ivar.type);
        }
        
        root_ivar.alias = left_slot;
        root_ivar.type.reset();
        
        this->mark_change();
    }
}
HMTypeInferrence::IVar& HMTypeInferrence::get_pointed_ivar(unsigned int slot) const
{
    auto index = slot;
    unsigned int count = 0;
    assert(index < m_ivars.size());
    while( m_ivars.at(index).is_alias() ) {
        index = m_ivars.at(index).alias;
        
        if( count >= m_ivars.size() ) {
            this->dump();
            BUG(Span(), "Loop detected in ivar list when starting at " << slot << ", current is " << index);
        }
        count ++;
    }
    return const_cast<IVar&>(m_ivars.at(index));
}

bool HMTypeInferrence::pathparams_contain_ivars(const ::HIR::PathParams& pps) const {
    for( const auto& ty : pps.m_types ) {
        if(this->type_contains_ivars(ty))
            return true;
    }
    return false;
}
bool HMTypeInferrence::type_contains_ivars(const ::HIR::TypeRef& ty) const {
    TU_MATCH(::HIR::TypeRef::Data, (this->get_type(ty).m_data), (e),
    (Infer, return true; ),
    (Primitive, return false; ),
    (Diverge, return false; ),
    (Generic, return false; ),
    (Path,
        TU_MATCH(::HIR::Path::Data, (e.path.m_data), (pe),
        (Generic,
            return pathparams_contain_ivars(pe.m_params);
            ),
        (UfcsKnown,
            if( type_contains_ivars(*pe.type) )
                return true;
            if( pathparams_contain_ivars(pe.trait.m_params) )
                return true;
            return pathparams_contain_ivars(pe.params);
            ),
        (UfcsInherent,
            if( type_contains_ivars(*pe.type) )
                return true;
            return pathparams_contain_ivars(pe.params);
            ),
        (UfcsUnknown,
            BUG(Span(), "UfcsUnknown");
            )
        )
        ),
    (Borrow,
        return type_contains_ivars(*e.inner);
        ),
    (Pointer,
        return type_contains_ivars(*e.inner);
        ),
    (Slice,
        return type_contains_ivars(*e.inner);
        ),
    (Array,
        return type_contains_ivars(*e.inner);
        ),
    (Closure,
        for(const auto& arg : e.m_arg_types)
            if( type_contains_ivars(arg) )
                return true;
        return type_contains_ivars(*e.m_rettype);
        ),
    (Function,
        for(const auto& arg : e.m_arg_types)
            if( type_contains_ivars(arg) )
                return true;
        return type_contains_ivars(*e.m_rettype);
        ),
    (TraitObject,
        for(const auto& marker : e.m_markers)
            if( pathparams_contain_ivars(marker.m_params) )
                return true;
        return pathparams_contain_ivars(e.m_trait.m_path.m_params);
        ),
    (Tuple,
        for(const auto& st : e)
            if( type_contains_ivars(st) )
                return true;
        return false;
        )
    )
    throw "";
}

namespace {
    bool type_list_equal(const HMTypeInferrence& context, const ::std::vector< ::HIR::TypeRef>& l, const ::std::vector< ::HIR::TypeRef>& r)
    {
        if( l.size() != r.size() )
            return false;
        
        for( unsigned int i = 0; i < l.size(); i ++ ) {
            if( !context.types_equal(l[i], r[i]) )
                return false;
        }
        return true;
    }
}
bool HMTypeInferrence::pathparams_equal(const ::HIR::PathParams& pps_l, const ::HIR::PathParams& pps_r) const
{
    return type_list_equal(*this, pps_l.m_types, pps_r.m_types);
}
bool HMTypeInferrence::types_equal(const ::HIR::TypeRef& rl, const ::HIR::TypeRef& rr) const
{
    const auto& l = this->get_type(rl);
    const auto& r = this->get_type(rr);
    if( l.m_data.tag() != r.m_data.tag() )
        return false;
    
    TU_MATCH(::HIR::TypeRef::Data, (l.m_data, r.m_data), (le, re),
    (Infer, return le.index == re.index; ),
    (Primitive, return le == re; ),
    (Diverge, return true; ),
    (Generic, return le.binding == re.binding; ),
    (Path,
        if( le.path.m_data.tag() != re.path.m_data.tag() )
            return false;
        TU_MATCH(::HIR::Path::Data, (le.path.m_data, re.path.m_data), (lpe, rpe),
        (Generic,
            if( lpe.m_path != rpe.m_path )
                return false;
            return pathparams_equal(lpe.m_params, rpe.m_params);
            ),
        (UfcsKnown,
            if( lpe.item != rpe.item )
                return false;
            if( types_equal(*lpe.type, *rpe.type) )
                return false;
            if( pathparams_equal(lpe.trait.m_params, rpe.trait.m_params) )
                return false;
            return pathparams_equal(lpe.params, rpe.params);
            ),
        (UfcsInherent,
            if( lpe.item != rpe.item )
                return false;
            if( types_equal(*lpe.type, *rpe.type) )
                return false;
            return pathparams_equal(lpe.params, rpe.params);
            ),
        (UfcsUnknown,
            BUG(Span(), "UfcsUnknown");
            )
        )
        ),
    (Borrow,
        if( le.type != re.type )
            return false;
        return types_equal(*le.inner, *re.inner);
        ),
    (Pointer,
        if( le.type != re.type )
            return false;
        return types_equal(*le.inner, *re.inner);
        ),
    (Slice,
        return types_equal(*le.inner, *re.inner);
        ),
    (Array,
        if( le.size_val != re.size_val )
            return false;
        return types_equal(*le.inner, *re.inner);
        ),
    (Closure,
        if( !type_list_equal(*this, le.m_arg_types, re.m_arg_types) )
            return false;
        return types_equal(*le.m_rettype, *re.m_rettype);
        ),
    (Function,
        if( !type_list_equal(*this, le.m_arg_types, re.m_arg_types) )
            return false;
        return types_equal(*le.m_rettype, *re.m_rettype);
        ),
    (TraitObject,
        if( le.m_markers.size() != re.m_markers.size() )
            return false;
        for(unsigned int i = 0; i < le.m_markers.size(); i ++) {
            const auto& lm = le.m_markers[i];
            const auto& rm = re.m_markers[i];
            if( lm.m_path != rm.m_path )
                return false;
            if( ! pathparams_equal(lm.m_params, rm.m_params) )
                return false;
        }
        if( le.m_trait.m_path.m_path != re.m_trait.m_path.m_path )
            return false;
        return pathparams_equal(le.m_trait.m_path.m_params, re.m_trait.m_path.m_params);
        ),
    (Tuple,
        return type_list_equal(*this, le, re);
        )
    )
    throw "";
}

// -------------------------------------------------------------------------------------------------------------------
//
// -------------------------------------------------------------------------------------------------------------------
void TraitResolution::prep_indexes()
{
    static Span sp_AAA;
    const Span& sp = sp_AAA;
    
    auto add_equality = [&](::HIR::TypeRef long_ty, ::HIR::TypeRef short_ty){
        DEBUG("[prep_indexes] ADD " << long_ty << " => " << short_ty);
        // TODO: Sort the two types by "complexity" (most of the time long >= short)
        this->m_type_equalities.insert(::std::make_pair( mv$(long_ty), mv$(short_ty) ));
        };
    
    this->iterate_bounds([&](const auto& b) {
        TU_MATCH_DEF(::HIR::GenericBound, (b), (be),
        (
            ),
        (TraitBound,
            DEBUG("[prep_indexes] `" << be.type << " : " << be.trait);
            for( const auto& tb : be.trait.m_type_bounds ) {
                DEBUG("[prep_indexes] Equality (TB) - <" << be.type << " as " << be.trait.m_path << ">::" << tb.first << " = " << tb.second);
                auto ty_l = ::HIR::TypeRef( ::HIR::Path( be.type.clone(), be.trait.m_path.clone(), tb.first ) );
                ty_l.m_data.as_Path().binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
                
                add_equality( mv$(ty_l), tb.second.clone() );
            }
            
            const auto& trait_params = be.trait.m_path.m_params;
            auto cb_mono = [&](const auto& ty)->const auto& {
                const auto& ge = ty.m_data.as_Generic();
                if( ge.binding == 0xFFFF ) {
                    return be.type;
                }
                else if( ge.binding < 256 ) {
                    unsigned idx = ge.binding % 256;
                    ASSERT_BUG(sp, idx < trait_params.m_types.size(), "Generic binding out of range in trait " << be.trait);
                    return trait_params.m_types[idx];
                }
                else {
                    BUG(sp, "Unknown generic binding " << ty);
                }
                };
            
            const auto& trait = m_crate.get_trait_by_path(sp, be.trait.m_path.m_path);
            for(const auto& a_ty : trait.m_types)
            {
                ::HIR::TypeRef ty_a;
                for( const auto& a_ty_b : a_ty.second.m_trait_bounds ) {
                    DEBUG("[prep_indexes] (Assoc) " << a_ty_b);
                    auto trait_mono = monomorphise_traitpath_with(sp, a_ty_b, cb_mono, false);
                    for( auto& tb : trait_mono.m_type_bounds ) {
                        if( ty_a == ::HIR::TypeRef() ) {
                            ty_a = ::HIR::TypeRef( ::HIR::Path( be.type.clone(), be.trait.m_path.clone(), a_ty.first ) );
                            ty_a.m_data.as_Path().binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
                        }
                        DEBUG("[prep_indexes] Equality (ATB) - <" << ty_a << " as " << a_ty_b.m_path << ">::" << tb.first << " = " << tb.second);
                        
                        auto ty_l = ::HIR::TypeRef( ::HIR::Path( ty_a.clone(), trait_mono.m_path.clone(), tb.first ) );
                        ty_l.m_data.as_Path().binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
                        
                        add_equality( mv$(ty_l), mv$(tb.second) );
                    }
                }
            }
            ),
        (TypeEquality,
            DEBUG("Equality - " << be.type << " = " << be.other_type);
            add_equality( be.type.clone(), be.other_type.clone() );
            )
        )
        return false;
        });
}


// -------------------------------------------------------------------------------------------------------------------
//
// -------------------------------------------------------------------------------------------------------------------
bool TraitResolution::iterate_bounds( ::std::function<bool(const ::HIR::GenericBound&)> cb) const
{
    const ::HIR::GenericParams* v[2] = { m_item_params, m_impl_params };
    for(auto p : v)
    {
        if( !p )    continue ;
        for(const auto& b : p->m_bounds)
            if(cb(b))   return true;
    }
    return false;
}
bool TraitResolution::find_trait_impls(const Span& sp,
        const ::HIR::SimplePath& trait, const ::HIR::PathParams& params,
        const ::HIR::TypeRef& ty,
        t_cb_trait_impl_r callback
        ) const
{
    static ::HIR::PathParams    null_params;
    static ::std::map< ::std::string, ::HIR::TypeRef>    null_assoc;

    const auto& type = this->m_ivars.get_type(ty);
    TRACE_FUNCTION_F("trait = " << trait << ", type = " << type);
    
    if( trait == this->m_crate.get_lang_item_path(sp, "sized") ) {
        TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (e),
        (
            // Any unknown - it's sized
            ),
        (Primitive,
            if( e == ::HIR::CoreType::Str )
                return false;
            ),
        (Slice,
            return false;
            ),
        (Path,
            // ... TODO (Search the innards or bounds)
            ),
        (TraitObject,
            return false;
            )
        )
        return callback( ImplRef(&type, &null_params, &null_assoc), ::HIR::Compare::Equal );
    }
    
    if( trait == this->m_crate.get_lang_item_path(sp, "copy") ) {
        struct H {
            static bool is_copy(const Span& sp, const TraitResolution& self, const ::HIR::TypeRef& ty) {
                const auto& type = self.m_ivars.get_type(ty);
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (e),
                (
                    // TODO: Search for impls?
                    TODO(sp, "Search for Copy impl on " << type);
                    ),
                (Primitive,
                    if( e == ::HIR::CoreType::Str )
                        return false;
                    return true;
                    ),
                (Borrow,
                    return e.type == ::HIR::BorrowType::Shared;
                    ),
                (Pointer,
                    return true;
                    ),
                (Array,
                    return is_copy(sp, self, *e.inner);
                    )
                )
            }
        };
        if( H::is_copy(sp, *this, type) ) {
            return callback( ImplRef(&type, &null_params, &null_assoc), ::HIR::Compare::Equal );
        }
        else {
            return false;
        }
    }

    const auto& trait_fn = this->m_crate.get_lang_item_path(sp, "fn");
    const auto& trait_fn_mut = this->m_crate.get_lang_item_path(sp, "fn_mut");
    const auto& trait_fn_once = this->m_crate.get_lang_item_path(sp, "fn_once");
    
    // Closures are magical. They're unnamable and all trait impls come from within the compiler
    TU_IFLET(::HIR::TypeRef::Data, type.m_data, Closure, e,
        DEBUG("Closure, "<< trait <<"  " << trait_fn << " " << trait_fn_mut << " " << trait_fn_once);
        if( trait == trait_fn || trait == trait_fn_mut || trait == trait_fn_once  ) {
            if( params.m_types.size() != 1 )
                BUG(sp, "Fn* traits require a single tuple argument");
            if( !params.m_types[0].m_data.is_Tuple() )
                BUG(sp, "Fn* traits require a single tuple argument");
            
            const auto& args_des = params.m_types[0].m_data.as_Tuple();
            if( args_des.size() != e.m_arg_types.size() ) {
                return false;
            }
            
            auto cmp = ::HIR::Compare::Equal;
            ::std::vector< ::HIR::TypeRef>  args;
            for(unsigned int i = 0; i < e.m_arg_types.size(); i ++)
            {
                const auto& at = e.m_arg_types[i];
                args.push_back( at.clone() );
                cmp &= at.compare_with_placeholders(sp, args_des[i], this->m_ivars.callback_resolve_infer());
            }
            
            // NOTE: This is a conditional "true", we know nothing about the move/mut-ness of this closure yet
            // - Could we?
            
            
            ::HIR::PathParams   pp;
            pp.m_types.push_back( ::HIR::TypeRef(mv$(args)) );
            ::std::map< ::std::string, ::HIR::TypeRef>  types;
            types.insert( ::std::make_pair( "Output", e.m_rettype->clone() ) );
            return callback( ImplRef(type.clone(), mv$(pp), mv$(types)), cmp );
        }
        else {
            return false;
        }
    )
    
    // Magic Fn* trait impls for function pointers
    TU_IFLET(::HIR::TypeRef::Data, type.m_data, Function, e,
        if( trait == trait_fn || trait == trait_fn_mut || trait == trait_fn_once  ) {
            if( params.m_types.size() != 1 )
                BUG(sp, "Fn* traits require a single tuple argument");
            
            // NOTE: unsafe or non-rust ABI functions aren't valid
            if( e.m_abi != "rust" || e.is_unsafe ) {
                DEBUG("- No magic impl, wrong ABI or unsafe in " << type);
                return false;
            }
            DEBUG("- Magic impl of Fn* for " << type);
            
            ::std::vector< ::HIR::TypeRef>  args;
            for(const auto& at : e.m_arg_types) {
                args.push_back( at.clone() );
            }
            
            // NOTE: This is a conditional "true", we know nothing about the move/mut-ness of this closure yet
            // - Could we?
            ::HIR::PathParams   pp;
            pp.m_types.push_back( ::HIR::TypeRef(mv$(args)) );
            ::std::map< ::std::string, ::HIR::TypeRef>  types;
            types.insert( ::std::make_pair( "Output", e.m_rettype->clone() ) );
            return callback( ImplRef(type.clone(), mv$(pp), mv$(types)), ::HIR::Compare::Equal );
        }
        // Continue
    )
    
    // 1. Search generic params
    if( find_trait_impls_bound(sp, trait, params, type, callback) )
        return true;
    // 2. Search crate-level impls
    return find_trait_impls_crate(sp, trait, params, type,  callback);
}

// -------------------------------------------------------------------------------------------------------------------
//
// -------------------------------------------------------------------------------------------------------------------

void TraitResolution::compact_ivars(HMTypeInferrence& m_ivars)
{
    //m_ivars.compact_ivars([&](const ::HIR::TypeRef& t)->auto{ return this->expand_associated_types(Span(), t.clone); });
    unsigned int i = 0;
    for(auto& v : m_ivars.m_ivars)
    {
        if( !v.is_alias() ) {
            m_ivars.expand_ivars( *v.type );
            // Don't expand unless it is needed
            if( this->has_associated_type(*v.type) ) {
                // TODO: cloning is expensive, BUT printing below is nice
                auto nt = this->expand_associated_types(Span(), v.type->clone());
                DEBUG("- " << i << " " << *v.type << " -> " << nt);
                *v.type = mv$(nt);
            }
        }
        else {
            
            auto index = v.alias;
            unsigned int count = 0;
            assert(index < m_ivars.m_ivars.size());
            while( m_ivars.m_ivars.at(index).is_alias() ) {
                index = m_ivars.m_ivars.at(index).alias;
                
                if( count >= m_ivars.m_ivars.size() ) {
                    this->m_ivars.dump();
                    BUG(Span(), "Loop detected in ivar list when starting at " << v.alias << ", current is " << index);
                }
                count ++;
            }
            v.alias = index;
        }
        i ++;
    }
}

bool TraitResolution::has_associated_type(const ::HIR::TypeRef& input) const
{
    struct H {
        static bool check_pathparams(const TraitResolution& r, const ::HIR::PathParams& pp) {
            for(const auto& arg : pp.m_types) {
                if( r.has_associated_type(arg) )
                    return true;
            }
            return false;
        }
    };
    //TRACE_FUNCTION_F(input);
    TU_MATCH(::HIR::TypeRef::Data, (input.m_data), (e),
    (Infer,
        auto& ty = this->m_ivars.get_type(input);
        if( ty != input ) {
            return this->has_associated_type(ty);
        }
        return false;
        ),
    (Diverge,
        return false;
        ),
    (Primitive,
        return false;
        ),
    (Path,
        TU_MATCH(::HIR::Path::Data, (e.path.m_data), (e2),
        (Generic,
            return H::check_pathparams(*this, e2.m_params);
            ),
        (UfcsInherent,
            TODO(Span(), "Path - UfcsInherent - " << e.path);
            ),
        (UfcsKnown,
            // - Only try resolving if the binding isn't known
            if( !e.binding.is_Unbound() )
                return false;
            return true;
            ),
        (UfcsUnknown,
            BUG(Span(), "Encountered UfcsUnknown");
            )
        )
        ),
    (Generic,
        return false;
        ),
    (TraitObject,
        // Recurse?
        if( H::check_pathparams(*this, e.m_trait.m_path.m_params) )
            return true;
        for(const auto& m : e.m_markers) {
            if( H::check_pathparams(*this, m.m_params) )
                return true;
        }
        return false;
        ),
    (Array,
        return has_associated_type(*e.inner);
        ),
    (Slice,
        return has_associated_type(*e.inner);
        ),
    (Tuple,
        bool rv = false;
        for(const auto& sub : e) {
            rv |= has_associated_type(sub);
        }
        return rv;
        ),
    (Borrow,
        return has_associated_type(*e.inner);
        ),
    (Pointer,
        return has_associated_type(*e.inner);
        ),
    (Function,
        // Recurse?
        return false;
        ),
    (Closure,
        // Recurse?
        return false;
        )
    )
    BUG(Span(), "Fell off the end of has_associated_type - input=" << input);
}
::HIR::TypeRef TraitResolution::expand_associated_types(const Span& sp, ::HIR::TypeRef input) const
{
    TRACE_FUNCTION_F(input);
    TU_MATCH(::HIR::TypeRef::Data, (input.m_data), (e),
    (Infer,
        auto& ty = this->m_ivars.get_type(input);
        if( ty != input ) {
            input = expand_associated_types(sp, ty.clone());
            return input;
        }
        else {
        }
        return input;
        ),
    (Diverge,
        ),
    (Primitive,
        ),
    (Path,
        TU_MATCH(::HIR::Path::Data, (e.path.m_data), (pe),
        (Generic,
            for(auto& arg : pe.m_params.m_types)
                arg = expand_associated_types(sp, mv$(arg));
            ),
        (UfcsInherent,
            TODO(sp, "Path - UfcsInherent - " << e.path);
            ),
        (UfcsKnown,
            // - Only try resolving if the binding isn't known
            if( !e.binding.is_Unbound() )
                return input;
            this->expand_associated_types__UfcsKnown(sp, input);
            return input;
            ),
        (UfcsUnknown,
            BUG(sp, "Encountered UfcsUnknown");
            )
        )
        ),
    (Generic,
        ),
    (TraitObject,
        // Recurse?
        ),
    (Array,
        *e.inner = expand_associated_types(sp, mv$(*e.inner));
        ),
    (Slice,
        *e.inner = expand_associated_types(sp, mv$(*e.inner));
        ),
    (Tuple,
        for(auto& sub : e) {
            sub = expand_associated_types(sp, mv$(sub));
        }
        ),
    (Borrow,
        *e.inner = expand_associated_types(sp, mv$(*e.inner));
        ),
    (Pointer,
        *e.inner = expand_associated_types(sp, mv$(*e.inner));
        ),
    (Function,
        // Recurse?
        ),
    (Closure,
        // Recurse?
        )
    )
    return input;
}


void TraitResolution::expand_associated_types__UfcsKnown(const Span& sp, ::HIR::TypeRef& input) const
{
    auto& e = input.m_data.as_Path();
    auto& pe = e.path.m_data.as_UfcsKnown();
    // TODO: If opaque, still search a list of known equalities
    
    DEBUG("Locating associated type for " << e.path);
    
    *pe.type = expand_associated_types(sp, mv$(*pe.type));
    
    
    // - If it's a closure, then the only trait impls are those generated by typeck
    TU_IFLET(::HIR::TypeRef::Data, pe.type->m_data, Closure, te,
        const auto trait_fn = this->m_crate.get_lang_item_path(sp, "fn");
        const auto trait_fn_mut = this->m_crate.get_lang_item_path(sp, "fn_mut");
        const auto trait_fn_once = this->m_crate.get_lang_item_path(sp, "fn_once");
        if( pe.trait.m_path == trait_fn || pe.trait.m_path == trait_fn_mut || pe.trait.m_path == trait_fn_once  ) {
            if( pe.item == "Output" ) {
                input = te.m_rettype->clone();
                return ;
            }
            else {
                ERROR(sp, E0000, "No associated type " << pe.item << " for trait " << pe.trait);
            }
        }
        else {
            ERROR(sp, E0000, "No implementation of " << pe.trait << " for " << *pe.type);
        }
    )
    
    //this->find_impl()
    
    // 1. Bounds
    bool rv;
    bool assume_opaque = true;
    rv = this->iterate_bounds([&](const auto& b) {
        TU_MATCH_DEF(::HIR::GenericBound, (b), (be),
        (
            ),
        (TraitBound,
            DEBUG("Trait bound - " << be.type << " : " << be.trait);
            // 1. Check if the type matches
            //  - TODO: This should be a fuzzier match?
            if( be.type != *pe.type )
                return false;
            // 2. Check if the trait (or any supertrait) includes pe.trait
            if( be.trait.m_path == pe.trait ) {
                auto it = be.trait.m_type_bounds.find(pe.item);
                // 1. Check if the bounds include the desired item
                if( it == be.trait.m_type_bounds.end() ) {
                    // If not, assume it's opaque and return as such
                    // TODO: What happens if there's two bounds that overlap? 'F: FnMut<()>, F: FnOnce<(), Output=Bar>'
                    DEBUG("Found impl for " << input << " but no bound on item, assuming opaque");
                }
                else {
                    assume_opaque = false;
                    input = it->second.clone();
                }
                return true;
            }
            
            bool found_supertrait = this->find_named_trait_in_trait(sp,
                pe.trait.m_path, pe.trait.m_params,
                *be.trait.m_trait_ptr, be.trait.m_path.m_path, be.trait.m_path.m_params, *pe.type,
                [&pe,&input,&assume_opaque](const auto&, const auto& x, const auto& assoc){
                    auto it = assoc.find(pe.item);
                    if( it != assoc.end() ) {
                        assume_opaque = false;
                        DEBUG("Found associated type " << input << " = " << it->second);
                        input = it->second.clone();
                    }
                    return true;
                }
                );
            if( found_supertrait ) {
                auto it = be.trait.m_type_bounds.find(pe.item);
                // 1. Check if the bounds include the desired item
                if( it == be.trait.m_type_bounds.end() ) {
                    // If not, assume it's opaque and return as such
                    // TODO: What happens if there's two bounds that overlap? 'F: FnMut<()>, F: FnOnce<(), Output=Bar>'
                    if( assume_opaque )
                        DEBUG("Found impl for " << input << " but no bound on item, assuming opaque");
                }
                else {
                    assume_opaque = false;
                    input = it->second.clone();
                }
                return true;
            }
            
            // - Didn't match
            ),
        (TypeEquality,
            DEBUG("Equality - " << be.type << " = " << be.other_type);
            if( input == be.type ) {
                assume_opaque = false;
                input = be.other_type.clone();
                return true;
            }
            )
        )
        return false;
        });
    if( rv ) {
        if( assume_opaque ) {
            DEBUG("Assuming that " << input << " is an opaque name");
            input.m_data.as_Path().binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
            
            DEBUG("- " << m_type_equalities.size() << " replacements");
            for( const auto& v : m_type_equalities )
                DEBUG(" > " << v.first << " = " << v.second);
            
            auto a = m_type_equalities.find(input);
            if( a != m_type_equalities.end() ) {
                input = a->second.clone();
            }
        }
        input = this->expand_associated_types(sp, mv$(input));
        return ;
    }

    // If the type of this UfcsKnown is ALSO a UfcsKnown - Check if it's bounded by this trait with equality
    // Use bounds on other associated types too (if `pe.type` was resolved to a fixed associated type)
    TU_IFLET(::HIR::TypeRef::Data, pe.type->m_data, Path, te_inner,
        TU_IFLET(::HIR::Path::Data, te_inner.path.m_data, UfcsKnown, pe_inner,
            // TODO: Search for equality bounds on this associated type (pe_inner) that match the entire type (pe)
            // - Does simplification of complex associated types
            const auto& trait_ptr = this->m_crate.get_trait_by_path(sp, pe_inner.trait.m_path);
            const auto& assoc_ty = trait_ptr.m_types.at(pe_inner.item);
            DEBUG("TODO: Search bounds on associated type - " << assoc_ty.m_trait_bounds);
            
            // Resolve where Self=pe_inner.type (i.e. for the trait this inner UFCS is on)
            auto cb_placeholders_trait = [&](const auto& ty)->const auto&{
                TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Generic, e,
                    if( e.binding == 0xFFFF )
                        return *pe_inner.type;
                    else {
                        // TODO: Look in pe_inner.trait.m_params
                        TODO(sp, "Handle type params when expanding associated bound (#" << e.binding << " " << e.name);
                    }
                )
                else {
                    return ty;
                }
                };
            for(const auto& bound : assoc_ty.m_trait_bounds)
            {
                // If the bound is for Self and the outer trait
                // - TODO: Parameters?
                if( bound.m_path == pe.trait ) {
                    auto it = bound.m_type_bounds.find( pe.item );
                    if( it != bound.m_type_bounds.end() ) {
                        if( monomorphise_type_needed(it->second) ) {
                            input = monomorphise_type_with(sp, it->second, cb_placeholders_trait);
                        }
                        else {
                            input = it->second.clone();
                        }
                        input = this->expand_associated_types(sp, mv$(input));
                        return ;
                    }
                }
            }
            DEBUG("pe = " << *pe.type << ", input = " << input);
        )
    )

    // 2. Crate-level impls
    // TODO: Search for the actual trait containing this associated type
    ::HIR::GenericPath  trait_path;
    if( !this->trait_contains_type(sp, pe.trait, this->m_crate.get_trait_by_path(sp, pe.trait.m_path), pe.item, trait_path) )
        BUG(sp, "Cannot find associated type " << pe.item << " anywhere in trait " << pe.trait);
    //pe.trait = mv$(trait_path);
    
    DEBUG("Searching for impl");
    rv = this->find_trait_impls_crate(sp, trait_path.m_path, trait_path.m_params, *pe.type, [&](auto impl, auto qual) {
        DEBUG("Found " << impl);
        auto ty = impl.get_type( pe.item.c_str() );
        if( ty == ::HIR::TypeRef() )
            ERROR(sp, E0000, "Couldn't find assocated type " << pe.item << " in " << pe.trait);
        
        DEBUG("Converted UfcsKnown - " << e.path << " = " << ty);
        input = mv$(ty);
        return true;
        });
    if( rv ) {
        input = this->expand_associated_types(sp, mv$(input));
        return ;
    }
    
    // If there are no ivars in this path, set its binding to Opaque
    if( !this->m_ivars.type_contains_ivars(input) ) {
        // TODO: If the type is a generic or an opaque associated, we can't know.
        // - If the trait contains any of the above, it's unknowable
        // - Otherwise, it's an error
        e.binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
        DEBUG("Couldn't resolve associated type for " << input << " (and won't ever be able to)");
    }
    else {
        DEBUG("Couldn't resolve associated type for " << input << " (will try again later)");
    }
}


// -------------------------------------------------------------------------------------------------------------------
//
// -------------------------------------------------------------------------------------------------------------------
bool TraitResolution::find_named_trait_in_trait(const Span& sp,
        const ::HIR::SimplePath& des, const ::HIR::PathParams& des_params,
        const ::HIR::Trait& trait_ptr, const ::HIR::SimplePath& trait_path, const ::HIR::PathParams& pp,
        const ::HIR::TypeRef& target_type,
        t_cb_trait_impl callback
    ) const
{
    TRACE_FUNCTION_F(des << " from " << trait_path << pp);
    if( pp.m_types.size() != trait_ptr.m_params.m_types.size() ) {
        BUG(sp, "Incorrect number of parameters for trait");
    }
    for( const auto& pt : trait_ptr.m_parent_traits )
    {
        auto pt_mono = monomorphise_traitpath_with(sp, pt, [&](const auto& gt)->const auto& {
            const auto& ge = gt.m_data.as_Generic();
            if( ge.binding == 0xFFFF ) {
                return target_type;
            }
            else {
                if( ge.binding >= pp.m_types.size() )
                    BUG(sp, "find_named_trait_in_trait - Generic #" << ge.binding << " " << ge.name << " out of range");
                return pp.m_types[ge.binding];
            }
            }, false);

        DEBUG(pt << " => " << pt_mono);
        if( pt.m_path.m_path == des ) {
            callback( target_type, pt_mono.m_path.m_params, pt_mono.m_type_bounds );
            return true;
        }
        
        const auto& tr = m_crate.get_trait_by_path(sp, pt.m_path.m_path);
        if( find_named_trait_in_trait(sp, des, des_params,  tr, pt.m_path.m_path, pt_mono.m_path.m_params,  target_type, callback) ) {
            return true;
        }
    }
    return false;
}
bool TraitResolution::find_trait_impls_bound(const Span& sp, const ::HIR::SimplePath& trait, const ::HIR::PathParams& params, const ::HIR::TypeRef& type,  t_cb_trait_impl_r callback) const
{
    struct H {
        static ::HIR::Compare compare_pp(const Span& sp, const TraitResolution& self, const ::HIR::PathParams& left, const ::HIR::PathParams& right) {
            ASSERT_BUG( sp, left.m_types.size() == right.m_types.size(), "Parameter count mismatch" );
            ::HIR::Compare  ord = ::HIR::Compare::Equal;
            for(unsigned int i = 0; i < left.m_types.size(); i ++) {
                ord &= left.m_types[i].compare_with_placeholders(sp, right.m_types[i], self.m_ivars.callback_resolve_infer());
                if( ord == ::HIR::Compare::Unequal )
                    return ord;
            }
            return ord;
        } 
    };
    const ::HIR::Path::Data::Data_UfcsKnown* assoc_info = nullptr;
    TU_IFLET(::HIR::TypeRef::Data, type.m_data, Path, e,
        TU_IFLET(::HIR::Path::Data, e.path.m_data, UfcsKnown, pe,
            assoc_info = &pe;
        )
    )
    
    // TODO: A bound can imply something via its associated types. How deep can this go?
    // E.g. `T: IntoIterator<Item=&u8>` implies `<T as IntoIterator>::IntoIter : Iterator<Item=&u8>`
    return this->iterate_bounds([&](const auto& b) {
        TU_IFLET(::HIR::GenericBound, b, TraitBound, e,
            const auto& b_params = e.trait.m_path.m_params;
            DEBUG("(bound) - " << e.type << " : " << e.trait);
            // TODO: Allow fuzzy equality?
            if( e.type == type )
            {
                if( e.trait.m_path.m_path == trait ) {
                    // Check against `params`
                    DEBUG("Checking " << params << " vs " << b_params);
                    auto ord = H::compare_pp(sp, *this, b_params, params);
                    if( ord == ::HIR::Compare::Unequal )
                        return false;
                    if( ord == ::HIR::Compare::Fuzzy ) {
                        DEBUG("Fuzzy match");
                    }
                    // Hand off to the closure, and return true if it does
                    // TODO: The type bounds are only the types that are specified.
                    if( callback( ImplRef(&e.type, &e.trait.m_path.m_params, &e.trait.m_type_bounds), ord) ) {
                        return true;
                    }
                }
                // HACK: The wrapping closure takes associated types from this bound and applies them to the returned set
                // - XXX: This is actually wrong (false-positive) in many cases. FIXME
                bool rv = this->find_named_trait_in_trait(sp,
                    trait,params,
                    *e.trait.m_trait_ptr, e.trait.m_path.m_path,e.trait.m_path.m_params,
                    type,
                    [&](const auto& ty, const auto& params, const auto& assoc) {
                        // TODO: Avoid duplicating this map every time
                        ::std::map< ::std::string,::HIR::TypeRef>   assoc2;
                        for(const auto& i : assoc) {
                            assoc2.insert( ::std::make_pair(i.first, i.second.clone())  );
                        }
                        for(const auto& i : e.trait.m_type_bounds) {
                            // TODO: Only include from above when needed
                            //if( des_trait_ref.m_types.count(i.first) ) {
                                assoc2.insert( ::std::make_pair(i.first, i.second.clone())  );
                            //}
                        }
                        return callback( ImplRef(ty.clone(), params.clone(), mv$(assoc2)), ::HIR::Compare::Equal );
                    });
                if( rv ) {
                    return true;
                }
            }
            
            // If the input type is an associated type controlled by this trait bound, check for added bounds.
            // TODO: This just checks a single layer, but it's feasable that there could be multiple layers
            if( assoc_info && e.trait.m_path.m_path == assoc_info->trait.m_path && e.type == *assoc_info->type ) {
                // Check the trait params
                auto ord = H::compare_pp(sp, *this, b_params, assoc_info->trait.m_params);
                if( ord == ::HIR::Compare::Fuzzy ) {
                    TODO(sp, "Handle fuzzy matches searching for associated type bounds");
                }
                
                const auto& trait_ref = *e.trait.m_trait_ptr;
                const auto& at = trait_ref.m_types.at(assoc_info->item);
                for(const auto& bound : at.m_trait_bounds) {
                    if( bound.m_path.m_path == trait ) {
                        DEBUG("- Found an associated type impl");
                        auto ord = H::compare_pp(sp, *this, b_params, params);
                        if( ord == ::HIR::Compare::Unequal )
                            return false;
                        if( ord == ::HIR::Compare::Fuzzy ) {
                            DEBUG("Fuzzy match");
                        }
                        
                        auto tp_mono = monomorphise_traitpath_with(sp, bound, [&](const auto& gt)->const auto& {
                            const auto& ge = gt.m_data.as_Generic();
                            if( ge.binding == 0xFFFF ) {
                                return *assoc_info->type;
                            }
                            else {
                                if( ge.binding >= assoc_info->trait.m_params.m_types.size() )
                                    BUG(sp, "find_trait_impls_bound - Generic #" << ge.binding << " " << ge.name << " out of range");
                                return assoc_info->trait.m_params.m_types[ge.binding];
                            }
                            }, false);
                        // - Expand associated types
                        for(auto& ty : tp_mono.m_type_bounds) {
                            ty.second = this->expand_associated_types(sp, mv$(ty.second));
                        }
                        DEBUG("- tp_mono = " << tp_mono);
                        // TODO: Instead of using `type` here, build the real type
                        if( callback( ImplRef(type.clone(), mv$(tp_mono.m_path.m_params), mv$(tp_mono.m_type_bounds)), ord ) ) {
                            return true;
                        }
                    }
                }
            }
            
            return false;
        )
        return false;
    });
}
bool TraitResolution::find_trait_impls_crate(const Span& sp,
        const ::HIR::SimplePath& trait, const ::HIR::PathParams& params,
        const ::HIR::TypeRef& type,
        t_cb_trait_impl_r callback
        ) const
{
    // TODO: Parameter defaults - apply here or in the caller?
    return this->m_crate.find_trait_impls(trait, type, [&](const auto& ty)->const auto&{
            if( ty.m_data.is_Infer() ) 
                return this->m_ivars.get_type(ty);
            else
                return ty;
        },
        [&](const auto& impl) {
            DEBUG("[find_trait_impls_crate] Found impl" << impl.m_params.fmt_args() << " " << trait << impl.m_trait_args << " for " << impl.m_type);
            // Compare with `params`
            auto    match = ::HIR::Compare::Equal;
            ::std::vector< const ::HIR::TypeRef*> impl_params;
            impl_params.resize( impl.m_params.m_types.size() );
            auto cb = [&](auto idx, const auto& ty) {
                DEBUG("[find_trait_impls_crate] Param " << idx << " = " << ty);
                assert( idx < impl_params.size() );
                if( ! impl_params[idx] ) {
                    impl_params[idx] = &ty;
                    return ::HIR::Compare::Equal;
                }
                else {
                    return impl_params[idx]->compare_with_placeholders(sp, ty, this->m_ivars.callback_resolve_infer());
                }
                };
            match &= impl.m_type.match_test_generics_fuzz(sp, type , this->m_ivars.callback_resolve_infer(), cb);
            // TODO: This is wrong (will false-positive), but works around an API deficiency (no "is there an impl for this trait with any param set")
            if( params.m_types.size() > 0 )
            {
                ASSERT_BUG(sp, impl.m_trait_args.m_types.size() == params.m_types.size(), "Param count mismatch between `" << impl.m_trait_args << "` and `" << params << "` for " << trait );
                for(unsigned int i = 0; i < impl.m_trait_args.m_types.size(); i ++)
                    match &= impl.m_trait_args.m_types[i].match_test_generics_fuzz(sp, params.m_types[i], this->m_ivars.callback_resolve_infer(), cb);
            }
            if( match == ::HIR::Compare::Unequal ) {
                DEBUG("- Failed to match parameters - " << impl.m_trait_args << "+" << impl.m_type << " != " << params << "+" << type);
                return false;
            }
            
            // TODO: Some impl blocks have type params used as part of type bounds.
            // - A rough idea is to have monomorph return a third class of generic for params that are not yet bound.
            //  - compare_with_placeholders gets called on both ivars and generics, so that can be used to replace it once known.
            ::std::vector< ::HIR::TypeRef>  placeholders;
            for(unsigned int i = 0; i < impl_params.size(); i ++ ) {
                if( !impl_params[i] ) {
                    if( placeholders.size() == 0 )
                        placeholders.resize(impl_params.size());
                    placeholders[i] = ::HIR::TypeRef("impl_?", 2*256 + i);
                }
            }
            auto cb_infer = [&](const auto& ty)->const auto& {
                if( ty.m_data.is_Infer() ) 
                    return this->m_ivars.get_type(ty);
                else if( ty.m_data.is_Generic() && ty.m_data.as_Generic().binding >> 8 == 2 ) { // Generic group 2 = Placeholders
                    unsigned int i = ty.m_data.as_Generic().binding % 256;
                    TODO(sp, "Obtain placeholder " << i);
                }
                else
                    return ty;
                };
            auto cb_match = [&](unsigned int idx, const auto& ty) {
                if( ty.m_data.is_Generic() && ty.m_data.as_Generic().binding == idx )
                    return ::HIR::Compare::Equal;
                if( idx >> 8 == 2 ) {
                    auto i = idx % 256;
                    ASSERT_BUG(sp, !impl_params[i], "Placeholder to populated type returned");
                    auto& ph = placeholders[i];
                    if( ph.m_data.is_Generic() && ph.m_data.as_Generic().binding == idx ) {
                        DEBUG("Bind placeholder " << i << " to " << ty);
                        ph = ty.clone();
                        return ::HIR::Compare::Equal;
                    }
                    else {
                        TODO(sp, "Compare placeholder " << i << " " << ph << " == " << ty);
                    }
                }
                else {
                    return ::HIR::Compare::Unequal;
                }
                };
            auto monomorph = [&](const auto& gt)->const auto& {
                    const auto& ge = gt.m_data.as_Generic();
                    ASSERT_BUG(sp, ge.binding >> 8 != 2, "");
                    assert( ge.binding < impl_params.size() );
                    if( !impl_params[ge.binding] ) {
                        //BUG(sp, "Param " << ge.binding << " for `impl" << impl.m_params.fmt_args() << " " << trait << impl.m_trait_args << " for " << impl.m_type << "` wasn't constrained");
                        return placeholders[ge.binding];
                    }
                    return *impl_params[ge.binding];
                    };
            auto ty_mono = monomorphise_type_with(sp, impl.m_type, monomorph, false);
            auto args_mono = monomorphise_path_params_with(sp, impl.m_trait_args, monomorph, false);
            
            // Check bounds for this impl
            // - If a bound fails, then this can't be a valid impl
            for(const auto& bound : impl.m_params.m_bounds)
            {
                TU_MATCH(::HIR::GenericBound, (bound), (be),
                (Lifetime,
                    ),
                (TypeLifetime,
                    ),
                (TraitBound,
                    DEBUG("Check bound " << be.type << " : " << be.trait);
                    auto real_type = monomorphise_type_with(sp, be.type, monomorph, false);
                    auto real_trait = monomorphise_traitpath_with(sp, be.trait, monomorph, false);
                    real_type = this->expand_associated_types(sp, mv$(real_type));
                    for(auto& p : real_trait.m_path.m_params.m_types) {
                        p = this->expand_associated_types(sp, mv$(p));
                    }
                    for(auto& ab : real_trait.m_type_bounds) {
                        ab.second = this->expand_associated_types(sp, mv$(ab.second));
                    }
                    const auto& real_trait_path = real_trait.m_path;
                    DEBUG("- " << real_type << " : " << real_trait);
                    auto rv = this->find_trait_impls(sp, real_trait_path.m_path, real_trait_path.m_params, real_type, [&](auto impl, auto impl_cmp) {
                        for(const auto& assoc_bound : real_trait.m_type_bounds) {
                            ::HIR::TypeRef  tmp;
                            const ::HIR::TypeRef*   ty_p;
                            
                            tmp = impl.get_type(assoc_bound.first.c_str());
                            if( tmp == ::HIR::TypeRef() ) {
                                // This bound isn't from this particular trait, go the slow way of using expand_associated_types
                                tmp = this->expand_associated_types(sp, ::HIR::TypeRef(
                                    ::HIR::Path(::HIR::Path::Data::Data_UfcsKnown { box$(real_type.clone()), real_trait_path.clone(), assoc_bound.first, {} }))
                                    );
                                ty_p = &tmp;
                            }
                            else {
                                ty_p = &this->m_ivars.get_type(tmp);
                            }
                            const auto& ty = *ty_p;
                            DEBUG(" - Compare " << ty << " and " << assoc_bound.second << ", matching generics");
                            auto cmp = assoc_bound.second .match_test_generics_fuzz(sp, ty, cb_infer, cb_match);
                            switch(cmp)
                            {
                            case ::HIR::Compare::Equal:
                                continue;
                            case ::HIR::Compare::Unequal:
                                DEBUG("Assoc failure - " << ty << " != " << assoc_bound.second);
                                return false;
                            case ::HIR::Compare::Fuzzy:
                                // TODO: When a fuzzy match is encountered on a conditional bound, returning `false` can lead to an false negative (and a compile error)
                                // BUT, returning `true` could lead to it being selected. (Is this a problem, should a later validation pass check?)
                                DEBUG("[find_trait_impls_crate] Fuzzy match assoc bound between " << ty << " and " << assoc_bound.second);
                                continue ;
                            }
                        }
                        return true;
                        });
                    if( !rv ) {
                        DEBUG("- Bound " << real_type << " : " << real_trait_path << " failed");
                        return false;
                    }
                    ),
                (TypeEquality,
                    TODO(sp, "Check bound " << be.type << " = " << be.other_type);
                    )
                )
            }
            
            ::std::map< ::std::string, ::HIR::TypeRef>  types;
            for( const auto& aty : impl.m_types )
            {
                types.insert( ::std::make_pair(aty.first,  this->expand_associated_types(sp, monomorphise_type_with(sp, aty.second, monomorph))) );
            }
            // TODO: Ensure that there are no-longer any magic params
            
            DEBUG("[find_trait_impls_crate] callback(args=" << args_mono << ", assoc={" << types << "})");
            //if( match == ::HIR::Compare::Fuzzy ) {
            //    TODO(sp, "- Pass on fuzzy match status");
            //}
            return callback(ImplRef(mv$(impl_params), impl, mv$(placeholders)), match);
            //return callback(ty_mono, args_mono, types/*, (match == ::HIR::Compare::Fuzzy)*/);
        }
        );
}

bool TraitResolution::trait_contains_method(const Span& sp, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait_ptr, const ::std::string& name,  ::HIR::GenericPath& out_path) const
{
    auto it = trait_ptr.m_values.find(name);
    if( it != trait_ptr.m_values.end() ) {
        if( it->second.is_Function() ) {
            const auto& v = it->second.as_Function();
            if( v.m_args.size() > 0 && v.m_args[0].first.m_binding.m_name == "self" ) {
                out_path = trait_path.clone();
                return true;
            }
        }
    }
    
    // TODO: Prevent infinite recursion
    for(const auto& st : trait_ptr.m_parent_traits)
    {
        auto& st_ptr = this->m_crate.get_trait_by_path(sp, st.m_path.m_path);
        if( trait_contains_method(sp, st.m_path, st_ptr, name, out_path) ) {
            out_path.m_params = monomorphise_path_params_with(sp, mv$(out_path.m_params), [&](const auto& gt)->const auto& {
                const auto& ge = gt.m_data.as_Generic();
                assert(ge.binding < 256);
                assert(ge.binding < trait_path.m_params.m_types.size());
                return trait_path.m_params.m_types[ge.binding];
                }, false);
            return true;
        }
    }
    return false;
}
bool TraitResolution::trait_contains_type(const Span& sp, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait_ptr, const ::std::string& name,  ::HIR::GenericPath& out_path) const
{
    auto it = trait_ptr.m_types.find(name);
    if( it != trait_ptr.m_types.end() ) {
        out_path = trait_path.clone();
        return true;
    }
    
    auto monomorph = [&](const auto& gt)->const auto& {
            const auto& ge = gt.m_data.as_Generic();
            assert(ge.binding < 256);
            assert(ge.binding < trait_path.m_params.m_types.size());
            return trait_path.m_params.m_types[ge.binding];
            };
    // TODO: Prevent infinite recursion
    for(const auto& st : trait_ptr.m_parent_traits)
    {
        auto& st_ptr = this->m_crate.get_trait_by_path(sp, st.m_path.m_path);
        if( trait_contains_type(sp, st.m_path, st_ptr, name, out_path) ) {
            out_path.m_params = monomorphise_path_params_with(sp, mv$(out_path.m_params), monomorph, false);
            return true;
        }
    }
    return false;
}



// -------------------------------------------------------------------------------------------------------------------
//
// -------------------------------------------------------------------------------------------------------------------
const ::HIR::TypeRef* TraitResolution::autoderef(const Span& sp, const ::HIR::TypeRef& ty,  ::HIR::TypeRef& tmp_type) const
{
    TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Borrow, e,
        DEBUG("Deref " << ty << " into " << *e.inner);
        return &*e.inner;
    )
    else TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
        DEBUG("Deref " << ty << " into [" << *e.inner << "]");
        tmp_type = ::HIR::TypeRef::new_slice( e.inner->clone() );
        return &tmp_type;
    )
    else {
        bool succ = this->find_trait_impls(sp, this->m_crate.get_lang_item_path(sp, "deref"), ::HIR::PathParams {}, ty, [&](auto impls, auto match) {
            tmp_type = impls.get_type("Target");
            return true;
            });
        if( succ ) {
            return &tmp_type;
        }
        else {
            return nullptr;
        }
    }
}
unsigned int TraitResolution::autoderef_find_method(const Span& sp, const HIR::t_trait_list& traits, const ::HIR::TypeRef& top_ty, const ::std::string& method_name,  /* Out -> */::HIR::Path& fcn_path) const
{
    unsigned int deref_count = 0;
    ::HIR::TypeRef  tmp_type;   // Temporary type used for handling Deref
    const auto* current_ty = &top_ty;
    TU_IFLET(::HIR::TypeRef::Data, this->m_ivars.get_type(top_ty).m_data, Borrow, e,
        current_ty = &*e.inner;
        deref_count += 1;
    )
    
    do {
        const auto& ty = this->m_ivars.get_type(*current_ty);
        if( ty.m_data.is_Infer() ) {
            return ~0u;
        }
        
        if( this->find_method(sp, traits, ty, method_name, fcn_path) ) {
            return deref_count;
        }
        
        // 3. Dereference and try again
        deref_count += 1;
        current_ty = this->autoderef(sp, ty,  tmp_type);
    } while( current_ty );
    
    TU_IFLET(::HIR::TypeRef::Data, this->m_ivars.get_type(top_ty).m_data, Borrow, e,
        const auto& ty = this->m_ivars.get_type(top_ty);
        
        if( find_method(sp, traits, ty, method_name, fcn_path) ) {
            return 0;
        }
    )
    
    // Dereference failed! This is a hard error (hitting _ is checked above and returns ~0)
    if( this->m_ivars.type_contains_ivars(top_ty) )
    {
        return ~0u;
    }
    else
    {
        this->m_ivars.dump();
        ERROR(sp, E0000, "Could not find method `" << method_name << "` on type `" << top_ty << "`");
    }
}

bool TraitResolution::find_method(const Span& sp, const HIR::t_trait_list& traits, const ::HIR::TypeRef& ty, const ::std::string& method_name,  /* Out -> */::HIR::Path& fcn_path) const
{
    TRACE_FUNCTION_F("ty=" << ty << ", name=" << method_name);
    // 1. Search generic bounds for a match
    const ::HIR::GenericParams* v[2] = { m_item_params, m_impl_params };
    for(auto p : v)
    {
        if( !p )    continue ;
        for(const auto& b : p->m_bounds)
        {
            TU_IFLET(::HIR::GenericBound, b, TraitBound, e,
                DEBUG("Bound " << e.type << " : " << e.trait.m_path);
                // TODO: Match using _ replacement
                if( e.type != ty )
                    continue ;
                
                // - Bound's type matches, check if the bounded trait has the method we're searching for
                //  > TODO: Search supertraits too
                DEBUG("- Matches " << ty);
                ::HIR::GenericPath final_trait_path;
                assert(e.trait.m_trait_ptr);
                if( !this->trait_contains_method(sp, e.trait.m_path, *e.trait.m_trait_ptr, method_name,  final_trait_path) )
                    continue ;
                DEBUG("- Found trait " << final_trait_path);
                
                // Found the method, return the UFCS path for it
                fcn_path = ::HIR::Path( ::HIR::Path::Data::make_UfcsKnown({
                    box$( ty.clone() ),
                    mv$(final_trait_path),
                    method_name,
                    {}
                    }) );
                return true;
            )
        }
    }

    TU_IFLET(::HIR::TypeRef::Data, ty.m_data, TraitObject, e,
        // TODO: This _Should_ be set, but almost needs a pass?
        //assert( e.m_trait.m_trait_ptr );
        //const auto& trait = *e.m_trait.m_trait_ptr;
        const auto& trait = this->m_crate.get_trait_by_path(sp, e.m_trait.m_path.m_path);
        auto it = trait.m_values.find( method_name );
        if( it != trait.m_values.end() )
        {
            if( it->second.is_Function() ) {
                const auto& v = it->second.as_Function();
                if( v.m_args.size() > 0 && v.m_args[0].first.m_binding.m_name == "self" ) {
                    fcn_path = ::HIR::Path( ::HIR::Path::Data::Data_UfcsKnown({
                        box$( ty.clone() ),
                        e.m_trait.m_path.clone(),
                        method_name,
                        {}
                        }) );
                    return true;
                }
            }
        }
    )
    
    TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Generic, e,
        // No match, keep trying.
    )
    else if( ty.m_data.is_Path() && ty.m_data.as_Path().path.m_data.is_UfcsKnown() )
    {
        const auto& e = ty.m_data.as_Path().path.m_data.as_UfcsKnown();
        // UFCS known - Assuming that it's reached the maximum resolvable level (i.e. a type within is generic), search for trait bounds on the type
        const auto& trait = this->m_crate.get_trait_by_path(sp, e.trait.m_path);
        const auto& assoc_ty = trait.m_types.at( e.item );
        // NOTE: The bounds here have 'Self' = the type
        for(const auto& bound : assoc_ty.m_trait_bounds )
        {
            ASSERT_BUG(sp, bound.m_trait_ptr, "Pointer to trait " << bound.m_path << " not set in " << e.trait.m_path);
            ::HIR::GenericPath final_trait_path;
            if( !this->trait_contains_method(sp, bound.m_path, *bound.m_trait_ptr, method_name,  final_trait_path) )
                continue ;
            DEBUG("- Found trait " << final_trait_path);
            
            // Found the method, return the UFCS path for it
            fcn_path = ::HIR::Path( ::HIR::Path::Data::make_UfcsKnown({
                box$( ty.clone() ),
                mv$(final_trait_path),
                method_name,
                {}
                }) );
            return true;
        }
    }
    else {
        // 2. Search for inherent methods
        for(const auto& impl : m_crate.m_type_impls)
        {
            if( impl.matches_type(ty) ) {
                auto it = impl.m_methods.find( method_name );
                if( it == impl.m_methods.end() )
                    continue ;
                if( it->second.m_args.size() > 0 && it->second.m_args[0].first.m_binding.m_name == "self" ) {
                    DEBUG("Matching `impl" << impl.m_params.fmt_args() << " " << impl.m_type << "`"/* << " - " << top_ty*/);
                    fcn_path = ::HIR::Path( ::HIR::Path::Data::make_UfcsInherent({
                        box$(ty.clone()),
                        method_name,
                        {}
                        }) );
                    return true;
                }
            }
        }
        // 3. Search for trait methods (using currently in-scope traits)
        for(const auto& trait_ref : ::reverse(traits))
        {
            if( trait_ref.first == nullptr )
                break;
            
            //::HIR::GenericPath final_trait_path;
            //if( !this->trait_contains_method(sp, *trait_ref.first, *trait_ref.second, method_name,  final_trait_path) )
            //    continue ;
            //DEBUG("- Found trait " << final_trait_path);
            
            // TODO: Search supertraits too
            auto it = trait_ref.second->m_values.find(method_name);
            if( it == trait_ref.second->m_values.end() )
                continue ;
            if( !it->second.is_Function() )
                continue ;
            const auto& v = it->second.as_Function();
            if( v.m_args.size() > 0 && v.m_args[0].first.m_binding.m_name == "self" ) {
                DEBUG("Search for impl of " << *trait_ref.first);
                
                //::HIR::PathParams   params;
                //for(const auto& t : trait_ref.second->m_params.m_types) {
                //    (void)t;
                //    params.m_types.push_back( m_ivars.new_ivar_tr() );
                //}
                
                // TODO: Need a "don't care" marker for the PathParams
                if( find_trait_impls_crate(sp, *trait_ref.first, ::HIR::PathParams{}, ty,  [](auto , auto ) { return true; }) ) {
                    DEBUG("Found trait impl " << *trait_ref.first << " (" /*<< m_ivars.fmt_type(*trait_ref.first)*/  << ") for " << ty << " ("<<m_ivars.fmt_type(ty)<<")");
                    fcn_path = ::HIR::Path( ::HIR::Path::Data::make_UfcsKnown({
                        box$( ty.clone() ),
                        trait_ref.first->clone(),
                        method_name,
                        {}
                        }) );
                    return true;
                }
            }
        }
    }
    
    return false;
}

unsigned int TraitResolution::autoderef_find_field(const Span& sp, const ::HIR::TypeRef& top_ty, const ::std::string& field_name,  /* Out -> */::HIR::TypeRef& field_type) const
{
    unsigned int deref_count = 0;
    ::HIR::TypeRef  tmp_type;   // Temporary type used for handling Deref
    const auto* current_ty = &top_ty;
    TU_IFLET(::HIR::TypeRef::Data, this->m_ivars.get_type(top_ty).m_data, Borrow, e,
        current_ty = &*e.inner;
        deref_count += 1;
    )
    
    do {
        const auto& ty = this->m_ivars.get_type(*current_ty);
        if( ty.m_data.is_Infer() ) {
            return ~0u;
        }
        
        if( this->find_field(sp, ty, field_name, field_type) ) {
            return deref_count;
        }
        
        // 3. Dereference and try again
        deref_count += 1;
        current_ty = this->autoderef(sp, ty,  tmp_type);
    } while( current_ty );
    
    TU_IFLET(::HIR::TypeRef::Data, this->m_ivars.get_type(top_ty).m_data, Borrow, e,
        const auto& ty = this->m_ivars.get_type(top_ty);
        
        if( find_field(sp, ty, field_name, field_type) ) {
            return 0;
        }
    )
    
    // Dereference failed! This is a hard error (hitting _ is checked above and returns ~0)
    this->m_ivars.dump();
    TODO(sp, "Error when no field could be found, but type is known - (: " << top_ty << ")." << field_name);
}
bool TraitResolution::find_field(const Span& sp, const ::HIR::TypeRef& ty, const ::std::string& name,  /* Out -> */::HIR::TypeRef& field_ty) const
{
    TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Path, e,
        TU_MATCH(::HIR::TypeRef::TypePathBinding, (e.binding), (be),
        (Unbound,
            // Wut?
            TODO(sp, "Handle TypePathBinding::Unbound - " << ty);
            ),
        (Opaque,
            // Ignore, no fields on an opaque
            ),
        (Struct,
            // Has fields!
            const auto& str = *be;
            const auto& params = e.path.m_data.as_Generic().m_params;
            auto monomorph = [&](const auto& gt)->const auto& {
                const auto& ge = gt.m_data.as_Generic();
                if( ge.binding == 0xFFFF )
                    TODO(sp, "Monomorphise struct field types (Self) - " << gt);
                else if( ge.binding < 256 ) {
                    assert(ge.binding < params.m_types.size());
                    return params.m_types[ge.binding];
                }
                else {
                    BUG(sp, "function-level param encountered in struct field");
                }
                return gt;
                };
            TU_MATCH(::HIR::Struct::Data, (str.m_data), (se),
            (Unit,
                // No fields on a unit struct
                ),
            (Tuple,
                for( unsigned int i = 0; i < se.size(); i ++ )
                {
                    // TODO: Privacy
                    if( FMT(i) == name ) {
                        field_ty = monomorphise_type_with(sp, se[i].ent, monomorph);
                        return true;
                    }
                }
                ),
            (Named,
                for( const auto& fld : se )
                {
                    // TODO: Privacy
                    if( fld.first == name ) {
                        field_ty = monomorphise_type_with(sp, fld.second.ent, monomorph);
                        return true;
                    }
                }
                )
            )
            ),
        (Enum,
            // No fields on enums either
            )
        )
    )
    else TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Tuple, e,
        for( unsigned int i = 0; i < e.size(); i ++ )
        {
            if( FMT(i) == name ) {
                field_ty = e[i].clone();
                return true;
            }
        }
    )
    else {
    }
    return false;
}


