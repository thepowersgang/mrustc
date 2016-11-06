/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/helpers.cpp
 * - Typecheck helpers
 */
#include "helpers.hpp"

// --------------------------------------------------------------------
// HMTypeInferrence
// --------------------------------------------------------------------
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
void HMTypeInferrence::check_for_loops()
{
    struct LoopChecker {
        ::std::vector<unsigned int> m_indexes;
        
        void check_pathparams(const HMTypeInferrence& ivars, const ::HIR::PathParams& pp) {
            for(const auto& ty : pp.m_types)
                this->check_ty(ivars, ty);
        }
        void check_ty(const HMTypeInferrence& ivars, const ::HIR::TypeRef& ty) {
            TU_MATCH( ::HIR::TypeRef::Data, (ty.m_data), (e),
            (Infer,
                for(auto idx : m_indexes)
                    ASSERT_BUG(Span(), e.index != idx, "Recursion in ivar #" << m_indexes.front() << " " << *ivars.m_ivars[m_indexes.front()].type
                        << " - loop with " << idx << " " << *ivars.m_ivars[idx].type);
                const auto& ivd = ivars.get_pointed_ivar(e.index);
                assert( !ivd.is_alias() );
                if( !ivd.type->m_data.is_Infer() ) {
                    m_indexes.push_back( e.index );
                    this->check_ty(ivars, *ivd.type);
                    m_indexes.pop_back( );
                }
                ),
            (Primitive,
                ),
            (Diverge, ),
            (Generic, ),
            (Path,
                TU_MATCH(::HIR::Path::Data, (e.path.m_data), (pe),
                (Generic,
                    this->check_pathparams(ivars, pe.m_params);
                    ),
                (UfcsKnown,
                    this->check_ty(ivars, *pe.type);
                    this->check_pathparams(ivars, pe.trait.m_params);
                    this->check_pathparams(ivars, pe.params);
                    ),
                (UfcsInherent,
                    this->check_ty(ivars, *pe.type);
                    this->check_pathparams(ivars, pe.params);
                    ),
                (UfcsUnknown,
                    BUG(Span(), "UfcsUnknown");
                    )
                )
                ),
            (Borrow,
                this->check_ty(ivars, *e.inner);
                ),
            (Pointer,
                this->check_ty(ivars, *e.inner);
                ),
            (Slice,
                this->check_ty(ivars, *e.inner);
                ),
            (Array,
                this->check_ty(ivars, *e.inner);
                ),
            (Closure,
                ),
            (Function,
                for(const auto& arg : e.m_arg_types) {
                    this->check_ty(ivars, arg);
                }
                this->check_ty(ivars, *e.m_rettype);
                ),
            (TraitObject,
                this->check_pathparams(ivars, e.m_trait.m_path.m_params);
                for(const auto& marker : e.m_markers) {
                    this->check_pathparams(ivars, marker.m_params);
                }
                ),
            (ErasedType,
                TODO(Span(), "ErasedType");
                ),
            (Tuple,
                for(const auto& st : e) {
                    this->check_ty(ivars, st);
                }
                )
            )
        }
    };
    unsigned int i = 0;
    for(const auto& v : m_ivars)
    {
        if( !v.is_alias() && !v.type->m_data.is_Infer() )
        {
            DEBUG("- " << i << " " << *v.type);
            (LoopChecker { {i} }).check_ty(*this, *v.type);
        }
        i ++;
    }
}
void HMTypeInferrence::compact_ivars()
{
    this->check_for_loops();
    
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
                case ::HIR::InferClass::Diverge:
                    rv = true;
                    DEBUG("- " << *v.type << " -> !");
                    *v.type = ::HIR::TypeRef(::HIR::TypeRef::Data::make_Diverge({}));
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
    (ErasedType,
        // TODO: Print correctly (with print_type calls)
        os << "impl ";
        for(const auto& tr : e.m_traits) {
            if( &tr != &e.m_traits[0] )
                os << "+";
            os << tr;
        }
        if( e.m_lifetime.name != "" )
            os << "+ '" << e.m_lifetime.name;
        os << "/*" << e.m_origin << "*/";
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
        this->expand_ivars_params(e.m_trait.m_path.m_params);
        for(auto& marker : e.m_markers)
            this->expand_ivars_params(marker.m_params);
        ),
    (ErasedType,
        TODO(Span(), "ErasedType");
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
        this->expand_ivars(*e.m_rettype);
        for(auto& ty : e.m_arg_types)
            this->expand_ivars(ty);
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
        this->add_ivars_params(e.m_trait.m_path.m_params);
        for(auto& marker : e.m_markers)
            this->add_ivars_params(marker.m_params);
        ),
    (ErasedType,
        BUG(Span(), "ErasedType getting ivars added");
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
        add_ivars(*e.m_rettype);
        for(auto& ty : e.m_arg_types)
            add_ivars(ty);
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
                check_type_class_primitive(sp, type, l_e.ty_class, e);
                ),
            (Infer,
                // Check for right having a ty_class
                if( e.ty_class != ::HIR::InferClass::None && e.ty_class != l_e.ty_class ) {
                    ERROR(sp, E0000, "Unifying types with mismatching literal classes - " << type << " := " << *root_ivar.type);
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
            case ::HIR::InferClass::Diverge:
                break;
            case ::HIR::InferClass::Integer:
            case ::HIR::InferClass::Float:
                // `type` can't be an ivar, so it has to be a primitive (or an associated?)
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (l_e),
                (
                    ),
                (Primitive,
                    check_type_class_primitive(sp, type, e.ty_class, l_e);
                    )
                )
                break;
            }
        )
        #if 0
        else TU_IFLET(::HIR::TypeRef::Data, root_ivar.type->m_data, Diverge, e,
            // Overwriting ! with anything is valid (it's like a magic ivar)
        )
        #endif
        else {
            BUG(sp, "Overwriting ivar " << slot << " (" << *root_ivar.type << ") with " << type);
        }
        
        #if 1
        TU_IFLET(::HIR::TypeRef::Data, type.m_data, Diverge, e,
            root_ivar.type->m_data.as_Infer().ty_class = ::HIR::InferClass::Diverge;
        )
        else
        #endif
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
            if(re.ty_class != ::HIR::InferClass::None && re.ty_class != ::HIR::InferClass::Diverge) {
                TU_MATCH_DEF(::HIR::TypeRef::Data, (left_ivar.type->m_data), (le),
                (
                    ERROR(sp, E0000, "Type unificiation of literal with invalid type - " << *left_ivar.type);
                    ),
                (Infer,
                    if( le.ty_class != ::HIR::InferClass::None && le.ty_class != re.ty_class )
                    {
                        ERROR(sp, E0000, "Unifying types with mismatching literal classes - " << *left_ivar.type << " := " << *root_ivar.type);
                    }
                    le.ty_class = re.ty_class;
                    ),
                (Primitive,
                    check_type_class_primitive(sp, *left_ivar.type, re.ty_class, le);
                    )
                )
            }
        )
        else {
            BUG(sp, "Unifying over a concrete type - " << *root_ivar.type);
        }
        
        DEBUG("IVar " << root_ivar.type->m_data.as_Infer().index << " = @" << left_slot);
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
    TRACE_FUNCTION_F("ty = " << ty);
    //TU_MATCH(::HIR::TypeRef::Data, (this->get_type(ty).m_data), (e),
    TU_MATCH(::HIR::TypeRef::Data, (ty.m_data), (e),
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
    (ErasedType,
        TODO(Span(), "ErasedType");
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
            if( ! types_equal(*lpe.type, *rpe.type) )
                return false;
            if( ! pathparams_equal(lpe.trait.m_params, rpe.trait.m_params) )
                return false;
            return pathparams_equal(lpe.params, rpe.params);
            ),
        (UfcsInherent,
            if( lpe.item != rpe.item )
                return false;
            if( ! types_equal(*lpe.type, *rpe.type) )
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
    (ErasedType,
        TODO(Span(), "ErasedType");
        ),
    (Tuple,
        return type_list_equal(*this, le, re);
        )
    )
    throw "";
}

// --------------------------------------------------------------------
// TraitResolution
// --------------------------------------------------------------------
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


::HIR::Compare TraitResolution::compare_pp(const Span& sp, const ::HIR::PathParams& left, const ::HIR::PathParams& right) const
{
    ASSERT_BUG( sp, left.m_types.size() == right.m_types.size(), "Parameter count mismatch - `"<<left<<"` vs `"<<right<<"`" );
    ::HIR::Compare  ord = ::HIR::Compare::Equal;
    for(unsigned int i = 0; i < left.m_types.size(); i ++) {
        // TODO: Should allow fuzzy matches using placeholders (match_test_generics_fuzz works for that)
        // - Better solution is to remove the placeholders in method searching.
        ord &= left.m_types[i].compare_with_placeholders(sp, right.m_types[i], this->m_ivars.callback_resolve_infer());
        if( ord == ::HIR::Compare::Unequal )
            return ord;
    }
    return ord;
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
    TRACE_FUNCTION_F("trait = " << trait << params  << ", type = " << type);
    
    const auto& lang_Sized = this->m_crate.get_lang_item_path(sp, "sized");
    const auto& lang_Copy = this->m_crate.get_lang_item_path(sp, "copy");
    const auto& lang_Unsize = this->m_crate.get_lang_item_path(sp, "unsize");
    const auto& lang_CoerceUnsized = this->m_crate.get_lang_item_path(sp, "coerce_unsized");
    const auto& trait_fn = this->m_crate.get_lang_item_path(sp, "fn");
    const auto& trait_fn_mut = this->m_crate.get_lang_item_path(sp, "fn_mut");
    const auto& trait_fn_once = this->m_crate.get_lang_item_path(sp, "fn_once");
    const auto& trait_index = this->m_crate.get_lang_item_path(sp, "index");
    const auto& trait_indexmut = this->m_crate.get_lang_item_path(sp, "index_mut");
    
    if( trait == lang_Sized ) {
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
    
    if( trait == lang_Copy ) {
        auto cmp = this->type_is_copy(sp, type);
        if( cmp != ::HIR::Compare::Unequal ) {
            return callback( ImplRef(&type, &null_params, &null_assoc), cmp );
        }
        else {
            return false;
        }
    }
    
    // Magic Unsize impls to trait objects
    if( trait == lang_Unsize ) {
        ASSERT_BUG(sp, params.m_types.size() == 1, "Unsize trait requires a single type param");
        const auto& dst_ty = this->m_ivars.get_type(params.m_types[0]);
        TU_IFLET( ::HIR::TypeRef::Data, dst_ty.m_data, TraitObject, e,
            // Magic impl if T: ThisTrait
            bool good;
            ::HIR::Compare  total_cmp = ::HIR::Compare::Equal;
            auto cb = [&](const auto&, auto cmp){
                if( cmp == ::HIR::Compare::Unequal )
                    return false;
                total_cmp &= cmp;
                return true;
                };
            if( e.m_trait.m_path.m_path == ::HIR::SimplePath() ) {
                ASSERT_BUG(sp, e.m_markers.size() > 0, "TraitObject with no traits - " << dst_ty);
                good = true;
            }
            else {
                good = find_trait_impls(sp, e.m_trait.m_path.m_path, e.m_trait.m_path.m_params, ty, cb);
            }
            for(const auto& marker : e.m_markers)
            {
                if(!good)   break;
                good &= find_trait_impls(sp, marker.m_path, marker.m_params, ty, cb);
            }
            if( good ) {
                return callback( ImplRef(type.clone(), params.clone(), {}), total_cmp );
            }
            else {
                return false;
            }
        )
        
        // [T;N] -> [T] is handled down with array indexing
    }
    
    // Magical CoerceUnsized impls for various types
    if( trait == lang_CoerceUnsized ) {
        const auto& dst_ty = params.m_types.at(0);
        // - `*mut T => *const T`
        TU_IFLET( ::HIR::TypeRef::Data, type.m_data, Pointer, e,
            TU_IFLET( ::HIR::TypeRef::Data, dst_ty.m_data, Pointer, de,
                if( de.type < e.type ) {
                    auto cmp = e.inner->compare_with_placeholders(sp, *de.inner, this->m_ivars.callback_resolve_infer());
                    if( cmp != ::HIR::Compare::Unequal )
                    {
                        ::HIR::PathParams   pp;
                        pp.m_types.push_back( dst_ty.clone() );
                        if( callback( ImplRef(type.clone(), mv$(pp), {}), cmp ) ) {
                            return true;
                        }
                    }
                }
            )
        )
    }
    
    // Magic impls of the Fn* traits for closure types
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
            if( cmp != ::HIR::Compare::Unequal )
            {
                // NOTE: This is a conditional "true", we know nothing about the move/mut-ness of this closure yet
                // - Could we?
                // - Not until after the first stage of typeck
                
                DEBUG("Closure Fn* impl - cmp = " << cmp);
                
                ::HIR::PathParams   pp;
                pp.m_types.push_back( ::HIR::TypeRef(mv$(args)) );
                ::std::map< ::std::string, ::HIR::TypeRef>  types;
                types.insert( ::std::make_pair( "Output", e.m_rettype->clone() ) );
                return callback( ImplRef(type.clone(), mv$(pp), mv$(types)), cmp );
            }
            else
            {
                DEBUG("Closure Fn* impl - cmp = Compare::Unequal");
                return false;
            }
        }
    )
    
    // Magic Fn* trait impls for function pointers
    TU_IFLET(::HIR::TypeRef::Data, type.m_data, Function, e,
        if( trait == trait_fn || trait == trait_fn_mut || trait == trait_fn_once  ) {
            if( params.m_types.size() != 1 )
                BUG(sp, "Fn* traits require a single tuple argument");
            if( !params.m_types[0].m_data.is_Tuple() )
                BUG(sp, "Fn* traits require a single tuple argument");
            const auto& args_des = params.m_types[0].m_data.as_Tuple();
            if( args_des.size() != e.m_arg_types.size() ) {
                return false;
            }
            
            // NOTE: unsafe or non-rust ABI functions aren't valid
            if( e.m_abi != ABI_RUST || e.is_unsafe ) {
                DEBUG("- No magic impl, wrong ABI or unsafe in " << type);
                return false;
            }
            DEBUG("- Magic impl of Fn* for " << type);
            
            auto cmp = ::HIR::Compare::Equal;
            ::std::vector< ::HIR::TypeRef>  args;
            for(unsigned int i = 0; i < e.m_arg_types.size(); i ++)
            {
                const auto& at = e.m_arg_types[i];
                args.push_back( at.clone() );
                cmp &= at.compare_with_placeholders(sp, args_des[i], this->m_ivars.callback_resolve_infer());
            }
            
            ::HIR::PathParams   pp;
            pp.m_types.push_back( ::HIR::TypeRef(mv$(args)) );
            ::std::map< ::std::string, ::HIR::TypeRef>  types;
            types.insert( ::std::make_pair( "Output", e.m_rettype->clone() ) );
            return callback( ImplRef(type.clone(), mv$(pp), mv$(types)), cmp );
        }
        // Continue
    )
    
    // Magic index and unsize impls for Arrays
    // NOTE: The index impl for [T] is in libcore.
    TU_IFLET(::HIR::TypeRef::Data, type.m_data, Array, e,
        if( trait == trait_index || trait == trait_indexmut ) {
            if( params.m_types.size() != 1 )
                BUG(sp, "Index* traits require a single argument");
            DEBUG("- Magic impl of Index* for " << type);
            const auto& index_ty = m_ivars.get_type(params.m_types[0]);
            
            ::HIR::Compare  cmp;
            
            // Index<usize> ?
            auto ty_usize = ::HIR::TypeRef(::HIR::CoreType::Usize);
            cmp = ty_usize.compare_with_placeholders(sp, index_ty, this->m_ivars.callback_resolve_infer());
            if( cmp != ::HIR::Compare::Unequal )
            {
                ::HIR::PathParams   pp;
                pp.m_types.push_back( mv$(ty_usize) );
                ::std::map< ::std::string, ::HIR::TypeRef>  types;
                types.insert( ::std::make_pair( "Output", e.inner->clone() ) );
                return callback( ImplRef(type.clone(), mv$(pp), mv$(types)), cmp );
            }
            
            /*
            // TODO: Index<Range/RangeFrom/RangeTo/FullRange>? - Requires knowing the path to the range ops (which isn't a lang item)
            ::HIR::PathParams   pp;
            pp.m_types.push_back( ::HIR::TypeRef(::HIR::CoreType::Usize) );
            auto ty_range = ::HIR::TypeRef( ::HIR::GenericPath(this->m_crate.get_lang_item_path(sp, "range"), mv$(pp)) );
            cmp = ty_range.compare_with_placeholders(sp, index_ty, this->m_ivars.callback_resolve_infer());
            if( cmp != ::HIR::Compare::Unequal ) {
                ::HIR::PathParams   pp;
                pp.m_types.push_back( mv$(ty_range) );
                ::std::map< ::std::string, ::HIR::TypeRef>  types;
                types.insert(::std::make_pair( "Output", ::HIR::TypeRef::new_slice(e.inner->clone()) ));
                return callback( ImplRef(type.clone(), mv$(pp), mv$(types)), cmp );
            )
            */
            return false;
        }
        
        // Unsize impl for arrays
        if( trait == lang_Unsize )
        {
            ASSERT_BUG(sp, params.m_types.size() == 1, "");
            const auto& dst_ty = m_ivars.get_type( params.m_types[0] );
            
            TU_IFLET(::HIR::TypeRef::Data, dst_ty.m_data, Slice, e2,
                auto cmp = e.inner->compare_with_placeholders(sp, *e2.inner, m_ivars.callback_resolve_infer());
                if( cmp != ::HIR::Compare::Unequal ) {
                    ::HIR::PathParams   pp;
                    // - <[`array_inner`]> so it can be matched with the param by the caller
                    pp.m_types.push_back( ::HIR::TypeRef::new_slice(e.inner->clone()) );
                    return callback( ImplRef(type.clone(), mv$(pp), {}), cmp );
                }
            )
        }
    )
    
    
    // Trait objects automatically implement their own traits
    // - IF object safe (TODO)
    TU_IFLET(::HIR::TypeRef::Data, type.m_data, TraitObject, e,
        if( trait == e.m_trait.m_path.m_path ) {
            auto cmp = compare_pp(sp, e.m_trait.m_path.m_params, params);
            if( cmp != ::HIR::Compare::Unequal ) {
                DEBUG("TraitObject impl params" << e.m_trait.m_path.m_params);
                return callback( ImplRef(&type, &e.m_trait.m_path.m_params, &e.m_trait.m_type_bounds), cmp );
            }
        }
        // Markers too
        for( const auto& mt : e.m_markers )
        {
            if( trait == mt.m_path ) {
                auto cmp = compare_pp(sp, mt.m_params, params);
                if( cmp != ::HIR::Compare::Unequal ) {
                    static ::std::map< ::std::string, ::HIR::TypeRef>  types;
                    return callback( ImplRef(&type, &mt.m_params, &types), cmp );
                }
            }
        }
        
        // - Check if the desired trait is a supertrait of this.
        // NOTE: `params` (aka des_params) is not used (TODO)
        bool rv = false;
        bool is_supertrait = this->find_named_trait_in_trait(sp, trait,params, *e.m_trait.m_trait_ptr, e.m_trait.m_path.m_path,e.m_trait.m_path.m_params, type,
            [&](const auto& i_ty, const auto& i_params, const auto& i_assoc) {
                // The above is just the monomorphised params and associated set. Comparison is still needed.
                auto cmp = this->compare_pp(sp, i_params, params);
                if( cmp != ::HIR::Compare::Unequal ) {
                    // Invoke callback with a proper ImplRef
                    ::std::map< ::std::string, ::HIR::TypeRef> assoc_clone;
                    for(const auto& e : i_assoc)
                        assoc_clone.insert( ::std::make_pair(e.first, e.second.clone()) );
                    auto ir = ImplRef(i_ty.clone(), i_params.clone(), mv$(assoc_clone));
                    DEBUG("- ir = " << ir);
                    rv = callback(mv$(ir), cmp);
                    return true;
                }
                return false;
            });
        if( is_supertrait )
        {
            return rv;
        }
        
        // Trait objects can unsize to a subset of their traits.
        if( trait == lang_Unsize )
        {
            ASSERT_BUG(sp, params.m_types.size() == 1, "");
            const auto& dst_ty = m_ivars.get_type( params.m_types[0] );
            if( ! dst_ty.m_data.is_TraitObject() ) {
                // If the destination isn't a trait object, don't even bother
                return false;
            }
            const auto& e2 = dst_ty.m_data.as_TraitObject();
            
            auto cmp = ::HIR::Compare::Equal;
            
            // TODO: Fuzzy compare
            if( e2.m_trait != e.m_trait ) {
                return false;
            }
            // The destination must have a strict subset of marker traits.
            const auto& src_markers = e.m_markers;
            const auto& dst_markers = e2.m_markers;
            for(const auto& mt : dst_markers)
            {
                // TODO: Fuzzy match
                bool found = false;
                for(const auto& omt : src_markers) {
                    if( omt == mt ) {
                        found = true;
                        break;
                    }
                }
                if( !found ) {
                    // Return early.
                    return false;
                }
            }
            
            return callback( ImplRef(&type, &e.m_trait.m_path.m_params, &e.m_trait.m_type_bounds), cmp );
        }
    )

    // If the type in question is a magic placeholder, return a placeholder impl :)
    TU_IFLET(::HIR::TypeRef::Data, type.m_data, Generic, e,
        if( (e.binding >> 8) == 2 )
        {
            // TODO: This is probably going to break something in the future.
            DEBUG("- Magic impl for placeholder type");
            return callback( ImplRef(&type, &null_params, &null_assoc), ::HIR::Compare::Fuzzy );
        }
    )
    
    // If this type is an opaque UfcsKnown - check bounds
    TU_IFLET(::HIR::TypeRef::Data, type.m_data, Path, e,
        if( e.binding.is_Opaque() )
        {
            ASSERT_BUG(sp, e.path.m_data.is_UfcsKnown(), "Opaque bound type wasn't UfcsKnown - " << type);
            const auto& pe = e.path.m_data.as_UfcsKnown();
            
            // If this associated type has a bound of the desired trait, return it.
            const auto& trait_ref = m_crate.get_trait_by_path(sp, pe.trait.m_path);
            ASSERT_BUG(sp, trait_ref.m_types.count( pe.item ) != 0, "Trait " << pe.trait.m_path << " doesn't contain an associated type " << pe.item);
            const auto& aty_def = trait_ref.m_types.find(pe.item)->second;
            
            auto monomorph_cb = monomorphise_type_get_cb(sp, &*pe.type, &pe.trait.m_params, nullptr, nullptr);

            for(const auto& bound : aty_def.m_trait_bounds)
            {
                const auto& b_params = bound.m_path.m_params;
                ::HIR::PathParams   params_mono_o;
                const auto& b_params_mono = (monomorphise_pathparams_needed(b_params) ? params_mono_o = monomorphise_path_params_with(sp, b_params, monomorph_cb, false) : b_params);
                
                // TODO: find trait in trait.
                if( bound.m_path.m_path == trait )
                {
                    auto cmp = ::HIR::Compare::Equal;
                    cmp = this->compare_pp(sp, b_params_mono, params);
                    
                    if( &b_params_mono == &params_mono_o )
                    {
                        if( callback( ImplRef(type.clone(), mv$(params_mono_o), {}), cmp ) )
                            return true;
                        params_mono_o = monomorphise_path_params_with(sp, params, monomorph_cb, false);
                    }
                    else
                    {
                        if( callback( ImplRef(&type, &bound.m_path.m_params, &null_assoc), cmp ) )
                            return true;
                    }
                }
                
                bool ret = this->find_named_trait_in_trait(sp,  trait, params,  *bound.m_trait_ptr,  bound.m_path.m_path, b_params_mono, type,
                    [&](const auto& i_ty, const auto& i_params, const auto& i_assoc) {
                        auto cmp = this->compare_pp(sp, i_params, params);
                        DEBUG("impl " << trait << i_params << " for " << i_ty << " -- desired " << trait << params);
                        return callback( ImplRef(i_ty.clone(), i_params.clone(), {}), cmp );
                    });
                if( ret )
                    return true;
            }
        }
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
    m_ivars.check_for_loops();
    
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
        static bool check_path(const TraitResolution& r, const ::HIR::Path& p) {
            TU_MATCH(::HIR::Path::Data, (p.m_data), (e2),
            (Generic,
                return H::check_pathparams(r, e2.m_params);
                ),
            (UfcsInherent,
                TODO(Span(), "Path - UfcsInherent - " << p);
                ),
            (UfcsKnown,
                if( r.has_associated_type(*e2.type) )
                    return true;
                if( H::check_pathparams(r, e2.trait.m_params) )
                    return true;
                if( H::check_pathparams(r, e2.params) )
                    return true;
                return false;
                ),
            (UfcsUnknown,
                BUG(Span(), "Encountered UfcsUnknown - " << p);
                )
            )
            throw "";
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
        // Unbounded UfcsKnown returns true (bound is false)
        if( e.path.m_data.is_UfcsKnown() && e.binding.is_Unbound() )
            return true;
        return H::check_path(*this, e.path);
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
    (ErasedType,
        if( H::check_path(*this, e.m_origin) )
            return true;
        for(const auto& m : e.m_traits) {
            if( H::check_pathparams(*this, m.m_path.m_params) )
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
void TraitResolution::expand_associated_types_inplace(const Span& sp, ::HIR::TypeRef& input, LList<const ::HIR::TypeRef*> stack) const
{
    for(const auto& ty : m_eat_active_stack)
    {
        if( input == ty ) {
            DEBUG("Recursive lookup, skipping - input = " << input);
            return ;
        }
    }
    //TRACE_FUNCTION_F(input);
    TU_MATCH(::HIR::TypeRef::Data, (input.m_data), (e),
    (Infer,
        auto& ty = this->m_ivars.get_type(input);
        if( ty != input ) {
            input = ty.clone();
            expand_associated_types_inplace(sp, input, stack);
        }
        ),
    (Diverge,
        ),
    (Primitive,
        ),
    (Path,
        TU_MATCH(::HIR::Path::Data, (e.path.m_data), (pe),
        (Generic,
            for(auto& arg : pe.m_params.m_types)
                expand_associated_types_inplace(sp, arg, stack);
            ),
        (UfcsInherent,
            TODO(sp, "Path - UfcsInherent - " << e.path);
            ),
        (UfcsKnown,
            // - Only try resolving if the binding isn't known
            if( !e.binding.is_Unbound() )
                return ;
            this->expand_associated_types_inplace__UfcsKnown(sp, input, stack);
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
    (ErasedType,
        // Recurse?
        ),
    (Array,
        expand_associated_types_inplace(sp, *e.inner, stack);
        ),
    (Slice,
        expand_associated_types_inplace(sp, *e.inner, stack);
        ),
    (Tuple,
        for(auto& sub : e) {
            expand_associated_types_inplace(sp, sub , stack);
        }
        ),
    (Borrow,
        expand_associated_types_inplace(sp, *e.inner, stack);
        ),
    (Pointer,
        expand_associated_types_inplace(sp, *e.inner, stack);
        ),
    (Function,
        // Recurse?
        ),
    (Closure,
        // Recurse?
        )
    )
}


void TraitResolution::expand_associated_types_inplace__UfcsKnown(const Span& sp, ::HIR::TypeRef& input, LList<const ::HIR::TypeRef*> prev_stack) const
{
    TRACE_FUNCTION_FR("input=" << input, input);
    auto& e = input.m_data.as_Path();
    auto& pe = e.path.m_data.as_UfcsKnown();
    
    struct D {
        const TraitResolution&  m_tr;
        D(const TraitResolution& tr, ::HIR::TypeRef v): m_tr(tr) {
            tr.m_eat_active_stack.push_back( mv$(v) );
        }
        ~D() {
            m_tr.m_eat_active_stack.pop_back();
        }
    };
    D   _(*this, input.clone());
    // State stack to avoid infinite recursion
    LList<const ::HIR::TypeRef*>    stack(&prev_stack, &m_eat_active_stack.back());
    
    expand_associated_types_inplace(sp, *pe.type, stack);
    
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
    )
    
    TU_IFLET(::HIR::TypeRef::Data, pe.type->m_data, Function, te,
        if( te.m_abi == ABI_RUST && !te.is_unsafe )
        {
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
        }
    )
    
    // If it's a TraitObject, then maybe we're asking for a bound
    TU_IFLET(::HIR::TypeRef::Data, pe.type->m_data, TraitObject, te,
        const auto& data_trait = te.m_trait.m_path;
        if( pe.trait.m_path == data_trait.m_path ) {
            auto cmp = ::HIR::Compare::Equal;
            if( pe.trait.m_params.m_types.size() != data_trait.m_params.m_types.size() )
            {
                cmp = ::HIR::Compare::Unequal;
            }
            else
            {
                for(unsigned int i = 0; i < pe.trait.m_params.m_types.size(); i ++)
                {
                    const auto& l = pe.trait.m_params.m_types[i];
                    const auto& r = data_trait.m_params.m_types[i];
                    cmp &= l.compare_with_placeholders(sp, r, m_ivars.callback_resolve_infer());
                }
            }
            if( cmp != ::HIR::Compare::Unequal )
            {
                auto it = te.m_trait.m_type_bounds.find( pe.item );
                if( it == te.m_trait.m_type_bounds.end() ) {
                    // TODO: Mark as opaque and return.
                    // - Why opaque? It's not bounded, don't even bother
                    TODO(sp, "Handle unconstrained associate type " << pe.item << " from " << *pe.type);
                }
                
                input = it->second.clone();
                return ;
            }
        }
        
        // - Check if the desired trait is a supertrait of this.
        // NOTE: `params` (aka des_params) is not used (TODO)
        bool is_supertrait = this->find_named_trait_in_trait(sp, pe.trait.m_path,pe.trait.m_params, *te.m_trait.m_trait_ptr, data_trait.m_path,data_trait.m_params, *pe.type,
            [&](const auto& i_ty, const auto& i_params, const auto& i_assoc) {
                // The above is just the monomorphised params and associated set. Comparison is still needed.
                auto cmp = this->compare_pp(sp, i_params, pe.trait.m_params);
                if( cmp != ::HIR::Compare::Unequal ) {
                    auto it = i_assoc.find( pe.item );
                    if( it != i_assoc.end() ) {
                        input = it->second.clone();
                        return true;
                    }
                    // NOTE: (currently) there can only be one trait with this name, so if we found this trait and the item is present - good.
                    it = te.m_trait.m_type_bounds.find( pe.item );
                    if( it != te.m_trait.m_type_bounds.end() ) {
                        input = it->second.clone();
                        return true;
                    }
                    return false;
                }
                return false;
            });
        if( is_supertrait )
        {
            return ;
        }
    )
    
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
        else {
            DEBUG("- Found replacement: " << input);
        }
        this->expand_associated_types_inplace(sp, input, stack);
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
                        DEBUG("- Found replacement: " << input);
                        this->expand_associated_types_inplace(sp, input, stack);
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
    //if( !this->trait_contains_type(sp, pe.trait, this->m_crate.get_trait_by_path(sp, pe.trait.m_path), *pe.type, pe.item, trait_path) )
    if( !this->trait_contains_type(sp, pe.trait, this->m_crate.get_trait_by_path(sp, pe.trait.m_path), pe.item, trait_path) )
        BUG(sp, "Cannot find associated type " << pe.item << " anywhere in trait " << pe.trait);
    //pe.trait = mv$(trait_path);
    
    DEBUG("Searching for impl");
    bool    can_fuzz = true;
    unsigned int    count = 0;
    bool is_specialisable = false;
    ImplRef best_impl;
    rv = this->find_trait_impls_crate(sp, trait_path.m_path, trait_path.m_params, *pe.type, [&](auto impl, auto qual) {
        DEBUG("[expand_associated_types__UfcsKnown] Found " << impl << " qual=" << qual);
        // If it's a fuzzy match, keep going (but count if a concrete hasn't been found)
        if( qual == ::HIR::Compare::Fuzzy ) {
            if( can_fuzz )
            {
                count += 1;
                if( count == 1 ) {
                    best_impl = mv$(impl);
                }
            }
            return false;
        }
        else {
            // If a fuzzy match could have been seen, ensure that best_impl is unsed
            if( can_fuzz ) {
                best_impl = ImplRef();
                can_fuzz = false;
            }
            
            // If the type is specialisable
            if( impl.type_is_specialisable(pe.item.c_str()) ) {
                // Check if this is more specific
                if( impl.more_specific_than( best_impl ) ) {
                    is_specialisable = true;
                    best_impl = mv$(impl);
                }
                return false;
            }
            else {
                auto ty = impl.get_type( pe.item.c_str() );
                if( ty == ::HIR::TypeRef() )
                    ERROR(sp, E0000, "Couldn't find assocated type " << pe.item << " in " << pe.trait);
                
                if( impl.has_magic_params() )
                    ;
                
                // TODO: What if there's multiple impls?
                DEBUG("Converted UfcsKnown - " << e.path << " = " << ty);
                input = mv$(ty);
                return true;
            }
        }
        });
    if( !rv && best_impl.is_valid() ) {
        if( can_fuzz && count > 1 ) {
            // Fuzzy match with multiple choices - can't know yet
        }
        else if( is_specialisable ) {
            e.binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
            return ;
        }
        else {
            auto ty = best_impl.get_type( pe.item.c_str() );
            if( ty == ::HIR::TypeRef() )
                ERROR(sp, E0000, "Couldn't find assocated type " << pe.item << " in " << pe.trait);
            
            // Try again later?
            if( best_impl.has_magic_params() ) {
                DEBUG("- Placeholder parameters present in impl, can't expand");
                return ;
            }
            
            DEBUG("Converted UfcsKnown - " << e.path << " = " << ty);
            input = mv$(ty);
            rv = true;
        }
    }
    if( rv ) {
        expand_associated_types_inplace(sp, input, stack);
        return ;
    }
    
    // If there are no ivars in this path, set its binding to Opaque
    if( !this->m_ivars.type_contains_ivars(input) ) {
        // TODO: If the type is a generic or an opaque associated, we can't know.
        // - If the trait contains any of the above, it's unknowable
        // - Otherwise, it's an error
        DEBUG("Assuming that " << input << " is an opaque name");
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
    TRACE_FUNCTION_F(des << des_params << " in " << trait_path << pp);
    if( pp.m_types.size() != trait_ptr.m_params.m_types.size() ) {
        BUG(sp, "Incorrect number of parameters for trait");
    }
    
    const auto monomorph_cb = [&](const auto& gt)->const auto& {
        const auto& ge = gt.m_data.as_Generic();
        if( ge.binding == 0xFFFF ) {
            return target_type;
        }
        else {
            if( ge.binding >= pp.m_types.size() )
                BUG(sp, "find_named_trait_in_trait - Generic #" << ge.binding << " " << ge.name << " out of range");
            return pp.m_types[ge.binding];
        }
        };
    
    for( const auto& pt : trait_ptr.m_parent_traits )
    {
        auto pt_mono = monomorphise_traitpath_with(sp, pt, monomorph_cb, false);

        DEBUG(pt << " => " << pt_mono);
        if( pt.m_path.m_path == des ) {
            // NOTE: Doesn't quite work...
            //auto cmp = this->compare_pp(sp, pt_mono.m_path.m_params, des_params);
            //if( cmp != ::HIR::Compare::Unequal )
            //{
                callback( target_type, pt_mono.m_path.m_params, pt_mono.m_type_bounds );
            //}
            return true;
        }
        
        const auto& tr = m_crate.get_trait_by_path(sp, pt.m_path.m_path);
        if( find_named_trait_in_trait(sp, des, des_params,  tr, pt.m_path.m_path, pt_mono.m_path.m_params,  target_type, callback) ) {
            return true;
        }
    }
    
    // Also check bounds for `Self: T` bounds
    for(const auto& b : trait_ptr.m_params.m_bounds)
    {
        if( !b.is_TraitBound() )    continue;
        const auto& be = b.as_TraitBound();
        
        if( be.type == ::HIR::TypeRef("Self", 0xFFFF) )
        {
            // Something earlier adds a "Self: SelfTrait" bound, prevent that from causing infinite recursion
            if( be.trait.m_path.m_path == trait_path )
                continue ;
            auto pt_mono = monomorphise_traitpath_with(sp, be.trait, monomorph_cb, false);
            DEBUG(be.trait << " (Bound) => " << pt_mono);

            if( pt_mono.m_path.m_path == des ) {
                // NOTE: Doesn't quite work...
                //auto cmp = this->compare_pp(sp, pt_mono.m_path.m_params, des_params);
                //if( cmp != ::HIR::Compare::Unequal )
                //{
                    callback( target_type, pt_mono.m_path.m_params, pt_mono.m_type_bounds );
                //}
                return true;
            }
            
            const auto& tr = m_crate.get_trait_by_path(sp, pt_mono.m_path.m_path);
            if( find_named_trait_in_trait(sp, des, des_params,  tr, pt_mono.m_path.m_path, pt_mono.m_path.m_params,  target_type, callback) ) {
                return true;
            }
        }
    }
    
    return false;
}
bool TraitResolution::find_trait_impls_bound(const Span& sp, const ::HIR::SimplePath& trait, const ::HIR::PathParams& params, const ::HIR::TypeRef& type,  t_cb_trait_impl_r callback) const
{
    TRACE_FUNCTION_F("trait = " << trait << params << " , type = " << type);
    const ::HIR::Path::Data::Data_UfcsKnown* assoc_info = nullptr;
    TU_IFLET(::HIR::TypeRef::Data, type.m_data, Path, e,
        TU_IFLET(::HIR::Path::Data, e.path.m_data, UfcsKnown, pe,
            assoc_info = &pe;
        )
    )

    if( m_ivars.get_type(type).m_data.is_Infer() )
        return false;
    
    // TODO: A bound can imply something via its associated types. How deep can this go?
    // E.g. `T: IntoIterator<Item=&u8>` implies `<T as IntoIterator>::IntoIter : Iterator<Item=&u8>`
    return this->iterate_bounds([&](const auto& b) {
        TU_IFLET(::HIR::GenericBound, b, TraitBound, e,
            const auto& b_params = e.trait.m_path.m_params;
            
            auto cmp = e.type .compare_with_placeholders(sp, type, m_ivars.callback_resolve_infer());
            if( cmp == ::HIR::Compare::Unequal )
                return false;
            
            if( e.trait.m_path.m_path == trait ) {
                // Check against `params`
                DEBUG("Checking " << params << " vs " << b_params);
                auto ord = cmp;
                ord &= this->compare_pp(sp, b_params, params);
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
            
            // TODO: Allow fuzzy equality?
            if( cmp == ::HIR::Compare::Equal )
            {
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
            if( assoc_info && e.trait.m_path.m_path == assoc_info->trait.m_path && e.type == *assoc_info->type )
            {
                // Check the trait params
                auto ord = this->compare_pp(sp, b_params, assoc_info->trait.m_params);
                if( ord == ::HIR::Compare::Fuzzy ) {
                    TODO(sp, "Handle fuzzy matches searching for associated type bounds");
                }
                if( ord == ::HIR::Compare::Unequal ) {
                    return false;
                }
                
                const auto& trait_ref = *e.trait.m_trait_ptr;
                const auto& at = trait_ref.m_types.at(assoc_info->item);
                for(const auto& bound : at.m_trait_bounds) {
                    if( bound.m_path.m_path == trait )
                    {
                        auto monomorph_cb = [&](const auto& gt)->const auto& {
                            const auto& ge = gt.m_data.as_Generic();
                            if( ge.binding == 0xFFFF ) {
                                return *assoc_info->type;
                            }
                            else {
                                if( ge.binding >= assoc_info->trait.m_params.m_types.size() )
                                    BUG(sp, "find_trait_impls_bound - Generic #" << ge.binding << " " << ge.name << " out of range");
                                return assoc_info->trait.m_params.m_types[ge.binding];
                            }
                            };
                        
                        DEBUG("- Found an associated type bound for this trait via another bound");
                        ::HIR::Compare  ord;
                        if( monomorphise_pathparams_needed(bound.m_path.m_params) ) {
                            // TODO: Use a compare+callback method instead
                            auto b_params_mono = monomorphise_path_params_with(sp, bound.m_path.m_params, monomorph_cb, false);
                            ord = this->compare_pp(sp, b_params_mono, params);
                        }
                        else {
                            ord = this->compare_pp(sp, bound.m_path.m_params, params);
                        }
                        if( ord == ::HIR::Compare::Unequal )
                            return false;
                        if( ord == ::HIR::Compare::Fuzzy ) {
                            DEBUG("Fuzzy match");
                        }
                        
                        auto tp_mono = monomorphise_traitpath_with(sp, bound, monomorph_cb, false);
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
        const ::HIR::SimplePath& trait, const ::HIR::PathParams* params_ptr,
        const ::HIR::TypeRef& type,
        t_cb_trait_impl_r callback
        ) const
{
    static ::std::map< ::std::string, ::HIR::TypeRef>    null_assoc;
    TRACE_FUNCTION_F(trait << FMT_CB(ss, if(params_ptr) { ss << *params_ptr; } else { ss << "<?>"; }) << " for " << type);
    
    // Handle auto traits (aka OIBITs)
    if( m_crate.get_trait_by_path(sp, trait).m_is_marker )
    {
        // Detect recursion and return true if detected
        static ::std::vector< ::std::tuple< const ::HIR::SimplePath*, const ::HIR::PathParams*, const ::HIR::TypeRef*> >    stack;
        for(const auto& ent : stack ) {
            if( *::std::get<0>(ent) != trait )
                continue ;
            if( ::std::get<1>(ent) && params_ptr && *::std::get<1>(ent) != *params_ptr )
                continue ;
            if( *::std::get<2>(ent) != type )
                continue ;
            
            return callback( ImplRef(&type, params_ptr, &null_assoc), ::HIR::Compare::Equal );
        }
        stack.push_back( ::std::make_tuple( &trait, params_ptr, &type ) );
        struct Guard {
            ~Guard() { stack.pop_back(); }
        };
        Guard   _;
        
        // NOTE: Expected behavior is for Ivars to return false
        // TODO: Should they return Compare::Fuzzy instead?
        if( type.m_data.is_Infer() ) {
            return false;
        }
        
        const ::HIR::TraitMarkings* markings = nullptr;
        TU_IFLET( ::HIR::TypeRef::Data, (type.m_data), Path, e,
            if( e.path.m_data.is_Generic() && e.path.m_data.as_Generic().m_params.m_types.size() == 0 )
            {
                TU_MATCH( ::HIR::TypeRef::TypePathBinding, (e.binding), (tpb),
                (Unbound,
                    ),
                (Opaque,
                    ),
                (Struct,
                    markings = &tpb->m_markings;
                    ),
                (Enum,
                    markings = &tpb->m_markings;
                    )
                )
            }
        )

        // NOTE: `markings` is only set if there's no type params to a path type
        // - Cache populated after destructure
        if( markings )
        {
            auto it = markings->auto_impls.find( trait );
            if( it != markings->auto_impls.end() )
            {
                if( ! it->second.conditions.empty() ) {
                    TODO(sp, "Conditional auto trait impl");
                }
                else if( it->second.is_impled ) {
                    return callback( ImplRef(&type, params_ptr, &null_assoc), ::HIR::Compare::Equal );
                }
                else {
                    return false;
                }
            }
        }
        
        // - Search for positive impls for this type
        DEBUG("- Search positive impls");
        bool positive_found = false;
        this->m_crate.find_auto_trait_impls(trait, type, this->m_ivars.callback_resolve_infer(),
            [&](const auto& impl) {
                // Skip any negative impls on this pass
                if( impl.is_positive != true )
                    return false;
                
                DEBUG("[find_trait_impls_crate] - Auto Pos Found impl" << impl.m_params.fmt_args() << " " << trait << impl.m_trait_args << " for " << impl.m_type << " " << impl.m_params.fmt_bounds());
                
                // Compare with `params`
                ::std::vector< const ::HIR::TypeRef*> impl_params;
                ::std::vector< ::HIR::TypeRef>  placeholders;
                auto match = this->ftic_check_params(sp, trait,  params_ptr, type,  impl.m_params, impl.m_trait_args, impl.m_type,  impl_params, placeholders);
                if( match == ::HIR::Compare::Unequal ) {
                    // If any bound failed, return false (continue searching)
                    return false;
                }
                
                auto monomorph = [&](const auto& gt)->const auto& {
                        const auto& ge = gt.m_data.as_Generic();
                        ASSERT_BUG(sp, ge.binding >> 8 != 2, "");
                        assert( ge.binding < impl_params.size() );
                        if( !impl_params[ge.binding] ) {
                            return placeholders[ge.binding];
                        }
                        return *impl_params[ge.binding];
                        };
                // TODO: Ensure that there are no-longer any magic params?
                
                auto ty_mono = monomorphise_type_with(sp, impl.m_type, monomorph, false);
                auto args_mono = monomorphise_path_params_with(sp, impl.m_trait_args, monomorph, false);
                // NOTE: Auto traits can't have items, so no associated types
                
                positive_found = true;
                DEBUG("[find_trait_impls_crate] Auto Positive callback(args=" << args_mono << ")");
                return callback(ImplRef(mv$(ty_mono), mv$(args_mono), {}), match);
            });
        if( positive_found ) {
            // A positive impl was found, so return true (callback should have been called)
            return true;
        }
        
        // - Search for negative impls for this type
        DEBUG("- Search negative impls");
        bool negative_found = this->m_crate.find_auto_trait_impls(trait, type, this->m_ivars.callback_resolve_infer(),
            [&](const auto& impl) {
                // Skip any positive impls
                if( impl.is_positive != false )
                    return false;
                DEBUG("[find_trait_impls_crate] - Found auto neg impl" << impl.m_params.fmt_args() << " " << trait << impl.m_trait_args << " for " << impl.m_type << " " << impl.m_params.fmt_bounds());
                
                // Compare with `params`
                ::std::vector< const ::HIR::TypeRef*> impl_params;
                ::std::vector< ::HIR::TypeRef>  placeholders;
                auto match = this->ftic_check_params(sp, trait,  params_ptr, type,  impl.m_params, impl.m_trait_args, impl.m_type,  impl_params, placeholders);
                if( match == ::HIR::Compare::Unequal ) {
                    // If any bound failed, return false (continue searching)
                    return false;
                }
                
                DEBUG("[find_trait_impls_crate] - Found neg impl");
                return true;
            });
        if( negative_found ) {
            // A negative impl _was_ found, so return false
            return false;
        }
        
        auto cmp = this->check_auto_trait_impl_destructure(sp, trait, params_ptr, type);
        if( cmp != ::HIR::Compare::Unequal )
        {
            if( markings ) {
                ASSERT_BUG(sp, cmp == ::HIR::Compare::Equal, "Auto trait with no params returned a fuzzy match from destructure");
                markings->auto_impls.insert( ::std::make_pair(trait, ::HIR::TraitMarkings::AutoMarking { {}, true }) );
            }
            return callback( ImplRef(&type, params_ptr, &null_assoc), cmp );
        }
        else
        {
            if( markings ) {
                markings->auto_impls.insert( ::std::make_pair(trait, ::HIR::TraitMarkings::AutoMarking { {}, false }) );
            }
            return false;
        }
    }
    
    return this->m_crate.find_trait_impls(trait, type, this->m_ivars.callback_resolve_infer(),
        [&](const auto& impl) {
            DEBUG("[find_trait_impls_crate] Found impl" << impl.m_params.fmt_args() << " " << trait << impl.m_trait_args << " for " << impl.m_type << " " << impl.m_params.fmt_bounds());
            // Compare with `params`
            ::std::vector< const ::HIR::TypeRef*> impl_params;
            ::std::vector< ::HIR::TypeRef>  placeholders;
            auto match = this->ftic_check_params(sp, trait,  params_ptr, type,  impl.m_params, impl.m_trait_args, impl.m_type,  impl_params, placeholders);
            if( match == ::HIR::Compare::Unequal ) {
                // If any bound failed, return false (continue searching)
                DEBUG("[find_trait_impls_crate] - Params mismatch");
                return false;
            }

            #if 0
            auto monomorph = [&](const auto& gt)->const auto& {
                    const auto& ge = gt.m_data.as_Generic();
                    ASSERT_BUG(sp, ge.binding >> 8 != 2, "");
                    assert( ge.binding < impl_params.size() );
                    if( !impl_params[ge.binding] ) {
                        return placeholders[ge.binding];
                    }
                    return *impl_params[ge.binding];
                    };
            
            auto ty_mono = monomorphise_type_with(sp, impl.m_type, monomorph, false);
            auto args_mono = monomorphise_path_params_with(sp, impl.m_trait_args, monomorph, false);
            // TODO: Expand associated types in these then ensure that they still match the desired types.
            
            DEBUG("- Making associated type output map - " << impl.m_types.size() << " entries");
            ::std::map< ::std::string, ::HIR::TypeRef>  types;
            for( const auto& aty : impl.m_types )
            {
                DEBUG(" > " << aty.first << " = monomorph(" << aty.second.data << ")");
                types.insert( ::std::make_pair(aty.first,  this->expand_associated_types(sp, monomorphise_type_with(sp, aty.second.data, monomorph))) );
            }
            // TODO: Ensure that there are no-longer any magic params?
            
            DEBUG("[find_trait_impls_crate] callback(args=" << args_mono << ", assoc={" << types << "})");
            #endif
            return callback(ImplRef(mv$(impl_params), trait, impl, mv$(placeholders)), match);
        }
        );
}

::HIR::Compare TraitResolution::check_auto_trait_impl_destructure(const Span& sp, const ::HIR::SimplePath& trait, const ::HIR::PathParams* params_ptr, const ::HIR::TypeRef& type) const
{
    TRACE_FUNCTION_F("trait = " << trait << ", type = " << type);
    // HELPER: Search for an impl of this trait for an inner type, and return the match type
    auto type_impls_trait = [&](const auto& inner_ty) -> ::HIR::Compare {
        auto l_res = ::HIR::Compare::Unequal;
        this->find_trait_impls(sp, trait, *params_ptr, inner_ty, [&](auto, auto cmp){ l_res = cmp; return (cmp == ::HIR::Compare::Equal); });
        DEBUG("[check_auto_trait_impl_destructure] " << inner_ty << " - " << l_res);
        return l_res;
        };
    
    // - If the type is a path (struct/enum/...), search for impls for all contained types.
    TU_IFLET( ::HIR::TypeRef::Data, type.m_data, Path, e,
        ::HIR::Compare  res = ::HIR::Compare::Equal;
        TU_MATCH( ::HIR::Path::Data, (e.path.m_data), (pe),
        (Generic,
            ::HIR::TypeRef  tmp;
            auto monomorph_cb = [&](const auto& gt)->const auto& {
                const auto& ge = gt.m_data.as_Generic();
                if( ge.binding == 0xFFFF ) {
                    BUG(sp, "Self type in struct/enum generics");
                }
                else if( ge.binding >> 8 == 0 ) {
                    auto idx = ge.binding & 0xFF;
                    ASSERT_BUG(sp, idx < pe.m_params.m_types.size(), "Type parameter out of range - " << gt);
                    return pe.m_params.m_types[idx];
                }
                else {
                    BUG(sp, "Unexpected type parameter - " << gt << " in content of " << type);
                }
                };
            // HELPER: Get a possibily monomorphised version of the input type (stored in `tmp` if needed)
            auto monomorph_get = [&](const auto& ty)->const auto& {
                if( monomorphise_type_needed(ty) ) {
                    return (tmp = monomorphise_type_with(sp, ty,  monomorph_cb));
                }
                else {
                    return ty;
                }
                };
            
            TU_MATCH( ::HIR::TypeRef::TypePathBinding, (e.binding), (tpb),
            (Opaque,
                BUG(sp, "Opaque binding on generic path - " << type);
                ),
            (Unbound,
                BUG(sp, "Unbound binding on generic path - " << type);
                ),
            (Struct,
                const auto& str = *tpb;
                
                // TODO: Somehow store a ruleset for auto traits on the type
                // - Map of trait->does_impl for local fields?
                // - Problems occur with type parameters
                TU_MATCH( ::HIR::Struct::Data, (str.m_data), (se),
                (Unit,
                    ),
                (Tuple,
                    for(const auto& fld : se)
                    {
                        const auto& fld_ty_mono = monomorph_get(fld.ent);
                        DEBUG("Struct::Tuple " << fld_ty_mono);
                        res &= type_impls_trait(fld_ty_mono);
                        if( res == ::HIR::Compare::Unequal )
                            return ::HIR::Compare::Unequal;
                    }
                    ),
                (Named,
                    for(const auto& fld : se)
                    {
                        const auto& fld_ty_mono = monomorph_get(fld.second.ent);
                        DEBUG("Struct::Named '" << fld.first << "' " << fld_ty_mono);
                        
                        res &= type_impls_trait(fld_ty_mono);
                        if( res == ::HIR::Compare::Unequal )
                            return ::HIR::Compare::Unequal;
                    }
                    )
                )
                ),
            (Enum,
                const auto& enm = *tpb;
                
                for(const auto& var : enm.m_variants)
                {
                    TU_MATCH(::HIR::Enum::Variant, (var.second), (ve),
                    (Unit,
                        ),
                    (Value,
                        ),
                    (Tuple,
                        for(const auto& fld : ve)
                        {
                            const auto& fld_ty_mono = monomorph_get(fld.ent);
                            DEBUG("Enum '" << var.first << "'::Tuple " << fld_ty_mono);
                            res &= type_impls_trait(fld_ty_mono);
                            if( res == ::HIR::Compare::Unequal )
                                return ::HIR::Compare::Unequal;
                        }
                        ),
                    (Struct,
                        for(const auto& fld : ve)
                        {
                            const auto& fld_ty_mono = monomorph_get(fld.second.ent);
                            DEBUG("Enum '" << var.first << "'::Struct '" << fld.first << "' " << fld_ty_mono);
                            
                            res &= type_impls_trait(fld_ty_mono);
                            if( res == ::HIR::Compare::Unequal )
                                return ::HIR::Compare::Unequal;
                        }
                        )
                    )
                }
                )
            )
            DEBUG("- Nothing failed, calling callback");
            ),
        (UfcsUnknown,
            BUG(sp, "UfcsUnknown in typeck - " << type);
            ),
        (UfcsKnown,
            TODO(sp, "Check trait bounds for bound on " << type);
            ),
        (UfcsInherent,
            TODO(sp, "Auto trait lookup on UFCS Inherent type");
            )
        )
        return res;
    )
    else TU_IFLET( ::HIR::TypeRef::Data, type.m_data, Tuple, e,
        ::HIR::Compare  res = ::HIR::Compare::Equal;
        for(const auto& sty : e)
        {
            res &= type_impls_trait(sty);
            if( res == ::HIR::Compare::Unequal )
                return ::HIR::Compare::Unequal;
        }
        return res;
    )
    else TU_IFLET( ::HIR::TypeRef::Data, type.m_data, Array, e,
        return type_impls_trait(*e.inner);
    )
    // Otherwise, there's no negative so it must be positive
    else {
        return ::HIR::Compare::Equal;
    }
}

::HIR::Compare TraitResolution::ftic_check_params(const Span& sp, const ::HIR::SimplePath& trait,
    const ::HIR::PathParams* params_ptr, const ::HIR::TypeRef& type,
    const ::HIR::GenericParams& impl_params_def, const ::HIR::PathParams& impl_trait_args, const ::HIR::TypeRef& impl_ty,
    /*Out->*/ ::std::vector< const ::HIR::TypeRef*>& impl_params, ::std::vector< ::HIR::TypeRef>& placeholders
    ) const
{
    impl_params.resize( impl_params_def.m_types.size() );
    auto cb = [&](auto idx, const auto& ty) {
        DEBUG("[ftic_check_params] Param " << idx << " = " << ty);
        assert( idx < impl_params.size() );
        if( ! impl_params[idx] ) {
            impl_params[idx] = &ty;
            return ::HIR::Compare::Equal;
        }
        else {
            return impl_params[idx]->compare_with_placeholders(sp, ty, this->m_ivars.callback_resolve_infer());
        }
        };

    //auto cb_res = [&](const auto& ty)->const auto& {
    //    if( ty.m_data.is_Infer() ) {
    //        return this->m_ivars.get_type(ty);
    //    }
    //    //else if( ty.m_data.is_Generic() && ty.m_data.as_Generic().binding >> 8 == 2 ) {
    //    //    return ::HIR::TypeRef();
    //    //}
    //    else {
    //        return ty;
    //    }
    //    };
    // NOTE: If this type references an associated type, the match will incorrectly fail.
    // - HACK: match_test_generics_fuzz has been changed to return Fuzzy if there's a tag mismatch and the LHS is an Opaque path
    auto    match = ::HIR::Compare::Equal;
    match &= impl_ty.match_test_generics_fuzz(sp, type , this->m_ivars.callback_resolve_infer(), cb);
    if( params_ptr )
    {
        const auto& params = *params_ptr;
        ASSERT_BUG(sp, impl_trait_args.m_types.size() == params.m_types.size(), "Param count mismatch between `" << impl_trait_args << "` and `" << params << "` for " << trait );
        for(unsigned int i = 0; i < impl_trait_args.m_types.size(); i ++)
            match &= impl_trait_args.m_types[i].match_test_generics_fuzz(sp, params.m_types[i], this->m_ivars.callback_resolve_infer(), cb);
        if( match == ::HIR::Compare::Unequal ) {
            DEBUG("[find_trait_impls_crate] - Failed to match parameters - " << impl_trait_args << "+" << impl_ty << " != " << params << "+" << type);
            return ::HIR::Compare::Unequal;
        }
    }
    else
    {
        if( match == ::HIR::Compare::Unequal ) {
            DEBUG("[find_trait_impls_crate] - Failed to match type - " << impl_ty << " != " << type);
            return ::HIR::Compare::Unequal;
        }
    }
    
    // TODO: Some impl blocks have type params used as part of type bounds.
    // - A rough idea is to have monomorph return a third class of generic for params that are not yet bound.
    //  - compare_with_placeholders gets called on both ivars and generics, so that can be used to replace it once known.
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
    
    // Check bounds for this impl
    // - If a bound fails, then this can't be a valid impl
    for(const auto& bound : impl_params_def.m_bounds)
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
            DEBUG("- bound mono " << real_type << " : " << real_trait);
            bool found_fuzzy_match = false;
            if( real_type.m_data.is_Path() && real_type.m_data.as_Path().binding.is_Unbound() ) {
                DEBUG("- Bounded type is unbound UFCS, assuming fuzzy match");
                found_fuzzy_match = true;
            }
            auto rv = this->find_trait_impls(sp, real_trait_path.m_path, real_trait_path.m_params, real_type, [&](auto impl, auto impl_cmp) {
                auto cmp = impl_cmp;
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
                        // Expand after extraction, just to make sure.
                        this->expand_associated_types_inplace(sp, tmp, {});
                        ty_p = &this->m_ivars.get_type(tmp);
                    }
                    const auto& ty = *ty_p;
                    DEBUG("[find_trait_impls_crate] - Compare " << ty << " and " << assoc_bound.second << ", matching generics");
                    // `ty` = Monomorphised actual type (< `be.type` as `be.trait` >::`assoc_bound.first`)
                    // `assoc_bound.second` = Desired type (monomorphised too)
                    auto cmp = assoc_bound.second .match_test_generics_fuzz(sp, ty, cb_infer, cb_match);
                    switch(cmp)
                    {
                    case ::HIR::Compare::Equal:
                        DEBUG("Equal");
                        continue;
                    case ::HIR::Compare::Unequal:
                        DEBUG("Assoc `" << assoc_bound.first << "` didn't match - " << ty << " != " << assoc_bound.second);
                        return false;
                    case ::HIR::Compare::Fuzzy:
                        // TODO: When a fuzzy match is encountered on a conditional bound, returning `false` can lead to an false negative (and a compile error)
                        // BUT, returning `true` could lead to it being selected. (Is this a problem, should a later validation pass check?)
                        DEBUG("[find_trait_impls_crate] Fuzzy match assoc bound between " << ty << " and " << assoc_bound.second);
                        cmp = ::HIR::Compare::Fuzzy;
                        continue ;
                    }
                }
                DEBUG("[ftic_check_params] impl_cmp = " << impl_cmp << ", cmp = " << cmp);
                if( cmp == ::HIR::Compare::Fuzzy ) {
                    found_fuzzy_match = true;
                }
                // If the match isn't a concrete equal, return false (to keep searching)
                return (cmp == ::HIR::Compare::Equal);
                });
            if( rv ) {
                DEBUG("- Bound " << real_type << " : " << real_trait_path << " matched");
            }
            else if( found_fuzzy_match ) {
                DEBUG("- Bound " << real_type << " : " << real_trait_path << " fuzzed");
                match = ::HIR::Compare::Fuzzy;
            }
            else if( real_type.m_data.is_Infer() && real_type.m_data.as_Infer().ty_class == ::HIR::InferClass::None ) {
                DEBUG("- Bound " << real_type << " : " << real_trait_path << " full infer type - make result fuzzy");
                match = ::HIR::Compare::Fuzzy;
            }
            else {
                DEBUG("- Bound " << real_type << " : " << real_trait_path << " failed");
                return ::HIR::Compare::Unequal;
            }
            ),
        (TypeEquality,
            TODO(sp, "Check bound " << be.type << " = " << be.other_type);
            )
        )
    }
    
    return match;
}

bool TraitResolution::trait_contains_method(const Span& sp, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait_ptr, const ::HIR::TypeRef& self, const ::std::string& name, AllowedReceivers ar,  ::HIR::GenericPath& out_path) const
{
    auto it = trait_ptr.m_values.find(name);
    if( it != trait_ptr.m_values.end() ) {
        if( it->second.is_Function() ) {
            const auto& v = it->second.as_Function();
            switch(v.m_receiver)
            {
            case ::HIR::Function::Receiver::Free:
                break;
            case ::HIR::Function::Receiver::Value:
                if( ar != AllowedReceivers::All && ar != AllowedReceivers::Value )
                    break;
                if(0)
            case ::HIR::Function::Receiver::Box:
                if( ar != AllowedReceivers::Box )
                    break;
            default:
                out_path = trait_path.clone();
                return true;
            }
        }
    }
    
    // TODO: Prevent infinite recursion
    for(const auto& st : trait_ptr.m_parent_traits)
    {
        auto& st_ptr = this->m_crate.get_trait_by_path(sp, st.m_path.m_path);
        if( trait_contains_method(sp, st.m_path, st_ptr, self, name, ar,  out_path) ) {
            out_path.m_params = monomorphise_path_params_with(sp, mv$(out_path.m_params), [&](const auto& gt)->const auto& {
                const auto& ge = gt.m_data.as_Generic();
                if( ge.binding == 0xFFFF ) {
                    return self;
                }
                else if( (ge.binding >> 8) == 0 ) {
                    auto idx = ge.binding & 0xFF;
                    assert(idx < trait_path.m_params.m_types.size());
                }
                else {
                    BUG(sp, "Unexpected type parameter " << gt);
                }
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

::HIR::Compare TraitResolution::type_is_copy(const Span& sp, const ::HIR::TypeRef& ty) const
{
    const auto& type = this->m_ivars.get_type(ty);
    TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (e),
    (
        const auto& lang_Copy = this->m_crate.get_lang_item_path(sp, "copy");
        // NOTE: Don't use find_trait_impls, because that calls this
        bool is_fuzzy = false;
        bool has_eq = find_trait_impls_crate(sp, lang_Copy, ::HIR::PathParams{}, ty,  [&](auto , auto c){
            switch(c)
            {
            case ::HIR::Compare::Equal: return true;
            case ::HIR::Compare::Fuzzy:
                is_fuzzy = true;
                return false;
            case ::HIR::Compare::Unequal:
                return false;
            }
            throw "";
            });
        if( has_eq ) {
            return ::HIR::Compare::Equal;
        }
        else if( is_fuzzy ) {
            return ::HIR::Compare::Fuzzy;
        }
        else {
            return ::HIR::Compare::Unequal;
        }
        ),
    (Infer,
        switch(e.ty_class)
        {
        case ::HIR::InferClass::Integer:
        case ::HIR::InferClass::Float:
            return ::HIR::Compare::Equal;
        default:
            DEBUG("Fuzzy Copy impl for ivar?");
            return ::HIR::Compare::Fuzzy;
        }
        ),
    (Generic,
        const auto& lang_Copy = this->m_crate.get_lang_item_path(sp, "copy");
        return this->iterate_bounds([&](const auto& b) {
            TU_IFLET(::HIR::GenericBound, b, TraitBound, be,
                if(be.type == ty && be.trait.m_path == lang_Copy ) {
                    return true;
                }
            )
            return false;
            }) ? ::HIR::Compare::Equal : ::HIR::Compare::Unequal ;
        ),
    (Primitive,
        if( e == ::HIR::CoreType::Str )
            return ::HIR::Compare::Unequal;
        return ::HIR::Compare::Equal;
        ),
    (Borrow,
        return e.type == ::HIR::BorrowType::Shared ? ::HIR::Compare::Equal : ::HIR::Compare::Unequal ;
        ),
    (Pointer,
        return ::HIR::Compare::Equal;
        ),
    (Tuple,
        auto rv = ::HIR::Compare::Equal;
        for(const auto& sty : e)
            rv &= type_is_copy(sp, sty);
        return rv;
        ),
    (Slice,
        return ::HIR::Compare::Unequal;
        ),
    (Function,
        return ::HIR::Compare::Equal;
        ),
    (Array,
        return type_is_copy(sp, *e.inner);
        )
    )
}


// -------------------------------------------------------------------------------------------------------------------
//
// -------------------------------------------------------------------------------------------------------------------
const ::HIR::TypeRef* TraitResolution::autoderef(const Span& sp, const ::HIR::TypeRef& ty_in,  ::HIR::TypeRef& tmp_type) const
{
    const auto& ty = this->m_ivars.get_type(ty_in);
    TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Borrow, e,
        DEBUG("Deref " << ty << " into " << *e.inner);
        return &*e.inner;
    )
    // TODO: Just doing `*[1,2,3]` doesn't work, but this is needed to allow `[1,2,3].iter()` to work
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
unsigned int TraitResolution::autoderef_find_method(const Span& sp, const HIR::t_trait_list& traits, const ::std::vector<unsigned>& ivars, const ::HIR::TypeRef& top_ty, const ::std::string& method_name,  /* Out -> */::HIR::Path& fcn_path) const
{
    unsigned int deref_count = 0;
    ::HIR::TypeRef  tmp_type;   // Temporary type used for handling Deref
    const auto& top_ty_r = this->m_ivars.get_type(top_ty);
    const auto* current_ty = &top_ty_r;
    
    bool unconditional_allow_move = true;
    
    // If the top is a borrow, search dereferenced first.
    TU_IFLET(::HIR::TypeRef::Data, top_ty_r.m_data, Borrow, e,
        if( e.type == ::HIR::BorrowType::Owned ) {
            // Can move, because we have &move
        }
        // TODO: What if this returns Fuzzy?
        else if( this->type_is_copy(sp, *e.inner) == ::HIR::Compare::Equal ) {
            // Can move, because it's Copy
        }
        else {
            unconditional_allow_move = false;
        }
        current_ty = &*e.inner;
        deref_count += 1;
    )
    
    // Handle `self: Box<Self>` methods by detecting m_lang_Box and searchig for box receiver methods
    TU_IFLET(::HIR::TypeRef::Data, top_ty_r.m_data, Path, e,
        TU_IFLET(::HIR::Path::Data, e.path.m_data, Generic, pe,
            if( pe.m_path == m_lang_Box )
            {
                const auto& ty = this->m_ivars.get_type( pe.m_params.m_types.at(0) );
                if( ! ty.m_data.is_Infer() )
                {
                    // Search for methods on the inner type with Receiver::Box
                    if( this->find_method(sp, traits, ivars, ty, method_name, AllowedReceivers::Box, fcn_path) ) {
                        DEBUG("FOUND Box, fcn_path = " << fcn_path);
                        return 1;
                    }
                }
            }
        )
    )
    
    // TODO: This appears to dereference a &mut to call a `self: Self` method, where it should use the trait impl on &mut Self.
    // - Shouldn't deref to get a by-value receiver.// unless it's via a &move.
    
    do {
        // TODO: Update `unconditional_allow_move` based on the current type.
        const auto& ty = this->m_ivars.get_type(*current_ty);
        if( ty.m_data.is_Infer() ) {
            return ~0u;
        }
        
        auto allowed_receivers = (unconditional_allow_move || (deref_count == 0) ? AllowedReceivers::All : AllowedReceivers::AnyBorrow);
        if( this->find_method(sp, traits, ivars, ty, method_name,  allowed_receivers, fcn_path) ) {
            DEBUG("FOUND " << deref_count << ", fcn_path = " << fcn_path);
            return deref_count;
        }
        
        // 3. Dereference and try again
        deref_count += 1;
        current_ty = this->autoderef(sp, ty,  tmp_type);
    } while( current_ty );
    
    // If the top is a borrow, search for methods on &/&mut 
    TU_IFLET(::HIR::TypeRef::Data, top_ty_r.m_data, Borrow, e,
        const auto& ty = top_ty_r;
        
        if( find_method(sp, traits, ivars, ty, method_name, AllowedReceivers::All, fcn_path) ) {
            DEBUG("FOUND " << 0 << ", fcn_path = " << fcn_path);
            return 0;
        }
    )
    
    // If there are ivars within the type, don't error (yet)
    if( this->m_ivars.type_contains_ivars(top_ty) )
    {
        return ~0u;
    }
    
    // TODO: Try the following after dereferencing a Box? - Requires indiciating via the return that the caller should deref+ref
    // - Should refactor to change searching to search for functions taking the current type as a receiver (not method searching as is currently done)
    
    // Insert a single reference and try again (only allowing by-value methods), returning a magic value (e.g. ~1u)
    // - Required for calling `(self[..]: str).into_searcher(haystack)` - Which invokes `<&str as Pattern>::into_searcher(&self[..], haystack)`
    // - Have to do several tries, each with different borrow classes.
    auto borrow_ty = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, top_ty.clone());
    if( find_method(sp, traits, ivars, borrow_ty, method_name,  AllowedReceivers::Value, fcn_path) ) {
        DEBUG("FOUND &, fcn_path = " << fcn_path);
        return ~1u;
    }
    borrow_ty.m_data.as_Borrow().type = ::HIR::BorrowType::Unique;
    if( find_method(sp, traits, ivars, borrow_ty, method_name,  AllowedReceivers::Value, fcn_path) ) {
        DEBUG("FOUND &mut, fcn_path = " << fcn_path);
        return ~2u;
    }
    borrow_ty.m_data.as_Borrow().type = ::HIR::BorrowType::Owned;
    if( find_method(sp, traits, ivars, borrow_ty, method_name,  AllowedReceivers::Value, fcn_path) ) {
        DEBUG("FOUND &mut, fcn_path = " << fcn_path);
        return ~3u;
    }
    
    // Handle `self: Box<Self>` methods by detecting m_lang_Box and searchig for box receiver methods
    TU_IFLET(::HIR::TypeRef::Data, top_ty_r.m_data, Path, e,
        TU_IFLET(::HIR::Path::Data, e.path.m_data, Generic, pe,
            if( pe.m_path == m_lang_Box )
            {
                const auto& ty = this->m_ivars.get_type( pe.m_params.m_types.at(0) );
                assert( ! ty.m_data.is_Infer() );
                
                auto borrow_ty = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, ty.clone());
                if( find_method(sp, traits, ivars, borrow_ty, method_name,  AllowedReceivers::Value, fcn_path) ) {
                    DEBUG("FOUND &*box, fcn_path = " << fcn_path);
                    return ~4u;
                }
                borrow_ty.m_data.as_Borrow().type = ::HIR::BorrowType::Unique;
                if( find_method(sp, traits, ivars, borrow_ty, method_name,  AllowedReceivers::Value, fcn_path) ) {
                    DEBUG("FOUND &mut*box, fcn_path = " << fcn_path);
                    return ~5u;
                }
                borrow_ty.m_data.as_Borrow().type = ::HIR::BorrowType::Owned;
                if( find_method(sp, traits, ivars, borrow_ty, method_name,  AllowedReceivers::Value, fcn_path) ) {
                    DEBUG("FOUND &mut*box, fcn_path = " << fcn_path);
                    return ~6u;
                }
            }
        )
    )
    
    // Dereference failed! This is a hard error (hitting _ is checked above and returns ~0)
    this->m_ivars.dump();
    ERROR(sp, E0000, "Could not find method `" << method_name << "` on type `" << top_ty << "`");
}

bool TraitResolution::find_method(
    const Span& sp,
    const HIR::t_trait_list& traits, const ::std::vector<unsigned>& ivars,
    const ::HIR::TypeRef& ty, const ::std::string& method_name, AllowedReceivers ar,
    /* Out -> */::HIR::Path& fcn_path) const
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
                // TODO: Do a fuzzy match here?
                if( e.type != ty )
                    continue ;
                
                // - Bound's type matches, check if the bounded trait has the method we're searching for
                DEBUG("Bound `" << e.type << " : " << e.trait.m_path << "` - Matches " << ty);
                ::HIR::GenericPath final_trait_path;
                assert(e.trait.m_trait_ptr);
                if( !this->trait_contains_method(sp, e.trait.m_path, *e.trait.m_trait_ptr, ty, method_name, ar,  final_trait_path) )
                    continue ;
                DEBUG("- Found trait " << final_trait_path);
                // TODO: Re-monomorphise final trait using `ty`?
                // - Could collide with legitimate uses of `Self`
                
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
                switch(v.m_receiver)
                {
                case ::HIR::Function::Receiver::Free:
                    break;
                case ::HIR::Function::Receiver::Value:
                    // Only accept by-value methods if not dereferencing to them
                    if( ar == AllowedReceivers::AnyBorrow ) 
                        break;
                case ::HIR::Function::Receiver::Box:
                    if( ar != AllowedReceivers::Box ) 
                        break;
                default:
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
        DEBUG("UfcsKnown - Search associated type bounds in trait - " << e.trait);
        // UFCS known - Assuming that it's reached the maximum resolvable level (i.e. a type within is generic), search for trait bounds on the type
        const auto& trait = this->m_crate.get_trait_by_path(sp, e.trait.m_path);
        const auto& assoc_ty = trait.m_types.at( e.item );
        // NOTE: The bounds here have 'Self' = the type
        for(const auto& bound : assoc_ty.m_trait_bounds )
        {
            ASSERT_BUG(sp, bound.m_trait_ptr, "Pointer to trait " << bound.m_path << " not set in " << e.trait.m_path);
            ::HIR::GenericPath final_trait_path;
            if( !this->trait_contains_method(sp, bound.m_path, *bound.m_trait_ptr, ::HIR::TypeRef("Self", 0xFFFF), method_name, ar,  final_trait_path) )
                continue ;
            DEBUG("- Found trait " << final_trait_path);
            
            if( monomorphise_pathparams_needed(final_trait_path.m_params) ) {
                // `Self` = `*e.type`
                // `/*I:#*/` := `e.trait.m_params`
                auto monomorph_cb = [&](const auto& gt)->const auto& {
                    const auto& ge = gt.m_data.as_Generic();
                    if( ge.binding == 0xFFFF ) {
                        return *e.type;
                    }
                    else if( ge.binding >> 8 == 0 ) {
                        auto idx = ge.binding & 0xFF;
                        ASSERT_BUG(sp, idx < e.trait.m_params.m_types.size(), "Type parameter out of range - " << gt);
                        return e.trait.m_params.m_types[idx];
                    }
                    else {
                        BUG(sp, "Unexpected type parameter - " << ty);
                    }
                    };
                final_trait_path.m_params = monomorphise_path_params_with(sp, final_trait_path.m_params, monomorph_cb, false);
                DEBUG("- Monomorph to " << final_trait_path);
            }
            
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
        bool rv = m_crate.find_type_impls(ty, m_ivars.callback_resolve_infer(), [&](const auto& impl) {
            // TODO: Should this take into account the actual suitability of this method? Or just that the name exists?
            // - If this impl matches fuzzily, it may not actually match
            auto it = impl.m_methods.find( method_name );
            if( it == impl.m_methods.end() )
                return false ;
            const ::HIR::Function&  fcn = it->second.data;
            switch(fcn.m_receiver)
            {
            case ::HIR::Function::Receiver::Free:
                break;
            case ::HIR::Function::Receiver::Value:
                if( ar == AllowedReceivers::AnyBorrow )
                    break;
                if( 0 )
            case ::HIR::Function::Receiver::Box:
                if( ar != AllowedReceivers::Box )
                    break;
                if(0)
            default:
                if( ar == AllowedReceivers::Value || ar == AllowedReceivers::Box )
                    break;
                DEBUG("Matching `impl" << impl.m_params.fmt_args() << " " << impl.m_type << "`"/* << " - " << top_ty*/);
                fcn_path = ::HIR::Path( ::HIR::Path::Data::make_UfcsInherent({
                    box$(ty.clone()),
                    method_name,
                    {}
                    }) );
                return true;
            }
            return false;
            });
        if( rv ) {
            return true;
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
            
            // TODO: Shouldn't this use trait_contains_method?
            // TODO: Search supertraits too
            auto it = trait_ref.second->m_values.find(method_name);
            if( it == trait_ref.second->m_values.end() )
                continue ;
            if( !it->second.is_Function() )
                continue ;
            const auto& v = it->second.as_Function();
            switch(v.m_receiver)
            {
            case ::HIR::Function::Receiver::Free:
                break;
            case ::HIR::Function::Receiver::Value:
                if( ar == AllowedReceivers::AnyBorrow )
                    break;
                if(0)
            case ::HIR::Function::Receiver::Box:
                if( ar != AllowedReceivers::Box )
                    break;
                if(0)
            default:
                if( ar == AllowedReceivers::Value || ar == AllowedReceivers::Box || (v.m_receiver == ::HIR::Function::Receiver::Box && ar != AllowedReceivers::AnyBorrow) )
                    break;
                DEBUG("Search for impl of " << *trait_ref.first);
                
                // Use the set of ivars we were given to populate the trait parameters
                unsigned int n_params = trait_ref.second->m_params.m_types.size();
                assert(n_params <= ivars.size());
                ::HIR::PathParams   trait_params;
                trait_params.m_types.reserve( n_params );
                for(unsigned int i = 0; i < n_params; i++) {
                    trait_params.m_types.push_back( ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Infer({ ivars[i], ::HIR::InferClass::None }) ) );
                    ASSERT_BUG(sp, m_ivars.get_type( trait_params.m_types.back() ).m_data.as_Infer().index == ivars[i], "A method selection ivar was bound");
                }
                
                if( find_trait_impls_crate(sp, *trait_ref.first, &trait_params, ty,  [](auto , auto ) { return true; }) ) {
                    DEBUG("Found trait impl " << *trait_ref.first << trait_params << " for " << ty << " ("<<m_ivars.fmt_type(ty)<<")");
                    fcn_path = ::HIR::Path( ::HIR::Path::Data::make_UfcsKnown({
                        box$( ty.clone() ),
                        ::HIR::GenericPath( *trait_ref.first, mv$(trait_params) ),
                        method_name,
                        {}
                        }) );
                    return true;
                }
                break;
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


