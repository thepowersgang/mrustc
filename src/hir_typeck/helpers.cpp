
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
        rv.m_types.push_back( monomorphise_type_with(sp, ty, callback) );
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
            TODO(sp, "UfcsUnknown");
            ),
        (UfcsInherent,
            TODO(sp, "UfcsInherent");
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
            DEBUG("#" << i << " = " << v.alias);
        }
        else {
            DEBUG("#" << i << " = " << *v.type);
        }
        i ++ ;
    }
}
void HMTypeInferrence::compact_ivars()
{
    #if 0
    unsigned int i = 0;
    for(auto& v : m_ivars)
    {
        if( !v.is_alias() ) {
            auto nt = this->expand_associated_types(Span(), v.type->clone());
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
                    *v.type = ::HIR::TypeRef( ::HIR::CoreType::I32 );
                    break;
                case ::HIR::InferClass::Float:
                    rv = true;
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
    struct H {
        static void print_pp(const HMTypeInferrence& ctxt, ::std::ostream& os, const ::HIR::PathParams& pps) {
            if( pps.m_types.size() > 0 ) {
                os << "<";
                for(const auto& pp_t : pps.m_types) {
                    ctxt.print_type(os, pp_t);
                    os << ",";
                }
                os << ">";
            }
        }
    };
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
            H::print_pp(*this, os, pe.m_params);
            ),
        (UfcsKnown,
            os << "<";
            this->print_type(os, *pe.type);
            os << " as " << pe.trait.m_path;
            H::print_pp(*this, os, pe.trait.m_params);
            os << ">::" << pe.item;
            H::print_pp(*this, os, pe.params);
            ),
        (UfcsInherent,
            os << "<";
            this->print_type(os, *pe.type);
            os << ">::" << pe.item;
            H::print_pp(*this, os, pe.params);
            ),
        (UfcsUnknown,
            BUG(Span(), "UfcsUnknown");
            )
        )
        ),
    (Borrow,
        os << "&";
        this->print_type(os, *e.inner);
        ),
    (Pointer,
        os << "*";
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
        //for(const auto& arg : e.m_arg_types)
        //    if( type_contains_ivars(arg) )
        //        return true;
        //return type_contains_ivars(*e.m_rettype);
        ),
    (Function,
        //for(const auto& arg : e.m_arg_types)
        //    if( type_contains_ivars(arg) )
        //        return true;
        //return type_contains_ivars(*e.m_rettype);
        ),
    (TraitObject,
        os << "(" << e.m_trait.m_path.m_path;
        H::print_pp(*this, os, e.m_trait.m_path.m_params);
        for(const auto& marker : e.m_markers) {
            os << "+" << marker.m_path;
            H::print_pp(*this, os, marker.m_params);
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
