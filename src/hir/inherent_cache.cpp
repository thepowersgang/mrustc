/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/inherent_cache.cpp
 * - Inherent method lookup cache
 */
#include "inherent_cache.hpp"
#include "type.hpp"
#include <hir/hir.hpp>


void HIR::InherentCache::Lowest::insert(const Span& sp, const HIR::TypeImpl& impl)
{
    const auto& type = impl.m_type;
    if(const auto* path = type.get_sort_path())
    {
        this->named[*path].push_back(&impl);
    }
    else if( type.data().is_Path() || type.data().is_Generic() )
    {
        this->generic.push_back(&impl);
    }
    else
    {
        this->non_named.push_back(&impl);
    }
}
void HIR::InherentCache::Lowest::iterate(const HIR::TypeRef& type, InherentCache::inner_callback_t& cb) const
{
    auto visit = [&](const list_t& l) {
        for(const HIR::TypeImpl* impl_ptr : l)
        {
            cb(type, *impl_ptr);
        }
        };

    visit(this->generic);

    if(const auto* path = type.get_sort_path())
    {
        auto it = this->named.find(*path);
        if(it != this->named.end())
        {
            visit(it->second);
        }
    }
    else if( type.data().is_Path() || type.data().is_Generic() )
    {
        // Already handled by the unconditional generic
    }
    else
    {
        visit(this->non_named);
    }
}

void HIR::InherentCache::Inner::insert(const Span& sp, const HIR::TypeRef& cur_ty, const HIR::TypeImpl& impl)
{
    struct H {
        static void insert_inner(const Span& sp, const HIR::TypeRef& inner_ty, const HIR::TypeImpl& impl, std::unique_ptr<Inner>& slot) {
            if(!slot)  slot = ::std::make_unique<Inner>();
            slot->insert(sp, inner_ty, impl);
        }
    };
    TU_MATCH_HDRA( (cur_ty.data()), { )
    default:
        BUG(sp, "Unknown receiver type - " << cur_ty);
    TU_ARMA(Generic, te) {
        if( te.is_self() ) {
            m_byvalue.insert(sp, impl);
        }
        else {
            BUG(sp, "Receiver generic not `Self` - " << cur_ty);
        }
        }
    TU_ARMA(Borrow, te) {
        switch(te.type)
        {
        case ::HIR::BorrowType::Shared:
            H::insert_inner(sp, te.inner, impl, m_ref);
            break;
        case ::HIR::BorrowType::Unique:
            H::insert_inner(sp, te.inner, impl, m_ref_mut);
            break;
        case ::HIR::BorrowType::Owned:
            H::insert_inner(sp, te.inner, impl, m_ref_move);
            break;
        }
        }
    TU_ARMA(Pointer, te) {
        switch(te.type)
        {
        case ::HIR::BorrowType::Shared:
            H::insert_inner(sp, te.inner, impl, m_ptr);
            break;
        case ::HIR::BorrowType::Unique:
            H::insert_inner(sp, te.inner, impl, m_ptr_mut);
            break;
        case ::HIR::BorrowType::Owned:
            H::insert_inner(sp, te.inner, impl, m_ptr_move);
            break;
        }
        }
    TU_ARMA(Path, te) {
        ASSERT_BUG(sp, te.path.m_data.is_Generic(), "Receiver path not a generic path - " << cur_ty);
        const auto& gp = te.path.m_data.as_Generic();
        ASSERT_BUG(sp, gp.m_params.m_types.size() > 0, "Receiver path has no type params (needs at least one) - " << cur_ty);
        m_path[gp.m_path].insert(sp, gp.m_params.m_types.at(0), impl);
        }
    }
}
void HIR::InherentCache::Inner::find(const Span& sp, const HIR::TypeRef& cur_ty_act, t_cb_resolve_type ty_res, InherentCache::inner_callback_t& cb) const
{
    const auto& cur_ty = ty_res(cur_ty_act);
    m_byvalue.iterate(cur_ty, cb);

    const Inner* inner = nullptr;
    const HIR::TypeRef* inner_ty = nullptr;
    TU_MATCH_HDRA( (cur_ty.data()), { )
    default:
        // No recursion possible
        break;
    TU_ARMA(Borrow, te) {
        inner_ty = &te.inner;
        switch(te.type)
        {
        case ::HIR::BorrowType::Shared: inner = m_ref.get();        break;
        case ::HIR::BorrowType::Unique: inner = m_ref_mut.get();    break;
        case ::HIR::BorrowType::Owned:  inner = m_ref_move.get();   break;
        }
        }
    TU_ARMA(Pointer, te) {
        inner_ty = &te.inner;
        switch(te.type)
        {
        case ::HIR::BorrowType::Shared: inner = m_ptr.get();        break;
        case ::HIR::BorrowType::Unique: inner = m_ptr_mut.get();    break;
        case ::HIR::BorrowType::Owned:  inner = m_ptr_move.get();   break;
        }
        }
    TU_ARMA(Path, te) {
        if( te.path.m_data.is_Generic())
        {
            const auto& gp = te.path.m_data.as_Generic();
            if( gp.m_params.m_types.size() > 0 )
            {
                auto it = m_path.find(gp.m_path);
                if(it != m_path.end()) {
                    inner_ty = &gp.m_params.m_types.at(0);
                    inner = &it->second;
                }
            }
        }
        }
    }

    if(inner) {
        assert(inner_ty);
        inner->find(sp, *inner_ty, ty_res, cb);
    }
}

void HIR::InherentCache::insert_all(const Span& sp, const HIR::TypeImpl& impl, const HIR::SimplePath& lang_Box)
{
    for(const auto& m : impl.m_methods)
    {
        const auto& name = m.first;
        const auto& fcn = m.second.data;
        struct H {
            static Inner& g(std::unique_ptr<Inner>& slot) {
                if(!slot)   slot = std::make_unique<Inner>();
                return *slot;
            }
        };
        switch(fcn.m_receiver)
        {
        case HIR::Function::Receiver::Free:
            break;
        case HIR::Function::Receiver::Custom:
            items[name].insert(sp, fcn.m_receiver_type, impl);
            break;
        case HIR::Function::Receiver::Box:
            // TODO: 1.54+ has an allocator param here.
            items[name].m_path[lang_Box].m_byvalue.insert(sp, impl);
            break;
        case HIR::Function::Receiver::Value:
            items[name].m_byvalue.insert(sp, impl);
            break;
        case HIR::Function::Receiver::BorrowOwned:
            H::g(items[name].m_ref_move).m_byvalue.insert(sp, impl);
            break;
        case HIR::Function::Receiver::BorrowUnique:
            H::g(items[name].m_ref_mut).m_byvalue.insert(sp, impl);
            break;
        case HIR::Function::Receiver::BorrowShared:
            H::g(items[name].m_ref).m_byvalue.insert(sp, impl);
            break;
        }
    }
}
void HIR::InherentCache::find(const Span& sp, const RcString& name, const HIR::TypeRef& ty, t_cb_resolve_type ty_res, callback_t cb) const
{
    TRACE_FUNCTION_F(name << ", " << ty);
    auto cb_resolve = [](const HIR::TypeRef& t)->const HIR::TypeRef& { return t; };
    // Callback that ensures that a potential impl fully matches the required receiver type
    inner_callback_t inner_cb = [&](const HIR::TypeRef& rough_self_ty, const HIR::TypeImpl& impl) {
        DEBUG("- " << rough_self_ty);
        const HIR::Function& fcn = impl.m_methods.at(name).data;
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

        if( fcn.m_receiver == HIR::Function::Receiver::Custom ) {
            if( fcn.m_receiver_type.match_test_generics(sp, ty, cb_resolve, getself) ) {
                ASSERT_BUG(sp, getself.detected_self_ty, "Unable to determine receiver type when matching " << fcn.m_receiver_type << " and " << ty);
                cb(*getself.detected_self_ty, impl);
            }
        }
        else {
            // No extra checks required?
            cb(rough_self_ty, impl);
        }
        };
    auto it = items.find(name);
    if(it != items.end()) {
        it->second.find(sp, ty, ty_res, inner_cb);
    }
}
