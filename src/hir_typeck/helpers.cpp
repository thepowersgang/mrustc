/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/helpers.cpp
 * - Typecheck helpers
 */
#include "helpers.hpp"
#include <algorithm>

// --------------------------------------------------------------------
// HMTypeInferrence
// --------------------------------------------------------------------
void HMTypeInferrence::dump() const
{
    for(const auto& v : m_ivars) {
        auto i = &v - &m_ivars.front();
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
    }
    for(const auto& v : m_values) {
        auto i = &v - &m_values.front();
        if(v.is_alias()) {
        }
        else {
            DEBUG("V#" << i << " = " << *v.val << FMT_CB(os,
                bool open = false;
                for(const auto& v2 : m_values) {
                    auto i2 = &v2 - &m_values.front();
                    if( v2.is_alias() && v2.alias == i ) {
                        if( !open )
                            os << " { ";
                        open = true;
                        os << "#" << i2 << " ";
                    }
                }
                if(open)
                    os << "}";
            ));
        }
    }
}
void HMTypeInferrence::check_for_loops()
{
#if 1
    struct LoopChecker {
        ::std::vector<unsigned int> m_indexes;
        void check_ty(const HMTypeInferrence& ivars, const ::HIR::TypeRef& ty) {

            visit_ty_with(ty, [&](const HIR::TypeRef& t) {
                if( const auto* ep = t.data().opt_Infer() ) {
                    const auto& e = *ep;
                    for(auto idx : m_indexes)
                        ASSERT_BUG(Span(), e.index != idx, "Recursion in ivar #" << m_indexes.front() << " " << *ivars.m_ivars[m_indexes.front()].type
                            << " - loop with " << idx << " " << *ivars.m_ivars[idx].type);
                    const auto& ivd = ivars.get_pointed_ivar(e.index);
                    assert( !ivd.is_alias() );
                    if( !ivd.type->data().is_Infer() ) {
                        m_indexes.push_back( e.index );
                        this->check_ty(ivars, *ivd.type);
                        m_indexes.pop_back( );
                    }
                }
                return false;
                });
        }
    };
#else
    struct LoopChecker {
        ::std::vector<unsigned int> m_indexes;

        void check_pathparams(const HMTypeInferrence& ivars, const ::HIR::PathParams& pp) {
            for(const auto& ty : pp.m_types)
                this->check_ty(ivars, ty);
        }
        void check_path(const HMTypeInferrence& ivars, const ::HIR::Path& path) {
            TU_MATCH(::HIR::Path::Data, (path.m_data), (pe),
            (Generic,
                this->check_pathparams(ivars, pe.m_params);
                ),
            (UfcsKnown,
                this->check_ty(ivars, pe.type);
                this->check_pathparams(ivars, pe.trait.m_params);
                this->check_pathparams(ivars, pe.params);
                ),
            (UfcsInherent,
                this->check_ty(ivars, pe.type);
                this->check_pathparams(ivars, pe.params);
                ),
            (UfcsUnknown,
                BUG(Span(), "UfcsUnknown");
                )
            )
        }
        void check_ty(const HMTypeInferrence& ivars, const ::HIR::TypeRef& ty) {
            TU_MATCH_HDRA( (ty.data()), {)
            TU_ARMA(Infer, e) {
                for(auto idx : m_indexes)
                    ASSERT_BUG(Span(), e.index != idx, "Recursion in ivar #" << m_indexes.front() << " " << *ivars.m_ivars[m_indexes.front()].type
                        << " - loop with " << idx << " " << *ivars.m_ivars[idx].type);
                const auto& ivd = ivars.get_pointed_ivar(e.index);
                assert( !ivd.is_alias() );
                if( !ivd.type->data().is_Infer() ) {
                    m_indexes.push_back( e.index );
                    this->check_ty(ivars, *ivd.type);
                    m_indexes.pop_back( );
                }
                }
            TU_ARMA(Primitive, e) {
                }
            TU_ARMA(Diverge, e) { }
            TU_ARMA(Generic, e) { }
            TU_ARMA(Path, e) {
                this->check_path(ivars, e.path);
                }
            TU_ARMA(Borrow, e) {
                this->check_ty(ivars, e.inner);
                }
            TU_ARMA(Pointer, e) {
                this->check_ty(ivars, e.inner);
                }
            TU_ARMA(Slice, e) {
                this->check_ty(ivars, e.inner);
                }
            TU_ARMA(Array, e) {
                this->check_ty(ivars, e.inner);
                }
            TU_ARMA(Closure, e) {
                }
            TU_ARMA(Generator, e) {
                }
            TU_ARMA(Function, e) {
                for(const auto& arg : e.m_arg_types) {
                    this->check_ty(ivars, arg);
                }
                this->check_ty(ivars, e.m_rettype);
                }
            TU_ARMA(TraitObject, e) {
                this->check_pathparams(ivars, e.m_trait.m_path.m_params);
                for(const auto& aty : e.m_trait.m_type_bounds)
                    this->check_ty(ivars, aty.second.type);
                for(const auto& marker : e.m_markers) {
                    this->check_pathparams(ivars, marker.m_params);
                }
                }
            TU_ARMA(ErasedType, e) {
                this->check_path(ivars, e.m_origin);
                for(const auto& trait : e.m_traits) {
                    this->check_pathparams(ivars, trait.m_path.m_params);
                    for(const auto& aty : trait.m_type_bounds)
                        this->check_ty(ivars, aty.second.type);
                }
                }
            TU_ARMA(Tuple, e) {
                for(const auto& st : e) {
                    this->check_ty(ivars, st);
                }
                }
            }
        }
    };
#endif
    unsigned int i = 0;
    for(const auto& v : m_ivars)
    {
        if( !v.is_alias() && !v.type->data().is_Infer() )
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
            if(const auto* e = v.type->data().opt_Infer())
            {
                switch(e->ty_class)
                {
                case ::HIR::InferClass::None:
                    break;
                case ::HIR::InferClass::Integer:
                    rv = true;
                    DEBUG("- IVar " << e->index << " = i32");
                    *v.type = ::HIR::TypeRef( ::HIR::CoreType::I32 );
                    break;
                case ::HIR::InferClass::Float:
                    rv = true;
                    DEBUG("- IVar " << e->index << " = f64");
                    *v.type = ::HIR::TypeRef( ::HIR::CoreType::F64 );
                    break;
                }
            }
        }
    }
    return rv;
}

void HMTypeInferrence::print_type(::std::ostream& os, const ::HIR::TypeRef& tr, LList<const ::HIR::TypeRef*> outer_stack) const
{
    const auto& ty = this->get_type(tr);
    for(const auto* pty : outer_stack) {
        if( pty  ) {
            if( pty == &ty ) {
                os << "/*RECURSE*/";
                return ;
            }
        }
    }
    auto stack = LList<const ::HIR::TypeRef*>(&outer_stack, &ty);

    auto print_traitpath = [&](const HIR::TraitPath& tp) {
        os << tp.m_path.m_path;
        this->print_pathparams(os, tp.m_path.m_params, stack);
    };

    TU_MATCH_HDRA( (ty.data()), {)
    TU_ARMA(Infer, e) {
        os << ty;
        }
    TU_ARMA(Primitive, e) {
        os << ty;
        }
    TU_ARMA(Diverge, e) { os << ty; }
    TU_ARMA(Generic, e) { os << ty; }
    TU_ARMA(Path, e) {
        TU_MATCH(::HIR::Path::Data, (e.path.m_data), (pe),
        (Generic,
            os << pe.m_path;
            this->print_pathparams(os, pe.m_params, stack);
            ),
        (UfcsKnown,
            os << "<";
            this->print_type(os, pe.type, stack);
            os << " as " << pe.trait.m_path;
            this->print_pathparams(os, pe.trait.m_params, stack);
            os << ">::" << pe.item;
            this->print_pathparams(os, pe.params, stack);
            ),
        (UfcsInherent,
            os << "<";
            this->print_type(os, pe.type, stack);
            os << ">::" << pe.item;
            this->print_pathparams(os, pe.params, stack);
            ),
        (UfcsUnknown,
            BUG(Span(), "UfcsUnknown");
            )
        )
        }
    TU_ARMA(Borrow, e) {
        os << "&";
        if(e.lifetime != ::HIR::LifetimeRef())
            os << e.lifetime << " ";
        switch(e.type)
        {
        case ::HIR::BorrowType::Shared: os << "";  break;
        case ::HIR::BorrowType::Unique: os << "mut ";  break;
        case ::HIR::BorrowType::Owned:  os << "move "; break;
        }
        this->print_type(os, e.inner, stack);
        }
    TU_ARMA(Pointer, e) {
        switch(e.type)
        {
        case ::HIR::BorrowType::Shared: os << "*const ";  break;
        case ::HIR::BorrowType::Unique: os << "*mut ";  break;
        case ::HIR::BorrowType::Owned:  os << "*move "; break;
        }
        this->print_type(os, e.inner, stack);
        }
    TU_ARMA(Slice, e) {
        os << "[";
        this->print_type(os, e.inner, stack);
        os << "]";
        }
    TU_ARMA(Array, e) {
        os << "[";
        this->print_type(os, e.inner, stack);
        os << "; " << e.size << "]";
        }
    TU_ARMA(Closure, e) {
        os << "{" << e.node << "}(";
        for(const auto& arg : e.m_arg_types) {
            this->print_type(os, arg, stack);
            os << ",";
        }
        os << ")->";
        this->print_type(os, e.m_rettype, stack);
        }
    TU_ARMA(Generator, e) {
        os << "{gen:" << e.node << "}";
        // TODO: Print the types?
        }
    TU_ARMA(Function, e) {
        if(e.is_unsafe)
            os << "unsafe ";
        if(e.m_abi != "")
            os << "extern \"" << e.m_abi << "\" ";
        os << "fn(";
        for(const auto& arg : e.m_arg_types) {
            this->print_type(os, arg, stack);
            os << ",";
        }
        os << ")->";
        this->print_type(os, e.m_rettype, stack);
        }
    TU_ARMA(TraitObject, e) {
        os << "dyn (";
        print_traitpath(e.m_trait);
        for(const auto& marker : e.m_markers) {
            os << "+" << marker.m_path;
            this->print_pathparams(os, marker.m_params, stack);
        }
        if( e.m_lifetime != ::HIR::LifetimeRef::new_static() )
            os << "+" << e.m_lifetime;
        os << ")";
        }
    TU_ARMA(ErasedType, e) {
        // TODO: Print correctly (with print_type calls)
        os << "impl ";
        for(const auto& tr : e.m_traits) {
            if( &tr != &e.m_traits[0] )
                os << "+";
            print_traitpath(tr);
        }
        if( e.m_lifetime != ::HIR::LifetimeRef::new_static() )
            os << "+" << e.m_lifetime;
        os << "/*" << e.m_origin << "#" << e.m_index << "*/";
        }
    TU_ARMA(Tuple, e) {
        os << "(";
        for(const auto& st : e) {
            this->print_type(os, st, stack);
            os << ",";
        }
        os << ")";
        }
    }
}
void HMTypeInferrence::print_pathparams(::std::ostream& os, const ::HIR::PathParams& pps, LList<const ::HIR::TypeRef*> stack) const
{
    if( pps.has_params() ) {
        os << "<";
        for(const auto& pp_t : pps.m_types) {
            this->print_type(os, pp_t, stack);
            os << ",";
        }
        for(const auto& pp_v : pps.m_values) {
            os << pp_v;
            os << ",";
        }
        os << ">";
    }
}

void HMTypeInferrence::expand_ivars(::HIR::TypeRef& type)
{
    thread_local static std::vector<const HIR::TypeRef*>  s_recurse_stack;
    for(const auto* p : s_recurse_stack) {
        if( p == &type ) {
            return ;
        }
    }
    struct _ {
        _(const HIR::TypeRef* ptr) {
            s_recurse_stack.push_back(ptr);
        }
        ~_() {
            s_recurse_stack.pop_back();
        }
    } h(&type);

    struct H {
        static void expand_ivars_path(/*const*/ HMTypeInferrence& self, ::HIR::Path& path)
        {
            TU_MATCH(::HIR::Path::Data, (path.m_data), (e2),
            (Generic,
                self.expand_ivars_params(e2.m_params);
                ),
            (UfcsKnown,
                self.expand_ivars(e2.type);
                self.expand_ivars_params(e2.trait.m_params);
                self.expand_ivars_params(e2.params);
                ),
            (UfcsUnknown,
                self.expand_ivars(e2.type);
                self.expand_ivars_params(e2.params);
                ),
            (UfcsInherent,
                self.expand_ivars(e2.type);
                self.expand_ivars_params(e2.params);
                )
            )
        }
    };
    TU_MATCH_HDRA( (type.data_mut()), {)
    TU_ARMA(Infer, e) {
        const auto& t = this->get_type(type);
        if( &t != &type ) {
            type = t.clone();
        }
        }
    TU_ARMA(Diverge, e) {
        }
    TU_ARMA(Primitive, e) {
        }
    TU_ARMA(Path, e) {
        // Iterate all arguments
        H::expand_ivars_path(*this, e.path);
        }
    TU_ARMA(Generic, e) {
        }
    TU_ARMA(TraitObject, e) {
        this->expand_ivars_params(e.m_trait.m_path.m_params);
        for(auto& marker : e.m_markers)
            this->expand_ivars_params(marker.m_params);
        // TODO: Associated types
        }
    TU_ARMA(ErasedType, e) {
        H::expand_ivars_path(*this, e.m_origin);
        for(auto& trait : e.m_traits)
        {
            this->expand_ivars_params(trait.m_path.m_params);
            // TODO: Associated types
        }
        }
    TU_ARMA(Array, e) {
        this->expand_ivars(e.inner);
        }
    TU_ARMA(Slice, e) {
        this->expand_ivars(e.inner);
        }
    TU_ARMA(Tuple, e) {
        for(auto& ty : e)
            this->expand_ivars(ty);
        }
    TU_ARMA(Borrow, e) {
        this->expand_ivars(e.inner);
        }
    TU_ARMA(Pointer, e) {
        this->expand_ivars(e.inner);
        }
    TU_ARMA(Function, e) {
        this->expand_ivars(e.m_rettype);
        for(auto& ty : e.m_arg_types)
            this->expand_ivars(ty);
        }
    TU_ARMA(Closure, e) {
        this->expand_ivars(e.m_rettype);
        for(auto& ty : e.m_arg_types)
            this->expand_ivars(ty);
        }
    TU_ARMA(Generator, e) {
        }
    }
}
void HMTypeInferrence::expand_ivars_params(::HIR::PathParams& params)
{
    for(auto& arg : params.m_types)
        expand_ivars(arg);
}

void HMTypeInferrence::add_ivars(::HIR::TypeRef& type)
{
    TU_MATCH_HDRA( (type.data_mut()), {)
    TU_ARMA(Infer, e) {
        if( e.index == ~0u ) {
            e.index = this->new_ivar(e.ty_class);
            this->mark_change();
            DEBUG("New ivar " << type);
        }
        }
    TU_ARMA(Diverge, e) {
        }
    TU_ARMA(Primitive, e) {
        }
    TU_ARMA(Path, e) {
        // Iterate all arguments
        TU_MATCH(::HIR::Path::Data, (e.path.m_data), (e2),
        (Generic,
            this->add_ivars_params(e2.m_params);
            ),
        (UfcsKnown,
            this->add_ivars(e2.type);
            this->add_ivars_params(e2.trait.m_params);
            this->add_ivars_params(e2.params);
            ),
        (UfcsUnknown,
            this->add_ivars(e2.type);
            this->add_ivars_params(e2.params);
            ),
        (UfcsInherent,
            this->add_ivars(e2.type);
            this->add_ivars_params(e2.params);
            )
        )
        }
    TU_ARMA(Generic, e) {
        }
    TU_ARMA(TraitObject, e) {
        // Iterate all paths
        this->add_ivars_params(e.m_trait.m_path.m_params);
        for(auto& aty : e.m_trait.m_type_bounds)
            this->add_ivars(aty.second.type);
        for(auto& marker : e.m_markers)
            this->add_ivars_params(marker.m_params);
        }
    TU_ARMA(ErasedType, e) {
        BUG(Span(), "ErasedType getting ivars added");
        }
    TU_ARMA(Array, e) {
        add_ivars(e.inner);
        if( e.size.is_Unevaluated() ) {
            add_ivars(e.size.as_Unevaluated());
        }
        }
    TU_ARMA(Slice, e) {
        add_ivars(e.inner);
        }
    TU_ARMA(Tuple, e) {
        for(auto& ty : e)
            add_ivars(ty);
        }
    TU_ARMA(Borrow, e) {
        add_ivars(e.inner);
        }
    TU_ARMA(Pointer, e) {
        add_ivars(e.inner);
        }
    TU_ARMA(Function, e) {
        add_ivars(e.m_rettype);
        for(auto& ty : e.m_arg_types)
            add_ivars(ty);
        }
    TU_ARMA(Closure, e) {
        // Shouldn't be possible
        }
    TU_ARMA(Generator, e) {
        // Shouldn't be possible
        }
    }
}
void HMTypeInferrence::add_ivars(::HIR::ConstGeneric& val)
{
    if(val.is_Infer()) {
        if(val.as_Infer().index == ~0u) {
            val.as_Infer().index = new_ivar_val();
            this->mark_change();
            DEBUG("New ivar " << val);
        }
    }
}
void HMTypeInferrence::add_ivars_params(::HIR::PathParams& params)
{
    for(auto& arg : params.m_types)
        add_ivars(arg);
    for(auto& arg : params.m_values)
        add_ivars(arg);
}



unsigned int HMTypeInferrence::new_ivar(HIR::InferClass ic/* = HIR::InferClass::None*/)
{
    m_ivars.push_back( IVar() );
    m_ivars.back().type->get_unique().as_Infer().index = m_ivars.size() - 1;
    m_ivars.back().type->get_unique().as_Infer().ty_class = ic;
    return m_ivars.size() - 1;
}
::HIR::TypeRef HMTypeInferrence::new_ivar_tr(HIR::InferClass ic/* = HIR::InferClass::None*/)
{
    return HIR::TypeRef::new_infer(this->new_ivar(ic));
}

unsigned int HMTypeInferrence::new_ivar_val()
{
    m_values.push_back(IVarValue());
    m_values.back().val->as_Infer().index = m_values.size() - 1;
    return m_values.size() - 1;
}
void HMTypeInferrence::set_ivar_val_to(unsigned int slot, ::HIR::ConstGeneric val)
{
    ASSERT_BUG(Span(), slot < m_values.size(), "slot " << slot << " >= " << m_values.size());
    ASSERT_BUG(Span(), !m_values[slot].is_alias(), "slot " << slot);
    if( *m_values[slot].val == val ) {
        //DEBUG("Set ValIVar " << slot << " == " << val);
    }
    else {
        DEBUG("Set ValIVar " << slot << " = " << val);
        ASSERT_BUG(Span(), m_values[slot].val->is_Infer(), "slot " << slot << " - " << *m_values[slot].val);
        ASSERT_BUG(Span(), m_values[slot].val->as_Infer().index == slot, "slot " << slot << " - " << *m_values[slot].val);
        *m_values[slot].val = std::move(val);
    }
}
void HMTypeInferrence::ivar_val_unify(unsigned int left_slot, unsigned int right_slot)
{
    Span    sp;
    ASSERT_BUG(sp, left_slot < m_values.size(), "slot " << left_slot << " >= " << m_values.size());
    ASSERT_BUG(sp, right_slot < m_values.size(), "slot " << left_slot << " >= " << m_values.size());
    ASSERT_BUG(sp, !m_values[left_slot].is_alias(), "slot " << left_slot);
    ASSERT_BUG(sp, !m_values[right_slot].is_alias(), "slot " << right_slot);

    if( const auto* re = m_values[right_slot].val->opt_Infer() )
    {
        DEBUG("Set ValIVar " << right_slot << " = @" << left_slot);
        m_values[right_slot].alias = left_slot;
        m_values[right_slot].val.reset();

        this->mark_change();
    }
    else
    {
        BUG(sp, "Unifiying over a set value");
    }
}

//::HIR::TypeRef& HMTypeInferrence::get_type(::HIR::TypeRef& type)
//{
//    if(const auto* e = type.data().opt_Infer()) {
//        assert(e->index != ~0u);
//        return *get_pointed_ivar(e->index).type;
//    }
//    else {
//        return type;
//    }
//}

const ::HIR::TypeRef& HMTypeInferrence::get_type(const ::HIR::TypeRef& type) const
{
    if(const auto* e = type.data().opt_Infer()) {
        ASSERT_BUG(Span(), e->index != ~0u, "Encountered non-populated IVar");
        return *get_pointed_ivar(e->index).type;
    }
    else {
        return type;
    }
}
::HIR::TypeRef& HMTypeInferrence::get_type(unsigned idx)
{
    assert(idx != ~0u);
    return *get_pointed_ivar(idx).type;
}
const ::HIR::TypeRef& HMTypeInferrence::get_type(unsigned idx) const
{
    assert(idx != ~0u);
    return *get_pointed_ivar(idx).type;
}

void HMTypeInferrence::set_ivar_to(unsigned int slot, ::HIR::TypeRef type)
{
    auto sp = Span();
    auto& root_ivar = this->get_pointed_ivar(slot);
    DEBUG("set_ivar_to(" << slot << " { " << *root_ivar.type << " }, " << type << ")");

    // If the left type was '_', alias the right to it
    if( const auto* l_e = type.data().opt_Infer() ) 
    {
        assert( l_e->index != slot );
        if( l_e->ty_class != ::HIR::InferClass::None ) {
            TU_MATCH_DEF(::HIR::TypeData, (root_ivar.type->data()), (e),
            (
                ERROR(sp, E0000, "Type unificiation of literal with invalid type - " << *root_ivar.type);
                ),
            (Primitive,
                check_type_class_primitive(sp, type, l_e->ty_class, e);
                ),
            (Infer,
                // Check for right having a ty_class
                if( e.ty_class != ::HIR::InferClass::None && e.ty_class != l_e->ty_class ) {
                    ERROR(sp, E0000, "Unifying types with mismatching literal classes - " << type << " := " << *root_ivar.type);
                }
                )
            )
        }

        #if 1
        // Alias `l_e.index` to this slot
        DEBUG("Set IVar " << l_e->index << " = @" << slot);
        auto& r_ivar = this->get_pointed_ivar(l_e->index);
        r_ivar.alias = slot;
        r_ivar.type.reset();
        #else
        DEBUG("Set IVar " << slot << " = @" << l_e->index);
        root_ivar.alias = l_e->index;
        root_ivar.type.reset();
        #endif
    }
    else if( *root_ivar.type == type ) {
        return ;
    }
    else {
        // Otherwise, store left in right's slot
        DEBUG("Set IVar " << slot << " = " << type);
        if(const auto* e = root_ivar.type->data().opt_Infer())
        {
            switch(e->ty_class)
            {
            case ::HIR::InferClass::None:
                break;
            case ::HIR::InferClass::Integer:
            case ::HIR::InferClass::Float:
                // `type` can't be an ivar, so it has to be a primitive (or an associated?)
                if( const auto* l_e = type.data().opt_Primitive() ) {
                    check_type_class_primitive(sp, type, e->ty_class, *l_e);
                }
                else if( type.data().is_Diverge() ) {
                    // ... acceptable
                }
                else {
                    BUG(sp, "Setting primitive to " << type);
                }
                break;
            }
        }
        #if 0
        else TU_IFLET(::HIR::TypeData, root_ivar.type->data(), Diverge, e,
            // Overwriting ! with anything is valid (it's like a magic ivar)
        )
        #endif
        else {
            BUG(sp, "Overwriting ivar " << slot << " (" << *root_ivar.type << ") with " << type);
        }

        root_ivar.type = box$( type );
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

        if( const auto* re = root_ivar.type->data().opt_Infer() )
        {
            DEBUG("Class unify " << *left_ivar.type << " <- " << *root_ivar.type);

            if(re->ty_class != ::HIR::InferClass::None)
            {
                TU_MATCH_DEF(::HIR::TypeData, (left_ivar.type->data_mut()), (le),
                (
                    ERROR(sp, E0000, "Type unificiation of literal with invalid type - " << *left_ivar.type);
                    ),
                (Infer,
                    if( le.ty_class != ::HIR::InferClass::None && le.ty_class != re->ty_class )
                    {
                        ERROR(sp, E0000, "Unifying types with mismatching literal classes - " << *left_ivar.type << " := " << *root_ivar.type);
                    }
                    else
                    {
                    }
                    le.ty_class = re->ty_class;
                    ),
                (Primitive,
                    check_type_class_primitive(sp, *left_ivar.type, re->ty_class, le);
                    )
                )
            }
            else
            {
            }
        }
        else {
            BUG(sp, "Unifying over a concrete type - " << *root_ivar.type);
        }

        DEBUG("IVar " << root_ivar.type->data().as_Infer().index << " = @" << left_slot);
        root_ivar.alias = left_slot;
        root_ivar.type.reset();

        this->mark_change();
    }
}



const ::HIR::ConstGeneric& HMTypeInferrence::get_value(const ::HIR::ConstGeneric& val) const
{
    if(val.is_Infer()) {
        return get_value(val.as_Infer().index);
    }
    else {
        return val;
    }
}
const ::HIR::ConstGeneric& HMTypeInferrence::get_value(unsigned slot) const
{
    ASSERT_BUG(Span(), slot != ~0u, "");
    auto index = slot;
    // Limit the iteration count to the number of ivars
    for(unsigned int count = 0; count < m_values.size(); count ++)
    {
        ASSERT_BUG(Span(), index < m_values.size(), "");
        auto& ent = m_values[index];
        if(!ent.is_alias())
        {
            return *ent.val;
        }
        index = ent.alias;
    }
    this->dump();
    BUG(Span(), "Loop detected in ivar list when starting at " << slot << ", current is " << index);
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
    //TU_MATCH(::HIR::TypeData, (this->get_type(ty).m_data), (e),
    TU_MATCH(::HIR::TypeData, (ty.data()), (e),
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
            if( type_contains_ivars(pe.type) )
                return true;
            if( pathparams_contain_ivars(pe.trait.m_params) )
                return true;
            return pathparams_contain_ivars(pe.params);
            ),
        (UfcsInherent,
            if( type_contains_ivars(pe.type) )
                return true;
            return pathparams_contain_ivars(pe.params);
            ),
        (UfcsUnknown,
            BUG(Span(), "UfcsUnknown");
            )
        )
        ),
    (Borrow,
        return type_contains_ivars(e.inner);
        ),
    (Pointer,
        return type_contains_ivars(e.inner);
        ),
    (Slice,
        return type_contains_ivars(e.inner);
        ),
    (Array,
        return type_contains_ivars(e.inner);
        ),
    (Closure,
        for(const auto& arg : e.m_arg_types)
            if( type_contains_ivars(arg) )
                return true;
        return type_contains_ivars(e.m_rettype);
        ),
    (Generator,
        // Generator types don't contain their own ivars.
        return false;
        ),
    (Function,
        for(const auto& arg : e.m_arg_types)
            if( type_contains_ivars(arg) )
                return true;
        return type_contains_ivars(e.m_rettype);
        ),
    (TraitObject,
        for(const auto& marker : e.m_markers)
            if( pathparams_contain_ivars(marker.m_params) )
                return true;
        return pathparams_contain_ivars(e.m_trait.m_path.m_params);
        ),
    (ErasedType,
        TU_MATCH(::HIR::Path::Data, (e.m_origin.m_data), (pe),
        (Generic,
            return pathparams_contain_ivars(pe.m_params);
            ),
        (UfcsKnown,
            if( type_contains_ivars(pe.type) )
                return true;
            if( pathparams_contain_ivars(pe.trait.m_params) )
                return true;
            return pathparams_contain_ivars(pe.params);
            ),
        (UfcsInherent,
            if( type_contains_ivars(pe.type) )
                return true;
            return pathparams_contain_ivars(pe.params);
            ),
        (UfcsUnknown,
            BUG(Span(), "UfcsUnknown");
            )
        )
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
    if( l.data().tag() != r.data().tag() )
        return false;

    struct H {
        static bool compare_path(const HMTypeInferrence& self, const ::HIR::Path& l, const ::HIR::Path& r) {
            if( l.m_data.tag() != r.m_data.tag() )
                return false;
            TU_MATCH(::HIR::Path::Data, (l.m_data, r.m_data), (lpe, rpe),
            (Generic,
                if( lpe.m_path != rpe.m_path )
                    return false;
                return self.pathparams_equal(lpe.m_params, rpe.m_params);
                ),
            (UfcsKnown,
                if( lpe.item != rpe.item )
                    return false;
                if( ! self.types_equal(lpe.type, rpe.type) )
                    return false;
                if( ! self.pathparams_equal(lpe.trait.m_params, rpe.trait.m_params) )
                    return false;
                return self.pathparams_equal(lpe.params, rpe.params);
                ),
            (UfcsInherent,
                if( lpe.item != rpe.item )
                    return false;
                if( ! self.types_equal(lpe.type, rpe.type) )
                    return false;
                return self.pathparams_equal(lpe.params, rpe.params);
                ),
            (UfcsUnknown,
                BUG(Span(), "UfcsUnknown");
                )
            )
            throw "";
        }
    };

    TU_MATCH(::HIR::TypeData, (l.data(), r.data()), (le, re),
    (Infer, return le.index == re.index; ),
    (Primitive, return le == re; ),
    (Diverge, return true; ),
    (Generic, return le.binding == re.binding; ),
    (Path,
        return H::compare_path(*this, le.path, re.path);
        ),
    (Borrow,
        if( le.type != re.type )
            return false;
        return types_equal(le.inner, re.inner);
        ),
    (Pointer,
        if( le.type != re.type )
            return false;
        return types_equal(le.inner, re.inner);
        ),
    (Slice,
        return types_equal(le.inner, re.inner);
        ),
    (Array,
        if( le.size != re.size )
            return false;
        return types_equal(le.inner, re.inner);
        ),
    (Closure,
        if( !type_list_equal(*this, le.m_arg_types, re.m_arg_types) )
            return false;
        return types_equal(le.m_rettype, re.m_rettype);
        ),
    (Generator,
        return le.node == re.node;
        ),
    (Function,
        if( le.is_unsafe != re.is_unsafe || le.m_abi != re.m_abi )
            return false;
        if( !type_list_equal(*this, le.m_arg_types, re.m_arg_types) )
            return false;
        return types_equal(le.m_rettype, re.m_rettype);
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
        ASSERT_BUG(Span(), le.m_origin != ::HIR::SimplePath(), "Erased type with unset origin");
        return H::compare_path(*this, le.m_origin, re.m_origin);
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
bool TraitResolution::iterate_bounds_traits(const Span& sp, const HIR::TypeRef& type, const HIR::SimplePath& trait, t_cb_bound cb) const {
    return iterate_bounds_traits(sp, type, [&](HIR::Compare cmp, const HIR::TypeRef& t, const HIR::GenericPath& tr, const CachedBound& b) {
        if( tr.m_path != trait )
            return false;
        return cb(cmp, t, tr, b);
        });
}
bool TraitResolution::iterate_bounds_traits(const Span& sp, const HIR::TypeRef& type, t_cb_bound cb) const {
    for(const auto& b : m_trait_bounds)
    {
        auto cmp = b.first.first.compare_with_placeholders(sp, type, this->m_ivars.callback_resolve_infer());
        if( cmp == HIR::Compare::Unequal )
            continue ;
        if( cb(cmp, b.first.first, b.first.second, b.second) )
            return true;
    }
    return false;
}
bool TraitResolution::iterate_bounds_traits(const Span& sp, t_cb_bound cb) const {
    for(const auto& b : m_trait_bounds)
    {
        if( cb(HIR::Compare::Equal, b.first.first, b.first.second, b.second) )
            return true;
    }
    return false;
}
bool TraitResolution::iterate_aty_bounds(const Span& sp, const ::HIR::Path::Data::Data_UfcsKnown& pe, ::std::function<bool(const ::HIR::TraitPath&)> cb) const
{
    ::HIR::GenericPath  trait_path;
    DEBUG("Checking ATY bounds on " << pe.trait << " :: " << pe.item);
    if( !this->trait_contains_type(sp, pe.trait, this->m_crate.get_trait_by_path(sp, pe.trait.m_path), pe.item.c_str(), trait_path) )
        BUG(sp, "Cannot find associated type " << pe.item << " anywhere in trait " << pe.trait);
    DEBUG("trait_path=" << trait_path);
    const auto& trait_ref = m_crate.get_trait_by_path(sp, trait_path.m_path);
    const auto& aty_def = trait_ref.m_types.find(pe.item)->second;

    for(const auto& bound : aty_def.m_trait_bounds)
    {
        if( cb(bound) )
            return true;
    }
    // Search `<Self as Trait>::Name` bounds on the trait itself
    for(const auto& bound : trait_ref.m_params.m_bounds)
    {
        if( ! bound.is_TraitBound() ) continue ;
        const auto& be = bound.as_TraitBound();

        if( ! be.type.data().is_Path() )   continue ;
        if( ! be.type.data().as_Path().binding.is_Opaque() )   continue ;

        const auto& be_type_pe = be.type.data().as_Path().path.m_data.as_UfcsKnown();
        if( be_type_pe.type != ::HIR::TypeRef("Self", GENERIC_Self) )
            continue ;
        if( be_type_pe.trait.m_path != pe.trait.m_path )
            continue ;
        if( be_type_pe.item != pe.item )
            continue ;

        if( cb(be.trait) )
            return true;
    }

    return false;
}

bool TraitResolution::find_trait_impls_magic(const Span& sp,
        const ::HIR::SimplePath& trait, const ::HIR::PathParams& params,
        const ::HIR::TypeRef& ty,
        t_cb_trait_impl_r callback
        ) const
{
    static ::HIR::PathParams    null_params;
    static ::HIR::TraitPath::assoc_list_t   null_assoc;

    const auto& lang_CoerceUnsized = this->m_crate.get_lang_item_path(sp, "coerce_unsized");

    const auto& type = this->m_ivars.get_type(ty);
    TRACE_FUNCTION_F("trait = " << trait << params  << ", type = " << type);

    if( trait == m_lang_Sized ) {
        auto cmp = type_is_sized(sp, type);
        if( cmp != ::HIR::Compare::Unequal ) {
            return callback( ImplRef(&type, &null_params, &null_assoc), cmp );
        }
        else {
            return false;
        }
    }

    if( trait == m_lang_Copy ) {
        auto cmp = this->type_is_copy(sp, type);
        if( cmp != ::HIR::Compare::Unequal ) {
            return callback( ImplRef(&type, &null_params, &null_assoc), cmp );
        }
        else {
            return false;
        }
    }

    if( TARGETVER_LEAST_1_29 && trait == m_lang_Clone )
    {
        auto cmp = this->type_is_clone(sp, type);
        if( cmp != ::HIR::Compare::Unequal ) {
            return callback( ImplRef(&type, &null_params, &null_assoc), cmp );
        }
        else {
            return false;
        }
    }

    // Generator
    if( TARGETVER_LEAST_1_39 && trait == m_lang_Generator )
    {
        if( const auto* ty_e = type.data().opt_Generator() )
        {
            ::HIR::TraitPath::assoc_list_t   assoc;
            assoc.insert(::std::make_pair("Yield" , ::HIR::TraitPath::AtyEqual { trait.clone(), ty_e->node->m_yield_ty.clone() }));
            assoc.insert(::std::make_pair("Return", ::HIR::TraitPath::AtyEqual { trait.clone(), ty_e->node->m_return.clone() }));
            return callback( ImplRef(type.clone(), HIR::PathParams(), mv$(assoc)), ::HIR::Compare::Equal );
        }
    }

    // - `DiscriminantKind`
    if( TARGETVER_LEAST_1_54 && trait == this->m_crate.get_lang_item_path(sp, "discriminant_kind") )
    {
        static auto name_Discriminant = RcString::new_interned("Discriminant");
        // TODO: This logic is near identical to the logic in `static.cpp` - can it be de-duplicated?

        if( type.data().is_Infer() || (type.data().is_Path() && type.data().as_Path().binding.is_Unbound()) ) {
            // TODO: How to prevent EAT from expanding (or setting opaque) too early?
            return callback( ImplRef(type.clone(), HIR::PathParams(), ::HIR::TraitPath::assoc_list_t()), ::HIR::Compare::Fuzzy );
        }
        else if( type.data().is_Generic() || (type.data().is_Path() && type.data().as_Path().binding.is_Opaque()) ) {
            ::HIR::TraitPath::assoc_list_t   assoc_list;
            assoc_list.insert(std::make_pair( name_Discriminant, HIR::TraitPath::AtyEqual {
                trait,
                HIR::TypeRef::new_path(
                    HIR::Path(type.clone(), trait.clone(), name_Discriminant),
                    HIR::TypePathBinding::make_Opaque({})
                    )
                } ));
            return callback( ImplRef(type.clone(), HIR::PathParams(), ::HIR::TraitPath::assoc_list_t()), ::HIR::Compare::Equal );
            //return false;
        }
        else if( type.data().is_Path() && type.data().as_Path().binding.is_Enum() ) {
            const auto& enm = *type.data().as_Path().binding.as_Enum();
            HIR::TypeRef    tag_ty = enm.get_repr_type(enm.m_tag_repr);
            ::HIR::TraitPath::assoc_list_t   assoc_list;
            assoc_list.insert(std::make_pair( name_Discriminant, HIR::TraitPath::AtyEqual {
                trait,
                std::move(tag_ty)
                } ));
            return callback(ImplRef(type.clone(), {}, std::move(assoc_list)), ::HIR::Compare::Equal);
        }
        else {
            ::HIR::TraitPath::assoc_list_t   assoc_list;
            assoc_list.insert(std::make_pair( name_Discriminant, HIR::TraitPath::AtyEqual {
                trait,
                HIR::TypeRef::new_unit()
                } ));
            return callback(ImplRef(type.clone(), {}, std::move(assoc_list)), ::HIR::Compare::Equal);
        }
    }
    if( TARGETVER_LEAST_1_54 && trait == this->m_crate.get_lang_item_path(sp, "pointee_trait") )
    {
        static auto name_Metadata = RcString::new_interned("Metadata");
        // TODO: This logic is near identical to the logic in `static.cpp` - can it be de-duplicated?

        HIR::TypeRef    meta_ty;
        if( type.data().is_Infer() || (type.data().is_Path() && type.data().as_Path().binding.is_Unbound()) ) {
            return callback( ImplRef(type.clone(), HIR::PathParams(), ::HIR::TraitPath::assoc_list_t()), ::HIR::Compare::Fuzzy );
        }
        // Generics (or opaque ATYs)
        if( type.data().is_Generic() || (type.data().is_Path() && type.data().as_Path().binding.is_Opaque()) ) {
            // If the type is `Sized` return `()` as the type
            if( type_is_sized(sp, type) ) {
                meta_ty = HIR::TypeRef::new_unit();
            }
            else {
                // Return unbounded
                // - leave as `_`
            }
        }
        // Trait object: `Metadata=DynMetadata<T>`
        if( type.data().is_TraitObject() ) {
            meta_ty = ::HIR::TypeRef::new_path(
                ::HIR::GenericPath(this->m_crate.get_lang_item_path(sp, "dyn_metadata"), HIR::PathParams(type.clone())),
                &m_crate.get_struct_by_path(sp, this->m_crate.get_lang_item_path(sp, "dyn_metadata"))
                );
        }
        // Slice and str
        if( type.data().is_Slice() || TU_TEST1(type.data(), Primitive, == HIR::CoreType::Str) ) {
            meta_ty = HIR::CoreType::Usize;
        }
        // Structs: Can delegate their metadata
        if( type.data().is_Path() && type.data().as_Path().binding.is_Struct() )
        {
            const auto& str = *type.data().as_Path().binding.as_Struct();
            switch(str.m_struct_markings.dst_type)
            {
            case HIR::StructMarkings::DstType::None:
                meta_ty = HIR::TypeRef::new_unit();
                break;
            case HIR::StructMarkings::DstType::Possible:
                TODO(sp, "m_lang_Pointee - " << type);
            case HIR::StructMarkings::DstType::Slice:
                meta_ty = HIR::CoreType::Usize;
                break;
            case HIR::StructMarkings::DstType::TraitObject:
                TODO(sp, "m_lang_Pointee - " << type);
            }
        }
        ::HIR::TraitPath::assoc_list_t  assoc_list;
        if(meta_ty != HIR::TypeRef()) {
            assoc_list.insert(std::make_pair( RcString::new_interned("Metadata"), HIR::TraitPath::AtyEqual { trait, mv$(meta_ty) } ));
        }

        return callback( ImplRef(type.clone(), {}, std::move(assoc_list)), ::HIR::Compare::Equal );
    }

    // Magic Unsize impls to trait objects
    if( trait == m_lang_Unsize )
    {
        ASSERT_BUG(sp, params.m_types.size() == 1, "Unsize trait requires a single type param");
        const auto& dst_ty = this->m_ivars.get_type(params.m_types[0]);

        if( find_trait_impls_bound(sp, trait, params, type, callback) )
            return true;

        bool rv = false;
        auto cb = [&](auto new_dst) {
            ::HIR::PathParams   real_params { mv$(new_dst) };
            rv = callback( ImplRef(type.clone(), mv$(real_params), {}), ::HIR::Compare::Fuzzy );
            };
        //if( dst_ty.data().is_Infer() || type.data().is_Infer() )
        //{
        //    rv = callback( ImplRef(type.clone(), params.clone(), {}), ::HIR::Compare::Fuzzy );
        //    return rv;
        //}
        auto cmp = this->can_unsize(sp, dst_ty, type, cb);
        if( cmp == ::HIR::Compare::Equal )
        {
            assert(!rv);
            rv = callback( ImplRef(type.clone(), params.clone(), {}), ::HIR::Compare::Equal );
        }
        return rv;
    }

    // Magical CoerceUnsized impls for various types
    if( trait == lang_CoerceUnsized ) {
        if( find_trait_impls_bound(sp, trait, params, type, callback) )
            return true;

        const auto& dst_ty = params.m_types.at(0);
        // - `*mut T => *const T`
        if( const auto* e = type.data().opt_Pointer() )
        {
            if( const auto* de = dst_ty.data().opt_Pointer() )
            {
                if( de->type < e->type ) {
                    auto cmp = e->inner.compare_with_placeholders(sp, de->inner, this->m_ivars.callback_resolve_infer());
                    if( cmp != ::HIR::Compare::Unequal )
                    {
                        ::HIR::PathParams   pp;
                        pp.m_types.push_back( dst_ty.clone() );
                        if( callback( ImplRef(type.clone(), mv$(pp), {}), cmp ) ) {
                            return true;
                        }
                    }
                }
            }
        }
    }

    return false;
}

bool TraitResolution::find_trait_impls(const Span& sp,
        const ::HIR::SimplePath& trait, const ::HIR::PathParams& params,
        const ::HIR::TypeRef& ty,
        t_cb_trait_impl_r callback,
        bool magic_trait_impls /*=true*/
        ) const
{
    static ::HIR::PathParams    null_params;
    static ::HIR::TraitPath::assoc_list_t   null_assoc;

    const auto& type = this->m_ivars.get_type(ty);
    TRACE_FUNCTION_F("trait = " << trait << params  << ", type = " << type);

#if 0
    if( const auto* te = type.data().opt_Infer() )
    {
        if( !te->is_lit() ) {
            // NOTE: Can't hope to find an impl if we know nothing about the type.
            return false;
        }
    }
#endif

    //const auto& trait_index = this->m_crate.get_lang_item_path(sp, "index");
    //const auto& trait_indexmut = this->m_crate.get_lang_item_path(sp, "index_mut");

    if( magic_trait_impls )
    {
        if( find_trait_impls_magic(sp, trait, params, ty, callback) ) {
            return true;
        }
    }

    TU_MATCH_HDRA( (type.data()), {)
    default:
        break;
    // Magic impls of the Fn* traits for closure types
    TU_ARMA(Closure, e) {
        DEBUG("Closure, " << trait << " ?= Fn*");
        if( trait == m_lang_Fn || trait == m_lang_FnMut || trait == m_lang_FnOnce ) {
            if( params.m_types.size() != 1 )
                BUG(sp, "Fn* traits require a single tuple argument");
            if( !params.m_types[0].data().is_Tuple() )
                BUG(sp, "Fn* traits require a single tuple argument");

            const auto& args_des = params.m_types[0].data().as_Tuple();
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
                ::HIR::TraitPath::assoc_list_t  types;
                types.insert( ::std::make_pair( "Output", ::HIR::TraitPath::AtyEqual { ::HIR::GenericPath(m_lang_FnOnce, pp.clone()), e.m_rettype.clone()} ) );
                return callback( ImplRef(type.clone(), mv$(pp), mv$(types)), cmp );
            }
            else
            {
                DEBUG("Closure Fn* impl - cmp = Compare::Unequal");
                return false;
            }
        }
        }
    // Magic Fn* trait impls for function pointers
    TU_ARMA(Function, e) {
        if( trait == m_lang_Fn || trait == m_lang_FnMut || trait == m_lang_FnOnce ) {
            if( params.m_types.size() != 1 )
                BUG(sp, "Fn* traits require a single tuple argument");
            if( !params.m_types[0].data().is_Tuple() )
                BUG(sp, "Fn* traits require a single tuple argument");
            const auto& args_des = params.m_types[0].data().as_Tuple();
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
            ::HIR::TraitPath::assoc_list_t  types;
            types.insert( ::std::make_pair( "Output", ::HIR::TraitPath::AtyEqual { ::HIR::GenericPath(m_lang_FnOnce, pp.clone()), e.m_rettype.clone()} ) );
            return callback( ImplRef(type.clone(), mv$(pp), mv$(types)), cmp );
        }
        }
    // Magic index and unsize impls for Arrays
    // NOTE: The index impl for [T] is in libcore.
    TU_ARMA(Array, e) {
#if 0
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
                DEBUG("- Magic impl of Index<usize> for " << type);
                ::HIR::PathParams   pp;
                pp.m_types.push_back( mv$(ty_usize) );
                ::std::map<RcString, ::HIR::TypeRef>  types;
                types.insert( ::std::make_pair( "Output", e.inner.clone() ) );
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
                types.insert(::std::make_pair( "Output", ::HIR::TypeRef::new_slice(e.inner.clone()) ));
                return callback( ImplRef(type.clone(), mv$(pp), mv$(types)), cmp );
            )
            */
            //return false;
        }
#endif
        }
    // Trait objects automatically implement their own traits
    // - IF object safe (TODO)
    TU_ARMA(TraitObject, e) {
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
                    return callback( ImplRef(&type, &mt.m_params, &null_assoc), cmp );
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
                    ::HIR::TraitPath::assoc_list_t assoc_clone;
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
        } // TU_ARMA(TraitObject, e)
    TU_ARMA(ErasedType, e) {
        for( const auto& trait_path : e.m_traits )
        {
            if( trait == trait_path.m_path.m_path ) {
                auto cmp = compare_pp(sp, trait_path.m_path.m_params, params);
                if( cmp != ::HIR::Compare::Unequal ) {
                    DEBUG("TraitObject impl params" << trait_path.m_path.m_params);
                    return callback( ImplRef(&type, &trait_path.m_path.m_params, &trait_path.m_type_bounds), cmp );
                }
            }

            // - Check if the desired trait is a supertrait of this.
            // NOTE: `params` (aka des_params) is not used (TODO)
            bool rv = false;
            bool is_supertrait = this->find_named_trait_in_trait(sp, trait,params, *trait_path.m_trait_ptr, trait_path.m_path.m_path,trait_path.m_path.m_params, type,
                [&](const auto& i_ty, const auto& i_params, const auto& i_assoc) {
                    // The above is just the monomorphised params and associated set. Comparison is still needed.
                    auto cmp = this->compare_pp(sp, i_params, params);
                    if( cmp != ::HIR::Compare::Unequal ) {
                        // Invoke callback with a proper ImplRef
                        ::HIR::TraitPath::assoc_list_t assoc_clone;
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
        }
        } // TU_ARMA(ErasedType)
    // If the type in question is a magic placeholder, return a placeholder impl :)
    TU_ARMA(Generic, e) {
        if( (e.binding >> 8) == 2 )
        {
            // TODO: This is probably going to break something in the future.
            DEBUG("- Magic impl for placeholder type");
            return callback( ImplRef(&type, &null_params, &null_assoc), ::HIR::Compare::Fuzzy );
        }
        } // TU_ARMA(Generic)
    // If this type is an opaque UfcsKnown - check bounds
    TU_ARMA(Path, e) {
        if( e.binding.is_Opaque() )
        {
            ASSERT_BUG(sp, e.path.m_data.is_UfcsKnown(), "Opaque bound type wasn't UfcsKnown - " << type);
            const auto& pe = e.path.m_data.as_UfcsKnown();

            // TODO: Should Self here be `type` or `pe.type`
            // - Depends... if implicit it should be `type` (as it relates to the associated type), but if explicit it's referring to the trait
            auto monomorph_cb = MonomorphStatePtr(&pe.type, &pe.trait.m_params, nullptr);
            auto rv = this->iterate_aty_bounds(sp, pe, [&](const auto& bound) {
                DEBUG("Bound on ATY: " << bound);
                const auto& b_params = bound.m_path.m_params;
                ::HIR::PathParams   params_mono_o;
                const auto& b_params_mono = (monomorphise_pathparams_needed(b_params) ? params_mono_o = monomorph_cb.monomorph_path_params(sp, b_params, false) : b_params);
                // TODO: Monormophise and EAT associated types
                ::HIR::TraitPath::assoc_list_t  b_atys;
                for(const auto& aty : bound.m_type_bounds)
                    b_atys.insert(::std::make_pair( aty.first, ::HIR::TraitPath::AtyEqual { monomorph_cb.monomorph_genericpath(sp, aty.second.source_trait, false), monomorph_cb.monomorph_type(sp, aty.second.type) } ));

                if( bound.m_path.m_path == trait )
                {
                    auto cmp = this->compare_pp(sp, b_params_mono, params);
                    if( cmp != ::HIR::Compare::Unequal )
                    {
                        if( &b_params_mono == &params_mono_o )
                        {
                            // TODO: assoc bounds
                            if( callback( ImplRef(type.clone(), mv$(params_mono_o), mv$(b_atys)), cmp ) )
                                return true;
                            params_mono_o = monomorph_cb.monomorph_path_params(sp, b_params, false);
                        }
                        else if( !b_atys.empty() )
                        {
                            if( callback( ImplRef(type.clone(), b_params_mono.clone(), mv$(b_atys)), cmp ) )
                                return true;
                        }
                        else
                        {
                            if( callback( ImplRef(&type, &bound.m_path.m_params, &null_assoc), cmp ) )
                                return true;
                        }
                    }
                }

                bool rv = false;
                bool ret = this->find_named_trait_in_trait(sp,  trait, params,  *bound.m_trait_ptr,  bound.m_path.m_path, b_params_mono, type,
                    [&](const auto& i_ty, const auto& i_params, const auto& i_assoc) {
                        auto cmp = this->compare_pp(sp, i_params, params);
                        DEBUG("cmp=" << cmp << ", impl " << trait << i_params << " for " << i_ty << " -- desired " << trait << params);
                        rv |= (cmp != ::HIR::Compare::Unequal && callback( ImplRef(i_ty.clone(), i_params.clone(), {}), cmp ));
                        return true;    // NOTE: actually ignored?
                    });
                if( ret )
                {
                    // NOTE: Callback called in closure's return statement
                    return rv;
                }
                return false;
                });
            if( rv )
                return true;
        }
        } // TU_ARMA(Path)
    } // TU_MATCH_HDRA

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
                if( r.has_associated_type(e2.type) )
                    return true;
                if( H::check_pathparams(r, e2.params) )
                    return true;
                return false;
                ),
            (UfcsKnown,
                if( r.has_associated_type(e2.type) )
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
    TU_MATCH_HDRA( (input.data()), {)
    TU_ARMA(Infer, e) {
        const auto& ty = this->m_ivars.get_type(input);
        if( ty != input ) {
            return this->has_associated_type(ty);
        }
        return false;
        }
    TU_ARMA(Diverge, e) {
        return false;
        }
    TU_ARMA(Primitive, e) {
        return false;
        }
    TU_ARMA(Path, e) {
        // Unbounded UfcsKnown returns true (bound is false)
        if( e.path.m_data.is_UfcsKnown() && e.binding.is_Unbound() )
            return true;
        return H::check_path(*this, e.path);
        }
    TU_ARMA(Generic, e) {
        return false;
        }
    TU_ARMA(TraitObject, e) {
        // Recurse?
        if( H::check_pathparams(*this, e.m_trait.m_path.m_params) )
            return true;
        for(const auto& m : e.m_markers) {
            if( H::check_pathparams(*this, m.m_params) )
                return true;
        }
        return false;
        }
    TU_ARMA(ErasedType, e) {
        if( H::check_path(*this, e.m_origin) )
            return true;
        for(const auto& m : e.m_traits) {
            if( H::check_pathparams(*this, m.m_path.m_params) )
                return true;
        }
        return false;
        }
    TU_ARMA(Array, e) {
        return has_associated_type(e.inner);
        }
    TU_ARMA(Slice, e) {
        return has_associated_type(e.inner);
        }
    TU_ARMA(Tuple, e) {
        bool rv = false;
        for(const auto& sub : e) {
            rv |= has_associated_type(sub);
        }
        return rv;
        }
    TU_ARMA(Borrow, e) {
        return has_associated_type(e.inner);
        }
    TU_ARMA(Pointer, e) {
        return has_associated_type(e.inner);
        }
    TU_ARMA(Function, e) {
        // Recurse?
        return false;
        }
    TU_ARMA(Closure, e) {
        // Recurse?
        return false;
        }
    TU_ARMA(Generator, e) {
        // Recurse?
        return false;
        }
    }
    BUG(Span(), "Fell off the end of has_associated_type - input=" << input);
}
void TraitResolution::expand_associated_types_inplace(const Span& sp, ::HIR::TypeRef& input, LList<const ::HIR::TypeRef*> stack) const
{
    struct H {
        static void expand_associated_types_params(const Span& sp, const TraitResolution& res, ::HIR::PathParams& params, LList<const ::HIR::TypeRef*> stack)
        {
            for(auto& arg : params.m_types)
                res.expand_associated_types_inplace(sp, arg, stack);
        }
        static void expand_associated_types_tp(const Span& sp, const TraitResolution& res, ::HIR::TraitPath& input, LList<const ::HIR::TypeRef*> stack)
        {
            expand_associated_types_params(sp, res, input.m_path.m_params, stack);
            for(auto& arg : input.m_type_bounds)
            {
                expand_associated_types_params(sp, res, arg.second.source_trait.m_params, stack);
                res.expand_associated_types_inplace(sp, arg.second.type, stack);
            }
            for(auto& arg : input.m_trait_bounds)
            {
                expand_associated_types_params(sp, res, arg.second.source_trait.m_params, stack);
                for(auto& t : arg.second.traits)
                    expand_associated_types_tp(sp, res, t, stack);
            }
        }
    };
    for(const auto& ty : m_eat_active_stack)
    {
        if( input == *ty ) {
            DEBUG("Recursive lookup, skipping - &input = " << &input);
            return ;
        }
    }
    //TRACE_FUNCTION_F(input);
    TU_MATCH_HDRA( (input.data_mut()), {)
    TU_ARMA(Infer, e) {
        auto& ty = this->m_ivars.get_type(input);
        if( ty != input ) {
            input = ty.clone();
            expand_associated_types_inplace(sp, input, stack);
        }
        }
    TU_ARMA(Diverge, e) {
        }
    TU_ARMA(Primitive, e) {
        }
    TU_ARMA(Path, e) {
        TU_MATCH_HDRA( (e.path.m_data), {)
        TU_ARMA(Generic, pe) {
            H::expand_associated_types_params(sp, *this, pe.m_params, stack);
            }
        TU_ARMA(UfcsInherent, pe) {
            expand_associated_types_inplace(sp, pe.type, stack);
            H::expand_associated_types_params(sp, *this, pe.params, stack);
            // TODO: only valid for enum variants? (and only in some contexts)
            if( TU_TEST1(pe.type.data(), Path, .binding.is_Enum()) )
            {
                return ;
            }
            TODO(sp, "Path - UfcsInherent - " << e.path);
            }
        TU_ARMA(UfcsKnown, pe) {
            struct D {
                const TraitResolution&  m_tr;
                D(const TraitResolution& tr, ::HIR::TypeRef v): m_tr(tr) {
                    tr.m_eat_active_stack.push_back( box$(v) );
                }
                ~D() {
                    m_tr.m_eat_active_stack.pop_back();
                }
                D(D&&) = delete;
                D(const D&) = delete;
            };
            D   _(*this, input.clone());
            // State stack to avoid infinite recursion
            assert(m_eat_active_stack.size() > 0);
            auto& prev_stack = stack;
            LList<const ::HIR::TypeRef*>    stack(&prev_stack, m_eat_active_stack.back().get());

            expand_associated_types_inplace(sp, pe.type, stack);
            H::expand_associated_types_params(sp, *this, pe.params, stack);
            H::expand_associated_types_params(sp, *this, pe.trait.m_params, stack);
            // - Only try resolving if the binding isn't known
            if( e.binding.is_Unbound() )
            {
                this->expand_associated_types_inplace__UfcsKnown(sp, input, stack);
            }
            }
        TU_ARMA(UfcsUnknown, pe) {
            BUG(sp, "Encountered UfcsUnknown");
            }
        }
        }
    TU_ARMA(Generic, e) {
        }
    TU_ARMA(TraitObject, e) {
        // Recurse?
        H::expand_associated_types_tp(sp, *this, e.m_trait, stack);
        for(auto& m : e.m_markers)
            H::expand_associated_types_params(sp, *this, m.m_params, stack);
        }
    TU_ARMA(ErasedType, e) {
        // Recurse?
        }
    TU_ARMA(Array, e) {
        expand_associated_types_inplace(sp, e.inner, stack);
        }
    TU_ARMA(Slice, e) {
        expand_associated_types_inplace(sp, e.inner, stack);
        }
    TU_ARMA(Tuple, e) {
        for(auto& sub : e) {
            expand_associated_types_inplace(sp, sub , stack);
        }
        }
    TU_ARMA(Borrow, e) {
        expand_associated_types_inplace(sp, e.inner, stack);
        }
    TU_ARMA(Pointer, e) {
        expand_associated_types_inplace(sp, e.inner, stack);
        }
    TU_ARMA(Function, e) {
        for(auto& ty : e.m_arg_types)
            expand_associated_types_inplace(sp, ty, stack);
        expand_associated_types_inplace(sp, e.m_rettype, stack);
        }
    TU_ARMA(Closure, e) {
        // Recurse?
        }
    TU_ARMA(Generator, e) {
        // Recurse?
        }
    }
}


void TraitResolution::expand_associated_types_inplace__UfcsKnown(const Span& sp, ::HIR::TypeRef& input, LList<const ::HIR::TypeRef*> stack) const
{
    TRACE_FUNCTION_FR("input=" << input, input);
    auto& e = input.data_mut().as_Path();
    auto& pe = e.path.m_data.as_UfcsKnown();

    expand_associated_types_inplace(sp, pe.type, stack);
    for(auto& ty : pe.trait.m_params.m_types)
        expand_associated_types_inplace(sp, ty, stack);

    // Ignore unbounder infer literals
    if( pe.type.data().is_Infer() && !pe.type.data().as_Infer().is_lit() )
    {
        return ;
    }
    // ATYs of placeholders are kept as unknown
    if( pe.type.data().is_Generic() && pe.type.data().as_Generic().is_placeholder() )
    {
        return ;
    }

    // If there are impl params present, return early
    // TODO: There is still information available for placeholders (if the impl block is available)
    {
        auto cb = [](const ::HIR::TypeRef& ty){ return !( ty.data().is_Generic() && ty.data().as_Generic().is_placeholder() ); };
        bool has_impl_placeholders = false;
        if( !visit_ty_with(pe.type, cb) )
            has_impl_placeholders = true;
        for(const auto& ty : pe.trait.m_params.m_types)
            if( !visit_ty_with(ty, cb) )
                has_impl_placeholders = true;
        if( has_impl_placeholders )
        {
            // TODO: Why opaque? Like ivars, these could resolve in the future.
            e.binding = ::HIR::TypePathBinding::make_Opaque({});
            return ;
        }
    }

    // Special type-specific rules
    TU_MATCH_HDRA( (pe.type.data()), {)
    default:
        // No special handling
    // - If it's a closure, then the only trait impls are those generated by typeck
    TU_ARMA(Closure, te) {
        if( pe.trait.m_path == m_lang_Fn || pe.trait.m_path == m_lang_FnMut || pe.trait.m_path == m_lang_FnOnce  ) {
            if( pe.item == "Output" ) {
                input = te.m_rettype.clone();
                return ;
            }
            else {
                ERROR(sp, E0000, "No associated type " << pe.item << " for trait " << pe.trait);
            }
        }
        // TODO: Fall through? Maybe there's a generic impl that could match.
        }
    TU_ARMA(Generator, te) {
        if( pe.trait.m_path == this->m_lang_Generator )
        {
            if( pe.item == "Return" ) {
                input = te.node->m_return.clone();
                return ;
            }
            else if( pe.item == "Yield" ) {
                input = te.node->m_yield_ty.clone();
                return ;
            }
            else {
                ERROR(sp, E0000, "No associated type " << pe.item << " for trait " << pe.trait);
            }
        }
        // Fall through for generic impls
        }
    TU_ARMA(Function, te) {
        if( te.m_abi == ABI_RUST && !te.is_unsafe )
        {
            if( pe.trait.m_path == m_lang_Fn || pe.trait.m_path == m_lang_FnMut || pe.trait.m_path == m_lang_FnOnce  ) {
                if( pe.item == "Output" ) {
                    input = te.m_rettype.clone();
                    return ;
                }
                else {
                    ERROR(sp, E0000, "No associated type " << pe.item << " for trait " << pe.trait);
                }
            }
        }
        }
    // If it's a TraitObject, then maybe we're asking for a bound
    TU_ARMA(TraitObject, te) {
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
                    TODO(sp, "Handle unconstrained associate type " << pe.item << " from " << pe.type);
                }

                input = it->second.type.clone();
                return ;
            }
        }

        // - Check if the desired trait is a supertrait of this.
        // NOTE: `params` (aka des_params) is not used (TODO)
        bool is_supertrait = this->find_named_trait_in_trait(sp, pe.trait.m_path,pe.trait.m_params, *te.m_trait.m_trait_ptr, data_trait.m_path,data_trait.m_params, pe.type,
            [&](const auto& i_ty, const auto& i_params, const auto& i_assoc) {
                // The above is just the monomorphised params and associated set. Comparison is still needed.
                auto cmp = this->compare_pp(sp, i_params, pe.trait.m_params);
                if( cmp != ::HIR::Compare::Unequal ) {
                    auto it = i_assoc.find( pe.item );
                    if( it != i_assoc.end() ) {
                        input = it->second.type.clone();
                        return true;
                    }
                    // NOTE: (currently) there can only be one trait with this name, so if we found this trait and the item is present - good.
                    it = te.m_trait.m_type_bounds.find( pe.item );
                    if( it != te.m_trait.m_type_bounds.end() ) {
                        input = it->second.type.clone();
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
        }
    // If it's a ErasedType, then maybe we're asking for a bound
    TU_ARMA(ErasedType, te) {
        for( const auto& trait : te.m_traits )
        {
            const auto& trait_gp = trait.m_path;
            if( pe.trait.m_path == trait_gp.m_path ) {
                auto cmp = ::HIR::Compare::Equal;
                if( pe.trait.m_params.m_types.size() != trait_gp.m_params.m_types.size() )
                {
                    cmp = ::HIR::Compare::Unequal;
                }
                else
                {
                    for(unsigned int i = 0; i < pe.trait.m_params.m_types.size(); i ++)
                    {
                        const auto& l = pe.trait.m_params.m_types[i];
                        const auto& r = trait_gp.m_params.m_types[i];
                        cmp &= l.compare_with_placeholders(sp, r, m_ivars.callback_resolve_infer());
                    }
                }
                if( cmp != ::HIR::Compare::Unequal )
                {
                    auto it = trait.m_type_bounds.find( pe.item );
                    if( it == trait.m_type_bounds.end() ) {
                        // TODO: Mark as opaque and return.
                        // - Why opaque? It's not bounded, don't even bother
                        TODO(sp, "Handle unconstrained associate type " << pe.item << " from " << pe.type);
                    }

                    input = it->second.type.clone();
                    return ;
                }
            }

            // - Check if the desired trait is a supertrait of this.
            // NOTE: `params` (aka des_params) is not used (TODO)
            bool is_supertrait = this->find_named_trait_in_trait(sp, pe.trait.m_path,pe.trait.m_params, *trait.m_trait_ptr, trait_gp.m_path,trait_gp.m_params, pe.type,
                [&](const auto& i_ty, const auto& i_params, const auto& i_assoc) {
                    // The above is just the monomorphised params and associated set. Comparison is still needed.
                    auto cmp = this->compare_pp(sp, i_params, pe.trait.m_params);
                    if( cmp != ::HIR::Compare::Unequal ) {
                        auto it = i_assoc.find( pe.item );
                        if( it != i_assoc.end() ) {
                            input = it->second.type.clone();
                            return true;
                        }
                        // NOTE: (currently) there can only be one trait with this name, so if we found this trait and the item is present - good.
                        it = trait.m_type_bounds.find( pe.item );
                        if( it != trait.m_type_bounds.end() ) {
                            input = it->second.type.clone();
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
        }
        }
    }

    // Search for the actual trait containing this associated type
    ::HIR::GenericPath  trait_path;
    if( !this->trait_contains_type(sp, pe.trait, this->m_crate.get_trait_by_path(sp, pe.trait.m_path), pe.item.c_str(), trait_path) )
        BUG(sp, "Cannot find associated type " << pe.item << " anywhere in trait " << pe.trait);
    //pe.trait = mv$(trait_path);

    // 1. Bounds
    bool rv = false;
    enum class ResultType {
        Opaque,
        LeaveUnbound,
        Recurse,
    } result_type = ResultType::Opaque;

    if(!rv)
    {
        auto it = m_type_equalities.find(input);
        if( it != m_type_equalities.end() )
        {
            result_type = ResultType::Recurse;
            input = it->second.clone();
            rv = true;
        }
    }
    if(!rv)
    {
        rv = this->iterate_bounds_traits(sp, pe.type, trait_path.m_path, [&](HIR::Compare _cmp, const ::HIR::TypeRef& bound_type, const ::HIR::GenericPath& bound_trait, const CachedBound& bound_info)->bool {
            DEBUG("[expand_associated_types_inplace__UfcsKnown] Trait bound - " << bound_type << " : " << bound_trait);
            // 2. Check if the trait (or any supertrait) includes pe.trait
            // TODO: If fuzzy, bail and leave unresolved?
            auto cmp = bound_trait.compare_with_placeholders(sp, trait_path, this->m_ivars.callback_resolve_infer());
            if( cmp != HIR::Compare::Unequal ) {
                auto it = bound_info.assoc.find(pe.item);
                // 1. Check if the bounds include the desired item
                if( it == bound_info.assoc.end() ) {
                    // If not, assume it's opaque and return as such
                    // TODO: What happens if there's two bounds that overlap? 'F: FnMut<()>, F: FnOnce<(), Output=Bar>'

                    DEBUG("[expand_associated_types_inplace__UfcsKnown] Found impl for " << input << " but no bound on item, assuming opaque");

                    if( cmp == HIR::Compare::Fuzzy )
                    {
                        result_type = ResultType::LeaveUnbound;
                        return true;
                    }
                }
                else {
                    result_type = ResultType::Recurse;
                    input = it->second.type.clone();
                }
                return true;
            }

            // - Didn't match
            return false;
            });
    }

    if( rv ) {
        switch(result_type)
        {
        case ResultType::Opaque: {
            DEBUG("Assuming that " << input << " is an opaque name");
            input.data_mut().as_Path().binding = ::HIR::TypePathBinding::make_Opaque({});
            ASSERT_BUG(sp, visit_ty_with(input, [](const HIR::TypeRef& ty){ return ty.data().is_Generic() || ty.data().is_ErasedType() || ty.data().is_Infer(); }), "Set opaque on a non-generic type: " << input);

            DEBUG("- " << m_type_equalities.size() << " replacements");
            for( const auto& v : m_type_equalities )
                DEBUG(" > " << v.first << " = " << v.second);

            auto a = m_type_equalities.find(input);
            if( a != m_type_equalities.end() ) {
                DEBUG("- Replace to " << a->second << " from " << input);
                input = a->second.clone();
            }
            this->expand_associated_types_inplace(sp, input, stack);
            } break;
        case ResultType::Recurse:
            DEBUG("- Found replacement: " << input);
            this->expand_associated_types_inplace(sp, input, stack);
            break;
        case ResultType::LeaveUnbound:
            DEBUG("- Keep as unbound: " << input);
            break;
        }
        return ;
    }

    // If the type of this UfcsKnown is ALSO a UfcsKnown - Check if it's bounded by this trait with equality
    //  e.g. `<<Foo as Bar>::Baz as Trait2>::Type` may have an ATY bound `trait Bar { type Baz: Trait2<Type=...> }`
    // Use bounds on other associated types too (if `pe.type` was resolved to a fixed associated type)
    if(const auto* te_inner = pe.type.data().opt_Path())
    {
        if(const auto* pe_inner_p = te_inner->path.m_data.opt_UfcsKnown())
        {
            const auto& pe_inner = *pe_inner_p;
            // TODO: Search for equality bounds on this associated type (pe_inner) that match the entire type (pe)
            // - Does simplification of complex associated types
            //
            ::HIR::GenericPath  trait_path;
            if( !this->trait_contains_type(sp, pe_inner.trait, this->m_crate.get_trait_by_path(sp, pe_inner.trait.m_path), pe_inner.item.c_str(), trait_path) )
                BUG(sp, "Cannot find associated type " << pe_inner.item << " anywhere in trait " << pe_inner.trait);
            const auto& trait_ptr = this->m_crate.get_trait_by_path(sp, trait_path.m_path);
            const auto& assoc_ty = trait_ptr.m_types.at(pe_inner.item);

            // Resolve where Self=pe_inner.type (i.e. for the trait this inner UFCS is on)
            auto cb_placeholders_trait = MonomorphStatePtr(&pe_inner.type, &pe_inner.trait.m_params, nullptr);
            for(const auto& bound : assoc_ty.m_trait_bounds)
            {
                // If the bound is for Self and the outer trait
                // - TODO: Fuzzy check the parameters?
                ::HIR::GenericPath  tmp_tp;
                const auto& bound_tp = monomorphise_genericpath_with_opt(sp, tmp_tp, bound.m_path, cb_placeholders_trait);
                DEBUG(bound_tp << " ?= " << pe.trait);
                if( bound_tp == pe.trait ) {
                    auto it = bound.m_type_bounds.find( pe.item );
                    if( it != bound.m_type_bounds.end() ) {
                        if( monomorphise_type_needed(it->second.type) ) {
                            input = cb_placeholders_trait.monomorph_type(sp, it->second.type);
                        }
                        else {
                            input = it->second.type.clone();
                        }
                        DEBUG("- Found replacement: " << input);
                        this->expand_associated_types_inplace(sp, input, stack);
                        return ;
                    }
                }

                // TODO: Find trait in this trait.
                const auto& bound_trait = m_crate.get_trait_by_path(sp, bound_tp.m_path);
                bool replaced = this->find_named_trait_in_trait(sp,
                        pe.trait.m_path,pe.trait.m_params,
                        bound_trait, bound_tp.m_path,bound_tp.m_params, pe.type,
                        [&](const auto&, const auto& x, const auto& assoc){
                            auto it = assoc.find(pe.item);
                            if( it != assoc.end() ) {
                                input = it->second.type.clone();
                                return true;
                            }
                            return false;
                        }
                        );
                if( replaced ) {
                    return ;
                }
            }
            DEBUG("pe = " << pe.type << ", input = " << input);
        }
    }

    if( this->find_trait_impls_magic(sp, trait_path.m_path, trait_path.m_params, pe.type, [&](auto impl, auto qual)->bool {
        DEBUG("[expand_associated_types__UfcsKnown] Found " << impl << " qual=" << qual);
        // If it's a fuzzy match, keep going (but count if a concrete hasn't been found)
        if( qual == ::HIR::Compare::Fuzzy ) {
        }
        else {
            auto ty = impl.get_type( pe.item.c_str() );
            if( ty == ::HIR::TypeRef() )
            {
                DEBUG("Assuming that " << input << " is an opaque name");
                e.binding = ::HIR::TypePathBinding::make_Opaque({});
            }
            else
            {
                input = mv$(ty);
            }
        }
        return true;
        }) )
    {
        return ;
    }

    // 2. Crate-level impls
    DEBUG("Searching for impl");
    bool    can_fuzz = true;
    unsigned int    count = 0;
    bool is_specialisable = false;
    ImplRef best_impl;
    rv = this->find_trait_impls_crate(sp, trait_path.m_path, trait_path.m_params, pe.type, [&](auto impl, auto qual)->bool {
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
                    ERROR(sp, E0000, "Couldn't find assocated type " << pe.item << " in impl of " << pe.trait << " for " << pe.type);

                if( impl.has_magic_params() ) {
                }

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
            e.binding = ::HIR::TypePathBinding::make_Opaque({});
            return ;
        }
        else {
            auto ty = best_impl.get_type( pe.item.c_str() );
            if( ty == ::HIR::TypeRef() )
                ERROR(sp, E0000, "Couldn't find assocated type " << pe.item << " in impl of " << pe.trait << " for " << pe.type);

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
        e.binding = ::HIR::TypePathBinding::make_Opaque({});
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

    const auto monomorph_cb = MonomorphStatePtr(&target_type, &pp, nullptr);

    for( const auto& pt : trait_ptr.m_all_parent_traits )
    {
        auto pt_mono = monomorph_cb.monomorph_traitpath(sp, pt, false);

        //DEBUG(pt << " => " << pt_mono);
        if( pt.m_path.m_path == des ) {
            //DEBUG("Found potential " << pt_mono);
            // NOTE: Doesn't quite work...
            //auto cmp = this->compare_pp(sp, pt_mono.m_path.m_params, des_params);
            //if( cmp != ::HIR::Compare::Unequal )
            //{
            if( callback( target_type, pt_mono.m_path.m_params, pt_mono.m_type_bounds ) )
                return true;
            //}
        }
    }

    return false;
}
bool TraitResolution::find_trait_impls_bound(const Span& sp, const ::HIR::SimplePath& trait, const ::HIR::PathParams& params, const ::HIR::TypeRef& type,  t_cb_trait_impl_r callback) const
{
    TRACE_FUNCTION_F("trait = " << trait << params << ", type = " << type);
    const ::HIR::Path::Data::Data_UfcsKnown* assoc_info = nullptr;
    if(const auto* e = type.data().opt_Path()) {
        assoc_info = e->path.m_data.opt_UfcsKnown();
    }

    if(type.data().is_Infer()) {
        return false;
    }

    // NOTE: Even if the type is completely unknown (unbound UFCS), search the bound list.

    // TODO: A bound can imply something via its associated types. How deep can this go?
    // E.g. `T: IntoIterator<Item=&u8>` implies `<T as IntoIterator>::IntoIter : Iterator<Item=&u8>`
    // > Would maybe want a list of all explicit and implied bounds instead.
    {
        bool rv = this->iterate_bounds_traits(sp, type, trait, [&](HIR::Compare cmp, const HIR::TypeRef& bound_ty, const ::HIR::GenericPath& bound_trait, const CachedBound& bound_info)->bool {
            const auto& b_params = bound_trait.m_params;

            DEBUG("[find_trait_impls_bound] " << bound_trait << " for " << bound_ty << " cmp = " << cmp);

            // Check against `params`
            DEBUG("[find_trait_impls_bound] Checking params " << params << " vs " << b_params);
            auto ord = cmp;
            ord &= this->compare_pp(sp, b_params, params);
            if( ord == ::HIR::Compare::Unequal ) {
                DEBUG("[find_trait_impls_bound] - Mismatch");
                return false;
            }
            if( ord == ::HIR::Compare::Fuzzy ) {
                DEBUG("[find_trait_impls_bound] - Fuzzy match");
            }
            DEBUG("[find_trait_impls_bound] Match " << bound_ty << " : " << bound_trait);
            // Hand off to the closure, and return true if it does
            // TODO: The type bounds are only the types that are specified.
            if( callback( ImplRef(&bound_ty, &bound_trait.m_params, &bound_info.assoc), ord) ) {
                return true;
            }

            return false;
            });
        if(rv)
            return rv;
    }

    if(assoc_info) {
        bool rv = this->iterate_bounds_traits(sp, assoc_info->type, assoc_info->trait.m_path, [&](HIR::Compare cmp, const HIR::TypeRef& bound_ty, const ::HIR::GenericPath& bound_trait, const CachedBound& bound_info)->bool {
            // Check the trait params
            cmp &= this->compare_pp(sp, bound_trait.m_params, assoc_info->trait.m_params);
            if( cmp == ::HIR::Compare::Fuzzy ) {
                //TODO(sp, "Handle fuzzy matches searching for associated type bounds");
            }
            else if( cmp == ::HIR::Compare::Unequal ) {
                return false;
            }
            auto outer_ord = cmp;

            const auto& trait_ref = *bound_info.trait_ptr;
            const auto& at = trait_ref.m_types.at(assoc_info->item);
            for(const auto& bound : at.m_trait_bounds)
            {
                if( bound.m_path.m_path == trait )
                {
                    auto monomorph_cb = MonomorphStatePtr(&assoc_info->type, &assoc_info->trait.m_params, nullptr);

                    DEBUG("- Found an associated type bound for this trait via another bound");
                    ::HIR::Compare  ord = outer_ord;
                    if( monomorphise_pathparams_needed(bound.m_path.m_params) ) {
                        // TODO: Use a compare+callback method instead
                        auto b_params_mono = monomorph_cb.monomorph_path_params(sp, bound.m_path.m_params, false);
                        ord &= this->compare_pp(sp, b_params_mono, params);
                    }
                    else {
                        ord &= this->compare_pp(sp, bound.m_path.m_params, params);
                    }
                    if( ord == ::HIR::Compare::Unequal )
                        return false;
                    if( ord == ::HIR::Compare::Fuzzy ) {
                        DEBUG("Fuzzy match");
                    }

                    auto tp_mono = monomorph_cb.monomorph_traitpath(sp, bound, false);
                    // - Expand associated types
                    for(auto& ty : tp_mono.m_type_bounds) {
                        ty.second.type = this->expand_associated_types(sp, mv$(ty.second.type));
                    }
                    DEBUG("- tp_mono = " << tp_mono);
                    // TODO: Instead of using `type` here, build the real type
                    if( callback( ImplRef(type.clone(), mv$(tp_mono.m_path.m_params), mv$(tp_mono.m_type_bounds)), ord ) ) {
                        return true;
                    }
                }
            }
            return false;
            });
        if(rv)
            return true;
    }
    return false;
}
bool TraitResolution::find_trait_impls_crate(const Span& sp,
        const ::HIR::SimplePath& trait, const ::HIR::PathParams* params_ptr,
        const ::HIR::TypeRef& type,
        t_cb_trait_impl_r callback
        ) const
{
    // TODO: Have a global cache of impls that don't reference either generics or ivars

    static ::HIR::TraitPath::assoc_list_t   null_assoc;
    TRACE_FUNCTION_F(trait << FMT_CB(ss, if(params_ptr) { ss << *params_ptr; } else { ss << "<?>"; }) << " for " << type);

    struct StackEnt {
        const ::HIR::SimplePath* trait;
        const ::HIR::PathParams* params_ptr;
        const ::HIR::TypeRef* type;
        StackEnt(const ::HIR::SimplePath& trait, const ::HIR::PathParams* params_ptr, const ::HIR::TypeRef& type)
            : trait(&trait), params_ptr(params_ptr), type(&type)
        {
        }
        bool operator==(const StackEnt& e) const {
            if( *e.trait != *trait )
                return false;
            if( !!e.params_ptr != !!params_ptr )
                return false;
            if( params_ptr && *e.params_ptr != *params_ptr )
                return false;
            if( *e.type != *type )
                return false;
            return true;
        }
    };
    struct StackHandle {
        std::vector<StackEnt>* stack;
        StackHandle(): stack(nullptr) {}
        StackHandle(std::vector<StackEnt>& stack) : stack(&stack) {}
        StackHandle(StackHandle&& x): stack(x.stack) { x.stack = nullptr; }
        StackHandle& operator=(StackHandle&& x) { this->~StackHandle(); stack = x.stack; x.stack = nullptr; return *this; }
        StackHandle(const StackHandle&) = delete;
        StackHandle& operator=(const StackHandle&) = delete;
        ~StackHandle() { if(stack) stack->pop_back(); stack = nullptr; }
    };
    static std::vector<StackEnt>    s_recurse_stack;
    auto se = StackEnt(trait, params_ptr, type);
    // NOTE: Allow 1 level of recursion (EAT being run)
    if( std::count(s_recurse_stack.begin(), s_recurse_stack.end(), se) > 1 ) {
        DEBUG("Recursion detected in `find_trait_impls_crate`");
        //return false;
        throw TraitResolution::RecursionDetected();
        //BUG(sp, "Recursion detected in `find_trait_impls_crate`");
    }
    s_recurse_stack.push_back(se);
    StackHandle sh { s_recurse_stack };

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
        if( type.data().is_Infer() ) {
            return callback( ImplRef(&type, params_ptr, &null_assoc), ::HIR::Compare::Fuzzy );
        }

        const ::HIR::TraitMarkings* markings = nullptr;
        if(const auto* e = type.data().opt_Path())
        {
            if( TU_TEST1(e->path.m_data, Generic, .m_params.m_types.size() == 0) )
            {
                markings = e->binding.get_trait_markings();
            }
        }

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
            [&](const auto& impl)->bool {
                // Skip any negative impls on this pass
                if( impl.is_positive != true )
                    return false;

                DEBUG("[find_trait_impls_crate] - Auto Pos Found impl" << impl.m_params.fmt_args() << " " << trait << impl.m_trait_args << " for " << impl.m_type << " " << impl.m_params.fmt_bounds());

                // Compare with `params`
                HIR::PathParams impl_params;
                auto match = this->ftic_check_params(sp, trait,  params_ptr, type,  impl.m_params, impl.m_trait_args, impl.m_type,  impl_params);
                if( match == ::HIR::Compare::Unequal ) {
                    // If any bound failed, return false (continue searching)
                    return false;
                }

                auto monomorph = MonomorphStatePtr(nullptr, &impl_params, nullptr);
                // TODO: Ensure that there are no-longer any magic params?

                auto ty_mono = monomorph.monomorph_type(sp, impl.m_type, false);
                auto args_mono = monomorph.monomorph_path_params(sp, impl.m_trait_args, false);
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
                HIR::PathParams impl_params;
                auto match = this->ftic_check_params(sp, trait,  params_ptr, type,  impl.m_params, impl.m_trait_args, impl.m_type,  impl_params);
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
                ASSERT_BUG(sp, cmp == ::HIR::Compare::Equal, "Auto trait with no params returned a fuzzy match from destructure - " << trait << " for " << type);
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

    // TODO: Don't search if ALL types are unbounded ivar (what about a tuple of unbounded?)
    // If the type is an unbounded ivar, don't search.
#if 1
    if( type.data().is_Infer() && !type.data().as_Infer().is_lit() ) {
        return false;
    }
#elif 0
    if( type.data().is_Infer() && !type.data().as_Infer().is_lit() ) {
        return this->m_crate.find_trait_impls(trait, type, this->m_ivars.callback_resolve_infer(),
            [&](const auto& impl) {
                HIR::PathParams impl_params;
                // Fill all params with placeholders?
                return callback(ImplRef(mv$(impl_params), trait, impl), HIR::Compare::Fuzzy);
            });
    }
#endif

    return this->m_crate.find_trait_impls(trait, type, this->m_ivars.callback_resolve_infer(),
        [&](const auto& impl) {
            DEBUG("[find_trait_impls_crate] Found impl" << impl.m_params.fmt_args() << " " << trait << impl.m_trait_args << " for " << impl.m_type << " " << impl.m_params.fmt_bounds());
            // Compare with `params`
            HIR::PathParams impl_params;
            auto match = this->ftic_check_params(sp, trait,  params_ptr, type,  impl.m_params, impl.m_trait_args, impl.m_type,  impl_params);
            if( match == ::HIR::Compare::Unequal ) {
                // If any bound failed, return false (continue searching)
                DEBUG("[find_trait_impls_crate] - Params mismatch");
                return false;
            }
            DEBUG("[find_trait_impls_crate] - Found with impl_params=" << impl_params);

            return callback(ImplRef(mv$(impl_params), m_crate.get_trait_by_path(sp, trait), trait, impl), match);
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
    TU_MATCH_HDRA( (type.data()), { )
    default:
        // Otherwise, there's no negative so it must be positive
        return ::HIR::Compare::Equal;
    TU_ARMA(Path, e) {
        ::HIR::Compare  res = ::HIR::Compare::Equal;
        TU_MATCH_HDRA( (e.path.m_data), {)
        TU_ARMA(Generic, pe) { //(
            ::HIR::TypeRef  tmp;
            auto monomorph = MonomorphStatePtr(nullptr, &pe.m_params, nullptr);
            // HELPER: Get a possibily monomorphised version of the input type (stored in `tmp` if needed)
            auto monomorph_get = [&](const auto& ty)->const ::HIR::TypeRef& {
                if( monomorphise_type_needed(ty) ) {
                    return (tmp = this->expand_associated_types(sp, monomorph.monomorph_type(sp, ty)));
                }
                else {
                    return ty;
                }
                };

            TU_MATCH_HDRA( (e.binding), {)
            TU_ARMA(Opaque, tpb) {
                BUG(sp, "Opaque binding on generic path - " << type);
                }
            TU_ARMA(Unbound, tpb) {
                BUG(sp, "Unbound binding on generic path - " << type);
                }
            TU_ARMA(Struct, tpb) {
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
                }
            TU_ARMA(Enum, tpb) {
                if( const auto* e = tpb->m_data.opt_Data() )
                {
                    for(const auto& var : *e)
                    {
                        const auto& fld_ty_mono = monomorph_get(var.type);
                        DEBUG("Enum '" << var.name << "'" << fld_ty_mono);
                        res &= type_impls_trait(fld_ty_mono);
                        if( res == ::HIR::Compare::Unequal )
                            return ::HIR::Compare::Unequal;
                    }
                }
                }
            TU_ARMA(Union, tpb) {
                for(const auto& fld : tpb->m_variants)
                {
                    const auto& fld_ty_mono = monomorph_get(fld.second.ent);
                    DEBUG("Union '" << fld.first << "' " << fld_ty_mono);
                    res &= type_impls_trait(fld_ty_mono);
                    if( res == ::HIR::Compare::Unequal )
                        return ::HIR::Compare::Unequal;
                }
                }
            TU_ARMA(ExternType, tpb) {
                TODO(sp, "Check auto trait destructure on extern type " << type);
                }
            }
            DEBUG("- Nothing failed, calling callback");
            }
        TU_ARMA(UfcsUnknown, pe) {
            BUG(sp, "UfcsUnknown in typeck - " << type);
            }
        TU_ARMA(UfcsKnown, pe) {
            // If unbound, use Fuzzy {
            if(e.binding.is_Unbound()) {
                DEBUG("- Unbound UfcsKnown, returning Fuzzy");
                return ::HIR::Compare::Fuzzy;
            }
            // Otherwise, it's opaque. Check the bounds on the trait.
            if( TU_TEST1(pe.type.data(), Generic, .binding >> 8 == 2) )
            {
                DEBUG("- UfcsKnown of placeholder, returning Fuzzy");
                return ::HIR::Compare::Fuzzy;
            }
            TODO(sp, "Check trait bounds for bound on " << type);
            }
        TU_ARMA(UfcsInherent, pe) {
            TODO(sp, "Auto trait lookup on UFCS Inherent type");
            }
        }
        return res;
        }
    TU_ARMA(Generic, e) {
        auto l_res = ::HIR::Compare::Unequal;
        this->find_trait_impls(sp, trait, *params_ptr, type, [&](auto, auto cmp){ l_res = cmp; return (cmp == ::HIR::Compare::Equal); });
        return l_res;
        }
    TU_ARMA(Tuple, e) {
        ::HIR::Compare  res = ::HIR::Compare::Equal;
        for(const auto& sty : e)
        {
            res &= type_impls_trait(sty);
            if( res == ::HIR::Compare::Unequal )
                return ::HIR::Compare::Unequal;
        }
        return res;
        }
    TU_ARMA(Array, e) {
        return type_impls_trait(e.inner);
        }
    }
    throw "";
}

::HIR::Compare TraitResolution::ftic_check_params(const Span& sp, const ::HIR::SimplePath& trait,
    const ::HIR::PathParams* params_ptr, const ::HIR::TypeRef& type,
    const ::HIR::GenericParams& impl_params_def, const ::HIR::PathParams& impl_trait_args, const ::HIR::TypeRef& impl_ty,
    /*Out->*/ HIR::PathParams& out_impl_params
    ) const
{
    TRACE_FUNCTION_FR("impl" << impl_params_def.fmt_args() << " " << trait << impl_trait_args << " for " << impl_ty, out_impl_params);
    class GetParams:
        public ::HIR::MatchGenerics
    {
        Span    sp;
        HIR::PathParams& out_impl_params;
    public:
        GetParams(Span sp, HIR::PathParams& out_impl_params):
            sp(sp),
            out_impl_params(out_impl_params)
        {}

        ::HIR::Compare match_ty(const ::HIR::GenericRef& g, const ::HIR::TypeRef& ty, ::HIR::t_cb_resolve_type resolve_cb) override {
            assert( g.binding < out_impl_params.m_types.size() );
            if( out_impl_params.m_types[g.binding] == HIR::TypeRef() ) {
                DEBUG("[ftic_check_params] Param " << g.binding << " = " << ty);
                out_impl_params.m_types[g.binding] = ty.clone();
                return ::HIR::Compare::Equal;
            }
            else {
                DEBUG("[ftic_check_params] Param " << g.binding << " " << out_impl_params.m_types[g.binding] << " == " << ty);
                auto rv = out_impl_params.m_types[g.binding].compare_with_placeholders(sp, ty, resolve_cb);
                // If the existing is an ivar, replace with this.
                // - TODO: Store the least fuzzy option, or store all fuzzy options?
                if( rv == ::HIR::Compare::Fuzzy && out_impl_params.m_types[g.binding].data().is_Infer() )
                {
                    DEBUG("[ftic_check_params] Param " << g.binding << " fuzzy, use " << ty);
                    out_impl_params.m_types[g.binding] = ty.clone();
                }
                return rv;
            }
        }
        ::HIR::Compare match_val(const ::HIR::GenericRef& g, const ::HIR::ConstGeneric& sz) override
        {
            ASSERT_BUG(sp, g.binding < out_impl_params.m_values.size(), "Value generic " << g << " out of range (" << out_impl_params.m_values.size() << ")");
            if( out_impl_params.m_values[g.binding].is_Infer() )
            {
                DEBUG("[ftic_check_params] Value param " << g.binding << " = " << sz);
                out_impl_params.m_values[g.binding] = sz.clone();
                return ::HIR::Compare::Equal;
            }
            else
            {
                if( out_impl_params.m_values[g.binding] == sz ) {
                    return ::HIR::Compare::Equal;
                }
                TODO(Span(), "PtrImplMatcher::match_val " << g << "(" << out_impl_params.m_values[g.binding] << ") with " << sz);
            }
        }
    };
    GetParams get_params { sp, out_impl_params };

    out_impl_params.m_types.resize( impl_params_def.m_types.size() );
    out_impl_params.m_values.resize( impl_params_def.m_values.size() );

    // NOTE: If this type references an associated type, the match will incorrectly fail.
    // - HACK: match_test_generics_fuzz has been changed to return Fuzzy if there's a tag mismatch and the LHS is an Opaque path
    auto    match = ::HIR::Compare::Equal;
    match &= impl_ty.match_test_generics_fuzz(sp, type , this->m_ivars.callback_resolve_infer(), get_params);
    if( params_ptr )
    {
        const auto& params = *params_ptr;
        ASSERT_BUG(sp, impl_trait_args.m_types.size() == params.m_types.size(), "Param count mismatch between `" << impl_trait_args << "` and `" << params << "` for " << trait );
        for(unsigned int i = 0; i < impl_trait_args.m_types.size(); i ++)
            match &= impl_trait_args.m_types[i] .match_test_generics_fuzz(sp, params.m_types[i], this->m_ivars.callback_resolve_infer(), get_params);
        if( match == ::HIR::Compare::Unequal ) {
            DEBUG("- Failed to match parameters - " << impl_trait_args << "+" << impl_ty << " != " << params << "+" << type);
            return ::HIR::Compare::Unequal;
        }
    }
    else
    {
        if( match == ::HIR::Compare::Unequal ) {
            DEBUG("- Failed to match type - " << impl_ty << " != " << type);
            return ::HIR::Compare::Unequal;
        }
    }

    DEBUG("Matched params: " << out_impl_params);

    // TODO: Some impl blocks have type params used as part of type bounds.
    // - A rough idea is to have monomorph return a third class of generic for params that are not yet bound.
    //  - compare_with_placeholders gets called on both ivars and generics, so that can be used to replace it once known.
    ::HIR::PathParams   placeholders;
    auto placeholder_name = RcString::new_interned(FMT("impl_?_" << &impl_params_def));
    for(unsigned int i = 0; i < out_impl_params.m_types.size(); i ++ )
    {
        if( out_impl_params.m_types[i] == HIR::TypeRef() )
        {
            if( placeholders.m_types.size() == 0 )
                placeholders.m_types.resize(out_impl_params.m_types.size());
            placeholders.m_types[i] = ::HIR::TypeRef(placeholder_name, 2*256 + i);
            DEBUG("Create placeholder type for " << i << " = " << placeholders.m_types[i]);
        }
    }
    for(unsigned int i = 0; i < out_impl_params.m_values.size(); i ++ )
    {
        if( out_impl_params.m_values[i].is_Infer() )
        {
            if( placeholders.m_values.size() == 0 )
                placeholders.m_values.resize(out_impl_params.m_values.size());
            placeholders.m_values[i] = ::HIR::GenericRef(placeholder_name, 2*256 + i);
            DEBUG("Create placeholder value for " << i << " = " << placeholders.m_values[i]);
        }
    }
    DEBUG("Placeholders: " << placeholders);
    auto cb_infer = [&](const auto& ty)->const ::HIR::TypeRef& {
        if( ty.data().is_Infer() )
            return this->m_ivars.get_type(ty);
        #if 0
        else if( ty.data().is_Generic() && ty.data().as_Generic().binding >> 8 == 2 ) { // Generic group 2 = Placeholders
            unsigned int i = ty.data().as_Generic().binding % 256;
            ASSERT_BUG(sp, i < impl_params.size(), "Placeholder param out of range - " << i << " >= " << placeholders.size());
            if( impl_params[i] )
            {
                DEBUG("[ftic_check_params:cb_infer] " << ty << " = " << *impl_params[i]);
                return *impl_params[i];
            }
            else
            {
                ASSERT_BUG(sp, i < placeholders.size(), "Placeholder param out of range - " << i << " >= " << placeholders.size());
                const auto& ph = placeholders[i];
                if( ph.m_data.is_Generic() && ph.m_data.as_Generic().binding == i )
                    TODO(sp, "[ftic_check_params:cb_infer] Placeholder " << i << " not yet bound");
                return ph;
            }
        }
        #endif
        else
            return ty;
        };
    struct Matcher:
        public ::HIR::MatchGenerics,
        public Monomorphiser
    {
        Span    sp;
        const HIR::PathParams& impl_params;
        RcString    placeholder_name;
        ::HIR::PathParams& placeholders;
        Matcher(Span sp, const HIR::PathParams& impl_params, RcString placeholder_name, ::HIR::PathParams& placeholders):
            sp(sp),
            impl_params(impl_params),
            placeholder_name(placeholder_name),
            placeholders(placeholders)
        {
        }

        ::HIR::Compare match_ty(const ::HIR::GenericRef& g, const ::HIR::TypeRef& ty, ::HIR::t_cb_resolve_type resolve_cb) override
        {
            if( const auto* e = ty.data().opt_Generic() )
            {
                if( e->binding == g.binding && e->name == g.name )
                {
                    return ::HIR::Compare::Equal;
                }
            }
            if( g.is_placeholder() && g.name == placeholder_name ) {
                auto i = g.idx();
                ASSERT_BUG(sp, impl_params.m_types[i] == HIR::TypeRef(), "Placeholder to populated type returned - " << impl_params.m_types[i] << " vs " << ty);
                auto& ph = placeholders.m_types[i];
                // TODO: Only want to do this if ... what?
                // - Problem: This can poison the output if the result was fuzzy
                // - E.g. `Q: Borrow<V>` can equate Q and V
                if( ph.data().is_Generic() && ph.data().as_Generic().binding == g.binding ) {
                    DEBUG("[ftic_check_params:cb_match] Bind placeholder " << i << " to " << ty);
                    ph = ty.clone();
                    return ::HIR::Compare::Equal;
                }
                else {
                    DEBUG("[ftic_check_params:cb_match] Compare placeholder " << i << " " << ph << " == " << ty);
                    return ph.compare_with_placeholders(sp, ty, resolve_cb);
                }
            }
            else {
                if( g.is_placeholder() ) {
                    DEBUG("[ftic_check_params:cb_match] External impl param " << g);
                    return ::HIR::Compare::Fuzzy;
                }
                // If the RHS is a non-literal ivar, return fuzzy
                if( ty.data().is_Infer() && !ty.data().as_Infer().is_lit() ) {
                    return ::HIR::Compare::Fuzzy;
                }
                // If the RHS is an unbound UfcsKnown, also fuzzy
                if( ty.data().is_Path() && ty.data().as_Path().binding.is_Unbound() ) {
                    return ::HIR::Compare::Fuzzy;
                }
                if( ty.data().is_Generic() && ty.data().as_Generic().is_placeholder() ) {
                    return ::HIR::Compare::Fuzzy;
                }
                DEBUG("Unequal generic - " << g << " != " << ty);
                return ::HIR::Compare::Unequal;
            }
        }
        ::HIR::Compare match_val(const ::HIR::GenericRef& g, const ::HIR::ConstGeneric& sz) override
        {
            TODO(Span(), "Matcher::match_val " << g << " with " << sz);
        }

        ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& ge) const override {
            //if( ge.is_self() ) {
            //    // TODO: `impl_type` or `des_type`
            //    DEBUG("[find_impl__check_crate_raw] Self - " << impl_type << " or " << des_type);
            //    //TODO(sp, "[find_impl__check_crate_raw] Self - " << impl_type << " or " << des_type);
            //    return impl_type;
            //}
            ASSERT_BUG(sp, !ge.is_placeholder(), "[find_impl__check_crate_raw] Placeholder param seen - " << ge);
            if( impl_params.m_types.at(ge.binding) != HIR::TypeRef() ) {
                return impl_params.m_types.at(ge.binding).clone();
            }
            ASSERT_BUG(sp, placeholders.m_types.size() == impl_params.m_types.size(), "Placeholder size mismatch: " << placeholders.m_types.size() << " != " << impl_params.m_types.size());
            return placeholders.m_types.at(ge.binding).clone();
        }
        ::HIR::ConstGeneric get_value(const Span& sp, const ::HIR::GenericRef& val) const override {
            ASSERT_BUG(sp, val.binding < 256, "Generic value binding in " << val << " out of range (>=256)");
            ASSERT_BUG(sp, val.binding < impl_params.m_values.size(), "Generic value binding in " << val << " out of range (>= " << impl_params.m_values.size() << ")");
            if( !impl_params.m_values.at(val.binding).is_Infer() ) {
                return impl_params.m_values.at(val.binding).clone();
            }
            ASSERT_BUG(sp, placeholders.m_values.size() == impl_params.m_values.size(), "Placeholder size mismatch: " << placeholders.m_values.size() << " != " << impl_params.m_values.size());
            return placeholders.m_values.at(val.binding).clone();
        }
    };
    Matcher matcher { sp, out_impl_params, placeholder_name, placeholders };

    //::std::vector<::HIR::TypeRef> saved_ph;
    //for(const auto& t : placeholders)
    //    saved_ph.push_back(t.clone());

    // Check bounds for this impl
    // - If a bound fails, then this can't be a valid impl
    for(const auto& bound : impl_params_def.m_bounds)
    {
        TU_MATCH_HDRA( (bound), {)
        TU_ARMA(Lifetime, be) {
            }
        TU_ARMA(TypeLifetime, be) {
            }
        TU_ARMA(TraitBound, be) {

            DEBUG("Check bound " << be.type << " : " << be.trait);
            auto real_type = matcher.monomorph_type(sp, be.type, false);
            auto real_trait = matcher.monomorph_traitpath(sp, be.trait, false);
            real_type = this->expand_associated_types(sp, mv$(real_type));
            for(auto& p : real_trait.m_path.m_params.m_types) {
                p = this->expand_associated_types(sp, mv$(p));
            }
            for(auto& ab : real_trait.m_type_bounds) {
                ab.second.type = this->expand_associated_types(sp, mv$(ab.second.type));
            }
            const auto& real_trait_path = real_trait.m_path;
            DEBUG("- bound mono " << real_type << " : " << real_trait);
            bool found_fuzzy_match = false;
            // If the type is an unbound UFCS path, assume fuzzy
            if( TU_TEST1(real_type.data(), Path, .binding.is_Unbound()) ) {
                DEBUG("- Bounded type is unbound UFCS, assuming fuzzy match");
                found_fuzzy_match = true;
            }
            // If the type is an ivar, but not a literal, assume fuzzy
            if( TU_TEST1(real_type.data(), Infer, .is_lit() == false) ) {
                DEBUG("- Bounded type is an ivar, assuming fuzzy match");
                found_fuzzy_match = true;
            }
            // NOTE: Save the placeholder state and restore if the result was Fuzzy
            ::HIR::PathParams saved_ph = placeholders.clone();
            ::HIR::PathParams fuzzy_ph;
            unsigned num_fuzzy = 0;     //!< Number of detected fuzzy impls
            bool fuzzy_compatible = true;   //!< Indicates that the `fuzzy_ph` applies to all detected fuzzy impls
            auto rv = this->find_trait_impls(sp, real_trait_path.m_path, real_trait_path.m_params, real_type, [&](auto impl, auto impl_cmp) {
                // TODO: Save and restore placeholders if this isn't a full match
                DEBUG("[ftic_check_params] impl_cmp = " << impl_cmp << ", impl = " << impl);
                auto cmp = impl_cmp;
                if( cmp == ::HIR::Compare::Fuzzy )
                {
                    // If the match was fuzzy, try again filling in with `cb_match`
                    auto i_ty = impl.get_impl_type();
                    this->expand_associated_types_inplace( sp, i_ty, {} );
                    auto i_tp = impl.get_trait_params();
                    for(auto& t : i_tp.m_types)
                        this->expand_associated_types_inplace( sp, t, {} );
                    DEBUG("[ftic_check_params] " << real_type << " ?= " << i_ty);
                    cmp &= real_type .match_test_generics_fuzz(sp, i_ty, cb_infer, matcher);
                    DEBUG("[ftic_check_params] " << real_trait_path.m_params << " ?= " << i_tp);
                    cmp &= real_trait_path.m_params .match_test_generics_fuzz(sp, i_tp, cb_infer, matcher);
                    DEBUG("[ftic_check_params] - Re-check result: " << cmp);
                }
                for(const auto& assoc_bound : real_trait.m_type_bounds) {
                    ::HIR::TypeRef  tmp;
                    const ::HIR::TypeRef*   ty_p;

                    tmp = impl.get_type(assoc_bound.first.c_str());
                    if( tmp == ::HIR::TypeRef() ) {
                        // This bound isn't from this particular trait, go the slow way of using expand_associated_types
                        tmp = this->expand_associated_types(sp, ::HIR::TypeRef::new_path(
                            ::HIR::Path(::HIR::Path::Data::Data_UfcsKnown { real_type.clone(), real_trait_path.clone(), assoc_bound.first, {} }),
                            {}
                            ));
                        ty_p = &tmp;
                    }
                    else {
                        // Expand after extraction, just to make sure.
                        this->expand_associated_types_inplace(sp, tmp, {});
                        ty_p = &this->m_ivars.get_type(tmp);
                    }
                    const auto& ty = *ty_p;
                    DEBUG("[ftic_check_params] - Compare " << ty << " and " << assoc_bound.second.type << ", matching generics");
                    // `ty` = Monomorphised actual type (< `be.type` as `be.trait` >::`assoc_bound.first`)
                    // `assoc_bound.second` = Desired type (monomorphised too)
                    auto cmp_i = assoc_bound.second.type .match_test_generics_fuzz(sp, ty, cb_infer, matcher);
                    switch(cmp_i)
                    {
                    case ::HIR::Compare::Equal:
                        DEBUG("Equal");
                        break;
                    case ::HIR::Compare::Unequal:
                        DEBUG("Assoc `" << assoc_bound.first << "` didn't match - " << ty << " != " << assoc_bound.second.type);
                        cmp = ::HIR::Compare::Unequal;
                        break;
                    case ::HIR::Compare::Fuzzy:
                        // TODO: When a fuzzy match is encountered on a conditional bound, returning `false` can lead to an false negative (and a compile error)
                        // BUT, returning `true` could lead to it being selected. (Is this a problem, should a later validation pass check?)
                        DEBUG("[ftic_check_params] Fuzzy match assoc bound between " << ty << " and " << assoc_bound.second.type);
                        cmp = ::HIR::Compare::Fuzzy;
                        break ;
                    }
                    if( cmp == ::HIR::Compare::Unequal )
                        break;
                }

                DEBUG("[ftic_check_params] impl_cmp = " << impl_cmp << ", cmp = " << cmp);
                if( cmp == ::HIR::Compare::Fuzzy )
                {
                    found_fuzzy_match |= true;
                    // `fuzzy_ph` is set (num_fuzzy > 0) then check if the PH set is equal, if not then flag not equal
                    if( num_fuzzy > 0 && fuzzy_ph != placeholders ) {
                        DEBUG("Multiple fuzzy matches, placeholders mismatch: " << fuzzy_ph << " != " << placeholders);
                        fuzzy_compatible = false;
                    }
                    num_fuzzy += 1;

                    fuzzy_ph = ::std::move(placeholders);
                    // TODO: Should this do some form of reset?
                    placeholders.m_types.resize(fuzzy_ph.m_types.size());
                    placeholders.m_values.resize(fuzzy_ph.m_values.size());
                }
                if( cmp != ::HIR::Compare::Equal )
                {
                    // Restore placeholders
                    // - Maybe save the results for later?
                    DEBUG("[ftic_check_params] Restore placeholders: " << saved_ph);
                    DEBUG("[ftic_check_params] OVERWRITTEN placeholders: " << placeholders);
                    placeholders = saved_ph.clone();
                }
                // If the match isn't a concrete equal, return false (to keep searching)
                return (cmp == ::HIR::Compare::Equal);
                });
            if( rv ) {
                DEBUG("- Bound " << real_type << " : " << real_trait_path << " matched");
            }
            else if( found_fuzzy_match ) {
                DEBUG("- Bound " << real_type << " : " << real_trait_path << " fuzzed");
                if( num_fuzzy == 0 )
                {
                    DEBUG("No placeholders");   // `real_type` was infer
                }
                else if( num_fuzzy == 1 )
                {
                    DEBUG("Use placeholders " << fuzzy_ph);
                    placeholders = ::std::move(fuzzy_ph);
                }
                else if( fuzzy_compatible ) 
                {
                    DEBUG("Multiple placeholders (" << num_fuzzy << "), but all equal " << fuzzy_ph);
                    placeholders = ::std::move(fuzzy_ph);
                }
                else
                {
                    // 
                    DEBUG("TODO: Multiple fuzzy matches, which placeholder set to use?");
                }
                match = ::HIR::Compare::Fuzzy;
            }
            else if( TU_TEST1(real_type.data(), Infer, .ty_class == ::HIR::InferClass::None) ) {
                DEBUG("- Bound " << real_type << " : " << real_trait_path << " full infer type - make result fuzzy");
                match = ::HIR::Compare::Fuzzy;
            }
            else if( TU_TEST1(real_type.data(), Generic, .is_placeholder()) ) {
                DEBUG("- Bound " << real_type << " : " << real_trait_path << " placeholder - make result fuzzy");
                match = ::HIR::Compare::Fuzzy;
            }
            else {
                DEBUG("- Bound " << real_type << " : " << real_trait_path << " failed");
                return ::HIR::Compare::Unequal;
            }

            //if( !rv ) {
            //    placeholders = ::std::move(saved_ph);
            //}
            }
        TU_ARMA(TypeEquality, be) {
            TODO(sp, "Check bound " << be.type << " = " << be.other_type);
            }
        }
    }

    for(size_t i = 0; i < out_impl_params.m_types.size(); i ++)
    {
        if( out_impl_params.m_types[i] == HIR::TypeRef() )
        {
            out_impl_params.m_types[i] = std::move(placeholders.m_types[i]);
        }
        ASSERT_BUG(sp, out_impl_params.m_types[i] != HIR::TypeRef(), "");
    }

    for(size_t i = 0; i < impl_params_def.m_types.size(); i ++)
    {
        if( impl_params_def.m_types.at(i).m_is_sized )
        {
            if( out_impl_params.m_types[i] != HIR::TypeRef() )
            {
                auto cmp = type_is_sized(sp, out_impl_params.m_types[i]);
                if( cmp == ::HIR::Compare::Unequal )
                {
                    DEBUG("- Sized bound failed for " << out_impl_params.m_types[i]);
                    return ::HIR::Compare::Unequal;
                }
            }
            else {
                // TODO: Set match to fuzzy?
            }
        }
    }


    //if( match == ::HIR::Compare::Fuzzy ) {
    //    out_impl_params.placeholder_types = ::std::move(fuzzy_ph);
    //}

    return match;
}

namespace {
    bool trait_contains_method_inner(const ::HIR::Trait& trait_ptr, const char* name,  const ::HIR::Function*& out_fcn_ptr)
    {
        auto it = trait_ptr.m_values.find(name);
        if( it != trait_ptr.m_values.end() )
        {
            if( it->second.is_Function() ) {
                const auto& v = it->second.as_Function();
                out_fcn_ptr = &v;
                return true;
            }
        }
        return false;
    }
}

const ::HIR::Function* TraitResolution::trait_contains_method(const Span& sp, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait_ptr, const ::HIR::TypeRef& self, const char* name,  ::HIR::GenericPath& out_path) const
{
    TRACE_FUNCTION_FR("trait_path=" << trait_path << ",name=" << name, out_path);
    const ::HIR::Function* rv = nullptr;

    if( trait_contains_method_inner(trait_ptr, name, rv) )
    {
        assert(rv);
        out_path = trait_path.clone();
        return rv;
    }

    auto monomorph_cb = MonomorphStatePtr(&self, &trait_path.m_params, nullptr);
    for(const auto& st : trait_ptr.m_all_parent_traits)
    {
        if( trait_contains_method_inner(*st.m_trait_ptr, name, rv) )
        {
            assert(rv);
            out_path.m_path = st.m_path.m_path;
            out_path.m_params = monomorph_cb.monomorph_path_params(sp, st.m_path.m_params, false);
            return rv;
        }
    }
    return nullptr;
}
bool TraitResolution::trait_contains_type(const Span& sp, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait_ptr, const char* name,  ::HIR::GenericPath& out_path) const
{
    TRACE_FUNCTION_FR(trait_path << " has " << name, out_path);

    auto it = trait_ptr.m_types.find(name);
    if( it != trait_ptr.m_types.end() ) {
        DEBUG("- Found in cur");
        out_path = trait_path.clone();
        return true;
    }

    auto monomorph_cb = MonomorphStatePtr(nullptr, &trait_path.m_params, nullptr);
    for(const auto& st : trait_ptr.m_all_parent_traits)
    {
        if( st.m_trait_ptr->m_types.count(name) )
        {
            DEBUG("- Found in " << st);
            out_path.m_path = st.m_path.m_path;
            out_path.m_params = monomorph_cb.monomorph_path_params(sp, st.m_path.m_params, false);
            return true;
        }
    }
    return false;
}

::HIR::Compare TraitResolution::type_is_sized(const Span& sp, const ::HIR::TypeRef& type) const
{
    TU_MATCH_DEF(::HIR::TypeData, (type.data()), (e),
    (
        // Any unknown - it's sized
        ),
    (Infer,
        switch(e.ty_class)
        {
        case ::HIR::InferClass::Integer:
        case ::HIR::InferClass::Float:
            return ::HIR::Compare::Equal;
        default:
            return ::HIR::Compare::Fuzzy;
        }
        ),
    (Primitive,
        if( e == ::HIR::CoreType::Str )
            return ::HIR::Compare::Unequal;
        ),
    (Slice,
        return ::HIR::Compare::Unequal;
        ),
    (Path,
        // TODO: Check that only ?Sized parameters are !Sized
        TU_MATCHA( (e.binding), (pb),
        (Unbound,
            //
            ),
        (Opaque,
            // TODO: Check bounds
            ),
        (ExternType,
            // Is it sized? No.
            return ::HIR::Compare::Unequal;
            ),
        (Enum,
            // HAS to be Sized
            ),
        (Union,
            // Pretty sure unions are Sized
            ),
        (Struct,
            // Possibly not sized
            switch( pb->m_struct_markings.dst_type )
            {
            case ::HIR::StructMarkings::DstType::None:
                break;
            case ::HIR::StructMarkings::DstType::Possible:
                // Check sized-ness of the unsized param
                return type_is_sized(sp, e.path.m_data.as_Generic().m_params.m_types.at(pb->m_struct_markings.unsized_param));
            case ::HIR::StructMarkings::DstType::Slice:
            case ::HIR::StructMarkings::DstType::TraitObject:
                return ::HIR::Compare::Unequal;
            }
            )
        )
        ),
    (ErasedType,
        return e.m_is_sized ? ::HIR::Compare::Equal : ::HIR::Compare::Unequal;
        ),
    (TraitObject,
        return ::HIR::Compare::Unequal;
        )
    )
    return ::HIR::Compare::Equal;
}
::HIR::Compare TraitResolution::type_is_copy(const Span& sp, const ::HIR::TypeRef& ty) const
{
    const auto& type = this->m_ivars.get_type(ty);
    TU_MATCH_HDRA( (type.data()), {)
    default: {
        bool is_fuzzy = false;
        bool has_eq = find_trait_impls(sp, m_lang_Copy, ::HIR::PathParams{}, ty,  [&](auto , auto c)->bool{
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
            }, /*magic_trait_impls=*/false);
        if( has_eq ) {
            return ::HIR::Compare::Equal;
        }
        else if( is_fuzzy ) {
            return ::HIR::Compare::Fuzzy;
        }
        else {
            return ::HIR::Compare::Unequal;
        }
        }
    TU_ARMA(Infer, e) {
        switch(e.ty_class)
        {
        case ::HIR::InferClass::Integer:
        case ::HIR::InferClass::Float:
            return ::HIR::Compare::Equal;
        default:
            DEBUG("Fuzzy Copy impl for ivar?");
            return ::HIR::Compare::Fuzzy;
        }
        }
    TU_ARMA(Generic, e) {
        // TODO: Store this result - or even pre-calculate it.
        return this->iterate_bounds_traits(sp, ty, m_lang_Copy, [&](HIR::Compare _cmp, const ::HIR::TypeRef& be_type, const ::HIR::GenericPath& be_trait, const CachedBound& info)->bool {
            return true;
            }) ? ::HIR::Compare::Equal : ::HIR::Compare::Unequal ;
        }
    TU_ARMA(Primitive, e) {
        if( e == ::HIR::CoreType::Str )
            return ::HIR::Compare::Unequal;
        return ::HIR::Compare::Equal;
        }
    TU_ARMA(Borrow, e) {
        return e.type == ::HIR::BorrowType::Shared ? ::HIR::Compare::Equal : ::HIR::Compare::Unequal ;
        }
    TU_ARMA(Pointer, e) {
        return ::HIR::Compare::Equal;
        }
    TU_ARMA(Tuple, e) {
        auto rv = ::HIR::Compare::Equal;
        for(const auto& sty : e)
            rv &= type_is_copy(sp, sty);
        return rv;
        }
    TU_ARMA(Slice, e) {
        return ::HIR::Compare::Unequal;
        }
    TU_ARMA(Function, e) {
        return ::HIR::Compare::Equal;
        }
    TU_ARMA(Closure, e) {
        // NOTE: This isn't strictly true, we're leaving the actual checking up to the validate pass
        return ::HIR::Compare::Equal;
        }
    TU_ARMA(Array, e) {
        return type_is_copy(sp, e.inner);
        }
    }
    throw "";
}
::HIR::Compare TraitResolution::type_is_clone(const Span& sp, const ::HIR::TypeRef& ty) const
{
    TRACE_FUNCTION_F(ty);
    const auto& type = this->m_ivars.get_type(ty);
    TU_MATCH_HDRA( (type.data()), {)
    default: {
        if( type.data().is_Path() && type.data().as_Path().is_closure() )
        {
            // If it was a closure, assume true (later code can check)
            return ::HIR::Compare::Equal;
        }
        bool is_fuzzy = false;
        bool has_eq = find_trait_impls(sp, m_lang_Clone, ::HIR::PathParams{}, ty,  [&](auto , auto c)->bool{
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
            }, /*magic_trait_impls=*/false);
        if( has_eq ) {
            return ::HIR::Compare::Equal;
        }
        else if( is_fuzzy ) {
            return ::HIR::Compare::Fuzzy;
        }
        else {
            return ::HIR::Compare::Unequal;
        }
        }
    TU_ARMA(Infer, e) {
        switch(e.ty_class)
        {
        case ::HIR::InferClass::Integer:
        case ::HIR::InferClass::Float:
            return ::HIR::Compare::Equal;
        default:
            DEBUG("Fuzzy Clone impl for ivar?");
            return ::HIR::Compare::Fuzzy;
        }
        }
    TU_ARMA(Generic, e) {
        // TODO: Store this result - or even pre-calculate it.
        return this->iterate_bounds_traits(sp, ty, m_lang_Clone, [&](HIR::Compare _cmp, const ::HIR::TypeRef& be_type, const ::HIR::GenericPath& be_trait, const CachedBound& info)->bool {
            return true;
            }) ? ::HIR::Compare::Equal : ::HIR::Compare::Unequal ;
        }
    TU_ARMA(Primitive, e) {
        if( e == ::HIR::CoreType::Str )
            return ::HIR::Compare::Unequal;
        return ::HIR::Compare::Equal;
        }
    TU_ARMA(Borrow, e) {
        return e.type == ::HIR::BorrowType::Shared ? ::HIR::Compare::Equal : ::HIR::Compare::Unequal ;
        }
    TU_ARMA(Pointer, e) {
        return ::HIR::Compare::Equal;
        }
    TU_ARMA(Tuple, e) {
        auto rv = ::HIR::Compare::Equal;
        for(const auto& sty : e)
            rv &= type_is_clone(sp, sty);
        return rv;
        }
    TU_ARMA(Slice, e) {
        return ::HIR::Compare::Unequal;
        }
    TU_ARMA(Function, e) {
        return ::HIR::Compare::Equal;
        }
    TU_ARMA(Closure, e) {
        // NOTE: This isn't strictly true, we're leaving the actual checking up to the validate pass
        // TODO: Determine captures earlier and check captures here
        return ::HIR::Compare::Equal;
        }
    TU_ARMA(Array, e) {
        return type_is_clone(sp, e.inner);
        }
    }
    throw "";
}
// Checks if a type can unsize to another
// - Returns Compare::Equal if the unsize is possible and fully known
// - Returns Compare::Fuzzy if the unsize is possible, but still unknown.
// - Returns Compare::Unequal if the unsize is impossibe (for any reason)
//
// Closure is called `get_new_type` is true, and the unsize is possible
//
// usecases:
// - Checking for an impl as part of impl selection (return True/False/Maybe with required match for Maybe)
// - Checking for an impl as part of typeck (return True/False/Maybe with unsize possibility OR required equality)
::HIR::Compare TraitResolution::can_unsize(
        const Span& sp, const ::HIR::TypeRef& dst_ty, const ::HIR::TypeRef& src_ty,
        ::std::function<void(::HIR::TypeRef new_dst)>* new_type_callback,
        ::std::function<void(const ::HIR::TypeRef& dst, const ::HIR::TypeRef& src)>* infer_callback
        ) const
{
    TRACE_FUNCTION_F(dst_ty << " <- " << src_ty);

    // 1. Test for type equality
    {
        auto cmp = dst_ty.compare_with_placeholders(sp, src_ty, m_ivars.callback_resolve_infer());
        if( cmp == ::HIR::Compare::Equal )
        {
            return ::HIR::Compare::Unequal;
        }
    }

    // 2. If either side is an ivar, fuzzy.
    if( dst_ty.data().is_Infer() || src_ty.data().is_Infer() )
    {
        // Inform the caller that these two types could unsize to each other
        // - This allows the coercions code to move the coercion rule up
        if( infer_callback )
        {
            (*infer_callback)(dst_ty, src_ty);
        }
        return ::HIR::Compare::Fuzzy;
    }

    {
        bool found_bound = this->iterate_bounds_traits(sp, src_ty, m_lang_Unsize, [&](HIR::Compare cmp, const ::HIR::TypeRef& be_type, const ::HIR::GenericPath& be_trait, const CachedBound& info)->bool {
            const auto& be_dst = be_trait.m_params.m_types.at(0);

            cmp &= dst_ty.compare_with_placeholders(sp, be_dst, m_ivars.callback_resolve_infer());
            if(cmp == ::HIR::Compare::Unequal)  return false;

            if( cmp != ::HIR::Compare::Equal )
            {
                TODO(sp, "Found bound " << dst_ty << "=" << be_dst << " <- " << src_ty << "=" << be_type);
            }
            return true;
            });
        if( found_bound )
        {
            return ::HIR::Compare::Equal;
        }
    }

    // Associated types, check the bounds in the trait.
    if( src_ty.data().is_Path() && src_ty.data().as_Path().path.m_data.is_UfcsKnown() )
    {
        ::HIR::Compare  rv = ::HIR::Compare::Equal;
        const auto& pe = src_ty.data().as_Path().path.m_data.as_UfcsKnown();
        auto monomorph_cb = MonomorphStatePtr(&pe.type, &pe.trait.m_params, nullptr);
        auto found_bound = this->iterate_aty_bounds(sp, pe, [&](const ::HIR::TraitPath& bound) {
            if( bound.m_path.m_path != m_lang_Unsize )
                return false;
            const auto& be_dst_tpl = bound.m_path.m_params.m_types.at(0);
            ::HIR::TypeRef  tmp_ty;
            const auto& be_dst = monomorph_cb.maybe_monomorph_type(sp, tmp_ty, be_dst_tpl);

            auto cmp = dst_ty.compare_with_placeholders(sp, be_dst, m_ivars.callback_resolve_infer());
            if(cmp == ::HIR::Compare::Unequal)  return false;

            if( cmp != ::HIR::Compare::Equal )
            {
                DEBUG("[can_unsize] > Found bound (fuzzy) " << dst_ty << "=" << be_dst << " <- " << src_ty);
                rv = ::HIR::Compare::Fuzzy;
            }
            return true;
            });
        if( found_bound )
        {
            return rv;
        }
    }

    // Struct<..., T, ...>: Unsize<Struct<..., U, ...>>
    if( dst_ty.data().is_Path() && src_ty.data().is_Path() )
    {
        bool dst_is_unsizable = dst_ty.data().as_Path().binding.is_Struct() && dst_ty.data().as_Path().binding.as_Struct()->m_struct_markings.can_unsize;
        bool src_is_unsizable = src_ty.data().as_Path().binding.is_Struct() && src_ty.data().as_Path().binding.as_Struct()->m_struct_markings.can_unsize;
        if( dst_is_unsizable || src_is_unsizable )
        {
            DEBUG("Struct unsize? " << dst_ty << " <- " << src_ty);
            const auto& str = *dst_ty.data().as_Path().binding.as_Struct();
            const auto& dst_gp = dst_ty.data().as_Path().path.m_data.as_Generic();
            const auto& src_gp = src_ty.data().as_Path().path.m_data.as_Generic();

            if( dst_gp == src_gp )
            {
                DEBUG("Can't Unsize, destination and source are identical");
                return ::HIR::Compare::Unequal;
            }
            else if( dst_gp.m_path == src_gp.m_path )
            {
                DEBUG("Checking for Unsize " << dst_gp << " <- " << src_gp);
                // Structures are equal, add the requirement that the ?Sized parameter also impl Unsize
                const auto& dst_inner = m_ivars.get_type( dst_gp.m_params.m_types.at(str.m_struct_markings.unsized_param) );
                const auto& src_inner = m_ivars.get_type( src_gp.m_params.m_types.at(str.m_struct_markings.unsized_param) );

                auto cb = [&](auto d){
                    assert(new_type_callback);

                    // Re-create structure with s/d
                    auto dst_gp_new = dst_gp.clone();
                    dst_gp_new.m_params.m_types.at(str.m_struct_markings.unsized_param) = mv$(d);
                    (*new_type_callback)( ::HIR::TypeRef::new_path(mv$(dst_gp_new), &str) );
                    };
                if( new_type_callback )
                {
                    ::std::function<void(::HIR::TypeRef)>   cb_p = cb;
                    return this->can_unsize(sp, dst_inner, src_inner, &cb_p, infer_callback);
                }
                else
                {
                    return this->can_unsize(sp, dst_inner, src_inner, nullptr, infer_callback);
                }
            }
            else
            {
                DEBUG("Can't Unsize, destination and source are different structs");
                return ::HIR::Compare::Unequal;
            }
        }
    }

    // (Trait) <- Foo
    if( const auto* de = dst_ty.data().opt_TraitObject() )
    {
        // TODO: Check if src_ty is !Sized
        // - Only allowed if the source is a trait object with the same data trait and lesser bounds

        DEBUG("TraitObject unsize? " << dst_ty << " <- " << src_ty);

        // (Trait) <- (Trait+Foo)
        if( const auto* se = src_ty.data().opt_TraitObject() )
        {
            auto rv = ::HIR::Compare::Equal;
            // 1. Data trait must be the same (TODO: Fuzzy)
            if( de->m_trait != se->m_trait )
            {
                return ::HIR::Compare::Unequal;
            }

            // 2. Destination markers must be a strict subset
            for(const auto& mt : de->m_markers)
            {
                // TODO: Fuzzy match
                bool found = false;
                for(const auto& omt : se->m_markers) {
                    if( omt == mt ) {
                        found = true;
                        break;
                    }
                }
                if( !found ) {
                    // Return early.
                    return ::HIR::Compare::Unequal;
                }
            }

            if( rv == ::HIR::Compare::Fuzzy && new_type_callback )
            {
                // TODO: Inner type
            }
            return ::HIR::Compare::Equal;
        }

        bool good;
        ::HIR::Compare  total_cmp = ::HIR::Compare::Equal;

        ::HIR::TypeData::Data_TraitObject  tmp_e;
        tmp_e.m_trait.m_path = de->m_trait.m_path.m_path;

        // Check data trait first.
        if( de->m_trait.m_path.m_path == ::HIR::SimplePath() ) {
            ASSERT_BUG(sp, de->m_markers.size() > 0, "TraitObject with no traits - " << dst_ty);
            good = true;
        }
        else {
            good = find_trait_impls(sp, de->m_trait.m_path.m_path, de->m_trait.m_path.m_params, src_ty,
                [&](const auto impl, auto cmp) {
                    if( cmp == ::HIR::Compare::Unequal )
                        return false;
                    total_cmp &= cmp;
                    tmp_e.m_trait.m_path.m_params = impl.get_trait_params();
                    for(const auto& aty : de->m_trait.m_type_bounds) {
                        auto atyv = impl.get_type(aty.first.c_str());
                        if( atyv == ::HIR::TypeRef() )
                        {
                            // Get the trait from which this associated type comes.
                            // Insert a UfcsKnown path for that
                            auto p = ::HIR::Path( src_ty.clone(), de->m_trait.m_path.clone(), aty.first );
                            // Run EAT
                            atyv = this->expand_associated_types( sp, ::HIR::TypeRef::new_path( mv$(p), {} ) );
                        }
                        TODO(sp, "Monomorph " << aty.second.source_trait << " from " << de->m_trait.m_path.m_params << " to " << tmp_e.m_trait.m_path.m_params);
                        tmp_e.m_trait.m_type_bounds[aty.first] = ::HIR::TraitPath::AtyEqual {
                            aty.second.source_trait.clone(),    // TODO: Monomorph from `de->m_trait.m_path.m_params` to `tmp_e.m_trait.m_path.m_params`
                            mv$(atyv)
                        };
                    }
                    return true;
                });
        }

        // Then markers
        auto cb = [&](const auto impl, auto cmp){
            if( cmp == ::HIR::Compare::Unequal )
                return false;
            total_cmp &= cmp;
            tmp_e.m_markers.back().m_params = impl.get_trait_params();
            return true;
            };
        for(const auto& marker : de->m_markers)
        {
            if(!good)   break;
            tmp_e.m_markers.push_back( marker.m_path );
            good &= find_trait_impls(sp, marker.m_path, marker.m_params, src_ty, cb);
        }

        if( good && total_cmp == ::HIR::Compare::Fuzzy && new_type_callback )
        {
            (*new_type_callback)( ::HIR::TypeRef(mv$(tmp_e)) );
        }
        return total_cmp;
    }

    // [T] <- [T; n]
    if( const auto* de = dst_ty.data().opt_Slice() )
    {
        if( const auto* se = src_ty.data().opt_Array() )
        {
            DEBUG("Array unsize? " << de->inner << " <- " << se->inner);
            auto cmp = de->inner.compare_with_placeholders(sp, se->inner, m_ivars.callback_resolve_infer());
            // TODO: Indicate to caller that for this to be true, these two must be the same.
            // - I.E. if true, equate these types
            if(cmp == ::HIR::Compare::Fuzzy && new_type_callback)
            {
                (*new_type_callback)( ::HIR::TypeRef::new_slice( se->inner.clone() ) );
            }
            return cmp;
        }
    }

    DEBUG("Can't unsize, no rules matched");
    return ::HIR::Compare::Unequal;
}
const ::HIR::TypeRef* TraitResolution::type_is_owned_box(const Span& sp, const ::HIR::TypeRef& ty) const
{
    if(const auto* e = ty.data().opt_Path()) {
        if(const auto* pe = e->path.m_data.opt_Generic()) {
            if( pe->m_path == m_lang_Box )
            {
                return &this->m_ivars.get_type( pe->m_params.m_types.at(0) );
            }
        }
    }
    return nullptr;
}


// -------------------------------------------------------------------------------------------------------------------
//
// -------------------------------------------------------------------------------------------------------------------
const ::HIR::TypeRef* TraitResolution::autoderef(const Span& sp, const ::HIR::TypeRef& ty_in,  ::HIR::TypeRef& tmp_type) const
{
    const auto& ty = this->m_ivars.get_type(ty_in);
    if(const auto* e = ty.data().opt_Borrow()) {
        DEBUG("Deref " << ty << " into " << e->inner);
        return &this->m_ivars.get_type(e->inner);
    }
    // HACK?: Just doing `*[1,2,3]` doesn't work, but this is needed to allow `[1,2,3].iter()` to work
    else if(const auto* e = ty.data().opt_Array()) {
        DEBUG("Deref " << ty << " into [" << e->inner << "]");
        tmp_type = ::HIR::TypeRef::new_slice( e->inner.clone() );
        return &tmp_type;
    }
    // Shortcut, don't look up a Deref impl for primitives or slices
    else if( ty.data().is_Slice() || ty.data().is_Primitive() || ty.data().is_Tuple() || ty.data().is_Array() ) {
        return nullptr;
    }
    else {
#if 0
        auto it = m_deref_cache.find(ty_in);
        if(it != m_deref_cache.end()) {
            if(it->second == HIR::TypeRef())
                return nullptr;
            return &it->second;
        }
#endif

        bool succ = this->find_trait_impls(sp, m_lang_Deref, ::HIR::PathParams {}, ty, [&](auto impls, auto match) {
            tmp_type = impls.get_type("Target");
            if( tmp_type == ::HIR::TypeRef() )
            {
                tmp_type = ::HIR::TypeRef::new_path(
                    ::HIR::Path( ty.clone(), m_lang_Deref, "Target" ),
                    ::HIR::TypePathBinding::make_Opaque({})
                    );
            }
            else
            {
                this->expand_associated_types_inplace(sp, tmp_type, {});
            }
            return true;
            });
        if( succ ) {
            DEBUG("Deref " << ty << " into " << tmp_type);
            return &tmp_type;
        }
        else {
            return nullptr;
        }
    }
}

unsigned int TraitResolution::autoderef_find_method(const Span& sp,
        const HIR::t_trait_list& traits, const ::std::vector<unsigned>& ivars, const ::HIR::TypeRef& top_ty, const char* method_name,
        /* Out -> */::std::vector<::std::pair<AutoderefBorrow,::HIR::Path>>& possibilities
        ) const
{
    TRACE_FUNCTION_F("{" << top_ty << "}." << method_name);
    unsigned int deref_count = 0;
    ::HIR::TypeRef  tmp_type;   // Temporary type used for handling Deref
    const auto& top_ty_r = this->m_ivars.get_type(top_ty);
    const auto* current_ty = &top_ty_r;


    // Correct algorithm:
    // - Find any available method with a receiver type of `T`
    // - If no, try &T
    // - If no, try &mut T
    // - If no, try &move T
    // - If no, dereference T and try again
    auto cur_access = MethodAccess::Move; // Assume that the input value is movable
    do
    {
        const auto& ty = this->m_ivars.get_type(*current_ty);
        auto should_pause = [](const auto& ty)->bool {
            if( type_is_unbounded_infer(ty) ) {
                DEBUG("- Ivar" << ty << ", pausing");
                return true;
            }
            if(ty.data().is_Path() && ty.data().as_Path().binding.is_Unbound()) {
                DEBUG("- Unbound type path " << ty << ", pausing");
                return true;
            }
            return false;
            };
        if( should_pause(ty) ) {
            return ~0u;
        }
        if( ty.data().is_Borrow() && should_pause( this->m_ivars.get_type(ty.data().as_Borrow().inner) ) ) {
            return ~0u;
        }
        // TODO: Pause on Box<_>?
        DEBUG(deref_count << ": " << ty);

        // Non-referenced
        if( this->find_method(sp, traits, ivars, ty, method_name,  cur_access, AutoderefBorrow::None, possibilities) )
        {
            DEBUG("FOUND *{" << deref_count << "}, fcn_path = " << possibilities.back().second);
        }

        // Auto-ref
        auto borrow_ty = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, ty.clone());
        if( this->find_method(sp, traits, ivars, borrow_ty, method_name,  MethodAccess::Move, AutoderefBorrow::Shared, possibilities) )
        {
            DEBUG("FOUND & *{" << deref_count << "}, fcn_path = " << possibilities.back().second);
        }
        borrow_ty.get_unique().as_Borrow().type = ::HIR::BorrowType::Unique;
        if( cur_access >= MethodAccess::Unique && this->find_method(sp, traits, ivars, borrow_ty, method_name,  MethodAccess::Move, AutoderefBorrow::Unique, possibilities) )
        {
            DEBUG("FOUND &mut *{" << deref_count << "}, fcn_path = " << possibilities.back().second);
        }
        borrow_ty.get_unique().as_Borrow().type = ::HIR::BorrowType::Owned;
        if( cur_access >= MethodAccess::Move && this->find_method(sp, traits, ivars, borrow_ty, method_name,  MethodAccess::Move, AutoderefBorrow::Owned, possibilities) )
        {
            DEBUG("FOUND &move *{" << deref_count << "}, fcn_path = " << possibilities.back().second);
        }
        if( !possibilities.empty() )
        {
            DEBUG("FOUND " << possibilities.size() << " options: " << possibilities);
            return deref_count;
        }

        // Auto-dereference
        deref_count += 1;
        if( const auto* typ = this->type_is_owned_box(sp, ty) )
        {
            // `cur_access` can stay as-is (Box can be moved out of)
            current_ty = typ;
        }
        else
        {
            // TODO: Update `cur_access` based on the avaliable Deref impls
            current_ty = this->autoderef(sp, ty,  tmp_type);
        }
    } while(current_ty);

    // No method found, return an empty list and return 0
    assert( possibilities.empty() );
    return 0;
}

::std::ostream& operator<<(::std::ostream& os, const TraitResolution::AutoderefBorrow& x)
{
    switch(x)
    {
    case TraitResolution::AutoderefBorrow::None:    os << "None";   break;
    case TraitResolution::AutoderefBorrow::Shared:  os << "Shared"; break;
    case TraitResolution::AutoderefBorrow::Unique:  os << "Unique"; break;
    case TraitResolution::AutoderefBorrow::Owned:   os << "Owned";  break;
    }
    return os;
}
::std::ostream& operator<<(::std::ostream& os, const TraitResolution::AllowedReceivers& x)
{
    switch(x)
    {
    case TraitResolution::AllowedReceivers::All: os << "All";    break;
    case TraitResolution::AllowedReceivers::AnyBorrow:   os << "AnyBorrow";  break;
    case TraitResolution::AllowedReceivers::SharedBorrow:os << "SharedBorrow";  break;
    case TraitResolution::AllowedReceivers::Value:   os << "Value";  break;
    case TraitResolution::AllowedReceivers::Box: os << "Box";    break;
    }
    return os;
}
::std::ostream& operator<<(::std::ostream& os, const TraitResolution::MethodAccess& x)
{
    switch(x)
    {
    case TraitResolution::MethodAccess::Shared: os << "Shared"; break;
    case TraitResolution::MethodAccess::Unique: os << "Unique"; break;
    case TraitResolution::MethodAccess::Move:   os << "Move";   break;
    }
    return os;
}

// Checks that a given real receiver type matches a desired receiver type (with the correct access)
// Returns the pointer to the `Self` type, or nullptr if there's a mismatch
const ::HIR::TypeRef* TraitResolution::check_method_receiver(const Span& sp, const ::HIR::Function& fcn, const ::HIR::TypeRef& ty, TraitResolution::MethodAccess access) const
{
    switch(fcn.m_receiver)
    {
    case ::HIR::Function::Receiver::Free:
        // Free functions are never usable
        return nullptr;
    case ::HIR::Function::Receiver::Value:
        if( access >= TraitResolution::MethodAccess::Move )
        {
            return &this->m_ivars.get_type(ty);
        }
        break;
    case ::HIR::Function::Receiver::BorrowOwned:
        if( !ty.data().is_Borrow() )
            ;
        else if( ty.data().as_Borrow().type != ::HIR::BorrowType::Owned )
            ;
        else if( access < TraitResolution::MethodAccess::Move )
            ;
        else
        {
            return &this->m_ivars.get_type(ty.data().as_Borrow().inner);
        }
        break;
    case ::HIR::Function::Receiver::BorrowUnique:
        if( !ty.data().is_Borrow() )
            ;
        else if( ty.data().as_Borrow().type != ::HIR::BorrowType::Unique )
            ;
        else if( access < TraitResolution::MethodAccess::Unique )
            ;
        else
        {
            return &this->m_ivars.get_type(ty.data().as_Borrow().inner);
        }
        break;
    case ::HIR::Function::Receiver::BorrowShared:
        if( !ty.data().is_Borrow() )
            ;
        else if( ty.data().as_Borrow().type != ::HIR::BorrowType::Shared )
            ;
        else if( access < TraitResolution::MethodAccess::Shared )
            ;
        else
        {
            return &this->m_ivars.get_type(ty.data().as_Borrow().inner);
        }
        break;
    case ::HIR::Function::Receiver::Custom:
        ASSERT_BUG(sp, visit_ty_with(fcn.m_receiver_type, [](const HIR::TypeRef& v){ return v.data().is_Generic() && v.data().as_Generic().is_self(); }), fcn.m_receiver_type);
        // TODO: Handle custom-receiver functions
        // - match_test_generics, if it succeeds return the matched Self
        {
            struct GetSelf:
                public ::HIR::MatchGenerics
            {
                const ::HIR::TypeRef*   detected_self_ty = nullptr;
                ::HIR::Compare match_ty(const ::HIR::GenericRef& g, const ::HIR::TypeRef& ty, ::HIR::t_cb_resolve_type _resolve_cb) override {
                    if( g.is_self() )
                    {
                        detected_self_ty = &ty;
                    }
                    return ::HIR::Compare::Equal;
                }
                ::HIR::Compare match_val(const ::HIR::GenericRef& g, const ::HIR::ConstGeneric& sz) override {
                    TODO(Span(), "GetSelf::match_val " << g << " with " << sz);
                }
            }   getself;
            if( fcn.m_receiver_type.match_test_generics(sp, ty, this->m_ivars.callback_resolve_infer(), getself) ) {
                ASSERT_BUG(sp, getself.detected_self_ty, "Unable to determine receiver type when matching " << fcn.m_receiver_type << " and " << ty);
                return &this->m_ivars.get_type(*getself.detected_self_ty);
            }
        }
        return nullptr;
    case ::HIR::Function::Receiver::Box:
        if(const auto* ity = this->type_is_owned_box(sp, ty))
        {
            if( access < TraitResolution::MethodAccess::Move )
            {
            }
            else
            {
                return &this->m_ivars.get_type(*ity);
            }
        }
        break;
    }
    return nullptr;
}

bool TraitResolution::find_method(const Span& sp,
    const HIR::t_trait_list& traits, const ::std::vector<unsigned>& ivars, const ::HIR::TypeRef& ty, const char* method_name, MethodAccess access,
    AutoderefBorrow borrow_type, /* Out -> */::std::vector<::std::pair<AutoderefBorrow,::HIR::Path>>& possibilities
    ) const
{
    bool rv = false;
    TRACE_FUNCTION_FR("ty=" << ty << ", name=" << method_name << ", access=" << access, rv << " " << possibilities);
    auto cb_infer = m_ivars.callback_resolve_infer();

    auto get_ivared_params = [&](const ::HIR::GenericParams& tpl)->::HIR::PathParams {
        unsigned int n_params = tpl.m_types.size();
        assert(n_params <= ivars.size());
        ::HIR::PathParams   trait_params;
        trait_params.m_types.reserve( n_params );
        for(unsigned int i = 0; i < n_params; i++) {
            trait_params.m_types.push_back( ::HIR::TypeRef::new_infer(ivars[i], ::HIR::InferClass::None) );
            ASSERT_BUG(sp, m_ivars.get_type( trait_params.m_types.back() ).data().as_Infer().index == ivars[i], "A method selection ivar was bound");
        }
        ASSERT_BUG(sp, tpl.m_values.empty(), "TODO: Handle value params");
        return trait_params;
        };

    // 1. Search for inherent methods
    // - Inherent methods are searched first.
    // TODO: Have a cache of name+receiver_type to a list of types and impls
    // e.g. `len` `&Self` = `[T]`
    DEBUG("> Inherent methods");
    m_crate.m_inherent_method_cache.find(sp, method_name, ty, m_ivars.callback_resolve_infer(), [&](const HIR::TypeRef& self_ty, const HIR::TypeImpl& impl) {
        if( impl.matches_type(self_ty, m_ivars.callback_resolve_infer()) )
        {
            DEBUG("Found `impl" << impl.m_params.fmt_args() << " " << impl.m_type << "` fn " << method_name/* << " - " << top_ty*/);
            possibilities.push_back(::std::make_pair( borrow_type, ::HIR::Path(self_ty.clone(), method_name, {}) ));
            DEBUG("++ " << possibilities.back());
            rv = true;
        }
        });

    // 2. Search the current trait (if in an impl block)
    if(m_current_trait_path)
    {
        ::HIR::GenericPath final_trait_path;
        const ::HIR::Function* fcn_ptr;
        if( (fcn_ptr = this->trait_contains_method(sp, *m_current_trait_path, *m_current_trait_ptr, ty, method_name, final_trait_path)) )
        {
            DEBUG("- Found trait " << final_trait_path << " (current)");
            if(const auto* self_ty = check_method_receiver(sp, *fcn_ptr, ty, access))
            {
                // If the type is an unbounded ivar, don't check.
                if( TU_TEST1(self_ty->data(), Infer, .is_lit() == false) )
                    return false;

                // Use the set of ivars we were given to populate the trait parameters
                const auto& trait = m_crate.get_trait_by_path(sp, final_trait_path.m_path);
                auto trait_params = get_ivared_params(trait.m_params);
                //auto trait_params = std::move(final_trait_path.m_params);

                bool crate_impl_found = false;
                find_trait_impls_crate(sp, final_trait_path.m_path, &trait_params, *self_ty,  [&](auto impl, auto cmp) {
                    DEBUG("[find_method] " << impl << ", cmp = " << cmp);
                    //magic_found = true;
                    crate_impl_found = true;
                    return true;
                    });
                if( crate_impl_found ) {
                    DEBUG("Found trait impl " << m_current_trait_path->m_path << trait_params << " for " << *self_ty << " ("<<m_ivars.fmt_type(*self_ty)<<")");
                    possibilities.push_back(::std::make_pair( borrow_type, ::HIR::Path(self_ty->clone(), ::HIR::GenericPath( final_trait_path.m_path, mv$(trait_params) ), method_name, {}) ));
                    DEBUG("++ " << possibilities.back());
                    return true;
                }
                else
                {
                }
            }
        }
    }

    // 3. Search generic bounds for a match
    // - If there is a bound on the receiver, then that bound is usable no-matter what
    DEBUG("> Bounds");
    for(const auto& tb : m_trait_bounds)
    {
        const auto& e_type = tb.first.first;
        const auto& e_trait_gp = tb.first.second;
        const auto& e_trait_info = tb.second;

        assert(e_trait_info.trait_ptr);
        // 1. Find the named method in the trait.
        ::HIR::GenericPath final_trait_path;
        const ::HIR::Function* fcn_ptr;
        if( !(fcn_ptr = this->trait_contains_method(sp, e_trait_gp, *e_trait_info.trait_ptr, e_type, method_name,  final_trait_path)) ) {
            DEBUG("- Method '" << method_name << "' missing");
            continue ;
        }
        DEBUG("- Found trait " << final_trait_path << " (bound)");

        // 2. Compare the receiver of the above to this type and the bound.
        if(const auto* self_ty = check_method_receiver(sp, *fcn_ptr, ty, access))
        {
            // If the type is an unbounded ivar, don't check.
            if( TU_TEST1(self_ty->data(), Infer, .is_lit() == false) )
                return false;
            // TODO: Do a fuzzy match here?
            auto cmp = self_ty->compare_with_placeholders(sp, e_type, cb_infer);
            if( cmp == ::HIR::Compare::Equal )
            {
                // TODO: Re-monomorphise final trait using `ty`?
                // - Could collide with legitimate uses of `Self`

                // Found the method, return the UFCS path for it
                possibilities.push_back(::std::make_pair( borrow_type,
                    ::HIR::Path( ::HIR::Path::Data::make_UfcsKnown({
                        self_ty->clone(),
                        mv$(final_trait_path),
                        method_name,
                        {}
                        }) ) ));
                DEBUG("++ " << possibilities.back());
                rv = true;
            }
            else if( cmp == ::HIR::Compare::Fuzzy )
            {
                DEBUG("Fuzzy match checking bounded method - " << *self_ty << " != " << e_type);

                // Found the method, return the UFCS path for it
                possibilities.push_back(::std::make_pair( borrow_type,
                    ::HIR::Path( ::HIR::Path::Data::make_UfcsKnown({
                        self_ty->clone(),
                        mv$(final_trait_path),
                        method_name,
                        {}
                        }) ) ));
                DEBUG("++ " << possibilities.back());
                rv = true;
            }
            else
            {
                DEBUG("> Type mismatch - " << *self_ty << " != " << e_type);
            }
        }
        else
        {
            DEBUG("> Receiver mismatch");
        }
    }

    auto get_inner_type = [this,sp](const ::HIR::TypeRef& ty, ::std::function<bool(const ::HIR::TypeRef&)> cb)->const ::HIR::TypeRef* {
        if( cb(ty) ) {
            return &ty;
        }
        else if( ty.data().is_Borrow() ) {
            const auto& ity = this->m_ivars.get_type(ty.data().as_Borrow().inner);
            if( cb(ity) ) {
                return &ity;
            }
            else {
                return nullptr;
            }
        }
        else {
            auto tp = this->type_is_owned_box(sp, ty);
            if( tp && cb(*tp) ) {
                return tp;
            }
            else {
                return nullptr;
            }
        }
        };

    DEBUG("> Special cases");
    // 4. If the type is a trait object, search for methods on that trait object
    // - NOTE: This isnt mutually exclusive with the below set (an inherent impl of `(Trait)` is valid)
    if( const auto* ityp = get_inner_type(ty, [](const auto& t){ return t.data().is_TraitObject(); }) )
    {
        const auto& e = ityp->data().as_TraitObject();
        const auto& trait = this->m_crate.get_trait_by_path(sp, e.m_trait.m_path.m_path);

        bool found_trait_object = false;
        ::HIR::GenericPath final_trait_path;
        if( const auto* fcn_ptr = this->trait_contains_method(sp, e.m_trait.m_path, trait, ::HIR::TypeRef("Self", GENERIC_Self), method_name,  final_trait_path) )
        {
            DEBUG("- Found trait " << final_trait_path << " (trait object)");
            // - If the receiver is valid, then it's correct (no need to check the type again)
            if(const auto* self_ty_p = check_method_receiver(sp, *fcn_ptr, ty, access))
            {
                possibilities.push_back(::std::make_pair(borrow_type, ::HIR::Path(self_ty_p->clone(), mv$(final_trait_path), method_name, {}) ));
                DEBUG("++ " << possibilities.back());
                rv = true;
                found_trait_object = true;
            }
        }

        // If the method was found on the trait object, prefer that over all others.
        if( found_trait_object )
        {
            return rv;
        }
    }

    // 5. Mutually exclusive searches
    // - Erased type - `impl Trait`
    if( const auto* ityp = get_inner_type(ty, [](const auto& t){ return t.data().is_ErasedType(); }) )
    {
        const auto& e = ityp->data().as_ErasedType();
        for(const auto& trait_path : e.m_traits)
        {
            const auto& trait = this->m_crate.get_trait_by_path(sp, trait_path.m_path.m_path);

            ::HIR::GenericPath final_trait_path;
            if( const auto* fcn_ptr = this->trait_contains_method(sp, trait_path.m_path, trait, ::HIR::TypeRef("Self", GENERIC_Self), method_name,  final_trait_path) )
            {
                DEBUG("- Found trait " << final_trait_path << " (erased type)");

                if(const auto* self_ty_p = check_method_receiver(sp, *fcn_ptr, ty, access))
                {
                    possibilities.push_back(::std::make_pair(borrow_type, ::HIR::Path(self_ty_p->clone(), mv$(final_trait_path), method_name, {}) ));
                    DEBUG("++ " << possibilities.back());
                    rv = true;
                }
            }
        }
    }
    // Generics: Nothing except the bounds (Which have already been checked)
    else if( get_inner_type(ty, [](const auto& t){ return t.data().is_Generic(); }) )
    {
    }
    // UfcsKnown paths: Can have trait bounds added by the definer
    else if( const auto* ityp = get_inner_type(ty, [](const auto& t){ return t.data().is_Path() && t.data().as_Path().path.m_data.is_UfcsKnown(); }) )
    {
        const auto& e = ityp->data().as_Path().path.m_data.as_UfcsKnown();
        DEBUG("UfcsKnown - Search associated type bounds in trait - " << e.trait);

        // UFCS known - Assuming that it's reached the maximum resolvable level (i.e. a type within is generic), search for trait bounds on the type

        // `Self` = `*.type`
        // `/*I:#*/` := `e.trait.m_params`
        auto monomorph_cb = MonomorphStatePtr(&e.type, &e.trait.m_params, nullptr);

        const auto& trait = this->m_crate.get_trait_by_path(sp, e.trait.m_path);
        const auto& assoc_ty = trait.m_types.at( e.item );
        // NOTE: The bounds here have 'Self' = the type
        for(const auto& bound : assoc_ty.m_trait_bounds )
        {
            ASSERT_BUG(sp, bound.m_trait_ptr, "Pointer to trait " << bound.m_path << " not set in " << e.trait.m_path);
            ::HIR::GenericPath final_trait_path;

            if( const auto* fcn_ptr = this->trait_contains_method(sp, bound.m_path, *bound.m_trait_ptr, ::HIR::TypeRef("Self", GENERIC_Self), method_name,  final_trait_path) )
            {
                DEBUG("- Found trait " << final_trait_path << " (UFCS Known, aty bounds)");

                if(const auto* self_ty_p = check_method_receiver(sp, *fcn_ptr, ty, access))
                {
                    if( *self_ty_p == *ityp )
                    {
                        if( monomorphise_pathparams_needed(final_trait_path.m_params) ) {
                            final_trait_path.m_params = monomorph_cb.monomorph_path_params(sp, final_trait_path.m_params, false);
                            DEBUG("- Monomorph to " << final_trait_path);
                        }

                        // Found the method, return the UFCS path for it
                        possibilities.push_back(::std::make_pair( borrow_type, ::HIR::Path(self_ty_p->clone(), mv$(final_trait_path), method_name, {}) ));
                        DEBUG("++ " << possibilities.back());
                        rv = true;
                    }
                }
            }
        }

        // Search `<Self as Trait>::Name` bounds on the trait itself
        for(const auto& bound : trait.m_params.m_bounds)
        {
            if( ! bound.is_TraitBound() ) continue ;
            const auto& be = bound.as_TraitBound();

            if( ! be.type.data().is_Path() )   continue ;
            if( ! be.type.data().as_Path().binding.is_Opaque() )   continue ;

            const auto& be_type_pe = be.type.data().as_Path().path.m_data.as_UfcsKnown();
            if( be_type_pe.type != ::HIR::TypeRef("Self", GENERIC_Self) )
                continue ;
            if( be_type_pe.trait.m_path != e.trait.m_path )
                continue ;
            if( be_type_pe.item != e.item )
                continue ;

            // Found such a bound, now to test if it is useful

            ::HIR::GenericPath final_trait_path;
            if( const auto* fcn_ptr = this->trait_contains_method(sp, be.trait.m_path, *be.trait.m_trait_ptr, ::HIR::TypeRef("Self", GENERIC_Self), method_name,  final_trait_path) )
            {
                DEBUG("- Found trait " << final_trait_path << " (UFCS Known, trait bounds)");

                if(const auto* self_ty_p = check_method_receiver(sp, *fcn_ptr, ty, access))
                {
                    if( *self_ty_p == *ityp )
                    {
                        if( monomorphise_pathparams_needed(final_trait_path.m_params) ) {
                            final_trait_path.m_params = monomorph_cb.monomorph_path_params(sp, final_trait_path.m_params, false);
                            DEBUG("- Monomorph to " << final_trait_path);
                        }

                        // Found the method, return the UFCS path for it
                        possibilities.push_back(::std::make_pair( borrow_type, ::HIR::Path(self_ty_p->clone(), mv$(final_trait_path), method_name, {}) ));
                        DEBUG("++ " << possibilities.back());
                        rv = true;
                    }
                }
            }
        }
    }
    else
    {
    }

    // 6. Search for trait methods (using currently in-scope traits)
    DEBUG("> Trait methods");
    for(const auto& trait_ref : ::reverse(traits))
    {
        if( trait_ref.first == nullptr )
            break;

        ::HIR::GenericPath final_trait_path;
        const ::HIR::Function* fcn_ptr;
        if( !(fcn_ptr = this->trait_contains_method(sp, *trait_ref.first, *trait_ref.second, ::HIR::TypeRef("Self", GENERIC_Self), method_name,  final_trait_path)) )
            continue ;
        DEBUG("- Found trait " << final_trait_path << " (in scope)");

        if( const auto* self_ty_p = check_method_receiver(sp, *fcn_ptr, ty, access) )
        {
            const auto& self_ty = *self_ty_p;
            DEBUG("Search for impl of " << *trait_ref.first << " for " << self_ty);

            // Use the set of ivars we were given to populate the trait parameters
            ::HIR::PathParams   trait_params = get_ivared_params(trait_ref.second->m_params);

            // TODO: Re-monomorphise the trait path!

            bool magic_found = false;
            bool crate_impl_found = false;

            crate_impl_found = find_trait_impls_magic(sp, *trait_ref.first, trait_params, self_ty,  [&](auto impl, auto cmp) {
                return true;
                });

            // NOTE: This just detects the presence of a trait impl, not the specifics
            find_trait_impls_crate(sp, *trait_ref.first, &trait_params, self_ty,  [&](auto impl, auto cmp) {
                DEBUG("[find_method] " << impl << ", cmp = " << cmp);
                magic_found = true;
                crate_impl_found = true;
                return true;
                });
            if( crate_impl_found ) {
                DEBUG("Found trait impl " << *trait_ref.first << trait_params << " for " << self_ty << " ("<<m_ivars.fmt_type(self_ty)<<")");
                possibilities.push_back(::std::make_pair( borrow_type, ::HIR::Path(self_ty.clone(), ::HIR::GenericPath( *trait_ref.first, mv$(trait_params) ), method_name, {}) ));
                DEBUG("++ " << possibilities.back());
                rv = true;
            }
        }
        else
        {
            DEBUG("> Incorrect receiver");
        }
    }

    return rv;
}

unsigned int TraitResolution::autoderef_find_field(const Span& sp, const ::HIR::TypeRef& top_ty, const char* field_name,  /* Out -> */::HIR::TypeRef& field_type) const
{
    unsigned int deref_count = 0;
    ::HIR::TypeRef  tmp_type;   // Temporary type used for handling Deref
    const auto* current_ty = &top_ty;
    if(const auto* e = this->m_ivars.get_type(top_ty).data().opt_Borrow()) {
        current_ty = &e->inner;
        deref_count += 1;
    }

    do {
        const auto& ty = this->m_ivars.get_type(*current_ty);
        if( ty.data().is_Infer() ) {
            return ~0u;
        }
        if(ty.data().is_Path() && ty.data().as_Path().binding.is_Unbound()) {
            return ~0u;
        }

        if( this->find_field(sp, ty, field_name, field_type) ) {
            return deref_count;
        }

        // 3. Dereference and try again
        deref_count += 1;
        current_ty = this->autoderef(sp, ty,  tmp_type);
    } while( current_ty );

    if(/*const auto* e =*/ this->m_ivars.get_type(top_ty).data().opt_Borrow()) {
        const auto& ty = this->m_ivars.get_type(top_ty);

        if( find_field(sp, ty, field_name, field_type) ) {
            return 0;
        }
    }

    // Dereference failed! This is a hard error (hitting _ is checked above and returns ~0)
    this->m_ivars.dump();
    TODO(sp, "Error when no field could be found, but type is known - (: " << top_ty << ")." << field_name);
}
bool TraitResolution::find_field(const Span& sp, const ::HIR::TypeRef& ty, const char* name,  /* Out -> */::HIR::TypeRef& field_ty) const
{
    if(const auto* e = ty.data().opt_Path())
    {
        TU_MATCH_HDRA( (e->binding), {)
        TU_ARMA(Unbound, be) {
            // Wut?
            TODO(sp, "Handle TypePathBinding::Unbound - " << ty);
            }
        TU_ARMA(Opaque, be) {
            // Ignore, no fields on an opaque
            }
        TU_ARMA(Struct, be) {
            // Has fields!
            const auto& str = *be;
            const auto& params = e->path.m_data.as_Generic().m_params;
            auto monomorph = MonomorphStatePtr(nullptr, &params, nullptr);
            TU_MATCH_HDRA( (str.m_data), {)
            TU_ARMA(Unit, se) {
                // No fields on a unit struct
                }
            TU_ARMA(Tuple, se) {
                for( unsigned int i = 0; i < se.size(); i ++ )
                {
                    DEBUG(i << ": " << se[i].publicity << ", " << this->m_vis_path << " : " << se[i].ent);
                    if( se[i].publicity.is_visible(this->m_vis_path) && FMT(i) == name ) {
                        field_ty = monomorph.monomorph_type(sp, se[i].ent);
                        return true;
                    }
                }
                }
            TU_ARMA(Named, se) {
                for( const auto& fld : se )
                {
                    DEBUG(fld.first << ": " << fld.second.publicity << ", " << this->m_vis_path << " : " << fld.second.ent);
                    if( fld.second.publicity.is_visible(this->m_vis_path) && fld.first == name ) {
                        field_ty = monomorph.monomorph_type(sp, fld.second.ent);
                        return true;
                    }
                }
                }
            }
            }
        TU_ARMA(Enum, be) {
            // No fields on enums either
            }
        TU_ARMA(ExternType, be) {
            // No fields on extern types
            }
        TU_ARMA(Union, be) {
            const auto& unm = *be;
            const auto& params = e->path.m_data.as_Generic().m_params;
            auto monomorph = MonomorphStatePtr(nullptr, &params, nullptr);

            for( const auto& fld : unm.m_variants )
            {
                // TODO: Privacy
                if( fld.second.publicity.is_visible(this->m_vis_path) && fld.first == name ) {
                    field_ty = monomorph.monomorph_type(sp, fld.second.ent);
                    return true;
                }
            }
            }
        }
    }
    else if(const auto* e = ty.data().opt_Tuple())
    {
        for( unsigned int i = 0; i < e->size(); i ++ )
        {
            if( FMT(i) == name ) {
                field_ty = (*e)[i].clone();
                return true;
            }
        }
    }
    else {
    }
    return false;
}
