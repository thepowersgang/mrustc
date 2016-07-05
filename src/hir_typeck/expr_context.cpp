/*
 */
#include "expr_simple.hpp"
#include <hir/hir.hpp>
#include <algorithm>    // std::find_if
#include "helpers.hpp"

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
    m_ivars.dump();
    DEBUG("TypecheckContext - " << m_locals.size() << " locals");
    unsigned int i = 0;
    for(const auto& v : m_locals) {
        DEBUG("VAR " << i << " '"<<v.name<<"' = " << v.type);
        i ++;
    }
}
void typeck::TypecheckContext::compact_ivars()
{
    TRACE_FUNCTION;
    
    this->m_resolve.compact_ivars(this->m_ivars);
    
    for(auto& v : m_locals) {
        v.type = this->get_type(v.type).clone();
    }
}
bool typeck::TypecheckContext::apply_defaults()
{
    return m_ivars.apply_defaults();
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
            ERROR(sp, E0000, "Type mismatch in struct pattern - " << type << " is not " << e.path);
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
            ERROR(sp, E0000, "Type mismatch in struct pattern - " << type << " is not " << e.path);
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
            ERROR(sp, E0000, "Type mismatch in enum pattern - " << type << " is not " << e.path);
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
                if( monomorphise_type_needed(tup_var[i].ent) ) {
                    auto var_ty = monomorphise_type(sp, enm.m_params, gp.m_params,  tup_var[i].ent);
                    this->add_binding(sp, e.sub_patterns[i], var_ty);
                }
                else {
                    // SAFE: Can't have a _ (monomorphise_type_needed checks for that)
                    this->add_binding(sp, e.sub_patterns[i], const_cast< ::HIR::TypeRef&>(tup_var[i].ent));
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
            ERROR(sp, E0000, "Type mismatch in enum pattern - " << type << " is not " << e.path);
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
            ERROR(sp, E0000, "Type mismatch in enum pattern - " << type << " is not " << e.path);
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
                const ::HIR::TypeRef& field_type = tup_var[f_idx].second.ent;
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
    
    if( /*l_t.m_data.is_Diverge() ||*/ r_t.m_data.is_Diverge() ) {
        DEBUG("Refusing to unify with !");
        return ;
    }
    if( l_t.m_data.is_Diverge() ) {
        // TODO: Error if the right type isn't infer
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
            
            DEBUG("CoerceUnsized - " << this->m_ivars.type_contains_ivars(l_t) << this->m_ivars.type_contains_ivars(r_t) << this->m_ivars.types_equal(l_t, r_t));
            if( !this->m_ivars.type_contains_ivars(l_t) && !this->m_ivars.type_contains_ivars(r_t) && !this->m_ivars.types_equal(l_t, r_t) )
            {
                // TODO: If the types are fully known, and not equal. Search for CoerceUnsized
                ::HIR::PathParams   pp;
                pp.m_types.push_back( l_t.clone() );
                bool succ = this->find_trait_impls(sp, this->m_crate.get_lang_item_path(sp, "coerce_unsized"), pp, r_t, [&](const auto& args, const auto& ) {
                    DEBUG("- Found coerce_unsized from " << l_t << " to " << r_t);
                    return true;
                    });
                if( succ )
                {
                    // TODO: 
                    TODO(sp, "Apply CoerceUnsized - " << l_t << " <- " << r_t);
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
                            return context.m_ivars.type_contains_ivars(ty);
                        )
                        return false;
                    }
                };

                if( H::type_is_free_ufcs(*this, l_t) || H::type_is_free_ufcs(*this, r_t) ) {
                    return ;
                }
                
                // Deref coercions 1 (when a & op is destructured and expected value is the deref)
                if( node_ptr_ptr )
                {
                    auto& node_ptr = *node_ptr_ptr;
                    
                    // - If right has a deref chain to left, build it
                    DEBUG("Trying deref coercion " << l_t << " " << r_t);
                    if( !l_t.m_data.is_Borrow() && ! this->m_ivars.type_contains_ivars(l_t) && ! this->m_ivars.type_contains_ivars(r_t) )
                    {
                        DEBUG("Trying deref coercion (2)");
                        ::HIR::TypeRef  tmp_ty;
                        const ::HIR::TypeRef*   out_ty = &r_t;
                        unsigned int count = 0;
                        while( (out_ty = this->m_resolve.autoderef(sp, *out_ty, tmp_ty)) )
                        {
                            count += 1;
                            
                            if( l_t != *out_ty ) {
                                TODO(sp, "Deref coercions " << l_t << " <- " << r_t << " (became " << *out_ty << ")");
                            }
                            
                            while(count --)
                            {
                                auto span = node_ptr->span();
                                node_ptr->m_res_type = this->new_ivar_tr();
                                node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Deref( mv$(span), mv$(node_ptr) ));
                            }
                            node_ptr->m_res_type = l_t.clone();
                            
                            this->mark_change();
                            return ;
                            //auto cmp = this->compare_types(left_inner_res, *out_ty);
                            //if( cmp == ::HIR::Compare::Equal ) {
                            //    
                            //}
                        }
                    }
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
                            if( left_inner_res != right_inner_res ) {
                                const auto& lie = left_inner_res .m_data.as_TraitObject();
                                const auto& rie = right_inner_res.m_data.as_TraitObject();
                                // 1. Check/equate the main trait (NOTE: Eventualy this may be a set of data traits)
                                if( lie.m_trait.m_path.m_path != rie.m_trait.m_path.m_path ) {
                                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " (trait mismatch)");
                                }
                                equality_typeparams(lie.m_trait.m_path.m_params, rie.m_trait.m_path.m_params);
                                
                                // 2. Ensure that the LHS's marker traits are a strict subset of the RHS
                                // NOTE: This assumes sorting - will false fail if ordering differs
                                unsigned int r_i = 0;
                                for(unsigned int l_i = 0; l_i < lie.m_markers.size(); l_i ++)
                                {
                                    while( r_i < rie.m_markers.size() && rie.m_markers[r_i].m_path != lie.m_markers[l_i].m_path ) {
                                        r_i += 1;
                                    }
                                    if( r_i == rie.m_markers.size() ) {
                                        ERROR(sp, E0000, "Can't coerce between trait objects - " << left_inner_res << " and " << right_inner_res << " (added marker)");
                                    }
                                }
                                // Coercion is possible and valid.
                                // HACK: Uses _Unsize as a coerce
                                auto span = node_ptr->span();
                                node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Unsize( mv$(span), mv$(node_ptr), l_t.clone() ));
                                node_ptr->m_res_type = l_t.clone();
                                
                                this->mark_change();
                                return ;
                            }
                            // - Equal, nothing to do
                            return ;
                        }
                        
                        // 1. Search for an implementation of the data trait for this type
                        auto r = this->expand_associated_types(sp, right_inner_res.clone());
                        //bool succ = this->find_trait_impls(sp, e.m_trait.m_path.m_path, e.m_trait.m_path.m_params,  right_inner_res, [&](const auto& args,const auto& types) {
                        bool succ = this->find_trait_impls(sp, e.m_trait.m_path.m_path, e.m_trait.m_path.m_params,  r, [&](const auto& args,const auto& types) {
                            if( args.m_types.size() > 0 )
                                TODO(sp, "Handle unsizing to traits with params");
                            // TODO: Check `types`
                            return true;
                            });
                        if(!succ) {
                            // XXX: Debugging - Resolves to the correct type in a failing case
                            //auto ty2 = this->expand_associated_types(sp, this->expand_associated_types(sp, right_inner_res.clone()) );
                            ERROR(sp, E0000, "Trait " << e.m_trait << " isn't implemented for " << m_ivars.fmt_type(right_inner_res) << " (converting to TraitObject) - (r="<<r<<")" );
                        }
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
                    node_ptr->m_res_type = r_t.clone();
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

