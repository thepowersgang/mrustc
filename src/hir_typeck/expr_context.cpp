/*
 */
#include "expr.hpp"
#include <hir/hir.hpp>
#include <algorithm>    // std::find_if

void typeck::TypecheckContext::push_traits(const ::std::vector<::std::pair< const ::HIR::SimplePath*, const ::HIR::Trait* > >& list)
{
    this->m_traits.insert( this->m_traits.end(), list.begin(), list.end() );
}
void typeck::TypecheckContext::pop_traits(const ::std::vector<::std::pair< const ::HIR::SimplePath*, const ::HIR::Trait* > >& list)
{
    this->m_traits.erase( this->m_traits.end() - list.size(), this->m_traits.end() );
}

void typeck::TypecheckContext::dump() const
{
    DEBUG("TypecheckContext - " << m_ivars.size() << " ivars, " << m_locals.size() << " locals");
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
    i = 0;
    for(const auto& v : m_locals) {
        DEBUG("VAR " << i << " '"<<v.name<<"' = " << v.type);
        i ++;
    }
}
void typeck::TypecheckContext::compact_ivars()
{
    TRACE_FUNCTION;
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
    
    for(auto& v : m_locals) {
        v.type = this->get_type(v.type).clone();
    }
}
bool typeck::TypecheckContext::apply_defaults()
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

void typeck::TypecheckContext::add_local(unsigned int index, const ::std::string& name, ::HIR::TypeRef type)
{
    if( m_locals.size() <= index )
        m_locals.resize(index+1);
    m_locals[index] = Variable(name, mv$(type));
}

const ::HIR::TypeRef& typeck::TypecheckContext::get_var_type(const Span& sp, unsigned int index)
{
    if( index >= m_locals.size() ) {
        this->dump();
        BUG(sp, "Local index out of range " << index << " >= " << m_locals.size());
    }
    return m_locals.at(index).type;
}


bool typeck::TypecheckContext::pathparams_contain_ivars(const ::HIR::PathParams& pps) const {
    for( const auto& ty : pps.m_types ) {
        if(this->type_contains_ivars(ty))
            return true;
    }
    return false;
}
bool typeck::TypecheckContext::type_contains_ivars(const ::HIR::TypeRef& ty) const {
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



///
/// Add inferrence variables to the provided type (if they're not already set)
///
void typeck::TypecheckContext::add_ivars(::HIR::TypeRef& type)
{
    TU_MATCH(::HIR::TypeRef::Data, (type.m_data), (e),
    (Infer,
        if( e.index == ~0u ) {
            e.index = this->new_ivar();
            this->m_ivars[e.index].type->m_data.as_Infer().ty_class = e.ty_class;
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
void typeck::TypecheckContext::add_ivars_params(::HIR::PathParams& params)
{
    for(auto& arg : params.m_types)
        add_ivars(arg);
}

void typeck::TypecheckContext::add_pattern_binding(const ::HIR::PatternBinding& pb, ::HIR::TypeRef type)
{
    assert( pb.is_valid() );
    switch( pb.m_type )
    {
    case ::HIR::PatternBinding::Type::Move:
        this->add_local( pb.m_slot, pb.m_name, mv$(type) );
        break;
    case ::HIR::PatternBinding::Type::Ref:
        this->add_local( pb.m_slot, pb.m_name, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, mv$(type)) );
        break;
    case ::HIR::PatternBinding::Type::MutRef:
        this->add_local( pb.m_slot, pb.m_name, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Unique, mv$(type)) );
        break;
    }
}

// -------------------------------------------------------------------------------------------------------------------
//
// -------------------------------------------------------------------------------------------------------------------
void typeck::TypecheckContext::add_binding(const Span& sp, ::HIR::Pattern& pat, ::HIR::TypeRef& type)
{
    TRACE_FUNCTION_F("pat = " << pat << ", type = " << type);
    
    if( pat.m_binding.is_valid() ) {
        this->add_pattern_binding(pat.m_binding, type.clone());
        // TODO: Can there be bindings within a bound pattern?
        //return ;
    }
    
    // 
    TU_MATCH(::HIR::Pattern::Data, (pat.m_data), (e),
    (Any,
        // Just leave it, the pattern says nothing
        ),
    (Value,
        //TODO(sp, "Value pattern");
        ),
    (Range,
        //TODO(sp, "Range pattern");
        ),
    (Box,
        TODO(sp, "Box pattern");
        ),
    (Ref,
        if( type.m_data.is_Infer() ) {
            this->apply_equality(sp, type, ::HIR::TypeRef::new_borrow( e.type, this->new_ivar_tr() ));
            type = this->get_type(type).clone();
        }
        // Type must be a &-ptr
        TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
        (
            ERROR(sp, E0000, "Pattern-type mismatch, expected &-ptr, got " << type);
            ),
        (Infer, throw "";),
        (Borrow,
            if( te.type != e.type ) {
                ERROR(sp, E0000, "Pattern-type mismatch, expected &-ptr, got " << type);
            }
            this->add_binding(sp, *e.sub, *te.inner );
            )
        )
        ),
    (Tuple,
        if( type.m_data.is_Infer() ) {
            ::std::vector< ::HIR::TypeRef>  sub_types;
            for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                sub_types.push_back( this->new_ivar_tr() );
            this->apply_equality(sp, type, ::HIR::TypeRef( mv$(sub_types) ));
            type = this->get_type(type).clone();
        }
        TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
        (
            ERROR(sp, E0000, "Pattern-type mismatch, expected tuple, got " << type);
            ),
        (Infer, throw ""; ),
        (Tuple,
            if( te.size() != e.sub_patterns.size() ) {
                ERROR(sp, E0000, "Pattern-type mismatch, expected " << e.sub_patterns.size() << "-tuple, got " << type);
            }
            for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                this->add_binding(sp, e.sub_patterns[i], te[i] );
            )
        )
        ),
    (Slice,
        if( type.m_data.is_Infer() ) {
            this->apply_equality(sp, type, ::HIR::TypeRef::new_slice( this->new_ivar_tr() ));
            type = this->get_type(type).clone();
        }
        TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
        (
            ERROR(sp, E0000, "Pattern-type mismatch, expected slice, got " << type);
            ),
        (Infer, throw""; ),
        (Slice,
            for(auto& sub : e.sub_patterns)
                this->add_binding(sp, sub, *te.inner );
            )
        )
        ),
    (SplitSlice,
        if( type.m_data.is_Infer() ) {
            this->apply_equality(sp, type, ::HIR::TypeRef::new_slice( this->new_ivar_tr() ));
            type = this->get_type(type).clone();
        }
        TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
        (
            ERROR(sp, E0000, "Pattern-type mismatch, expected slice, got " << type);
            ),
        (Infer, throw ""; ),
        (Slice,
            for(auto& sub : e.leading)
                this->add_binding( sp, sub, *te.inner );
            for(auto& sub : e.trailing)
                this->add_binding( sp, sub, *te.inner );
            if( e.extra_bind.is_valid() ) {
                this->add_local( e.extra_bind.m_slot, e.extra_bind.m_name, type.clone() );
            }
            )
        )
        ),
    
    // - Enums/Structs
    (StructTuple,
        this->add_ivars_params( e.path.m_params );
        if( type.m_data.is_Infer() ) {
            this->apply_equality( sp, type, ::HIR::TypeRef::new_path(e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)) );
            type = this->get_type(type).clone();
        }
        assert(e.binding);
        const auto& str = *e.binding;
        // - assert check from earlier pass
        assert( str.m_data.is_Tuple() );
        const auto& sd = str.m_data.as_Tuple();
        
        TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
        (
            ERROR(sp, E0000, "Pattern-type mismatch, expected struct, got " << type);
            ),
        (Infer, throw ""; ),
        (Path,
            if( ! te.binding.is_Struct() || te.binding.as_Struct() != &str ) {
                ERROR(sp, E0000, "Type mismatch in struct pattern - " << type << " is not " << e.path);
            }
            // NOTE: Must be Generic for the above to have passed
            auto& gp = te.path.m_data.as_Generic();
            
            if( e.sub_patterns.size() != sd.size() ) { 
                ERROR(sp, E0000, "Tuple struct pattern with an incorrect number of fields");
            }
            for( unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
            {
                const auto& field_type = sd[i].ent;
                if( monomorphise_type_needed(field_type) ) {
                    auto var_ty = monomorphise_type(sp, str.m_params, gp.m_params,  field_type);
                    this->add_binding(sp, e.sub_patterns[i], var_ty);
                }
                else {
                    // SAFE: Can't have _ as monomorphise_type_needed checks for that
                    this->add_binding(sp, e.sub_patterns[i], const_cast< ::HIR::TypeRef&>(field_type));
                }
            }
            )
        )
        ),
    (StructTupleWildcard,
        this->add_ivars_params( e.path.m_params );
        if( type.m_data.is_Infer() ) {
            this->apply_equality( sp, type, ::HIR::TypeRef::new_path(e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)) );
            type = this->get_type(type).clone();
        }
        assert(e.binding);
        const auto& str = *e.binding;
        // - assert check from earlier pass
        assert( str.m_data.is_Tuple() );
        
        TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
        (
            // TODO: Type mismatch
            ),
        (Infer, throw ""; ),
        (Path,
            if( ! te.binding.is_Struct() || te.binding.as_Struct() != &str ) {
                ERROR(sp, E0000, "Type mismatch in struct pattern - " << type << " is not " << e.path);
            }
            )
        )
        ),
    (Struct,
        this->add_ivars_params( e.path.m_params );
        if( type.m_data.is_Infer() ) {
            this->apply_equality( sp, type, ::HIR::TypeRef::new_path(e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)) );
            type = this->get_type(type).clone();
        }
        assert(e.binding);
        const auto& str = *e.binding;
        // - assert check from earlier pass
        assert( str.m_data.is_Named() );
        const auto& sd = str.m_data.as_Named();
        
        TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
        (
            // TODO: Type mismatch
            ),
        (Infer, throw ""; ),
        (Path,
            if( ! te.binding.is_Struct() || te.binding.as_Struct() != &str ) {
                ERROR(sp, E0000, "Type mismatch in struct pattern - " << type << " is not " << e.path);
            }
            // NOTE: Must be Generic for the above to have passed
            auto& gp = te.path.m_data.as_Generic();
            for( auto& field_pat : e.sub_patterns )
            {
                unsigned int f_idx = ::std::find_if( sd.begin(), sd.end(), [&](const auto& x){ return x.first == field_pat.first; } ) - sd.begin();
                if( f_idx == sd.size() ) {
                    ERROR(sp, E0000, "Struct " << e.path << " doesn't have a field " << field_pat.first);
                }
                const ::HIR::TypeRef& field_type = sd[f_idx].second.ent;
                if( monomorphise_type_needed(field_type) ) {
                    auto field_type_mono = monomorphise_type(sp, str.m_params, gp.m_params,  field_type);
                    this->add_binding(sp, field_pat.second, field_type_mono);
                }
                else {
                    // SAFE: Can't have _ as monomorphise_type_needed checks for that
                    this->add_binding(sp, field_pat.second, const_cast< ::HIR::TypeRef&>(field_type));
                }
            }
            )
        )
        ),
    (EnumTuple,
        this->add_ivars_params( e.path.m_params );
        if( type.m_data.is_Infer() ) {
            auto path = e.path.clone();
            path.m_path.m_components.pop_back();

            this->apply_equality( sp, type, ::HIR::TypeRef::new_path(mv$(path), ::HIR::TypeRef::TypePathBinding(e.binding_ptr)) );
            type = this->get_type(type).clone();
        }
        assert(e.binding_ptr);
        const auto& enm = *e.binding_ptr;
        const auto& var = enm.m_variants[e.binding_idx].second;
        assert(var.is_Tuple());
        const auto& tup_var = var.as_Tuple();
        
        TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
        (
            // TODO: Type mismatch
            ),
        (Infer, throw ""; ),
        (Path,
            if( ! te.binding.is_Enum() || te.binding.as_Enum() != &enm ) {
                ERROR(sp, E0000, "Type mismatch in enum pattern - " << type << " is not " << e.path);
            }
            // NOTE: Must be Generic for the above to have passed
            auto& gp = te.path.m_data.as_Generic();
            if( e.sub_patterns.size() != tup_var.size() ) { 
                ERROR(sp, E0000, "Enum pattern with an incorrect number of fields - " << e.path << " - expected " << tup_var.size() << ", got " << e.sub_patterns.size());
            }
            for( unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
            {
                if( monomorphise_type_needed(tup_var[i]) ) {
                    auto var_ty = monomorphise_type(sp, enm.m_params, gp.m_params,  tup_var[i]);
                    this->add_binding(sp, e.sub_patterns[i], var_ty);
                }
                else {
                    // SAFE: Can't have a _ (monomorphise_type_needed checks for that)
                    this->add_binding(sp, e.sub_patterns[i], const_cast< ::HIR::TypeRef&>(tup_var[i]));
                }
            }
            )
        )
        ),
    (EnumTupleWildcard,
        this->add_ivars_params( e.path.m_params );
        if( type.m_data.is_Infer() ) {
            auto path = e.path.clone();
            path.m_path.m_components.pop_back();

            this->apply_equality( sp, type, ::HIR::TypeRef::new_path(mv$(path), ::HIR::TypeRef::TypePathBinding(e.binding_ptr)) );
            type = this->get_type(type).clone();
        }
        assert(e.binding_ptr);
        const auto& enm = *e.binding_ptr;
        const auto& var = enm.m_variants[e.binding_idx].second;
        assert(var.is_Tuple());
        
        TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
        (
            // TODO: Type mismatch
            ),
        (Infer, throw ""; ),
        (Path,
            if( ! te.binding.is_Enum() || te.binding.as_Enum() != &enm ) {
                ERROR(sp, E0000, "Type mismatch in enum pattern - " << type << " is not " << e.path);
            }
            )
        )
        ),
    (EnumStruct,
        this->add_ivars_params( e.path.m_params );
        if( type.m_data.is_Infer() ) {
            auto path = e.path.clone();
            path.m_path.m_components.pop_back();

            this->apply_equality( sp, type, ::HIR::TypeRef::new_path(mv$(path), ::HIR::TypeRef::TypePathBinding(e.binding_ptr)) );
            type = this->get_type(type).clone();
        }
        assert(e.binding_ptr);
        const auto& enm = *e.binding_ptr;
        const auto& var = enm.m_variants[e.binding_idx].second;
        assert(var.is_Struct());
        const auto& tup_var = var.as_Struct();
        
        TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
        (
            // TODO: Type mismatch
            ),
        (Infer, throw ""; ),
        (Path,
            if( ! te.binding.is_Enum() || te.binding.as_Enum() != &enm ) {
                ERROR(sp, E0000, "Type mismatch in enum pattern - " << type << " is not " << e.path);
            }
            // NOTE: Must be Generic for the above to have passed
            auto& gp = te.path.m_data.as_Generic();
            
            for( auto& field_pat : e.sub_patterns )
            {
                unsigned int f_idx = ::std::find_if( tup_var.begin(), tup_var.end(), [&](const auto& x){ return x.first == field_pat.first; } ) - tup_var.begin();
                if( f_idx == tup_var.size() ) {
                    ERROR(sp, E0000, "Enum variant " << e.path << " doesn't have a field " << field_pat.first);
                }
                const ::HIR::TypeRef& field_type = tup_var[f_idx].second;
                if( monomorphise_type_needed(field_type) ) {
                    auto field_type_mono = monomorphise_type(sp, enm.m_params, gp.m_params,  field_type);
                    this->add_binding(sp, field_pat.second, field_type_mono);
                }
                else {
                    // SAFE: Can't have _ as monomorphise_type_needed checks for that
                    this->add_binding(sp, field_pat.second, const_cast< ::HIR::TypeRef&>(field_type));
                }
            }
            )
        )
        )
    )
}


// -------------------------------------------------------------------------------------------------------------------
//
// -------------------------------------------------------------------------------------------------------------------
void typeck::TypecheckContext::apply_pattern(const ::HIR::Pattern& pat, ::HIR::TypeRef& type)
{
    static Span _sp;
    const Span& sp = _sp;

    auto& ty = this->get_type(type);
    
    TU_MATCH(::HIR::Pattern::Data, (pat.m_data), (e),
    (Any,
        // Just leave it, the pattern says nothing about the type
        ),
    (Value,
        //TODO(sp, "Value pattern");
        ),
    (Range,
        //TODO(sp, "Range pattern");
        ),
    // - Pointer destructuring
    (Box,
        // Type must be box-able
        //TODO(sp, "Box patterns");
        ),
    (Ref,
        if( ty.m_data.is_Infer() ) {
            BUG(sp, "Infer type hit that should already have been fixed");
        }
        // Type must be a &-ptr
        TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
        (
            // TODO: Type mismatch
            ),
        (Infer, throw "";),
        (Borrow,
            if( te.type != e.type ) {
                // TODO: Type mismatch
            }
            this->apply_pattern( *e.sub, *te.inner );
            )
        )
        ),
    (Tuple,
        if( ty.m_data.is_Infer() ) {
            BUG(sp, "Infer type hit that should already have been fixed");
        }
        TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
        (
            // TODO: Type mismatch
            ),
        (Infer, throw "";),
        (Tuple,
            if( te.size() != e.sub_patterns.size() ) {
                // TODO: Type mismatch
            }
            for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                this->apply_pattern( e.sub_patterns[i], te[i] );
            )
        )
        ),
    // --- Slices
    (Slice,
        if( ty.m_data.is_Infer() ) {
            BUG(sp, "Infer type hit that should already have been fixed");
        }
        TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
        (
            // TODO: Type mismatch
            ),
        (Infer, throw "";),
        (Slice,
            for(const auto& sp : e.sub_patterns )
                this->apply_pattern( sp, *te.inner );
            )
        )
        ),
    (SplitSlice,
        if( ty.m_data.is_Infer() ) {
            BUG(sp, "Infer type hit that should already have been fixed");
        }
        TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
        (
            // TODO: Type mismatch
            ),
        (Infer, throw "";),
        (Slice,
            for(const auto& sp : e.leading)
                this->apply_pattern( sp, *te.inner );
            for(const auto& sp : e.trailing)
                this->apply_pattern( sp, *te.inner );
            // TODO: extra_bind? (see comment at start of function)
            )
        )
        ),
    
    // - Enums/Structs
    (StructTuple,
        if( ty.m_data.is_Infer() ) {
            ty.m_data = ::HIR::TypeRef::Data::make_Path( {e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)} );
            this->mark_change();
        }
        
        TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
        (
            // TODO: Type mismatch
            ),
        (Infer, throw "";),
        (Path,
            // TODO: Does anything need to happen here? This can only introduce equalities?
            )
        )
        ),
    (StructTupleWildcard,
        ),
    (Struct,
        if( ty.m_data.is_Infer() ) {
            //TODO: Does this lead to issues with generic parameters?
            ty.m_data = ::HIR::TypeRef::Data::make_Path( {e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)} );
            this->mark_change();
        }
        
        TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
        (
            // TODO: Type mismatch
            ),
        (Infer, throw "";),
        (Path,
            // TODO: Does anything need to happen here? This can only introduce equalities?
            )
        )
        ),
    (EnumTuple,
        if( ty.m_data.is_Infer() ) {
            TODO(sp, "EnumTuple - infer");
            this->mark_change();
        }
        
        TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
        (
            // TODO: Type mismatch
            ),
        (Infer, throw "";),
        (Path,
            )
        )
        ),
    (EnumTupleWildcard,
        ),
    (EnumStruct,
        if( ty.m_data.is_Infer() ) {
            TODO(sp, "EnumStruct - infer");
            this->mark_change();
        }
        
        TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
        (
            // TODO: Type mismatch
            ),
        (Infer, throw "";),
        (Path,
            )
        )
        )
    )
}

// -------------------------------------------------------------------------------------------------------------------
//
// -------------------------------------------------------------------------------------------------------------------
const ::HIR::TypeRef& typeck::TypecheckContext::expand_associated_types_to(const Span& sp, const ::HIR::TypeRef& t, ::HIR::TypeRef& tmp_t) const
{
    TU_IFLET(::HIR::TypeRef::Data, t.m_data, Path, e,
        if( e.path.m_data.is_Generic() )
            return t;
        else if( !e.binding.is_Unbound() ) {
            return t;
        }
        else {
            // HACK! Run twice, to expand deeper.
            // - Should this recurse itself when it resolves?
            tmp_t = this->expand_associated_types(sp, this->expand_associated_types(sp, t.clone()));
            DEBUG("Expanded " << t << " into " << tmp_t);
            return tmp_t;
        }
    )
    else {
        return t;
    }
}

void typeck::TypecheckContext::apply_equality(const Span& sp, const ::HIR::TypeRef& left, t_cb_generic cb_left, const ::HIR::TypeRef& right, t_cb_generic cb_right, ::HIR::ExprNodeP* node_ptr_ptr)
{
    TRACE_FUNCTION_F(left << ", " << right);
    assert( ! left.m_data.is_Infer() ||  left.m_data.as_Infer().index != ~0u );
    assert( !right.m_data.is_Infer() || right.m_data.as_Infer().index != ~0u );
    // - Convert left/right types into resolved versions (either root ivar, or generic replacement)
    const auto& l_t1 = left .m_data.is_Generic() ? cb_left (left ) : this->get_type(left );
    const auto& r_t1 = right.m_data.is_Generic() ? cb_right(right) : this->get_type(right);
    if( l_t1 == r_t1 ) {
        DEBUG(l_t1 << " == " << r_t1);
        return ;
    }
    
    ::HIR::TypeRef  left_tmp;
    const auto& l_t = this->expand_associated_types_to(sp, l_t1, left_tmp);
    ::HIR::TypeRef  right_tmp;
    const auto& r_t = this->expand_associated_types_to(sp, r_t1, right_tmp);
    if( l_t == r_t ) {
        DEBUG(l_t << " == " << r_t);
        return ;
    }
    
    DEBUG("- l_t = " << l_t << ", r_t = " << r_t);
    TU_IFLET(::HIR::TypeRef::Data, r_t.m_data, Infer, r_e,
        TU_IFLET(::HIR::TypeRef::Data, l_t.m_data, Infer, l_e,
            // If both are infer, unify the two ivars (alias right to point to left)
            this->ivar_unify(l_e.index, r_e.index);
        )
        else {
            // Righthand side is infer, alias it to the left
            //  TODO: that `true` should be `false` if the callback isn't unity (for bug checking)
            this->set_ivar_to(r_e.index, this->expand_associated_types(sp, monomorphise_type_with(sp, left, cb_left, true)));
        }
    )
    else {
        TU_IFLET(::HIR::TypeRef::Data, l_t.m_data, Infer, l_e,
            // Lefthand side is infer, alias it to the right
            //  TODO: that `true` should be `false` if the callback isn't unity (for bug checking)
            this->set_ivar_to(l_e.index, this->expand_associated_types(sp, monomorphise_type_with(sp, right, cb_right, true)));
        )
        else {
            // If generic replacement happened, clear the callback
            if( left.m_data.is_Generic() || left.m_data.is_Infer() ) {
                cb_left = [](const auto& x)->const auto&{return x;};
            }
            if( right.m_data.is_Generic() || right.m_data.is_Infer() ) {
                cb_right = [](const auto& x)->const auto&{return x;};
            }
            
            // Neither are infer - both should be of the same form
            // - If either side is `!`, return early (diverging type, matches anything)
            if( l_t.m_data.is_Diverge() || r_t.m_data.is_Diverge() ) {
                // TODO: Should diverge check be done elsewhere? what happens if a ! ends up in an ivar?
                return ;
            }
            
            // Helper function for Path and TraitObject
            auto equality_typeparams = [&](const ::HIR::PathParams& l, const ::HIR::PathParams& r) {
                    if( l.m_types.size() != r.m_types.size() ) {
                        ERROR(sp, E0000, "Type mismatch in type params `" << l << "` and `" << r << "`");
                    }
                    for(unsigned int i = 0; i < l.m_types.size(); i ++)
                    {
                        this->apply_equality(sp, l.m_types[i], cb_left, r.m_types[i], cb_right, nullptr);
                    }
                };

            if( l_t.m_data.is_Pointer() && r_t.m_data.is_Borrow() ) {
                const auto& l_e = l_t.m_data.as_Pointer();
                const auto& r_e = r_t.m_data.as_Borrow();
                if( l_e.type != r_e.type ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " (pointer type mismatch)");
                }
                // 1. Equate inner types
                this->apply_equality(sp, *l_e.inner, cb_left, *r_e.inner, cb_right, nullptr);

                // 2. If that succeeds, add a coerce
                if( node_ptr_ptr != nullptr )
                {
                    auto& node_ptr = *node_ptr_ptr;
                    auto span = node_ptr->span();
                    // - Override the existing result type (to prevent infinite recursion)
                    node_ptr->m_res_type = r_t.clone();
                    node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Cast( mv$(span), mv$(node_ptr), l_t.clone() ));
                    //node_ptr->m_res_type = l_t.clone();   // < Set by _Cast
                    
                    DEBUG("- Borrow->Pointer cast added - " << l_t << " <- " << r_t);
                    this->mark_change();
                    return ;
                }
                else
                {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " (can't coerce)");
                }
            }

            // - If tags don't match, error
            if( l_t.m_data.tag() != r_t.m_data.tag() ) {
                
                // If either side is UFCS, and still contains unconstrained ivars - don't error
                struct H {
                    static bool type_is_free_ufcs(const TypecheckContext& context, const ::HIR::TypeRef& ty) {
                        TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Path, e,
                            if( e.path.m_data.is_Generic() ) {
                                return false;
                            }
                            return context.type_contains_ivars(ty);
                        )
                        return false;
                    }
                };

                if( H::type_is_free_ufcs(*this, l_t) || H::type_is_free_ufcs(*this, r_t) ) {
                    return ;
                }
                
                // Type error
                this->dump();
                ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
            }
            TU_MATCH(::HIR::TypeRef::Data, (l_t.m_data, r_t.m_data), (l_e, r_e),
            (Infer,
                throw "";
                ),
            (Diverge,
                TODO(sp, "Handle !");
                ),
            (Primitive,
                if( l_e != r_e ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                ),
            (Path,
                if( l_e.path.m_data.tag() != r_e.path.m_data.tag() ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                TU_MATCH(::HIR::Path::Data, (l_e.path.m_data, r_e.path.m_data), (lpe, rpe),
                (Generic,
                    if( lpe.m_path != rpe.m_path ) {
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                    }
                    equality_typeparams(lpe.m_params, rpe.m_params);
                    ),
                (UfcsInherent,
                    equality_typeparams(lpe.params, rpe.params);
                    if( lpe.item != rpe.item )
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                    this->apply_equality(sp, *lpe.type, cb_left, *rpe.type, cb_right, nullptr);
                    ),
                (UfcsKnown,
                    equality_typeparams(lpe.trait.m_params, rpe.trait.m_params);
                    equality_typeparams(lpe.params, rpe.params);
                    if( lpe.item != rpe.item )
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                    this->apply_equality(sp, *lpe.type, cb_left, *rpe.type, cb_right, nullptr);
                    ),
                (UfcsUnknown,
                    // TODO: If the type is fully known, locate a suitable trait item
                    equality_typeparams(lpe.params, rpe.params);
                    if( lpe.item != rpe.item )
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                    this->apply_equality(sp, *lpe.type, cb_left, *rpe.type, cb_right, nullptr);
                    )
                )
                ),
            (Generic,
                if( l_e.binding != r_e.binding ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                ),
            (TraitObject,
                if( l_e.m_trait.m_path.m_path != r_e.m_trait.m_path.m_path ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                equality_typeparams(l_e.m_trait.m_path.m_params, r_e.m_trait.m_path.m_params);
                for(auto it_l = l_e.m_trait.m_type_bounds.begin(), it_r = r_e.m_trait.m_type_bounds.begin(); it_l != l_e.m_trait.m_type_bounds.end(); it_l++, it_r++ ) {
                    if( it_l->first != it_r->first ) {
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - associated bounds differ");
                    }
                    this->apply_equality(sp, it_l->second, cb_left, it_r->second, cb_right, nullptr);
                }
                // TODO: Possibly allow inferrence reducing the set?
                if( l_e.m_markers.size() != r_e.m_markers.size() ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - trait counts differ");
                }
                // TODO: Is this list sorted in any way? (if it's not sorted, this could fail when source does Send+Any instead of Any+Send)
                for(unsigned int i = 0; i < l_e.m_markers.size(); i ++ )
                {
                    auto& l_p = l_e.m_markers[i];
                    auto& r_p = r_e.m_markers[i];
                    if( l_p.m_path != r_p.m_path ) {
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                    }
                    equality_typeparams(l_p.m_params, r_p.m_params);
                }
                // NOTE: Lifetime is ignored
                ),
            (Array,
                this->apply_equality(sp, *l_e.inner, cb_left, *r_e.inner, cb_right, nullptr);
                if( l_e.size_val != r_e.size_val ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - sizes differ");
                }
                ),
            (Slice,
                this->apply_equality(sp, *l_e.inner, cb_left, *r_e.inner, cb_right, nullptr);
                ),
            (Tuple,
                if( l_e.size() != r_e.size() ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - Tuples are of different length");
                }
                for(unsigned int i = 0; i < l_e.size(); i ++)
                {
                    this->apply_equality(sp, l_e[i], cb_left, r_e[i], cb_right, nullptr);
                }
                ),
            (Borrow,
                // If using `&mut T` where `&const T` is expected - insert a reborrow (&*)
                if( l_e.type == ::HIR::BorrowType::Shared && r_e.type == ::HIR::BorrowType::Unique && node_ptr_ptr ) {
                    this->apply_equality(sp, *l_e.inner, cb_left, *r_e.inner, cb_right, nullptr);
                    
                    // Add cast down
                    auto& node_ptr = *node_ptr_ptr;
                    
                    auto span = node_ptr->span();
                    // *<inner>
                    node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Deref(mv$(span), mv$(node_ptr)));
                    node_ptr->m_res_type = l_e.inner->clone();
                    // &*<inner>
                    node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_UniOp(mv$(span), ::HIR::ExprNode_UniOp::Op::Ref, mv$(node_ptr)));
                    node_ptr->m_res_type = l_t.clone();
                    
                    this->mark_change();
                    return ;
                }
                
                if( l_e.type != r_e.type ) {
                    // TODO: This could be allowed if left == Shared && right == Unique (reborrowing)
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - Borrow classes differ");
                }
                // ------------------
                // Coercions!
                // ------------------
                if( node_ptr_ptr != nullptr )
                {
                    auto& node_ptr = *node_ptr_ptr;
                    const auto& left_inner_res  = this->get_type(*l_e.inner);
                    const auto& right_inner_res = this->get_type(*r_e.inner);
                    
                    // Allow cases where `right`: ::core::marker::Unsize<`left`>
                    ::HIR::PathParams   pp;
                    pp.m_types.push_back( left_inner_res.clone() );
                    bool succ = this->find_trait_impls(sp, this->m_crate.get_lang_item_path(sp, "unsize"), pp, right_inner_res, [&](const auto& args, const auto& ) {
                        DEBUG("- Found unsizing with args " << args);
                        return args.m_types[0] == left_inner_res;
                        });
                    if( succ ) {
                        auto span = node_ptr->span();
                        node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Unsize( mv$(span), mv$(node_ptr), l_t.clone() ));
                        node_ptr->m_res_type = l_t.clone();
                        
                        this->mark_change();
                        return ;
                    }
                    // - If left is a trait object, right can unsize
                    if( left_inner_res.m_data.is_TraitObject() ) {
                        // If the righthand side is still an ivar
                        if( right_inner_res.m_data.is_Infer() ) {
                            // Assume it's valid for now, and return 
                            return ;
                        }
                        
                        const auto& e = left_inner_res.m_data.as_TraitObject();
                        if( right_inner_res.m_data.is_TraitObject() ) {
                            // TODO: Can Debug+Send be coerced to Debug?
                            if( left_inner_res != right_inner_res )
                                ERROR(sp, E0000, "Can't coerce between trait objects - " << left_inner_res << " and " << right_inner_res);
                            // - Equal, nothing to do
                            return ;
                        }
                        
                        // 1. Search for an implementation of the data trait for this type
                        bool succ = this->find_trait_impls(sp, e.m_trait.m_path.m_path, e.m_trait.m_path.m_params,  right_inner_res, [&](const auto& args,const auto& types) {
                            if( args.m_types.size() > 0 )
                                TODO(sp, "Handle unsizing to traits with params");
                            // TODO: Check `types`
                            return true;
                            });
                        if(!succ)
                            ERROR(sp, E0000, "Trait " << e.m_trait << " isn't implemented for " << right_inner_res );
                        for(const auto& marker : e.m_markers)
                        {
                            TODO(sp, "Check for marker trait impls ("<<marker<<") when unsizing to " << left_inner_res << " from " << right_inner_res);
                            //bool succ = this->find_trait_impls(e.m_trait.m_path, right_inner_res, [&](const auto& args) {
                            //    if( args.m_types.size() > 0 )
                            //        TODO(sp, "Handle unsizing to traits with params");
                            //    return true;
                            //    });
                        }
                        for(const auto& assoc : e.m_trait.m_type_bounds)
                        {
                            TODO(sp, "Check for associated type ("<<assoc.first<<") validity when unsizing to " << left_inner_res << " from " << right_inner_res);
                        }
                        
                        // Valid unsize, insert unsize operation
                        auto span = node_ptr->span();
                        node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Unsize( mv$(span), mv$(node_ptr), l_t.clone() ));
                        node_ptr->m_res_type = l_t.clone();
                        
                        this->mark_change();
                        return ;
                    }
                    // - If left is a slice, right can unsize/deref
                    if( left_inner_res.m_data.is_Slice() && !right_inner_res.m_data.is_Slice() )
                    {
                        const auto& left_slice = left_inner_res.m_data.as_Slice();
                        TU_IFLET(::HIR::TypeRef::Data, right_inner_res.m_data, Array, right_array,
                            this->apply_equality(sp, *left_slice.inner, cb_left, *right_array.inner, cb_right, nullptr);
                            auto span = node_ptr->span();
                            node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Unsize( mv$(span), mv$(node_ptr), l_t.clone() ));
                            node_ptr->m_res_type = l_t.clone();
                            
                            this->mark_change();
                            return ;
                        )
                        else TU_IFLET(::HIR::TypeRef::Data, right_inner_res.m_data, Generic, right_arg,
                            TODO(sp, "Search for Unsize bound on generic");
                        )
                        else
                        {
                            // Apply deref coercions
                        }
                    }
                    // - If right has a deref chain to left, build it
                }
                this->apply_equality(sp, *l_e.inner, cb_left, *r_e.inner, cb_right, nullptr);
                ),
            (Pointer,
                // If using `*mut T` where `*const T` is expected - add cast
                if( l_e.type == ::HIR::BorrowType::Shared && r_e.type == ::HIR::BorrowType::Unique && node_ptr_ptr ) {
                    this->apply_equality(sp, *l_e.inner, cb_left, *r_e.inner, cb_right, nullptr);
                    
                    // Add cast down
                    auto& node_ptr = *node_ptr_ptr;
                    
                    auto span = node_ptr->span();
                    node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Cast( mv$(span), mv$(node_ptr), l_t.clone() ));
                    node_ptr->m_res_type = l_t.clone();
                    
                    this->mark_change();
                    return ;
                }
                
                if( l_e.type != r_e.type ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - Pointer mutability differs");
                }
                this->apply_equality(sp, *l_e.inner, cb_left, *r_e.inner, cb_right, nullptr);
                ),
            (Function,
                if( l_e.is_unsafe != r_e.is_unsafe
                    || l_e.m_abi != r_e.m_abi
                    || l_e.m_arg_types.size() != r_e.m_arg_types.size()
                    )
                {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                // NOTE: No inferrence in fn types? Not sure, lazy way is to allow it.
                this->apply_equality(sp, *l_e.m_rettype, cb_left, *r_e.m_rettype, cb_right, nullptr);
                for(unsigned int i = 0; i < l_e.m_arg_types.size(); i ++ ) {
                    this->apply_equality(sp, l_e.m_arg_types[i], cb_left, r_e.m_arg_types[i], cb_right, nullptr);
                }
                ),
            (Closure,
                TODO(sp, "apply_equality - Closure");
                )
            )
        }
    }
}

// -------------------------------------------------------------------------------------------------------------------
//
// -------------------------------------------------------------------------------------------------------------------
bool typeck::TypecheckContext::check_trait_bound(const Span& sp, const ::HIR::TypeRef& type, const ::HIR::GenericPath& trait, ::std::function<const ::HIR::TypeRef&(const ::HIR::TypeRef&)> placeholder) const
{
    if( this->find_trait_impls_bound(sp, trait.m_path, trait.m_params, placeholder(type), [&](const auto& args, const auto& _){
            DEBUG("TODO: Check args for " << trait.m_path << args << " against " << trait);
            return true;
        })
        )
    {
        // Satisfied by generic
        return true;
    }
    else if( this->m_crate.find_trait_impls(trait.m_path, type, placeholder, [&](const auto& impl) {
            DEBUG("- Bound " << type << " : " << trait << " satisfied by impl" << impl.m_params.fmt_args());
            // TODO: Recursively check
            return true;
        })
        )
    {
        // Match!
        return true;
    }
    else {
        DEBUG("- Bound " << type << " ("<<placeholder(type)<<") : " << trait << " failed");
        return false;
    }
}


// -------------------------------------------------------------------------------------------------------------------
//
// -------------------------------------------------------------------------------------------------------------------
bool typeck::TypecheckContext::iterate_bounds( ::std::function<bool(const ::HIR::GenericBound&)> cb) const
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
bool typeck::TypecheckContext::find_trait_impls(const Span& sp,
        const ::HIR::SimplePath& trait, const ::HIR::PathParams& params,
        const ::HIR::TypeRef& type,
        t_cb_trait_impl callback
        ) const
{
    TRACE_FUNCTION_F("trait = " << trait << ", type = " << type);
    
    // Closures are magical. They're unnamable and all trait impls come from within the compiler
    TU_IFLET(::HIR::TypeRef::Data, type.m_data, Closure, e,
        const auto trait_fn = this->m_crate.get_lang_item_path(sp, "fn");
        const auto trait_fn_mut = this->m_crate.get_lang_item_path(sp, "fn_mut");
        const auto trait_fn_once = this->m_crate.get_lang_item_path(sp, "fn_once");
        
        if( trait == trait_fn || trait == trait_fn_mut || trait == trait_fn_once  ) {
            if( params.m_types.size() != 1 )
                BUG(sp, "Fn* traits require a single tuple argument");
            TU_MATCH_DEF( ::HIR::TypeRef::Data, (params.m_types[0].m_data), (te),
            (
                ),
            (Tuple,
                )
            )
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
            return callback( pp, types );
        }
        else {
            return false;
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
::HIR::TypeRef typeck::TypecheckContext::expand_associated_types(const Span& sp, ::HIR::TypeRef input) const
{
    TRACE_FUNCTION_F(input);
    TU_MATCH(::HIR::TypeRef::Data, (input.m_data), (e),
    (Infer,
        auto& ty = this->get_type(input);
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
        TU_MATCH(::HIR::Path::Data, (e.path.m_data), (e2),
        (Generic,
            for(auto& arg : e2.m_params.m_types)
                arg = expand_associated_types(sp, mv$(arg));
            ),
        (UfcsInherent,
            TODO(sp, "Path - UfcsInherent - " << e.path);
            ),
        (UfcsKnown,
            // - Only try resolving if the binding isn't known
            if( !e.binding.is_Unbound() )
                return input;
            
            DEBUG("Locating associated type for " << e.path);
            
            *e2.type = expand_associated_types(sp, mv$(*e2.type));
            
            
            // - If it's a closure, then the only trait impls are those generated by typeck
            TU_IFLET(::HIR::TypeRef::Data, e2.type->m_data, Closure, te,
                const auto trait_fn = this->m_crate.get_lang_item_path(sp, "fn");
                const auto trait_fn_mut = this->m_crate.get_lang_item_path(sp, "fn_mut");
                const auto trait_fn_once = this->m_crate.get_lang_item_path(sp, "fn_once");
                if( e2.trait.m_path == trait_fn || e2.trait.m_path == trait_fn_mut || e2.trait.m_path == trait_fn_once  ) {
                    if( e2.item == "Output" ) {
                        return te.m_rettype->clone();
                    }
                    else {
                        ERROR(sp, E0000, "No associated type " << e2.item << " for trait " << e2.trait);
                    }
                }
                else {
                    ERROR(sp, E0000, "No implementation of " << e2.trait << " for " << *e2.type);
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
                    if( be.type != *e2.type )
                        return false;
                    // 2. Check if the trait (or any supertrait) includes e2.trait
                    if( be.trait.m_path == e2.trait ) {
                        auto it = be.trait.m_type_bounds.find(e2.item);
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
                        e2.trait.m_path, e2.trait.m_params,
                        *be.trait.m_trait_ptr, be.trait.m_path.m_path, be.trait.m_path.m_params, *e2.type,
                        [&e2,&input,&assume_opaque](const auto& x, const auto& assoc){
                            auto it = assoc.find(e2.item);
                            if( it != assoc.end() ) {
                                assume_opaque = false;
                                DEBUG("Found associated type " << input << " = " << it->second);
                                input = it->second.clone();
                            }
                            return true;
                        }
                        );
                    if( found_supertrait ) {
                        auto it = be.trait.m_type_bounds.find(e2.item);
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
                }
                input = this->expand_associated_types(sp, mv$(input));
                return input;
            }

            // If the type of this UfcsKnown is ALSO a UfcsKnown - Check if it's bounded by this trait with equality
            // Use bounds on other associated types too (if `e2.type` was resolved to a fixed associated type)
            TU_IFLET(::HIR::TypeRef::Data, e2.type->m_data, Path, te_inner,
                TU_IFLET(::HIR::Path::Data, te_inner.path.m_data, UfcsKnown, pe_inner,
                    // TODO: Search for equality bounds on this associated type (e3) that match the entire type (e2)
                    // - Does simplification of complex associated types
                    const auto& trait_ptr = this->m_crate.get_trait_by_path(sp, pe_inner.trait.m_path);
                    const auto& assoc_ty = trait_ptr.m_types.at(pe_inner.item);
                    DEBUG("TODO: Search bounds on associated type - " << assoc_ty.m_params.fmt_bounds());
                    
                    // Resolve where Self=e2.type, for the associated type check.
                    auto cb_placeholders_type = [&](const auto& ty)->const auto&{
                        TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Generic, e,
                            if( e.binding == 0xFFFF )
                                return *e2.type;
                            else
                                TODO(sp, "Handle type params when expanding associated bound (#" << e.binding << " " << e.name);
                        )
                        else {
                            return ty;
                        }
                        };
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
                    for(const auto& bound : assoc_ty.m_params.m_bounds)
                    {
                        TU_MATCH_DEF(::HIR::GenericBound, (bound), (be),
                        (
                            ),
                        (TraitBound,
                            // If the bound is for Self and the outer trait
                            // - TODO: Parameters?
                            if( be.type == ::HIR::TypeRef("Self", 0xFFFF) && be.trait.m_path == e2.trait ) {
                                auto it = be.trait.m_type_bounds.find( e2.item );
                                if( it != be.trait.m_type_bounds.end() ) {
                                    if( monomorphise_type_needed(it->second) ) {
                                        return monomorphise_type_with(sp, it->second, cb_placeholders_trait);
                                    }
                                    else {
                                        return it->second.clone();
                                    }
                                }
                            }
                            ),
                        (TypeEquality,
                            // IF: bound's type matches the input, replace with bounded equality
                            // `<Self::IntoIter as Iterator>::Item = Self::Item`
                            if( be.type.compare_with_placeholders(sp, input, cb_placeholders_type ) ) {
                                DEBUG("Match of " << be.type << " with " << input);
                                DEBUG("- Replace `input` with " << be.other_type << ", Self=" << *pe_inner.type);
                                if( monomorphise_type_needed(be.other_type) ) {
                                    return monomorphise_type_with(sp, be.other_type, cb_placeholders_trait);
                                }
                                else {
                                    return be.other_type.clone();
                                }
                            }
                            )
                        )
                    }
                    DEBUG("e2 = " << *e2.type << ", input = " << input);
                )
            )

            // 2. Crate-level impls
            // TODO: Search for the actual trait containing this associated type
            ::HIR::GenericPath  trait_path;
            if( !this->trait_contains_type(sp, e2.trait, this->m_crate.get_trait_by_path(sp, e2.trait.m_path), e2.item, trait_path) )
                BUG(sp, "Cannot find associated type " << e2.item << " anywhere in trait " << e2.trait);
            //e2.trait = mv$(trait_path);
            
            rv = this->find_trait_impls_crate(sp, trait_path.m_path, trait_path.m_params, *e2.type, [&](const auto& args, const auto& assoc) {
                DEBUG("Found impl for " << e2.trait.m_path << args << " with types {" << assoc << "}");
                auto it = assoc.find( e2.item );
                if( it == assoc.end() )
                    ERROR(sp, E0000, "Couldn't find assocated type " << e2.item << " in " << e2.trait);
                
                DEBUG("Converted UfcsKnown - " << e.path << " = " << it->second);
                input = it->second.clone();
                return true;
                });
            if( rv ) {
                input = this->expand_associated_types(sp, mv$(input));
                return input;
            }
            
            // TODO: If there are no ivars in this path, set its binding to Opaque
            if( !this->type_contains_ivars(input) ) {
                e.binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
            }
            
            DEBUG("Couldn't resolve associated type for " << input);
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


// -------------------------------------------------------------------------------------------------------------------
//
// -------------------------------------------------------------------------------------------------------------------
bool typeck::TypecheckContext::find_named_trait_in_trait(const Span& sp,
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
            callback( pt_mono.m_path.m_params, pt_mono.m_type_bounds );
            return true;
        }
    }
    return false;
}
bool typeck::TypecheckContext::find_trait_impls_bound(const Span& sp, const ::HIR::SimplePath& trait, const ::HIR::PathParams& params, const ::HIR::TypeRef& type,  t_cb_trait_impl callback) const
{
    return this->iterate_bounds([&](const auto& b) {
        TU_IFLET(::HIR::GenericBound, b, TraitBound, e,
            // TODO: Allow fuzzy equality?
            if( e.type != type )
                return false;
            
            if( e.trait.m_path.m_path == trait ) {
                const auto& b_params = e.trait.m_path.m_params;
                if( params.m_types.size() != b_params.m_types.size() ) {
                    // Bug?
                    BUG(sp, "Paremter count mismatch");
                }
                DEBUG("Checking " << params << " vs " << b_params);
                bool is_fuzzy = false;
                // Check against `params`
                for(unsigned int i = 0; i < params.m_types.size(); i ++) {
                    auto ord = b_params.m_types[i].compare_with_placeholders(sp, params.m_types[i], this->callback_resolve_infer());
                    if( ord == ::HIR::Compare::Unequal )
                        return false;
                    if( ord == ::HIR::Compare::Fuzzy )
                        is_fuzzy = true;
                }
                if( is_fuzzy ) {
                    DEBUG("Fuzzy match");
                }
                // Hand off to the closure, and return true if it does
                if( callback(e.trait.m_path.m_params, e.trait.m_type_bounds) ) {
                    return true;
                }
            }
            if( this->find_named_trait_in_trait(sp, trait,params,  *e.trait.m_trait_ptr, e.trait.m_path.m_path, e.trait.m_path.m_params, type,  callback) ) {
                return true;
            }
        )
        return false;
    });
}
bool typeck::TypecheckContext::find_trait_impls_crate(const Span& sp,
        const ::HIR::SimplePath& trait, const ::HIR::PathParams& params,
        const ::HIR::TypeRef& type,
        t_cb_trait_impl callback
        ) const
{
    return this->m_crate.find_trait_impls(trait, type, [&](const auto& ty)->const auto&{
            if( ty.m_data.is_Infer() ) 
                return this->get_type(ty);
            else
                return ty;
        },
        [&](const auto& impl) {
            DEBUG("[find_trait_impls_crate] Found impl" << impl.m_params.fmt_args() << " " << trait << impl.m_trait_args << " for " << impl.m_type);
            // Compare with `params`
            // - TODO: Would prefer a fuzzy compare, but match is easiest
            bool    fail = false;
            ::std::vector< const ::HIR::TypeRef*> impl_params;
            impl_params.resize( impl.m_params.m_types.size() );
            auto cb = [&](auto idx, const auto& ty) {
                DEBUG("[find_trait_impls_crate] " << idx << " = " << ty);
                assert( idx < impl_params.size() );
                if( ! impl_params[idx] ) {
                    impl_params[idx] = &ty;
                }
                else if( *impl_params[idx] != ty ) {
                    fail = true;
                }
                else {
                }
                };
            assert( impl.m_trait_args.m_types.size() == params.m_types.size() );
            fail |= !impl.m_type.match_test_generics(sp, type , this->callback_resolve_infer(), cb);
            for(unsigned int i = 0; i < impl.m_trait_args.m_types.size(); i ++)
                fail |= !impl.m_trait_args.m_types[i].match_test_generics(sp, params.m_types[i], this->callback_resolve_infer(), cb);
            if( fail ) {
                DEBUG("- Failed to match");
                return false;
            }
            for(const auto& ty : impl_params)
                assert( ty );
            
            auto monomorph = [&](const auto& gt)->const auto& {
                    const auto& ge = gt.m_data.as_Generic();
                    assert( ge.binding < impl_params.size() );
                    return *impl_params[ge.binding];
                    };
            auto args_mono = monomorphise_path_params_with(sp, impl.m_trait_args, monomorph, false);
            
            // TODO: Check bounds
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
                    for(auto& ab : real_trait.m_type_bounds) {
                        ab.second = this->expand_associated_types(sp, mv$(ab.second));
                    }
                    DEBUG("- " << real_type << " : " << real_trait);
                    auto rv = this->find_trait_impls(sp, real_trait.m_path.m_path, real_trait.m_path.m_params, real_type, [&](const auto& a, const auto& t) {
                        for(const auto& assoc_bound : real_trait.m_type_bounds) {
                            auto it = t.find(assoc_bound.first);
                            if( it == t.end() )
                            {
                                auto ty = ::HIR::TypeRef(::HIR::Path(::HIR::Path::Data::Data_UfcsKnown { box$(real_type.clone()), real_trait.m_path.clone(), assoc_bound.first, {} }));
                                auto ty2 = this->expand_associated_types(sp, mv$(ty));
                                
                                if( ty2 == assoc_bound.second ) {
                                    return true;
                                }
                                this->dump();
                                TODO(sp, "Check type bound (fuzz) " << ty2 << " = " << assoc_bound.second);
                            }
                            else {
                                if( this->get_type(it->second) == assoc_bound.second ) {
                                    return true;
                                }
                                this->dump();
                                TODO(sp, "Check type bound (fuzz) " << it->second << " = " << assoc_bound.second);
                            }
                        }
                        return true;
                        });
                    if( !rv ) {
                        // false = keep going
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
            
            DEBUG("[find_trait_impls_crate] callback(args=" << args_mono << ", assoc={" << types << "})");
            return callback(args_mono, types);
        }
        );
}

bool typeck::TypecheckContext::trait_contains_method(const Span& sp, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait_ptr, const ::std::string& name,  ::HIR::GenericPath& out_path) const
{
    auto it = trait_ptr.m_values.find(name);
    if( it != trait_ptr.m_values.end() ) {
        if( it->second.is_Function() ) {
            out_path = trait_path.clone();
            return true;
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
bool typeck::TypecheckContext::trait_contains_type(const Span& sp, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait_ptr, const ::std::string& name,  ::HIR::GenericPath& out_path) const
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
const ::HIR::TypeRef* typeck::TypecheckContext::autoderef(const Span& sp, const ::HIR::TypeRef& ty,  ::HIR::TypeRef& tmp_type) const
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
        bool succ = this->find_trait_impls(sp, this->m_crate.get_lang_item_path(sp, "deref"), ::HIR::PathParams {}, ty, [&](const auto& args, const auto& types) {
            assert(args.m_types.size() == 0);
            // TODO: Use `types`
            return true;
            });
        if( succ ) {
            TODO(sp, "Found a Deref impl for " << ty << ", use the output of it");
        }
        else {
            return nullptr;
        }
    }
}
unsigned int typeck::TypecheckContext::autoderef_find_method(const Span& sp, const ::HIR::TypeRef& top_ty, const ::std::string& method_name,  /* Out -> */::HIR::Path& fcn_path) const
{
    unsigned int deref_count = 0;
    ::HIR::TypeRef  tmp_type;   // Temporary type used for handling Deref
    const auto* current_ty = &top_ty;
    TU_IFLET(::HIR::TypeRef::Data, this->get_type(top_ty).m_data, Borrow, e,
        current_ty = &*e.inner;
        deref_count += 1;
    )
    
    do {
        const auto& ty = this->get_type(*current_ty);
        if( ty.m_data.is_Infer() ) {
            return ~0u;
        }
        
        if( find_method(sp, ty, method_name, fcn_path) ) {
            return deref_count;
        }
        
        // 3. Dereference and try again
        deref_count += 1;
        current_ty = this->autoderef(sp, ty,  tmp_type);
    } while( current_ty );
    
    TU_IFLET(::HIR::TypeRef::Data, this->get_type(top_ty).m_data, Borrow, e,
        const auto& ty = this->get_type(top_ty);
        
        if( find_method(sp, ty, method_name, fcn_path) ) {
            return 0;
        }
    )
    
    // Dereference failed! This is a hard error (hitting _ is checked above and returns ~0)
    this->dump();
    TODO(sp, "Error when no method could be found, but type is known - (: " << top_ty << ")." << method_name);
}

bool typeck::TypecheckContext::find_method(const Span& sp, const ::HIR::TypeRef& ty, const ::std::string& method_name,  /* Out -> */::HIR::Path& fcn_path) const
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
                fcn_path = ::HIR::Path( ::HIR::Path::Data::Data_UfcsKnown({
                    box$( ty.clone() ),
                    e.m_trait.m_path.clone(),
                    method_name,
                    {}
                    }) );
                return true;
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
        for(const auto& bound : assoc_ty.m_params.m_bounds )
        {
            TU_IFLET(::HIR::GenericBound, bound, TraitBound, be,
                assert(be.trait.m_trait_ptr);
                ::HIR::GenericPath final_trait_path;
                if( !this->trait_contains_method(sp, be.trait.m_path, *be.trait.m_trait_ptr, method_name,  final_trait_path) )
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
    else {
        // 2. Search for inherent methods
        for(const auto& impl : m_crate.m_type_impls)
        {
            if( impl.matches_type(ty) ) {
                auto it = impl.m_methods.find( method_name );
                if( it == impl.m_methods.end() )
                    continue ;
                DEBUG("Matching `impl" << impl.m_params.fmt_args() << " " << impl.m_type << "`"/* << " - " << top_ty*/);
                fcn_path = ::HIR::Path( ::HIR::Path::Data::make_UfcsInherent({
                    box$(ty.clone()),
                    method_name,
                    {}
                    }) );
                return true;
            }
        }
        // 3. Search for trait methods (using currently in-scope traits)
        for(const auto& trait_ref : ::reverse(m_traits))
        {
            if( trait_ref.first == nullptr )
                break;
            
            // TODO: Search supertraits too
            auto it = trait_ref.second->m_values.find(method_name);
            if( it == trait_ref.second->m_values.end() )
                continue ;
            if( !it->second.is_Function() )
                continue ;
            DEBUG("Search for impl of " << *trait_ref.first);
            // TODO: Need a "don't care" marker for the PathParams
            if( find_trait_impls_crate(sp, *trait_ref.first, ::HIR::PathParams{}, ty,  [](const auto&,const auto&) { return true; }) ) {
                DEBUG("Found trait impl " << *trait_ref.first << " for " << ty);
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
    
    return false;
}

unsigned int typeck::TypecheckContext::autoderef_find_field(const Span& sp, const ::HIR::TypeRef& top_ty, const ::std::string& field_name,  /* Out -> */::HIR::TypeRef& field_type) const
{
    unsigned int deref_count = 0;
    ::HIR::TypeRef  tmp_type;   // Temporary type used for handling Deref
    const auto* current_ty = &top_ty;
    TU_IFLET(::HIR::TypeRef::Data, this->get_type(top_ty).m_data, Borrow, e,
        current_ty = &*e.inner;
        deref_count += 1;
    )
    
    do {
        const auto& ty = this->get_type(*current_ty);
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
    
    TU_IFLET(::HIR::TypeRef::Data, this->get_type(top_ty).m_data, Borrow, e,
        const auto& ty = this->get_type(top_ty);
        
        if( find_field(sp, ty, field_name, field_type) ) {
            return 0;
        }
    )
    
    // Dereference failed! This is a hard error (hitting _ is checked above and returns ~0)
    this->dump();
    TODO(sp, "Error when no field could be found, but type is known - (: " << top_ty << ")." << field_name);
}
bool typeck::TypecheckContext::find_field(const Span& sp, const ::HIR::TypeRef& ty, const ::std::string& name,  /* Out -> */::HIR::TypeRef& field_ty) const
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
            TU_MATCH(::HIR::Struct::Data, (str.m_data), (se),
            (Unit,
                // No fields on a unit struct
                ),
            (Tuple,
                for( unsigned int i = 0; i < se.size(); i ++ )
                {
                    // TODO: Privacy
                    if( FMT(i) == name ) {
                        field_ty = monomorphise_type_with(sp, se[i].ent, [&](const auto& gt)->const auto&{ TODO(sp, "Monomorphise tuple struct field types"); return gt; });
                        return true;
                    }
                }
                ),
            (Named,
                for( const auto& fld : se )
                {
                    // TODO: Privacy
                    if( fld.first == name ) {
                        field_ty = monomorphise_type_with(sp, fld.second.ent, [&](const auto& gt)->const auto&{ TODO(sp, "Monomorphise named struct field types"); return gt; });
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


// -------------------------------------------------------------------------------------------------------------------
//
// -------------------------------------------------------------------------------------------------------------------
void typeck::TypecheckContext::set_ivar_to(unsigned int slot, ::HIR::TypeRef type)
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

void typeck::TypecheckContext::ivar_unify(unsigned int left_slot, unsigned int right_slot)
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
                    check_type_class_primitive(sp, *left_ivar.type, re.ty_class, le);
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
typeck::TypecheckContext::IVar& typeck::TypecheckContext::get_pointed_ivar(unsigned int slot) const
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
