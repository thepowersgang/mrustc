/*
 */
#include "main_bindings.hpp"
#include <hir/expr.hpp>
#include <hir/hir.hpp>
#include <hir/visitor.hpp>
#include <algorithm>    // std::find_if

namespace {
    
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
            TODO(Span(), "TraitObject - " << tpl);
            ),
        (Array,
            TODO(Span(), "Array - " << tpl);
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
            TODO(Span(), "Function - " << tpl);
            )
        )
        throw "";
    }
    typedef ::std::function<const ::HIR::TypeRef&(const ::HIR::TypeRef&)>   t_cb_generic;
    ::HIR::TypeRef monomorphise_type_with(const Span& sp, const ::HIR::TypeRef& tpl, t_cb_generic callback, bool allow_infer=true);
    
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
    ::HIR::TypeRef monomorphise_type_with(const Span& sp, const ::HIR::TypeRef& tpl, t_cb_generic callback, bool allow_infer)
    {
        TRACE_FUNCTION_F("tpl = " << tpl);
        TU_MATCH(::HIR::TypeRef::Data, (tpl.m_data), (e),
        (Infer,
            if( allow_infer ) {
                return ::HIR::TypeRef(e);
            }
            else {
               BUG(sp, "_ type found in monomorphisation target");
            }
            ),
        (Diverge,
            return ::HIR::TypeRef(e);
            ),
        (Primitive,
            return ::HIR::TypeRef(e);
            ),
        (Path,
            TU_MATCH(::HIR::Path::Data, (e.path.m_data), (e2),
            (Generic,
                return ::HIR::TypeRef( monomorphise_genericpath_with(sp, e2, callback, allow_infer) );
                ),
            (UfcsKnown,
                return ::HIR::TypeRef( ::HIR::Path::Data::make_UfcsKnown({
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
            return callback(tpl).clone();
            ),
        (TraitObject,
            TODO(sp, "TraitObject");
            ),
        (Array,
            if( e.size_val == ~0u ) {
                BUG(sp, "Attempting to clone array with unknown size - " << tpl);
            }
            return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Array({
                box$( monomorphise_type_with(sp, *e.inner, callback) ),
                ::HIR::ExprPtr(),
                e.size_val
                }) );
            ),
        (Slice,
            return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Slice({ box$(monomorphise_type_with(sp, *e.inner, callback)) }) );
            ),
        (Tuple,
            ::std::vector< ::HIR::TypeRef>  types;
            for(const auto& ty : e) {
                types.push_back( monomorphise_type_with(sp, ty, callback) );
            }
            return ::HIR::TypeRef( mv$(types) );
            ),
        (Borrow,
            return ::HIR::TypeRef::new_borrow(e.type, monomorphise_type_with(sp, *e.inner, callback));
            ),
        (Pointer,
            return ::HIR::TypeRef::new_pointer(e.type, monomorphise_type_with(sp, *e.inner, callback));
            ),
        (Function,
            TODO(sp, "Function");
            )
        )
        throw "";
        
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
    
    
    struct IVar
    {
        unsigned int alias; // If not ~0, this points to another ivar
        ::std::unique_ptr< ::HIR::TypeRef> type;    // Type (only nullptr if alias!=0)
        
        IVar():
            alias(~0u),
            type(new ::HIR::TypeRef())
        {}
        bool is_alias() const { return alias != ~0u; }
    };
    static const ::std::string EMPTY_STRING;
    struct Variable
    {
        ::std::string   name;
        ::HIR::TypeRef  type;
        
        Variable()
        {}
        Variable(const ::std::string& name, ::HIR::TypeRef type):
            name( name ),
            type( mv$(type) )
        {}
        Variable(Variable&&) = default;
        
        Variable& operator=(Variable&&) = default;
    };
    
    class TypecheckContext
    {
    public:
        const ::HIR::Crate& m_crate;
        ::std::vector< ::std::pair< const ::HIR::SimplePath*, const ::HIR::Trait* > >   m_traits;
    private:
        ::std::vector< Variable>    m_locals;
        ::std::vector< IVar>    m_ivars;
        bool    m_has_changed;
        
        const ::HIR::GenericParams* m_impl_params;
        const ::HIR::GenericParams* m_item_params;
        
    public:
        TypecheckContext(const ::HIR::Crate& crate, const ::HIR::GenericParams* impl_params, const ::HIR::GenericParams* item_params):
            m_crate(crate),
            m_has_changed(false),
            m_impl_params( impl_params ),
            m_item_params( item_params )
        {
        }
        
        void dump() const {
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
        
        bool take_changed() {
            bool rv = m_has_changed;
            m_has_changed = false;
            return rv;
        }
        void mark_change() {
            DEBUG("- CHANGE");
            m_has_changed = true;
        }
        
        /// Adds a local variable binding (type is mutable so it can be inferred if required)
        void add_local(unsigned int index, const ::std::string& name, ::HIR::TypeRef type)
        {
            if( m_locals.size() <= index )
                m_locals.resize(index+1);
            m_locals[index] = Variable(name, mv$(type));
        }

        const ::HIR::TypeRef& get_var_type(const Span& sp, unsigned int index)
        {
            if( index >= m_locals.size() ) {
                this->dump();
                BUG(sp, "Local index out of range " << index << " >= " << m_locals.size());
            }
            return m_locals.at(index).type;
        }

        /// Add (and bind) all '_' types in `type`
        void add_ivars(::HIR::TypeRef& type)
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
                )
            )
        }
        void add_ivars_params(::HIR::PathParams& params)
        {
            for(auto& arg : params.m_types)
                add_ivars(arg);
        }
        
        
        void add_pattern_binding(const ::HIR::PatternBinding& pb, ::HIR::TypeRef type)
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
        
        void add_binding(const Span& sp, ::HIR::Pattern& pat, ::HIR::TypeRef& type)
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
                    type.m_data = ::HIR::TypeRef::Data::make_Borrow({ e.type, box$(this->new_ivar_tr()) });
                }
                // Type must be a &-ptr
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw "";),
                (Borrow,
                    if( te.type != e.type ) {
                        // TODO: Type mismatch
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
                    type.m_data = ::HIR::TypeRef::Data::make_Tuple( mv$(sub_types) );
                }
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw ""; ),
                (Tuple,
                    if( te.size() != e.sub_patterns.size() ) {
                        // TODO: Type mismatch
                    }
                    for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                        this->add_binding(sp, e.sub_patterns[i], te[i] );
                    )
                )
                ),
            (Slice,
                if( type.m_data.is_Infer() ) {
                    type.m_data = ::HIR::TypeRef::Data::make_Slice( {box$(this->new_ivar_tr())} );
                    this->mark_change();
                }
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
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
                    type.m_data = ::HIR::TypeRef::Data::make_Slice( {box$(this->new_ivar_tr())} );
                    this->mark_change();
                }
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
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
                    type.m_data = ::HIR::TypeRef::Data::make_Path( {e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)} );
                }
                assert(e.binding);
                const auto& str = *e.binding;
                // - assert check from earlier pass
                assert( str.m_data.is_Tuple() );
                const auto& sd = str.m_data.as_Tuple();
                
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
                    type.m_data = ::HIR::TypeRef::Data::make_Path( {e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)} );
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
                    type.m_data = ::HIR::TypeRef::Data::make_Path( {e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)} );
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
                    type.m_data = ::HIR::TypeRef::Data::make_Path( {mv$(path), ::HIR::TypeRef::TypePathBinding(e.binding_ptr)} );
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
                    type.m_data = ::HIR::TypeRef::Data::make_Path( {mv$(path), ::HIR::TypeRef::TypePathBinding(e.binding_ptr)} );
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
                    type.m_data = ::HIR::TypeRef::Data::make_Path( {mv$(path), ::HIR::TypeRef::TypePathBinding(e.binding_ptr)} );
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
        
        /// Run inferrence using a pattern
        void apply_pattern(const ::HIR::Pattern& pat, ::HIR::TypeRef& type)
        {
            static Span _sp;
            const Span& sp = _sp;
            // TODO: Should this do an equality on the binding?

            auto& ty = this->get_type(type);
            
            TU_MATCH(::HIR::Pattern::Data, (pat.m_data), (e),
            (Any,
                // Just leave it, the pattern says nothing about the type
                ),
            (Value,
                TODO(sp, "Value pattern");
                ),
            (Range,
                TODO(sp, "Range pattern");
                ),
            // - Pointer destructuring
            (Box,
                // Type must be box-able
                TODO(sp, "Box patterns");
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
        // Adds a rule that two types must be equal
        // - NOTE: The ordering does matter, as the righthand side will get unsizing/deref coercions applied if possible
        /// \param sp   Span for reporting errors
        /// \param left     Lefthand type (destination for coercions)
        /// \param right    Righthand type (source for coercions)
        /// \param node_ptr Pointer to ExprNodeP, updated with new nodes for coercions
        void apply_equality(const Span& sp, const ::HIR::TypeRef& left, const ::HIR::TypeRef& right, ::HIR::ExprNodeP* node_ptr_ptr = nullptr)
        {
            apply_equality(sp, left, [](const auto& x)->const auto&{return x;}, right, [](const auto& x)->const auto&{return x;}, node_ptr_ptr);
        }
        
        const ::HIR::TypeRef& expand_associated_types_to(const Span& sp, const ::HIR::TypeRef& t, ::HIR::TypeRef& tmp_t) const {
            TU_IFLET(::HIR::TypeRef::Data, t.m_data, Path, e,
                if( e.path.m_data.is_Generic() )
                    return t;
                else {
                    tmp_t = this->expand_associated_types(sp, t.clone());
                    DEBUG("Expanded " << t << " into " << tmp_t);
                    return tmp_t;
                }
            )
            else {
                return t;
            }
        }
        void apply_equality(const Span& sp, const ::HIR::TypeRef& left, t_cb_generic cb_left, const ::HIR::TypeRef& right, t_cb_generic cb_right, ::HIR::ExprNodeP* node_ptr_ptr)
        {
            TRACE_FUNCTION_F(left << ", " << right);
            assert( ! left.m_data.is_Infer() ||  left.m_data.as_Infer().index != ~0u );
            assert( !right.m_data.is_Infer() || right.m_data.as_Infer().index != ~0u );
            // - Convert left/right types into resolved versions (either root ivar, or generic replacement)
            const auto& l_t1 = left.m_data.is_Generic()  ? cb_left (left ) : this->get_type(left );
            const auto& r_t1 = right.m_data.is_Generic() ? cb_right(right) : this->get_type(right);
            if( l_t1 == r_t1 ) {
                return ;
            }
            // If generic replacement happened, clear the callback
            if( left.m_data.is_Generic() ) {
                cb_left = [](const auto& x)->const auto&{return x;};
            }
            if( right.m_data.is_Generic() ) {
                cb_right = [](const auto& x)->const auto&{return x;};
            }
            
            ::HIR::TypeRef  left_tmp;
            const auto& l_t = this->expand_associated_types_to(sp, l_t1, left_tmp);
            ::HIR::TypeRef  right_tmp;
            const auto& r_t = this->expand_associated_types_to(sp, r_t1, right_tmp);
            
            DEBUG("- l_t = " << l_t << ", r_t = " << r_t);
            TU_IFLET(::HIR::TypeRef::Data, r_t.m_data, Infer, r_e,
                TU_IFLET(::HIR::TypeRef::Data, l_t.m_data, Infer, l_e,
                    // If both are infer, unify the two ivars (alias right to point to left)
                    this->ivar_unify(l_e.index, r_e.index);
                )
                else {
                    // Righthand side is infer, alias it to the left
                    //  TODO: that `true` should be `false` if the callback isn't unity (for bug checking)
                    this->set_ivar_to(r_e.index, monomorphise_type_with(sp, left, cb_left, true));
                }
            )
            else {
                TU_IFLET(::HIR::TypeRef::Data, l_t.m_data, Infer, l_e,
                    // Lefthand side is infer, alias it to the right
                    //  TODO: that `true` should be `false` if the callback isn't unity (for bug checking)
                    this->set_ivar_to(l_e.index, monomorphise_type_with(sp, right, cb_right, true));
                )
                else {
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
                        if( l_e.m_traits.size() != r_e.m_traits.size() ) {
                            // TODO: Possibly allow inferrence reducing the set?
                            ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - trait counts differ");
                        }
                        // NOTE: Lifetime is ignored
                        // TODO: Is this list sorted in any way? (if it's not sorted, this could fail when source does Send+Any instead of Any+Send)
                        for(unsigned int i = 0; i < l_e.m_traits.size(); i ++ )
                        {
                            auto& l_p = l_e.m_traits[i];
                            auto& r_p = r_e.m_traits[i];
                            if( l_p.m_path != r_p.m_path ) {
                                ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                            }
                            equality_typeparams(l_p.m_params, r_p.m_params);
                        }
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
                            bool succ = this->find_trait_impls(this->m_crate.get_lang_item_path(sp, "unsize"), right_inner_res, [&](const auto& args) {
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
                        )
                    )
                }
            }
        }
        
        bool check_trait_bound(const Span& sp, const ::HIR::TypeRef& type, const ::HIR::GenericPath& trait, ::std::function<const ::HIR::TypeRef&(const ::HIR::TypeRef&)> placeholder) const
        {
            if( this->find_trait_impls_bound(sp, trait.m_path, placeholder(type), [&](const auto& args){
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
        
        ///
        ///
        ///
        ::HIR::TypeRef expand_associated_types(const Span& sp, ::HIR::TypeRef input) const
        {
            TRACE_FUNCTION_F(input);
            TU_MATCH(::HIR::TypeRef::Data, (input.m_data), (e),
            (Infer,
                auto& ty = this->get_type(input);
                return ty.clone();
                ),
            (Diverge,
                ),
            (Primitive,
                ),
            (Path,
                // - Only try resolving if the binding isn't known
                if( !e.binding.is_Unbound() )
                    return input;
                
                TU_MATCH(::HIR::Path::Data, (e.path.m_data), (e2),
                (Generic,
                    for(auto& arg : e2.m_params.m_types)
                        arg = expand_associated_types(sp, mv$(arg));
                    ),
                (UfcsInherent,
                    TODO(sp, "Path - UfcsInherent - " << e.path);
                    ),
                (UfcsKnown,
                    DEBUG("Locating associated type for " << e.path);
                    // TODO: Use the marker `e.binding` to tell if it's worth trying
                    
                    *e2.type = expand_associated_types(sp, mv$(*e2.type));
                    
                    // Search for a matching trait impl
                    const ::HIR::TraitImpl* impl_ptr = nullptr;
                    ::std::vector< const ::HIR::TypeRef*>   impl_args;
                    
                    auto cb_get_infer = [&](const auto& ty)->const auto& {
                            if( ty.m_data.is_Infer() )
                                return this->get_type(ty);
                            else
                                return ty;
                        };
                    
                    // 1. Bounds
                    bool rv;
                    rv = this->iterate_bounds([&](const auto& b) {
                        TU_IFLET(::HIR::GenericBound, b, TypeEquality, be,
                            DEBUG("Equality - " << be.type << " = " << be.other_type);
                            if( input == be.type ) {
                                input = be.other_type.clone();
                                return true;
                            }
                        )
                        return false;
                        });
                    if( rv ) {
                        return input;
                    }

                    // Use bounds on other associated types too (if `e2.type` was resolved to a fixed associated type)
                    TU_IFLET(::HIR::TypeRef::Data, e2.type->m_data, Path, e3,
                        TU_IFLET(::HIR::Path::Data, e3.path.m_data, UfcsKnown, pe,
                            // TODO: Search for equality bounds on this associated type (e3) that match the entire type (e2)
                            // - Does simplification of complex associated types
                            const auto& trait_ptr = this->m_crate.get_trait_by_path(sp, pe.trait.m_path);
                            const auto& assoc_ty = trait_ptr.m_types.at(pe.item);
                            DEBUG("TODO: Search bounds " << assoc_ty.m_params.fmt_bounds());
                            auto cb_placeholders = [&](const auto& ty)->const auto&{
                                TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Generic, e,
                                    if( e.binding == 0xFFFF )
                                        return *e2.type;
                                    else
                                        TODO(sp, "Handle type pareters when expanding associated bound");
                                )
                                else {
                                    return ty;
                                }
                                };
                            for(const auto& bound : assoc_ty.m_params.m_bounds)
                            {
                                TU_IFLET(::HIR::GenericBound, bound, TypeEquality, be,
                                    // IF: bound's type matches the input, replace with bounded equality
                                    // `<Self::IntoIter as Iterator>::Item = Self::Item`
                                    if( be.type.compare_with_paceholders(sp, input, cb_placeholders ) ) {
                                        if( monomorphise_type_needed(be.other_type) ) {
                                            TODO(sp, "Monomorphise associated type replacment");
                                        }
                                        else {
                                            return be.other_type.clone();
                                        }
                                    }
                                )
                            }
                            DEBUG("e2 = " << *e2.type << ", input = " << input);
                        )
                    )

                    // 2. Crate-level impls
                    rv = this->m_crate.find_trait_impls(e2.trait.m_path, *e2.type, cb_get_infer,
                        [&](const auto& impl) {
                            DEBUG("Found impl" << impl.m_params.fmt_args() << " " << e2.trait.m_path << impl.m_trait_args << " for " << impl.m_type);
                            // - Populate the impl's type arguments
                            impl_args.clear();
                            impl_args.resize( impl.m_params.m_types.size() );
                            // - Match with `Self`
                            auto cb_res = [&](unsigned int slot, const ::HIR::TypeRef& ty) {
                                    DEBUG("- Set " << slot << " = " << ty);
                                    if( slot >= impl_args.size() ) {
                                        BUG(sp, "Impl parameter out of range - " << slot);
                                    }
                                    auto& slot_r = impl_args.at(slot);
                                    if( slot_r != nullptr ) {
                                        DEBUG("TODO: Match " << slot_r << " == " << ty << " when encountered twice");
                                    }
                                    else {
                                        slot_r = &ty;
                                    }
                                };
                            impl.m_type.match_generics(sp, *e2.type, cb_get_infer, cb_res);
                            for( unsigned int i = 0; i < impl.m_trait_args.m_types.size(); i ++ )
                            {
                                impl.m_trait_args.m_types[i].match_generics(sp, e2.trait.m_params.m_types.at(i), cb_get_infer, cb_res);
                            }
                            auto expand_placeholder = [&](const auto& ty)->const auto& {
                                    if( ty.m_data.is_Infer() )
                                        return this->get_type(ty);
                                    else TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Generic, e,
                                        if( e.binding == 0xFFFF ) {
                                            //TODO(sp, "Look up 'Self' in expand_associated_types::expand_placeholder (" << *e2.type << ")");
                                            return *e2.type;
                                        }
                                        else {
                                            assert(e.binding < impl_args.size());
                                            assert( impl_args[e.binding] );
                                            return *impl_args[e.binding];
                                        }
                                    )
                                    else
                                        return ty;
                                };
                            for( const auto& bound : impl.m_params.m_bounds )
                            {
                                TU_MATCH_DEF(::HIR::GenericBound, (bound), (be),
                                (
                                    ),
                                (TraitBound,
                                    if( !this->check_trait_bound(sp, be.type, be.trait.m_path, expand_placeholder) )
                                    {
                                        return false;
                                    }
                                    )
                                )
                            }
                            // TODO: Bounds check? (here or elsewhere?)
                            // - Need to check bounds before picking this impl, because the bound could be preventing false matches
                            if( impl.m_trait_args.m_types.size() > 0 )
                            {
                                TODO(sp, "Check trait type parameters in expand_associated_types");
                            }
                            impl_ptr = &impl;
                            return true;
                        });
                    if( rv )
                    {
                        // An impl was found:
                        assert(impl_ptr);
                        
                        // - Monomorphise the output type
                        auto new_type = monomorphise_type_with(sp, impl_ptr->m_types.at( e2.item ), [&](const auto& ty)->const auto& {
                            const auto& ge = ty.m_data.as_Generic();
                            assert(ge.binding < impl_args.size());
                            return *impl_args[ge.binding];
                            });
                        DEBUG("Converted UfcsKnown - " << e.path << " = " << new_type << " using " << e2.item << " = " << impl_ptr->m_types.at( e2.item ));
                        return new_type;
                    }
                    
                    // TODO: If there are no ivars in this path, set its binding to Opaque
                    
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
                )
            )
            return input;
        }
        
        bool iterate_bounds( ::std::function<bool(const ::HIR::GenericBound&)> cb) const
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
        
        /// Searches for a trait impl that matches the provided trait name and type
        bool find_trait_impls(const ::HIR::SimplePath& trait, const ::HIR::TypeRef& type,  ::std::function<bool(const ::HIR::PathParams&)> callback)
        {
            Span    sp = Span();
            TRACE_FUNCTION_F("trait = " << trait << ", type = " << type);
            // 1. Search generic params
            if( find_trait_impls_bound(sp, trait, type, callback) )
                return true;
            // 2. Search crate-level impls
            return find_trait_impls_crate(trait, type,  callback);
        }
        bool find_named_trait_in_trait(const Span& sp, const ::HIR::SimplePath& des, const ::HIR::Trait& trait_ptr, const ::HIR::PathParams& pp,  ::std::function<bool(const ::HIR::PathParams&)> callback) const
        {
            assert( pp.m_types.size() == trait_ptr.m_params.m_types.size() );
            for( const auto& pt : trait_ptr.m_parent_traits )
            {
                auto pt_pp = monomorphise_path_params_with(Span(), pt.m_params.clone(), [&](const auto& gt)->const auto& {
                    const auto& ge = gt.m_data.as_Generic();
                    if( ge.binding >= pp.m_types.size() )
                        BUG(sp, "find_named_trait_in_trait - Generic #" << ge.binding << " " << ge.name << " out of range");
                    return pp.m_types[ge.binding];
                    }, false);
                
                if( pt.m_path == des ) {
                    //TODO(Span(), "Fix arguments for a parent trait and call callback - " << pt << " with paramset " << trait_ptr.m_params.fmt_args() << " = " << pt_pp);
                    callback( pt_pp );
                    return true;
                }
            }
            return false;
        }
        bool find_trait_impls_bound(const Span& sp, const ::HIR::SimplePath& trait, const ::HIR::TypeRef& type,  ::std::function<bool(const ::HIR::PathParams&)> callback) const
        {
            return this->iterate_bounds([&](const auto& b) {
                TU_IFLET(::HIR::GenericBound, b, TraitBound, e,
                    if( e.type != type )
                        return false;
                    if( e.trait.m_path.m_path == trait ) {
                        if( callback(e.trait.m_path.m_params) ) {
                            return true;
                        }
                    }
                    if( this->find_named_trait_in_trait(sp, trait,  *e.trait.m_trait_ptr, e.trait.m_path.m_params,  callback) ) {
                        return true;
                    }
                )
                return false;
            });
        }
        bool find_trait_impls_crate(const ::HIR::SimplePath& trait, const ::HIR::TypeRef& type,  ::std::function<bool(const ::HIR::PathParams&)> callback) const
        {
            return this->m_crate.find_trait_impls(trait, type, [&](const auto& ty)->const auto&{
                    if( ty.m_data.is_Infer() ) 
                        return this->get_type(ty);
                    else
                        return ty;
                },
                [&](const auto& impl) {
                    DEBUG("[find_trait_impls_crate] Found impl" << impl.m_params.fmt_args() << " " << trait << impl.m_trait_args << " for " << impl.m_type);
                    return callback(impl.m_trait_args);
                }
                );
        }
        
        bool trait_contains_method(const Span& sp, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait_ptr, const ::std::string& name,  ::HIR::GenericPath& out_path) const
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
                auto& st_ptr = this->m_crate.get_trait_by_path(sp, st.m_path);
                if( trait_contains_method(sp, st, st_ptr, name, out_path) ) {
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
        
        /// Locate the named method by applying auto-dereferencing.
        /// \return Number of times deref was applied (or ~0 if _ was hit)
        unsigned int autoderef_find_method(const Span& sp, const ::HIR::TypeRef& top_ty, const ::std::string& method_name,  /* Out -> */::HIR::Path& fcn_path) const
        {
            unsigned int deref_count = 0;
            const auto* current_ty = &top_ty;
            do {
                const auto& ty = this->get_type(*current_ty);
                if( ty.m_data.is_Infer() ) {
                    return ~0u;
                }
                
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
                            return deref_count;
                        )
                    }
                }
                
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
                            return deref_count;
                        )
                    }
                }
                else {
                    // 2. Search for inherent methods
                    for(const auto& impl : m_crate.m_type_impls)
                    {
                        if( impl.matches_type(ty) ) {
                            DEBUG("Mactching impl " << impl.m_type);
                            fcn_path = ::HIR::Path( ::HIR::Path::Data::make_UfcsInherent({
                                box$(ty.clone()),
                                method_name,
                                {}
                                }) );
                            return deref_count;
                        }
                    }
                    // 3. Search for trait methods (using currently in-scope traits)
                    for(const auto& trait_ref : ::reverse(m_traits))
                    {
                        // TODO: Search supertraits too
                        auto it = trait_ref.second->m_values.find(method_name);
                        if( it == trait_ref.second->m_values.end() )
                            continue ;
                        if( !it->second.is_Function() )
                            continue ;
                        DEBUG("Search for impl of " << *trait_ref.first);
                        if( find_trait_impls_crate(*trait_ref.first, ty,  [](const auto&) { return true; }) ) {
                            DEBUG("Found trait impl " << *trait_ref.first << " for " << ty);
                            fcn_path = ::HIR::Path( ::HIR::Path::Data::make_UfcsKnown({
                                box$( ty.clone() ),
                                trait_ref.first->clone(),
                                method_name,
                                {}
                                }) );
                            return deref_count;
                        }
                    }
                }
                
                // 3. Dereference and try again
                deref_count += 1;
                TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Borrow, e,
                    current_ty = &*e.inner;
                )
                else {
                    // TODO: Search for a Deref impl
                    current_ty = nullptr;
                }
            } while( current_ty );
            // Dereference failed! This is a hard error (hitting _ is checked above and returns ~0)
            this->dump();
            TODO(sp, "Error when no method could be found, but type is known - (: " << top_ty << ")." << method_name);
        }
        
    public:
        ::std::function<const ::HIR::TypeRef&(const ::HIR::TypeRef&)> callback_resolve_infer() {
            return [&](const auto& ty)->const auto& {
                    if( ty.m_data.is_Infer() ) 
                        return this->get_type(ty);
                    else
                        return ty;
                };
        }
        
        unsigned int new_ivar()
        {
            m_ivars.push_back( IVar() );
            m_ivars.back().type->m_data.as_Infer().index = m_ivars.size() - 1;
            return m_ivars.size() - 1;
        }
        void del_ivar(unsigned int index)
        {
            DEBUG("Deleting ivar " << index << " of  " << m_ivars.size());
            if( index == m_ivars.size() - 1 ) {
                m_ivars.pop_back();
            }
            else {
                assert(!"Can't delete an ivar after it's been used");
            }
        }
        ::HIR::TypeRef new_ivar_tr() {
            ::HIR::TypeRef rv;
            rv.m_data.as_Infer().index = this->new_ivar();
            return rv;
        }
        
        ::HIR::TypeRef& get_type(::HIR::TypeRef& type)
        {
            TU_IFLET(::HIR::TypeRef::Data, type.m_data, Infer, e,
                assert(e.index != ~0u);
                return *get_pointed_ivar(e.index).type;
            )
            else {
                return type;
            }
        }
        const ::HIR::TypeRef& get_type(const ::HIR::TypeRef& type) const
        {
            TU_IFLET(::HIR::TypeRef::Data, type.m_data, Infer, e,
                assert(e.index != ~0u);
                return *get_pointed_ivar(e.index).type;
            )
            else {
                return type;
            }
        }
        
        void check_type_class_primitive(const Span& sp, const ::HIR::TypeRef& type, ::HIR::InferClass ic, ::HIR::CoreType ct)
        {
            switch(ic)
            {
            case ::HIR::InferClass::None:
                break;
            case ::HIR::InferClass::Float:
                switch(ct)
                {
                case ::HIR::CoreType::F32:
                case ::HIR::CoreType::F64:
                    break;
                default:
                    ERROR(sp, E0000, "Type unificiation of integer literal with non-integer - " << type);
                }
                break;
            case ::HIR::InferClass::Integer:
                switch(ct)
                {
                case ::HIR::CoreType::I8:    case ::HIR::CoreType::U8:
                case ::HIR::CoreType::I16:   case ::HIR::CoreType::U16:
                case ::HIR::CoreType::I32:   case ::HIR::CoreType::U32:
                case ::HIR::CoreType::I64:   case ::HIR::CoreType::U64:
                case ::HIR::CoreType::Isize: case ::HIR::CoreType::Usize:
                    break;
                default:
                    ERROR(sp, E0000, "Type unificiation of integer literal with non-integer - " << type);
                }
                break;
            }
        }
        
        void set_ivar_to(unsigned int slot, ::HIR::TypeRef type)
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
            else {
                // Otherwise, store left in right's slot
                DEBUG("Set IVar " << slot << " = " << type);
                root_ivar.type = box$( mv$(type) );
            }
            
            this->mark_change();
        }

        void ivar_unify(unsigned int left_slot, unsigned int right_slot)
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
    
    private:
        IVar& get_pointed_ivar(unsigned int slot) const
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
    };
    
    // Enumerate inferrence variables (most of them) in the expression tree
    //
    // - Any type equalities here are mostly optimisations (as this gets run only once)
    //  - If ivars can be shared down the tree - good.
    class ExprVisitor_Enum:
        public ::HIR::ExprVisitorDef
    {
        TypecheckContext& context;
        const ::HIR::TypeRef&   ret_type;
    public:
        ExprVisitor_Enum(TypecheckContext& context, const ::HIR::TypeRef& ret_type):
            context(context),
            ret_type(ret_type)
        {
        }
        
        void visit_node(::HIR::ExprNode& node) override {
            this->context.add_ivars(node.m_res_type);
            DEBUG(typeid(node).name() << " : " << node.m_res_type);
        }
        void visit(::HIR::ExprNode_Block& node) override
        {
            ::HIR::ExprVisitorDef::visit(node);
            if( node.m_nodes.size() > 0 ) {
                auto& ln = *node.m_nodes.back();
                // If the child node didn't set a real return type, force it to be the same as this node's
                if( ln.m_res_type.m_data.is_Infer() ) {
                    ln.m_res_type = node.m_res_type.clone();
                }
                else {
                    // If it was set, equate with possiblity of coercion
                    this->context.apply_equality(ln.span(), node.m_res_type, ln.m_res_type, &node.m_nodes.back());
                }
            }
            else {
                node.m_res_type = ::HIR::TypeRef::new_unit();
            }
        }
        void visit(::HIR::ExprNode_Return& node) override
        {
            ::HIR::ExprVisitorDef::visit(node);
            this->context.apply_equality(node.span(), this->ret_type, node.m_value->m_res_type,  &node.m_value);
        }
        
        void visit(::HIR::ExprNode_Let& node) override
        {
            ::HIR::ExprVisitorDef::visit(node);
            TRACE_FUNCTION_F("let " << node.m_pattern << ": " << node.m_type);
            
            this->context.add_ivars(node.m_type);
            
            this->context.add_binding(node.span(), node.m_pattern, node.m_type);
        }
        
        void visit(::HIR::ExprNode_Match& node) override
        {
            TRACE_FUNCTION_F("match ...");
            
            this->context.add_ivars(node.m_value->m_res_type);
            
            for(auto& arm : node.m_arms)
            {
                DEBUG("ARM " << arm.m_patterns);
                for(auto& pat : arm.m_patterns)
                {
                    this->context.add_binding(node.span(), pat, node.m_value->m_res_type);
                }
            }

            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_If& node) override
        {
            node.m_cond->m_res_type = ::HIR::TypeRef( ::HIR::CoreType::Bool );
            if( node.m_false ) {
                node.m_true->m_res_type = node.m_res_type.clone();
                node.m_false->m_res_type = node.m_res_type.clone();
            }
            else {
                this->context.apply_equality(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
                node.m_true->m_res_type = node.m_res_type.clone();
            }
            
            ::HIR::ExprVisitorDef::visit(node);
        }

        void visit(::HIR::ExprNode_UniOp& node) override
        {
            ::HIR::ExprVisitorDef::visit(node);
            
            switch(node.m_op)
            {
            case ::HIR::ExprNode_UniOp::Op::Ref:
                this->context.apply_equality(node.span(), node.m_res_type, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, node.m_value->m_res_type.clone()));
                break;
            case ::HIR::ExprNode_UniOp::Op::RefMut:
                this->context.apply_equality(node.span(), node.m_res_type, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Unique, node.m_value->m_res_type.clone()));
                break;
            case ::HIR::ExprNode_UniOp::Op::Invert:
                break;
            case ::HIR::ExprNode_UniOp::Op::Negate:
                break;
            }
        }

        void visit(::HIR::ExprNode_Tuple& node) override
        {
            // Only delete and apply if the return type is an ivar
            // - Can happen with `match (a, b)`
            TU_IFLET(::HIR::TypeRef::Data, node.m_res_type.m_data, Infer, e,
                // - Remove the ivar created by the generic visitor
                this->context.dump();
                this->context.del_ivar( e.index );
            )

            ::HIR::ExprVisitorDef::visit(node);

            if( node.m_res_type.m_data.is_Infer() )
            {
                ::std::vector< ::HIR::TypeRef>  types;
                for( const auto& sn : node.m_vals )
                    types.push_back( sn->m_res_type.clone() );
                node.m_res_type = ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Tuple(mv$(types)) );
            }
        }
        void visit(::HIR::ExprNode_Closure& node) override
        {
            for(auto& a : node.m_args) {
                this->context.add_ivars(a.second);
                this->context.add_binding(node.span(), a.first, a.second);
            }
            this->context.add_ivars(node.m_return);
            node.m_code->m_res_type = node.m_return.clone();

            ::HIR::ExprVisitorDef::visit(node);
        }
        // - Variable: Bind to same ivar
        void visit(::HIR::ExprNode_Variable& node) override
        {
            TU_IFLET(::HIR::TypeRef::Data, node.m_res_type.m_data, Infer, e,
                this->context.del_ivar( e.index );
            )
            node.m_res_type = this->context.get_var_type(node.span(), node.m_slot).clone();
        }
        
        void visit_generic_path(const Span& sp, ::HIR::GenericPath& gp) {
            for(auto& ty : gp.m_params.m_types)
                this->context.add_ivars(ty);
        }
        void visit_path(const Span& sp, ::HIR::Path& path) {
            TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
            (Generic,
                this->visit_generic_path(sp, e);
                ),
            (UfcsKnown,
                this->context.add_ivars(*e.type);
                this->visit_generic_path(sp, e.trait);
                for(auto& ty : e.params.m_types)
                    this->context.add_ivars(ty);
                ),
            (UfcsUnknown,
                TODO(sp, "Hit a UfcsUnknown (" << path << ") - Is this an error?");
                ),
            (UfcsInherent,
                this->context.add_ivars(*e.type);
                for(auto& ty : e.params.m_types)
                    this->context.add_ivars(ty);
                )
            )
        }
        void visit(::HIR::ExprNode_PathValue& node) override {
            this->visit_path(node.span(), node.m_path);
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_CallPath& node) override {
            this->visit_path(node.span(), node.m_path);
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_StructLiteral& node) override {
            this->visit_generic_path(node.span(), node.m_path);
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_TupleVariant& node) override {
            this->visit_generic_path(node.span(), node.m_path);
            ::HIR::ExprVisitorDef::visit(node);
        }
    };
    
    // Continually run over the expression tree until nothing changes
    class ExprVisitor_Run:
        public ::HIR::ExprVisitorDef
    {
        TypecheckContext& context;
        ::HIR::ExprNodeP    *m_node_ptr_ptr;
    public:
        ExprVisitor_Run(TypecheckContext& context):
            context(context),
            m_node_ptr_ptr(nullptr)
        {
        }
        
        void visit_node_ptr(::std::unique_ptr< ::HIR::ExprNode>& node_ptr) {
            m_node_ptr_ptr = &node_ptr;
            ::HIR::ExprVisitorDef::visit_node_ptr(node_ptr);
            m_node_ptr_ptr = nullptr;
        }
        
        // - Block: Ignore all return values except the last one (which is yeilded)
        void visit(::HIR::ExprNode_Block& node) override
        {
            TRACE_FUNCTION_F("{ }");
            if( node.m_nodes.size() ) {
                auto& lastnode = node.m_nodes.back();
                this->context.apply_equality(node.span(), node.m_res_type, lastnode->m_res_type,  &lastnode);
            }
            else {
                this->context.apply_equality(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
            }
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - Let: Equates inner to outer
        void visit(::HIR::ExprNode_Let& node) override
        {
            TRACE_FUNCTION_F("let " << node.m_pattern << " : " << node.m_type);
            if( node.m_value ) {
                this->context.apply_equality(node.span(), node.m_type, node.m_value->m_res_type,  &node.m_value);
            }

            ::HIR::ExprVisitorDef::visit(node);
        }
        
        // - If: Both branches have to agree
        void visit(::HIR::ExprNode_If& node) override
        {
            TRACE_FUNCTION_F("if ...");
            this->context.apply_equality(node.span(), node.m_res_type, node.m_true->m_res_type,  &node.m_true);
            if( node.m_false ) {
                this->context.apply_equality(node.span(), node.m_res_type, node.m_false->m_res_type, &node.m_false);
            }
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - Match: all branches match
        void visit(::HIR::ExprNode_Match& node) override
        {
            TRACE_FUNCTION_F("match ...");
            
            for(auto& arm : node.m_arms)
            {
                DEBUG("ARM " << arm.m_patterns);
                // TODO: Span on the arm
                this->context.apply_equality(node.span(), node.m_res_type, arm.m_code->m_res_type, &arm.m_code);
            }
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - Assign: both sides equal
        void visit(::HIR::ExprNode_Assign& node) override
        {
            TRACE_FUNCTION_F("... = ...");
            // Plain assignment can't be overloaded, requires equal types
            if( node.m_op == ::HIR::ExprNode_Assign::Op::None ) {
                this->context.apply_equality(node.span(),
                    node.m_slot->m_res_type, node.m_value->m_res_type,
                    &node.m_value
                    );
            }
            else {
                const auto& ty_left  = this->context.get_type(node.m_slot->m_res_type );
                const auto& ty_right = this->context.get_type(node.m_value->m_res_type);
                
                bool isprim_l = ty_left .m_data.is_Primitive() || (ty_left .m_data.is_Infer() && ty_left .m_data.as_Infer().ty_class != ::HIR::InferClass::None);
                bool isprim_r = ty_right.m_data.is_Primitive() || (ty_right.m_data.is_Infer() && ty_right.m_data.as_Infer().ty_class != ::HIR::InferClass::None);
                // SHORTCUT - If both sides are primitives (either confirmed or literals)
                if( isprim_l && isprim_r ) {
                    // - The only option is for them both to be the same type (because primitives can't have multiple overloads)
                    // TODO: Check that this operation is valid to peform. (e.g. not doing f32_val <<= f32_val)
                    // TODO: Aren't the bitwise shift operators valid with any integer type count?
                    this->context.apply_equality(node.span(), node.m_slot->m_res_type, node.m_value->m_res_type);
                }
                else {
                    const char *lang_item = nullptr;
                    switch( node.m_op )
                    {
                    case ::HIR::ExprNode_Assign::Op::None:  throw "";
                    case ::HIR::ExprNode_Assign::Op::Add: lang_item = "add_assign"; break;
                    case ::HIR::ExprNode_Assign::Op::Sub: lang_item = "sub_assign"; break;
                    case ::HIR::ExprNode_Assign::Op::Mul: lang_item = "mul_assign"; break;
                    case ::HIR::ExprNode_Assign::Op::Div: lang_item = "div_assign"; break;
                    case ::HIR::ExprNode_Assign::Op::Mod: lang_item = "rem_assign"; break;
                    case ::HIR::ExprNode_Assign::Op::And: lang_item = "bitand_assign"; break;
                    case ::HIR::ExprNode_Assign::Op::Or : lang_item = "bitor_assign" ; break;
                    case ::HIR::ExprNode_Assign::Op::Xor: lang_item = "bitxor_assign"; break;
                    case ::HIR::ExprNode_Assign::Op::Shr: lang_item = "shl_assign"; break;
                    case ::HIR::ExprNode_Assign::Op::Shl: lang_item = "shr_assign"; break;
                    }
                    assert(lang_item);
                    const auto& trait_path = this->context.m_crate.get_lang_item_path(node.span(), lang_item);
                    // TODO: Look for implementation of ops trait
                    TODO(node.span(), "Search for implementation of " << trait_path << "<" << ty_right << "> for " << ty_left);
                }
            }
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - BinOp: Look for overload or primitive
        void visit(::HIR::ExprNode_BinOp& node) override
        {
            ::HIR::ExprVisitorDef::visit(node);
            const auto& ty_left  = this->context.get_type(node.m_left->m_res_type );
            const auto& ty_right = this->context.get_type(node.m_right->m_res_type);
            
            // Boolean ops can't be overloaded, and require `bool` on both sides
            if( node.m_op == ::HIR::ExprNode_BinOp::Op::BoolAnd || node.m_op == ::HIR::ExprNode_BinOp::Op::BoolOr )
            {
                assert(node.m_res_type.m_data.is_Primitive() && node.m_res_type.m_data.as_Primitive() == ::HIR::CoreType::Bool);
                this->context.apply_equality( node.span(), node.m_res_type, node.m_left->m_res_type );
                this->context.apply_equality( node.span(), node.m_res_type, node.m_right->m_res_type );
                return ;
            }
            
            // NOTE: Inferrence rules when untyped integer literals are in play
            // - `impl Add<Foo> for u32` is valid, and makes `1 + Foo` work
            //  - But `[][0] + Foo` doesn't
            //  - Adding `impl Add<Foo> for u64` leads to "`Add<Foo>` is not implemented for `i32`"
            // - HACK! (kinda?) libcore includes impls of `Add<i32> for i32`, which means that overloads work for inferrence purposes
            if( ty_left.m_data.is_Primitive() && ty_right.m_data.is_Primitive() ) 
            {
                const auto& prim_left  = ty_left.m_data.as_Primitive();
                const auto& prim_right = ty_right.m_data.as_Primitive();
                switch(node.m_op)
                {
                case ::HIR::ExprNode_BinOp::Op::CmpEqu:
                case ::HIR::ExprNode_BinOp::Op::CmpNEqu:
                case ::HIR::ExprNode_BinOp::Op::CmpLt:
                case ::HIR::ExprNode_BinOp::Op::CmpLtE:
                case ::HIR::ExprNode_BinOp::Op::CmpGt:
                case ::HIR::ExprNode_BinOp::Op::CmpGtE:
                    if( prim_left != prim_right ) {
                        ERROR(node.span(), E0000, "Mismatched types in comparison");
                    }
                    break;
               
                case ::HIR::ExprNode_BinOp::Op::BoolAnd:    BUG(node.span(), "Encountered BoolAnd in primitive op");
                case ::HIR::ExprNode_BinOp::Op::BoolOr:     BUG(node.span(), "Encountered BoolOr in primitive op");

                case ::HIR::ExprNode_BinOp::Op::Add:
                case ::HIR::ExprNode_BinOp::Op::Sub:
                case ::HIR::ExprNode_BinOp::Op::Mul:
                case ::HIR::ExprNode_BinOp::Op::Div:
                case ::HIR::ExprNode_BinOp::Op::Mod:
                    if( prim_left != prim_right ) {
                        ERROR(node.span(), E0000, "Mismatched types in arithmatic operation");
                    }
                    switch(prim_left)
                    {
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::Bool:
                        ERROR(node.span(), E0000, "Invalid use of arithmatic on " << ty_left);
                        break;
                    default:
                        this->context.apply_equality(node.span(), node.m_res_type, ty_left);
                    }
                    break;
                case ::HIR::ExprNode_BinOp::Op::And:
                case ::HIR::ExprNode_BinOp::Op::Or:
                case ::HIR::ExprNode_BinOp::Op::Xor:
                    if( prim_left != prim_right ) {
                        ERROR(node.span(), E0000, "Mismatched types in bitwise operation");
                    }
                    switch(prim_left)
                    {
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::Bool:
                    case ::HIR::CoreType::F32:
                    case ::HIR::CoreType::F64:
                        ERROR(node.span(), E0000, "Invalid use of bitwise operation on " << ty_left);
                        break;
                    default:
                        this->context.apply_equality(node.span(), node.m_res_type, ty_left);
                    }
                    break;
                case ::HIR::ExprNode_BinOp::Op::Shr:
                case ::HIR::ExprNode_BinOp::Op::Shl:
                    switch(prim_left)
                    {
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::Bool:
                    case ::HIR::CoreType::F32:
                    case ::HIR::CoreType::F64:
                        ERROR(node.span(), E0000, "Invalid type for shift count - " << ty_right);
                    default:
                        break;
                    }
                    switch(prim_left)
                    {
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::Bool:
                    case ::HIR::CoreType::F32:
                    case ::HIR::CoreType::F64:
                        ERROR(node.span(), E0000, "Invalid use of shift on " << ty_left);
                        break;
                    default:
                        this->context.apply_equality(node.span(), node.m_res_type, ty_left);
                    }
                    break;
                }
            }
            else
            {
                const char* item_name = nullptr;
                bool has_output = true;
                switch(node.m_op)
                {
                case ::HIR::ExprNode_BinOp::Op::CmpEqu:  item_name = "eq"; has_output = false; break;
                case ::HIR::ExprNode_BinOp::Op::CmpNEqu: item_name = "eq"; has_output = false; break;
                case ::HIR::ExprNode_BinOp::Op::CmpLt:   item_name = "ord"; has_output = false; break;
                case ::HIR::ExprNode_BinOp::Op::CmpLtE:  item_name = "ord"; has_output = false; break;
                case ::HIR::ExprNode_BinOp::Op::CmpGt:   item_name = "ord"; has_output = false; break;
                case ::HIR::ExprNode_BinOp::Op::CmpGtE:  item_name = "ord"; has_output = false; break;
                case ::HIR::ExprNode_BinOp::Op::BoolAnd:    BUG(node.span(), "Encountered BoolAnd in overload search");
                case ::HIR::ExprNode_BinOp::Op::BoolOr:     BUG(node.span(), "Encountered BoolOr in overload search");

                case ::HIR::ExprNode_BinOp::Op::Add: item_name = "add"; break;
                case ::HIR::ExprNode_BinOp::Op::Sub: item_name = "sub"; break;
                case ::HIR::ExprNode_BinOp::Op::Mul: item_name = "mul"; break;
                case ::HIR::ExprNode_BinOp::Op::Div: item_name = "div"; break;
                case ::HIR::ExprNode_BinOp::Op::Mod: item_name = "rem"; break;
                
                case ::HIR::ExprNode_BinOp::Op::And: item_name = "bit_and"; break;
                case ::HIR::ExprNode_BinOp::Op::Or:  item_name = "bit_or";  break;
                case ::HIR::ExprNode_BinOp::Op::Xor: item_name = "bit_xor"; break;
                
                case ::HIR::ExprNode_BinOp::Op::Shr: item_name = "shr"; break;
                case ::HIR::ExprNode_BinOp::Op::Shl: item_name = "shl"; break;
                }
                assert(item_name);
                
                // Search for ops trait impl
                const ::HIR::TraitImpl* impl_ptr = nullptr;
                unsigned int count = 0;
                const auto& ops_trait = this->context.m_crate.get_lang_item_path(node.span(), item_name);
                bool found_exact = this->context.m_crate.find_trait_impls(ops_trait, ty_left, this->context.callback_resolve_infer(),
                    [&](const auto& impl) {
                        // TODO: Check how concretely the types matched
                        assert( impl.m_trait_args.m_types.size() == 1 );
                        const auto& arg_type = impl.m_trait_args.m_types[0];
                        // TODO: What if the trait arguments depend on a generic parameter?
                        if( monomorphise_type_needed(arg_type) )
                            TODO(node.span(), "Compare trait type when it contains generics");
                        auto cmp = arg_type.compare_with_paceholders(node.span(), ty_right, this->context.callback_resolve_infer());
                        if( cmp == ::HIR::Compare::Unequal ) {
                            return false;
                        }
                        count += 1;
                        impl_ptr = &impl;
                        if( cmp == ::HIR::Compare::Equal ) {
                            DEBUG("Operator impl exact match - '"<<item_name<<"' - " << arg_type << " == " << ty_right);
                            return true;
                        }
                        else {
                            DEBUG("Operator fuzzy exact match - '"<<item_name<<"' - " << arg_type << " == " << ty_right);
                            return false;
                        }
                    }
                    );
                // If there wasn't an exact match, BUT there was one partial match - assume the partial match is what we want
                if( !found_exact && count == 1 ) {
                    assert(impl_ptr);
                    this->context.apply_equality(node.span(), impl_ptr->m_trait_args.m_types[0], ty_right);
                }
                if( impl_ptr ) {
                    if( has_output )
                    {
                        const auto& type = impl_ptr->m_types.at("Output");
                        if( monomorphise_type_needed(type) ) {
                            TODO(node.span(), "BinOp output = " << type);
                        }
                        else {
                            this->context.apply_equality(node.span(), node.m_res_type, type);
                        }
                    }
                    else
                    {
                        this->context.apply_equality(node.span(), node.m_res_type, ::HIR::TypeRef(::HIR::CoreType::Bool));
                    }
                }
                else {
                    // TODO: Determine if this could ever succeed, and error if not.
                    // - Likely `count` can help, but only if fuzzy matching of the impl type is done
                }
            }
        }
        // - UniOp: Look for overload or primitive
        void visit(::HIR::ExprNode_UniOp& node) override
        {
            ::HIR::ExprVisitorDef::visit(node);
            
            const auto& ty = this->context.get_type(node.m_value->m_res_type);
            switch(node.m_op)
            {
            case ::HIR::ExprNode_UniOp::Op::Ref:
                // - Handled above?
                break;
            case ::HIR::ExprNode_UniOp::Op::RefMut:
                // - Handled above?
                break;
            case ::HIR::ExprNode_UniOp::Op::Invert:
                TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Primitive, e,
                    switch(e)
                    {
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::F32:
                    case ::HIR::CoreType::F64:
                        ERROR(node.span(), E0000, "Invalid use of ! on " << ty);
                        break;
                    default:
                        this->context.apply_equality(node.span(), node.m_res_type, ty);
                        break;
                    }
                )
                else {
                    // TODO: Search for an implementation of ops::Not
                }
                break;
            case ::HIR::ExprNode_UniOp::Op::Negate:
                TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Primitive, e,
                    switch(e)
                    {
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::Bool:
                        ERROR(node.span(), E0000, "Invalid use of - on " << ty);
                        break;
                    default:
                        this->context.apply_equality(node.span(), node.m_res_type, ty);
                        break;
                    }
                )
                else {
                    // TODO: Search for an implementation of ops::Neg
                }
                break;
            }
        }
        // - Cast: Nothing needs to happen
        void visit(::HIR::ExprNode_Cast& node) override
        {
            const auto& val_ty = this->context.get_type( node.m_value->m_res_type );
            const auto& target_ty = this->context.get_type( node.m_res_type );
            TU_MATCH_DEF(::HIR::TypeRef::Data, (target_ty.m_data), (e),
            (
                ERROR(node.span(), E0000, "Invalid cast");
                ),
            (Primitive,
                switch(e)
                {
                case ::HIR::CoreType::Char:
                    break;
                case ::HIR::CoreType::Str:
                case ::HIR::CoreType::Bool:
                    ERROR(node.span(), E0000, "Invalid cast");
                    break;
                default:
                    // TODO: Check that the source and destination are integer types.
                    break;
                }
                ),
            (Borrow,
                // TODO: Actually a coerce - check it
                ),
            (Infer,
                // - wait
                ),
            (Pointer,
                // Valid source:
                // *<same> <any>
                // *<other> <same>
                // &<same> <same>
                TU_MATCH_DEF(::HIR::TypeRef::Data, (val_ty.m_data), (e2),
                (
                    ),
                (Infer,
                    ),
                (Borrow,
                    if( e.type != e2.type ) {
                        // ERROR
                    }
                    this->context.apply_equality(node.span(), *e2.inner, *e.inner);
                    ),
                (Pointer,
                    if( e.type != e2.type ) {
                        this->context.apply_equality(node.span(), *e2.inner, *e.inner);
                    }
                    else {
                        // Nothing
                    }
                    )
                )
                )
            )
            // TODO: Check cast validity and do inferrence
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - Index: Look for implementation of the Index trait
        void visit(::HIR::ExprNode_Index& node) override
        {
            this->context.find_trait_impls(this->context.m_crate.get_lang_item_path(node.span(), "index"), node.m_value->m_res_type, [&](const auto& args) {
                DEBUG("TODO: Insert index operator (if index arg matches)");
                return false;
                });
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - Deref: Look for impl of Deref
        void visit(::HIR::ExprNode_Deref& node) override
        {
            const auto& ty = this->context.get_type( node.m_value->m_res_type );
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Borrow, e,
                this->context.apply_equality(node.span(), node.m_res_type, *e.inner);
            )
            else {
                // TODO: Search for Deref impl
            }
            ::HIR::ExprVisitorDef::visit(node);
        }
        
        void fix_param_count(const Span& sp, const ::HIR::Path& path, const ::HIR::GenericParams& param_defs,  ::HIR::PathParams& params)
        {
            if( params.m_types.size() == param_defs.m_types.size() ) {
                // Nothing to do, all good
                return ;
            }
            
            if( params.m_types.size() == 0 ) {
                for(const auto& typ : param_defs.m_types) {
                    (void)typ;
                    params.m_types.push_back( this->context.new_ivar_tr() );
                }
            }
            else if( params.m_types.size() > param_defs.m_types.size() ) {
                ERROR(sp, E0000, "Too many type parameters passed to " << path);
            }
            else {
                while( params.m_types.size() < param_defs.m_types.size() ) {
                    const auto& typ = param_defs.m_types[params.m_types.size()];
                    if( typ.m_default.m_data.is_Infer() ) {
                        ERROR(sp, E0000, "Omitted type parameter with no default in " << path);
                    }
                    else {
                        // TODO: What if this contains a generic param? (is that valid? Self maybe, what about others?)
                        params.m_types.push_back( typ.m_default.clone() );
                    }
                }
            }
        }
        void visit(::HIR::ExprNode_TupleVariant& node) override {
            const Span& sp = node.span();
            auto& arg_types = node.m_arg_types;
            if( arg_types.size() == 0 )
            {
                auto& path_params = node.m_path.m_params;
                auto monomorph_cb = [&](const auto& gt)->const auto& {
                        const auto& e = gt.m_data.as_Generic();
                        if( e.name == "Self" )
                            TODO(sp, "Handle 'Self' when monomorphising type");
                        if( e.binding < 256 ) {
                            auto idx = e.binding;
                            if( idx >= path_params.m_types.size() ) {
                                BUG(sp, "Generic param out of input range - " << idx << " '"<<e.name<<"' >= " << path_params.m_types.size());
                            }
                            return path_params.m_types[idx];
                        }
                        else if( e.binding < 512 ) {
                            BUG(sp, "Method-level parameter on struct/enum");
                        }
                        else {
                            BUG(sp, "Generic bounding out of total range");
                        }
                    };
                
                if( node.m_is_struct )
                {
                    const auto& str = this->context.m_crate.get_struct_by_path(sp, node.m_path.m_path);
                    // TODO: Remove this clone
                    this->fix_param_count(sp, ::HIR::Path(node.m_path.clone()), str.m_params,  path_params);
                    const auto& fields = str.m_data.as_Tuple();
                    arg_types.reserve( fields.size() );
                    for(const auto& fld : fields)
                    {
                        if( monomorphise_type_needed(fld.ent) ) {
                            arg_types.push_back( this->context.expand_associated_types(sp, monomorphise_type_with(sp, fld.ent,  monomorph_cb)) );
                        }
                        else {
                            arg_types.push_back( fld.ent.clone() );
                        }
                    }
                    
                    arg_types.push_back( ::HIR::TypeRef(node.m_path.clone()) );
                }
                else
                {
                    const auto& variant_name = node.m_path.m_path.m_components.back();
                    auto type_path = node.m_path.m_path;
                    type_path.m_components.pop_back();
                    
                    const auto& enm = this->context.m_crate.get_enum_by_path(sp, type_path);
                    // TODO: Remove this clone
                    this->fix_param_count(sp, ::HIR::Path(node.m_path.clone()), enm.m_params,  path_params);
                    
                    auto it = ::std::find_if( enm.m_variants.begin(), enm.m_variants.end(), [&](const auto& x){ return x.first == variant_name; });
                    if( it == enm.m_variants.end() ) {
                        ERROR(sp, E0000, "Unable to find variant '" << variant_name << " of " << type_path);
                    }
                    const auto& fields = it->second.as_Tuple();
                    arg_types.reserve( fields.size() );
                    for(const auto& fld : fields)
                    {
                        if( monomorphise_type_needed(fld) ) {
                            arg_types.push_back( this->context.expand_associated_types(sp, monomorphise_type_with(sp, fld,  monomorph_cb)) );
                        }
                        else {
                            arg_types.push_back( fld.clone() );
                        }
                    }
                    arg_types.push_back( ::HIR::TypeRef( ::HIR::GenericPath(type_path, path_params.clone()) ) );
                }
                
                if( node.m_args.size() != arg_types.size() - 1 ) {
                    ERROR(sp, E0000, "Incorrect number of arguments to " << node.m_path);
                }
                DEBUG("--- RESOLVED");
            }
            
            for( unsigned int i = 0; i < arg_types.size() - 1; i ++ )
            {
                auto& arg_expr_ptr = node.m_args[i];
                const auto& arg_ty = arg_types[i];
                DEBUG("Arg " << i << ": " << arg_ty);
                this->context.apply_equality(sp, arg_ty, arg_expr_ptr->m_res_type,  &arg_expr_ptr);
            }
            
            DEBUG("Rreturn: " << arg_types.back());
            this->context.apply_equality(sp, node.m_res_type, arg_types.back() /*,  &this_node_ptr*/);
            
            ::HIR::ExprVisitorDef::visit(node);
        }
        
        void visit_call(const Span& sp,
                ::HIR::Path& path, bool is_method,
                ::std::vector< ::HIR::ExprNodeP>& args, ::HIR::TypeRef& res_type, ::HIR::ExprNodeP& this_node_ptr,
                ::std::vector< ::HIR::TypeRef>& arg_types
                )
        {
            TRACE_FUNCTION_F("path = " << path);
            unsigned int arg_ofs = (is_method ? 1 : 0);
            
            if( arg_types.size() == 0 )
            {
                const ::HIR::Function*  fcn_ptr = nullptr;
                ::std::function<const ::HIR::TypeRef&(const ::HIR::TypeRef&)>    monomorph_cb;
                
                TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
                (Generic,
                    // TODO: This could also point to an enum variant, or to a struct constructor
                    // - Current code only handles functions.

                    const auto& fcn = this->context.m_crate.get_function_by_path(sp, e.m_path);
                    this->fix_param_count(sp, path, fcn.m_params,  e.m_params);
                    fcn_ptr = &fcn;
                    
                    //const auto& params_def = fcn.m_params;
                    const auto& path_params = e.m_params;
                    monomorph_cb = [&](const auto& gt)->const auto& {
                            const auto& e = gt.m_data.as_Generic();
                            if( e.name == "Self" )
                                TODO(sp, "Handle 'Self' when monomorphising");
                            if( e.binding < 256 ) {
                                BUG(sp, "Impl-level parameter on free function (#" << e.binding << " " << e.name << ")");
                            }
                            else if( e.binding < 512 ) {
                                auto idx = e.binding - 256;
                                if( idx >= path_params.m_types.size() ) {
                                    BUG(sp, "Generic param out of input range - " << idx << " '"<<e.name<<"' >= " << path_params.m_types.size());
                                }
                                return path_params.m_types[idx];
                            }
                            else {
                                BUG(sp, "Generic bounding out of total range");
                            }
                        };
                    ),
                (UfcsKnown,
                    const auto& trait = this->context.m_crate.get_trait_by_path(sp, e.trait.m_path);
                    this->fix_param_count(sp, path, trait.m_params, e.trait.m_params);
                    const auto& fcn = trait.m_values.at(e.item).as_Function();
                    this->fix_param_count(sp, path, fcn.m_params,  e.params);
                    
                    fcn_ptr = &fcn;
                    
                    const auto& trait_params = e.trait.m_params;
                    const auto& path_params = e.params;
                    monomorph_cb = [&](const auto& gt)->const auto& {
                            const auto& ge = gt.m_data.as_Generic();
                            if( ge.binding == 0xFFFF ) {
                                return *e.type;
                            }
                            else if( ge.binding < 256 ) {
                                auto idx = ge.binding;
                                if( idx >= trait_params.m_types.size() ) {
                                    BUG(sp, "Generic param (impl) out of input range - " << idx << " '"<<ge.name<<"' >= " << trait_params.m_types.size());
                                }
                                return trait_params.m_types[idx];
                            }
                            else if( ge.binding < 512 ) {
                                auto idx = ge.binding - 256;
                                if( idx >= path_params.m_types.size() ) {
                                    BUG(sp, "Generic param out of input range - " << idx << " '"<<ge.name<<"' >= " << path_params.m_types.size());
                                }
                                return path_params.m_types[idx];
                            }
                            else {
                                BUG(sp, "Generic bounding out of total range");
                            }
                        };
                    ),
                (UfcsUnknown,
                    TODO(sp, "Hit a UfcsUnknown (" << path << ") - Is this an error?");
                    ),
                (UfcsInherent,
                    const ::HIR::TypeImpl* impl_ptr = nullptr;
                    this->context.m_crate.find_type_impls(*e.type, [&](const auto& ty)->const auto& {
                            if( ty.m_data.is_Infer() )
                                return this->context.get_type(ty);
                            else
                                return ty;
                        },
                        [&](const auto& impl) {
                            auto it = impl.m_methods.find(e.item);
                            if( it == impl.m_methods.end() )
                                return false;
                            fcn_ptr = &it->second;
                            impl_ptr = &impl;
                            return true;
                        });
                    if( !fcn_ptr ) {
                        ERROR(sp, E0000, "Failed to locate function " << path);
                    }
                    assert(impl_ptr);
                    this->fix_param_count(sp, path, fcn_ptr->m_params,  e.params);
                    
                    //const auto& impl_params = .m_params;
                    const auto& fcn_params = e.params;
                    monomorph_cb = [&](const auto& gt)->const auto& {
                            const auto& ge = gt.m_data.as_Generic();
                            if( ge.binding == 0xFFFF ) {
                                return *e.type;
                            }
                            else if( ge.binding < 256 ) {
                                TODO(sp, "Expand impl-leve generic params in UfcsInherent (path = " << path << ", param = #" << ge.binding << " '" << ge.name << "')");
                            }
                            else if( ge.binding < 512 ) {
                                auto idx = ge.binding - 256;
                                if( idx >= fcn_params.m_types.size() ) {
                                    BUG(sp, "Generic param out of input range - " << idx << " '" << ge.name << "' >= " << fcn_params.m_types.size());
                                }
                                return fcn_params.m_types[idx];
                            }
                            else {
                                BUG(sp, "Generic bounding out of total range");
                            }
                        };
                    )
                )

                assert( fcn_ptr );
                const auto& fcn = *fcn_ptr;
                
                if( args.size() + (is_method ? 1 : 0) != fcn.m_args.size() ) {
                    ERROR(sp, E0000, "Incorrect number of arguments to " << path);
                }
                
                for(const auto& arg : fcn.m_args) {
                    if( monomorphise_type_needed(arg.second) ) {
                        arg_types.push_back( this->context.expand_associated_types(sp, monomorphise_type_with(sp, arg.second,  monomorph_cb)) );
                    }
                    else {
                        arg_types.push_back( arg.second.clone() );
                    }
                }
                if( monomorphise_type_needed(fcn.m_return) ) {
                    arg_types.push_back( this->context.expand_associated_types(sp, monomorphise_type_with(sp, fcn.m_return,  monomorph_cb)) );
                }
                else {
                    arg_types.push_back( fcn.m_return.clone() );
                }
            }
            
            for( unsigned int i = arg_ofs; i < arg_types.size() - 1; i ++ )
            {
                auto& arg_expr_ptr = args[i - arg_ofs];
                const auto& arg_ty = arg_types[i];
                DEBUG("Arg " << i << ": " << arg_ty);
                this->context.apply_equality(sp, arg_ty, arg_expr_ptr->m_res_type,  &arg_expr_ptr);
            }
            
            // HACK: Expand UFCS again
            arg_types.back() = this->context.expand_associated_types(sp, mv$(arg_types.back()));
            
            DEBUG("RV " << arg_types.back());
            this->context.apply_equality(sp, res_type, arg_types.back(),  &this_node_ptr);
        }
        
        // - Call Path: Locate path and build return
        void visit(::HIR::ExprNode_CallPath& node) override
        {
            auto& node_ptr = *m_node_ptr_ptr;
            TRACE_FUNCTION_F("CallPath " << node.m_path);
            assert(node_ptr.get() == &node);
            // - Pass m_arg_types as a cache to avoid constant lookups
            visit_call(node.span(), node.m_path, false, node.m_args, node.m_res_type, node_ptr,  node.m_arg_types);
            
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - Call Value: If type is known, locate impl of Fn/FnMut/FnOnce
        void visit(::HIR::ExprNode_CallValue& node) override
        {
            const auto& ty = this->context.get_type(node.m_value->m_res_type);
            DEBUG("(CallValue) ty = " << ty);
            
            if( node.m_arg_types.size() == 0 )
            {
                TU_MATCH_DEF(decltype(ty.m_data), (ty.m_data), (e),
                (
                    ::HIR::TypeRef  fcn_args_tup;
                    ::HIR::TypeRef  fcn_ret;
                    // Locate impl of FnOnce
                    const auto& lang_FnOnce = this->context.m_crate.get_lang_item_path(node.span(), "fn_once");
                    auto was_bounded = this->context.find_trait_impls_bound(node.span(), lang_FnOnce, ty, [&](const auto& args) {
                            const auto& tup = args.m_types[0];
                            if( !tup.m_data.is_Tuple() )
                                ERROR(node.span(), E0000, "FnOnce expects a tuple argument, got " << tup);
                            fcn_args_tup = tup.clone();
                            return true;
                            });
                    if( was_bounded )
                    {
                        // RV must be in a bound
                        fcn_ret = ::HIR::TypeRef( ::HIR::Path(::HIR::Path::Data::make_UfcsKnown({
                            box$( ty.clone() ),
                            ::HIR::GenericPath(lang_FnOnce),
                            "Output",
                            {}
                            })) );
                        fcn_ret.m_data.as_Path().path.m_data.as_UfcsKnown().trait.m_params.m_types.push_back( fcn_args_tup.clone() );
                    }
                    else if( !ty.m_data.is_Generic() )
                    {
                        TODO(node.span(), "Search for other implementations of FnOnce for " << ty);
                    }
                    else
                    {
                        // Didn't find anything. Error?
                        TODO(node.span(), "Unable to find an implementation of Fn* for " << ty);
                    }
                    
                    node.m_arg_types = mv$( fcn_args_tup.m_data.as_Tuple() );
                    node.m_arg_types.push_back( mv$(fcn_ret) );
                    ),
                (Function,
                    TODO(node.span(), "CallValue with Function - " << ty);
                    ),
                (Infer,
                    )
                )
            }
            
            if( node.m_args.size() + 1 != node.m_arg_types.size() ) {
                ERROR(node.span(), E0000, "Incorrect number of arguments when calling " << ty);
            }
            
            for( unsigned int i = 0; i < node.m_args.size(); i ++ )
            {
                auto& arg_node = node.m_args[i];
                this->context.apply_equality(node.span(), node.m_arg_types[i], arg_node->m_res_type,  &arg_node);
            }
            // TODO: Allow infer
            this->context.apply_equality(node.span(), node.m_res_type, node.m_arg_types.back());
            
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - Call Method: Locate method on type
        void visit(::HIR::ExprNode_CallMethod& node) override
        {
            auto& node_ptr = *m_node_ptr_ptr;
            
            ::HIR::ExprVisitorDef::visit(node);
            if( node.m_method_path.m_data.is_Generic() && node.m_method_path.m_data.as_Generic().m_path.m_components.size() == 0 )
            {
                const auto& ty = this->context.get_type(node.m_value->m_res_type);
                DEBUG("ty = " << ty);
                // Using autoderef, locate this method on the type
                ::HIR::Path   fcn_path { ::HIR::SimplePath() };
                unsigned int deref_count = this->context.autoderef_find_method(node.span(), ty, node.m_method,  fcn_path);
                if( deref_count != ~0u )
                {
                    DEBUG("Found method " << fcn_path);
                    node.m_method_path = mv$(fcn_path);
                    // NOTE: Steals the params from the node
                    TU_MATCH(::HIR::Path::Data, (node.m_method_path.m_data), (e),
                    (Generic,
                        ),
                    (UfcsUnknown,
                        ),
                    (UfcsKnown,
                        e.params = mv$(node.m_params);
                        ),
                    (UfcsInherent,
                        e.params = mv$(node.m_params);
                        )
                    )
                    DEBUG("Adding " << deref_count << " dereferences");
                    while( deref_count > 0 )
                    {
                        node.m_value = ::HIR::ExprNodeP( new ::HIR::ExprNode_Deref(node.span(), mv$(node.m_value)) );
                        this->context.add_ivars( node.m_value->m_res_type );
                        deref_count -= 1;
                    }
                }
            }
            
            assert(node_ptr.get() == &node);
            // - Pass m_arg_types as a cache to avoid constant lookups
            visit_call(node.span(), node.m_method_path, true, node.m_args, node.m_res_type, node_ptr,  node.m_arg_types);
        }
        // - Field: Locate field on type
        void visit(::HIR::ExprNode_Field& node) override
        {
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - PathValue: Insert type from path
        void visit(::HIR::ExprNode_PathValue& node) override
        {
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - Variable: Bind to same ivar
        void visit(::HIR::ExprNode_Variable& node) override
        {
            // TODO: How to apply deref coercions here?
            // - Don't need to, instead construct "higher" nodes to avoid it
            TRACE_FUNCTION_F("var #"<<node.m_slot<<" '"<<node.m_name<<"'");
            this->context.apply_equality(node.span(),
                node.m_res_type, this->context.get_var_type(node.span(), node.m_slot)
                );
        }
        // - Struct literal: Semi-known types
        void visit(::HIR::ExprNode_StructLiteral& node) override
        {
            const Span& sp = node.span();
            // TODO: what if this is an enum struct variant constructor?
            
            auto& val_types = node.m_value_types;
            
            if( val_types.size() == 0 )
            {
                const auto& str = this->context.m_crate.get_struct_by_path(node.span(), node.m_path.m_path);
                this->fix_param_count(node.span(), node.m_path.clone(), str.m_params, node.m_path.m_params);
                
                this->context.apply_equality(node.span(), node.m_res_type, ::HIR::TypeRef(node.m_path.clone()));
                
                if( !str.m_data.is_Named() )
                    ERROR(sp, E0000, "Struct literal constructor for non-struct-like struct");
                const auto& flds_def = str.m_data.as_Named();
                
                for(auto& field : node.m_values)
                {
                    auto fld_def_it = ::std::find_if( flds_def.begin(), flds_def.end(), [&](const auto& x){ return x.first == field.first; } );
                    if( fld_def_it == flds_def.end() ) {
                        ERROR(sp, E0000, "Struct " << node.m_path << " doesn't have a field " << field.first);
                    }
                    const ::HIR::TypeRef& field_type = fld_def_it->second.ent;
                    
                    if( monomorphise_type_needed(field_type) ) {
                        val_types.push_back( monomorphise_type(sp, str.m_params, node.m_path.m_params,  field_type) );
                    }
                    else {
                        // SAFE: Can't have _ as monomorphise_type_needed checks for that
                        val_types.push_back( field_type.clone() );
                    }
                }
            }
            
            for( unsigned int i = 0; i < node.m_values.size(); i ++ )
            {
                auto& field = node.m_values[i];
                this->context.apply_equality(sp, val_types[i], field.second->m_res_type,  &field.second);
            }
            
            if( node.m_base_value ) {
                this->context.apply_equality(node.span(), node.m_res_type, node.m_base_value->m_res_type);
            }
            
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - Tuple literal: 
        void visit(::HIR::ExprNode_Tuple& node) override
        {
            assert( node.m_res_type.m_data.is_Tuple() );
            auto& tup_ents = node.m_res_type.m_data.as_Tuple();
            assert( tup_ents.size() == node.m_vals.size() );
            
            for(unsigned int i = 0; i < tup_ents.size(); i ++)
            {
                this->context.apply_equality(node.span(), tup_ents[i], node.m_vals[i]->m_res_type,  &node.m_vals[i]);
            }
            
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - Array list
        void visit(::HIR::ExprNode_ArrayList& node) override
        {
            const auto& val_type = *node.m_res_type.m_data.as_Array().inner;
            ::HIR::ExprVisitorDef::visit(node);
            for(auto& sn : node.m_vals)
                this->context.apply_equality(sn->span(), val_type, sn->m_res_type,  &sn);
        }
        // - Array (sized)
        void visit(::HIR::ExprNode_ArraySized& node) override
        {
            const auto& val_type = *node.m_res_type.m_data.as_Array().inner;
            ::HIR::ExprVisitorDef::visit(node);
            this->context.apply_equality(node.span(), val_type, node.m_val->m_res_type,  &node.m_val);
        }
        // - Closure
        void visit(::HIR::ExprNode_Closure& node) override
        {
            ::HIR::ExprVisitorDef::visit(node);
            this->context.apply_equality(node.span(), node.m_return, node.m_code->m_res_type,  &node.m_code);
        }
    };
    
    /// Visitor that applies the inferred types, and checks that all of them are fully resolved
    class ExprVisitor_Apply:
        public ::HIR::ExprVisitorDef
    {
        TypecheckContext& context;
    public:
        ExprVisitor_Apply(TypecheckContext& context):
            context(context)
        {
        }
        void visit_node(::HIR::ExprNode& node) override {
            DEBUG(typeid(node).name() << " : " << node.m_res_type);
            this->check_type_resolved(node.span(), node.m_res_type, node.m_res_type);
            DEBUG(typeid(node).name() << " : = " << node.m_res_type);
        }
        
    private:
        void check_type_resolved(const Span& sp, ::HIR::TypeRef& ty, const ::HIR::TypeRef& top_type) const {
            TU_MATCH(::HIR::TypeRef::Data, (ty.m_data), (e),
            (Infer,
                auto new_ty = this->context.get_type(ty).clone();
                if( new_ty.m_data.is_Infer() ) {
                    ERROR(sp, E0000, "Failed to infer type " << top_type);
                }
                ty = mv$(new_ty);
                ),
            (Diverge,
                // Leaf
                ),
            (Primitive,
                // Leaf
                ),
            (Path,
                // TODO:
                ),
            (Generic,
                // Leaf
                ),
            (TraitObject,
                // TODO:
                ),
            (Array,
                this->check_type_resolved(sp, *e.inner, top_type);
                ),
            (Slice,
                this->check_type_resolved(sp, *e.inner, top_type);
                ),
            (Tuple,
                for(auto& st : e)
                    this->check_type_resolved(sp, st, top_type);
                ),
            (Borrow,
                this->check_type_resolved(sp, *e.inner, top_type);
                ),
            (Pointer,
                this->check_type_resolved(sp, *e.inner, top_type);
                ),
            (Function,
                // TODO:
                )
            )
        }
    };
};

void Typecheck_Code(TypecheckContext context, const ::HIR::TypeRef& result_type, ::HIR::ExprPtr& expr)
{
    TRACE_FUNCTION;
    
    // Convert ExprPtr into unique_ptr for the execution of this function
    auto root_ptr = expr.into_unique();
    
    //context.apply_equality(expr->span(), result_type, expr->m_res_type);

    // 1. Enumerate inferrence variables and assign indexes to them
    {
        ExprVisitor_Enum    visitor { context, result_type };
        visitor.visit_node_ptr(root_ptr);
    }
    // - Apply equality between the node result and the expected type
    DEBUG("- Apply RV");
    context.apply_equality(root_ptr->span(), result_type, root_ptr->m_res_type,  &root_ptr);
    
    context.dump();
    // 2. Iterate through nodes applying rules until nothing changes
    {
        ExprVisitor_Run visitor { context };
        unsigned int count = 0;
        do {
            count += 1;
            DEBUG("==== PASS " << count << " ====");
            visitor.visit_node_ptr(root_ptr);
            assert(count < 1000);
        } while( context.take_changed() );
    }
    
    // 3. Check that there's no unresolved types left
    expr = ::HIR::ExprPtr( mv$(root_ptr) );
    context.dump();
    {
        DEBUG("==== VALIDATE ====");
        ExprVisitor_Apply   visitor { context };
        expr->visit( visitor );
    }
}



namespace {
    class OuterVisitor:
        public ::HIR::Visitor
    {
        ::HIR::Crate& m_crate;
        
        ::HIR::GenericParams*   m_impl_generics;
        ::HIR::GenericParams*   m_item_generics;
        ::std::vector< ::std::pair< const ::HIR::SimplePath*, const ::HIR::Trait* > >   m_traits;
    public:
        OuterVisitor(::HIR::Crate& crate):
            m_crate(crate),
            m_impl_generics(nullptr),
            m_item_generics(nullptr)
        {
        }
        
    private:
        template<typename T>
        class NullOnDrop {
            T*& ptr;
        public:
            NullOnDrop(T*& ptr):
                ptr(ptr)
            {}
            ~NullOnDrop() {
                ptr = nullptr;
            }
        };
        NullOnDrop< ::HIR::GenericParams> set_impl_generics(::HIR::GenericParams& gps) {
            assert( !m_impl_generics );
            m_impl_generics = &gps;
            return NullOnDrop< ::HIR::GenericParams>(m_impl_generics);
        }
        NullOnDrop< ::HIR::GenericParams> set_item_generics(::HIR::GenericParams& gps) {
            assert( !m_item_generics );
            m_item_generics = &gps;
            return NullOnDrop< ::HIR::GenericParams>(m_item_generics);
        }
    
    public:
        void visit_module(::HIR::PathChain p, ::HIR::Module& mod) override
        {
            DEBUG("Module has " << mod.m_traits.size() << " in-scope traits");
            for( const auto& trait_path : mod.m_traits ) {
                DEBUG("Push " << trait_path);
                m_traits.push_back( ::std::make_pair( &trait_path, &this->m_crate.get_trait_by_path(Span(), trait_path) ) );
            }
            ::HIR::Visitor::visit_module(p, mod);
            for(unsigned int i = 0; i < mod.m_traits.size(); i ++ )
                m_traits.pop_back();
        }
        
        // NOTE: This is left here to ensure that any expressions that aren't handled by higher code cause a failure
        void visit_expr(::HIR::ExprPtr& exp) {
            TODO(Span(), "visit_expr");
        }

        void visit_trait(::HIR::PathChain p, ::HIR::Trait& item) override
        {
            auto _ = this->set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_trait(p, item);
        }
        
        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << impl.m_type);
            auto _ = this->set_impl_generics(impl.m_params);
            
            ::HIR::Visitor::visit_type_impl(impl);
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type);
            auto _ = this->set_impl_generics(impl.m_params);
            
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
        }
        void visit_marker_impl(const ::HIR::SimplePath& trait_path, ::HIR::MarkerImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type << " { }");
            auto _ = this->set_impl_generics(impl.m_params);
            
            ::HIR::Visitor::visit_marker_impl(trait_path, impl);
        }
        
        void visit_type(::HIR::TypeRef& ty) override
        {
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
                this->visit_type( *e.inner );
                TypecheckContext    typeck_context { m_crate, m_impl_generics, m_item_generics };
                typeck_context.m_traits = this->m_traits;
                DEBUG("Array size " << ty);
                Typecheck_Code( mv$(typeck_context), ::HIR::TypeRef(::HIR::CoreType::Usize), e.size );
            )
            else {
                ::HIR::Visitor::visit_type(ty);
            }
        }
        // ------
        // Code-containing items
        // ------
        void visit_function(::HIR::PathChain p, ::HIR::Function& item) override {
            auto _ = this->set_item_generics(item.m_params);
            if( item.m_code )
            {
                TypecheckContext typeck_context { m_crate, m_impl_generics, m_item_generics };
                typeck_context.m_traits = this->m_traits;
                for( auto& arg : item.m_args ) {
                    typeck_context.add_binding( Span(), arg.first, arg.second );
                }
                DEBUG("Function code " << p);
                Typecheck_Code( mv$(typeck_context), item.m_return, item.m_code );
            }
        }
        void visit_static(::HIR::PathChain p, ::HIR::Static& item) override {
            //auto _ = this->set_item_generics(item.m_params);
            if( item.m_value )
            {
                TypecheckContext typeck_context { m_crate, m_impl_generics, m_item_generics };
                typeck_context.m_traits = this->m_traits;
                DEBUG("Static value " << p);
                Typecheck_Code( mv$(typeck_context), item.m_type, item.m_value );
            }
        }
        void visit_constant(::HIR::PathChain p, ::HIR::Constant& item) override {
            auto _ = this->set_item_generics(item.m_params);
            if( item.m_value )
            {
                TypecheckContext typeck_context { m_crate, m_impl_generics, m_item_generics };
                typeck_context.m_traits = this->m_traits;
                DEBUG("Const value " << p);
                Typecheck_Code( mv$(typeck_context), item.m_type, item.m_value );
            }
        }
        void visit_enum(::HIR::PathChain p, ::HIR::Enum& item) override {
            auto _ = this->set_item_generics(item.m_params);
            
            // TODO: Use a different type depding on repr()
            auto enum_type = ::HIR::TypeRef(::HIR::CoreType::Usize);
            
            // TODO: Check types too?
            for(auto& var : item.m_variants)
            {
                TU_IFLET(::HIR::Enum::Variant, var.second, Value, e,
                    TypecheckContext typeck_context { m_crate, m_impl_generics, m_item_generics };
                    typeck_context.m_traits = this->m_traits;
                    DEBUG("Enum value " << p << " - " << var.first);
                    Typecheck_Code( mv$(typeck_context), enum_type, e );
                )
            }
        }
    };
}

void Typecheck_Expressions(::HIR::Crate& crate)
{
    OuterVisitor    visitor { crate };
    visitor.visit_crate( crate );
}
