/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/helpers.hpp
 * - MIR Manipulation helpers
 */
#include "helpers.hpp"

#include <hir/hir.hpp>
#include <hir/type.hpp>
#include <mir/mir.hpp>
#include <algorithm>    // ::std::find

void ::MIR::TypeResolve::fmt_pos(::std::ostream& os, bool include_path/*=false*/) const
{
    if( include_path ) {
        os << this->m_path << " ";
    }
    os << "BB" << this->bb_idx << "/";
    if( this->stmt_idx == STMT_TERM ) {
        os << "TERM";
    }
    else {
        os << this->stmt_idx;
    }
    os << ": ";
}
void ::MIR::TypeResolve::print_msg(const char* tag, ::std::function<void(::std::ostream& os)> cb) const
{
    auto& os = ::std::cerr;
    os << "MIR " << tag << ": ";
    fmt_pos(os, true);
    cb(os);
    os << ::std::endl;
    abort();
    //throw CheckFailure {};
}

unsigned int ::MIR::TypeResolve::get_cur_stmt_ofs() const
{
    if( this->stmt_idx == STMT_TERM )
        return m_fcn.blocks.at(this->bb_idx).statements.size();
    else
        return this->stmt_idx;
}
const ::MIR::BasicBlock& ::MIR::TypeResolve::get_block(::MIR::BasicBlockId id) const
{
    MIR_ASSERT(*this, id < m_fcn.blocks.size(), "Block ID " << id << " out of range");
    return m_fcn.blocks[id];
}

const ::HIR::TypeRef& ::MIR::TypeResolve::get_static_type(::HIR::TypeRef& tmp, const ::HIR::Path& path) const
{
    TU_MATCHA( (path.m_data), (pe),
    (Generic,
        MIR_ASSERT(*this, pe.m_params.m_types.empty(), "Path params on static");
        const auto& s = m_crate.get_static_by_path(sp, pe.m_path);
        return s.m_type;
        ),
    (UfcsKnown,
        MIR_TODO(*this, "LValue::Static - UfcsKnown - " << path);
        ),
    (UfcsUnknown,
        MIR_BUG(*this, "Encountered UfcsUnknown in LValue::Static - " << path);
        ),
    (UfcsInherent,
        MIR_TODO(*this, "LValue::Static - UfcsInherent - " << path);
        )
    )
    throw "";
}
const ::HIR::TypeRef& ::MIR::TypeResolve::get_lvalue_type(::HIR::TypeRef& tmp, const ::MIR::LValue& val, unsigned wrapper_skip_count/*=0*/) const
{
    const ::HIR::TypeRef* rv = nullptr;
    TU_MATCHA( (val.m_root), (e),
    (Return,
        rv = m_monomorphed_rettype ? m_monomorphed_rettype : &m_ret_type;
        ),
    (Argument,
        MIR_ASSERT(*this, e < m_args.size(), "Argument " << val << " out of range (" << m_args.size() << ")");
        rv = &m_args.at(e).second;
        ),
    (Local,
        MIR_ASSERT(*this, e < m_fcn.locals.size(), "Local " << val << " out of range (" << m_fcn.locals.size() << ")");
        rv = m_monomorphed_locals ? &m_monomorphed_locals->at(e) : &m_fcn.locals.at(e);
        ),
    (Static,
        rv = &get_static_type(tmp,  e);
        )
    )
    if(val.m_wrappers.size() > 0)
    {
        assert(wrapper_skip_count <= val.m_wrappers.size());
        const auto* stop_wrapper = &val.m_wrappers[ val.m_wrappers.size() - wrapper_skip_count ];
        for(const auto& w : val.m_wrappers)
        {
            if( &w == stop_wrapper )
                break;
            rv = &this->get_unwrapped_type(tmp, w, *rv);
        }
    }
    else
    {
        assert(wrapper_skip_count == 0);
    }
    return *rv;
}
const ::HIR::TypeRef& ::MIR::TypeResolve::get_unwrapped_type(::HIR::TypeRef& tmp, const ::MIR::LValue::Wrapper& w, const ::HIR::TypeRef& ty) const
{
    TU_MATCH_HDRA( (w), {)
    TU_ARMA(Field, field_index) {
        TU_MATCH_HDRA( (ty.data()), {)
        default:
            MIR_BUG(*this, "Field access on unexpected type - " << ty);
        // Array and Slice use LValue::Field when the index is constant and known-good
        TU_ARMA(Array, te) {
            return te.inner;
            }
        TU_ARMA(Slice, te) {
            return te.inner;
            }
        TU_ARMA(Tuple, te) {
            MIR_ASSERT(*this, field_index < te.size(), "Field index out of range in tuple " << field_index << " >= " << te.size());
            return te[field_index];
            }
        TU_ARMA(Path, te) {
            // TODO: Cache result (to avoid needing to re-monomorph)
            if( const auto* tep = te.binding.opt_Struct() )
            {
                const auto& str = **tep;
                auto maybe_monomorph = [&](const auto& ty)->const auto& {
                    return m_resolve.monomorph_expand_opt(sp, tmp, ty, MonomorphStatePtr(nullptr, &te.path.m_data.as_Generic().m_params, nullptr));
                    };
                TU_MATCHA( (str.m_data), (se),
                (Unit,
                    MIR_BUG(*this, "Field on unit-like struct - " << ty);
                    ),
                (Tuple,
                    MIR_ASSERT(*this, field_index < se.size(), "Field index out of range in tuple-struct " << te.path);
                    return maybe_monomorph(se[field_index].ent);
                    ),
                (Named,
                    MIR_ASSERT(*this, field_index < se.size(), "Field index out of range in struct " << te.path);
                    return maybe_monomorph(se[field_index].second.ent);
                    )
                )
            }
            else if( const auto* tep = te.binding.opt_Union() )
            {
                const auto& unm = **tep;
                auto maybe_monomorph = [&](const ::HIR::TypeRef& t)->const ::HIR::TypeRef& {
                    return m_resolve.monomorph_expand_opt(sp, tmp, t, MonomorphStatePtr(nullptr, &te.path.m_data.as_Generic().m_params, nullptr));
                    };
                MIR_ASSERT(*this, field_index < unm.m_variants.size(), "Field index out of range for union");
                return maybe_monomorph(unm.m_variants.at(field_index).second.ent);
            }
            else
            {
                MIR_BUG(*this, "Field access on invalid type - " << ty);
            }
            }
        }
        }
    TU_ARMA(Deref, _e) {
        TU_MATCH_HDRA( (ty.data()), {)
        default:
            MIR_BUG(*this, "Deref on unexpected type - " << ty);
        TU_ARMA(Path, te) {
            if( const auto* inner_ptr = this->is_type_owned_box(ty) )
            {
                return *inner_ptr;
            }
            else {
                MIR_BUG(*this, "Deref on unexpected type - " << ty);
            }
            }
        TU_ARMA(Pointer, te) {
            return te.inner;
            }
        TU_ARMA(Borrow, te) {
            return te.inner;
            }
        }
        }
    TU_ARMA(Index, index_local) {
        TU_MATCH_HDRA( (ty.data()), { )
        default:
            MIR_BUG(*this, "Index on unexpected type - " << ty);
        TU_ARMA(Slice, te) {
            return te.inner;
            }
        TU_ARMA(Array, te) {
            return te.inner;
            }
        }
        }
    TU_ARMA(Downcast, variant_index) {
        TU_MATCH_HDRA( (ty.data()), {)
        default:
            MIR_BUG(*this, "Downcast on unexpected type - " << ty);
        TU_ARMA(Path, te) {
            MIR_ASSERT(*this, te.binding.is_Enum() || te.binding.is_Union(), "Downcast on non-Enum");
            if( te.binding.is_Enum() )
            {
                const auto& enm = *te.binding.as_Enum();
                MIR_ASSERT(*this, enm.m_data.is_Data(), "Downcast on non-data enum - " << ty);
                const auto& variants = enm.m_data.as_Data();
                MIR_ASSERT(*this, variant_index < variants.size(), "Variant index out of range for " << ty);
                const auto& variant = variants[variant_index];

                const auto& var_ty = variant.type;
                return m_resolve.monomorph_expand_opt(sp, tmp, var_ty, MonomorphStatePtr(nullptr, &te.path.m_data.as_Generic().m_params, nullptr));
            }
            else
            {
                const auto& unm = *te.binding.as_Union();
                MIR_ASSERT(*this, variant_index < unm.m_variants.size(), "Variant index out of range");
                const auto& variant = unm.m_variants[variant_index];
                const auto& var_ty = variant.second.ent;

                return m_resolve.monomorph_expand_opt(sp, tmp, var_ty, MonomorphStatePtr(nullptr, &te.path.m_data.as_Generic().m_params, nullptr));
            }
            }
        }
        }
    }
    throw "";
}
const ::HIR::TypeRef& MIR::TypeResolve::get_param_type(::HIR::TypeRef& tmp, const ::MIR::Param& val) const
{
    if (const auto* p = val.opt_LValue()) {
        return get_lvalue_type(tmp, *p);
    }
    else {
        return tmp = get_const_type(val.as_Constant());
    }
}

::HIR::TypeRef MIR::TypeResolve::get_const_type(const ::MIR::Constant& c) const
{
    TU_MATCH_HDRA( (c), {)
    TU_ARMA(Int, e) {
        return e.t;
        }
    TU_ARMA(Uint, e) {
        return e.t;
        }
    TU_ARMA(Float, e) {
        return e.t;
        }
    TU_ARMA(Bool, e) {
        return ::HIR::CoreType::Bool;
        }
    TU_ARMA(Bytes, e) {
        return ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, ::HIR::TypeRef::new_array( ::HIR::CoreType::U8, e.size() ) );
        }
    TU_ARMA(StaticString, e) {
        return ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, ::HIR::CoreType::Str );
        }
    TU_ARMA(Const, e) {
        MonomorphState  p;
        auto v = m_resolve.get_value(this->sp, *e.p, p, /*signature_only=*/true);
        if( const auto* ve = v.opt_Constant() ) {
            const auto& ty = (*ve)->m_type;
            if( monomorphise_type_needed(ty) ) {
                auto rv = p.monomorph_type(this->sp, ty);
                m_resolve.expand_associated_types(this->sp, rv);
                return rv;
            }
            else
                return ty.clone();
        }
        else {
            MIR_BUG(*this, "get_const_type - Not a constant " << *e.p);
        }
        }
    TU_ARMA(Generic, e) {
        return m_resolve.get_const_param_type(this->sp, e.binding).clone();
        }
    TU_ARMA(ItemAddr, e) {
        MonomorphState  p;
        auto v = m_resolve.get_value(this->sp, *e, p, /*signature_only=*/true);
        TU_MATCH_HDRA( (v), {)
        TU_ARMA(NotFound, ve) {
            MIR_BUG(*this, "get_const_type - ItemAddr points to unknown value - " << c);
            }
        TU_ARMA(NotYetKnown, ve) {
            MIR_BUG(*this, "get_const_type - get_value returned NotYetKnown with signature_only=true");
            }
        TU_ARMA(Constant, ve) {
            const auto& ty = ve->m_type;
            HIR::TypeRef    rv;
            if( monomorphise_type_needed(ty) ) {
                rv = p.monomorph_type(this->sp, ty);
                m_resolve.expand_associated_types(this->sp, rv);
            }
            else {
                rv = ty.clone();
            }
            return HIR::TypeRef::new_borrow(HIR::BorrowType::Shared, mv$(rv));
            }
        TU_ARMA(Static, ve) {
            const auto& ty = ve->m_type;
            HIR::TypeRef    rv;
            if( monomorphise_type_needed(ty) ) {
                rv = p.monomorph_type(this->sp, ty);
                m_resolve.expand_associated_types(this->sp, rv);
            }
            else {
                rv = ty.clone();
            }
            return HIR::TypeRef::new_borrow(HIR::BorrowType::Shared, mv$(rv));
            }
        TU_ARMA(Function, ve) {
            ::HIR::FunctionType ft;
            ft.is_unsafe = ve->m_unsafe;
            ft.m_abi = ve->m_abi;
            ft.m_rettype = p.monomorph_type(this->sp, ve->m_return);
            ft.m_arg_types.reserve(ve->m_args.size());
            for(const auto& arg : ve->m_args)
                ft.m_arg_types.push_back( p.monomorph_type(this->sp, arg.second) );
            auto rv = ::HIR::TypeRef( mv$(ft) );
            m_resolve.expand_associated_types(this->sp, rv);
            return rv;
            }
        TU_ARMA(EnumValue, ve) {
            MIR_BUG(*this, "get_const_type - ItemAddr points to an enum value - " << c);
            }
        TU_ARMA(EnumConstructor, ve) {
            const auto& data_variant = ve.e->m_data.as_Data()[ve.v];
            MIR_ASSERT(*this, data_variant.type.data().is_Path(), c << " enum variant type must be Path - " << data_variant.type);
            const auto& dvt_path = data_variant.type.data().as_Path();
            MIR_ASSERT(*this, dvt_path.binding.is_Struct(), c << " enum variant type path binding must be Struct - " << data_variant.type);
            const auto& str = *dvt_path.binding.as_Struct();
            MIR_ASSERT(*this, str.m_data.is_Tuple(), c << " must point to a tuple-like variant");
            const auto& str_data = str.m_data.as_Tuple();

            ::HIR::FunctionType ft;
            ft.is_unsafe = false;
            ft.m_abi = ABI_RUST;
            auto enum_path = e->clone();
            enum_path.m_data.as_Generic().m_path.m_components.pop_back();
            ft.m_rettype = ::HIR::TypeRef::new_path(mv$(enum_path), ve.e);
            ft.m_arg_types.reserve(str_data.size());
            for(const auto& fld : str_data)
                ft.m_arg_types.push_back( p.monomorph_type(this->sp, fld.ent) );

            auto rv = ::HIR::TypeRef( mv$(ft) );
            m_resolve.expand_associated_types(this->sp, rv);
            return rv;
            }
        TU_ARMA(StructConstant, ve) {
            MIR_BUG(*this, c << " pointing to a struct constant");
            }
        TU_ARMA(StructConstructor, ve) {
            // TODO: Move this to a method on the struct?
            const auto& str = *ve.s;
            MIR_ASSERT(*this, str.m_data.is_Tuple(), c << " must point to a tuple-like struct");
            const auto& str_data = str.m_data.as_Tuple();

            ::HIR::FunctionType ft;
            ft.is_unsafe = false;
            ft.m_abi = ABI_RUST;
            ft.m_rettype = ::HIR::TypeRef::new_path( ::HIR::GenericPath(*ve.p, e->m_data.as_Generic().m_params.clone()), &str);
            ft.m_arg_types.reserve(str_data.size());
            for(const auto& fld : str_data)
                ft.m_arg_types.push_back( p.monomorph_type(this->sp, fld.ent) );

            auto rv = ::HIR::TypeRef( mv$(ft) );
            m_resolve.expand_associated_types(this->sp, rv);
            return rv;
            }
        }
        }
    }
    throw "";
}
bool ::MIR::TypeResolve::lvalue_is_copy(const ::MIR::LValue& val) const
{
    ::HIR::TypeRef  tmp;
    return m_resolve.type_is_copy( this->sp, get_lvalue_type(tmp, val) );
}
const ::HIR::TypeRef* ::MIR::TypeResolve::is_type_owned_box(const ::HIR::TypeRef& ty) const
{
    return m_resolve.is_type_owned_box(ty);
}

using namespace MIR::visit;

namespace MIR {

namespace visit {
    struct LValueCbVisitor:
        public Visitor
    {
        ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb;

        LValueCbVisitor(::std::function<bool(const ::MIR::LValue& , ValUsage)> cb):
            cb(std::move(cb))
        {
        }

        bool visit_lvalue(const ::MIR::LValue& lv, ValUsage u) override {
            if(cb(lv, u))
                return true;
            return Visitor::visit_lvalue(lv, u);
        }
    };

    bool visit_mir_lvalue(const ::MIR::LValue& lv, ValUsage u, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        LValueCbVisitor v { mv$(cb) };
        return v.visit_lvalue(lv, u);
    }

    bool visit_mir_lvalue(const ::MIR::Param& p, ValUsage u, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        LValueCbVisitor v { mv$(cb) };
        return v.visit_param(p, u);
    }

    bool visit_mir_lvalues(const ::MIR::RValue& rval, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        LValueCbVisitor v { mv$(cb) };
        return v.visit_rvalue(rval);
    }

    bool visit_mir_lvalues(const ::MIR::Statement& stmt, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        LValueCbVisitor v { mv$(cb) };
        return v.visit_stmt(stmt);
    }

    bool visit_mir_lvalues(const ::MIR::Terminator& term, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        LValueCbVisitor v { mv$(cb) };
        return v.visit_terminator(term);
    }
    /*
    void visit_mir_lvalues_mut(::MIR::TypeResolve& state, ::MIR::Function& fcn, ::std::function<bool(::MIR::LValue& , ValUsage)> cb)
    {
        for(unsigned int block_idx = 0; block_idx < fcn.blocks.size(); block_idx ++)
        {
            auto& block = fcn.blocks[block_idx];
            for(auto& stmt : block.statements)
            {
                state.set_cur_stmt(block_idx, (&stmt - &block.statements.front()));
                visit_mir_lvalues_mut(stmt, cb);
            }
            if( block.terminator.tag() == ::MIR::Terminator::TAGDEAD )
                continue ;
            state.set_cur_stmt_term(block_idx);
            visit_mir_lvalues_mut(block.terminator, cb);
        }
    }
    void visit_mir_lvalues(::MIR::TypeResolve& state, const ::MIR::Function& fcn, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        visit_mir_lvalues_mut(state, const_cast<::MIR::Function&>(fcn), [&](auto& lv, auto im){ return cb(lv, im); });
    }
    */

    void visit_terminator_target_mut(::MIR::Terminator& term, ::std::function<void(::MIR::BasicBlockId&)> cb) {
        struct TermCbVisitorMut:
            public VisitorMut
        {
            ::std::function<void(::MIR::BasicBlockId&)> cb;

            bool visit_block_id(::MIR::BasicBlockId& x) override {
                cb(x);
                return false;
            }
        } v;
        v.cb = std::move(cb);
        v.visit_terminator(term);
    }
    void visit_terminator_target(const ::MIR::Terminator& term, ::std::function<void(const ::MIR::BasicBlockId&)> cb) {
        visit_terminator_target_mut(const_cast<::MIR::Terminator&>(term), cb);
    }
}   // namespace visit
}   // namespace MIR
// --------------------------------------------------------------------
// MIR_Helper_GetLifetimes
// --------------------------------------------------------------------
namespace
{
    struct ValueLifetime
    {
        /// Bitmap of locations where the variable is valid
        ::std::vector<bool> stmt_bitmap;

        ValueLifetime(size_t stmt_count):
            stmt_bitmap(stmt_count)
        {}

        void fill(const ::std::vector<size_t>& block_offsets, size_t bb, size_t first_stmt, size_t last_stmt)
        {
            size_t  limit = block_offsets[bb+1] - block_offsets[bb] - 1;
            DEBUG("bb" << bb << " : " << first_stmt << "--" << last_stmt);
            assert(first_stmt <= limit);
            assert(last_stmt <= limit);
            for(size_t stmt = first_stmt; stmt <= last_stmt; stmt++)
            {
                stmt_bitmap[block_offsets[bb] + stmt] = true;
            }
        }

        void dump_debug(const char* suffix, unsigned i, const ::std::vector<size_t>& block_offsets)
        {
            ::std::string   name = FMT(suffix << "$" << i);
            while(name.size() < 3+1+3)
                name += " ";
            DEBUG(name << " : " << FMT_CB(os,
                for(unsigned int j = 0; j < this->stmt_bitmap.size(); j++)
                {
                    if(j != 0 && ::std::find(block_offsets.begin(), block_offsets.end(), j) != block_offsets.end())
                        os << "|";
                    os << (this->stmt_bitmap[j] ? "X" : " ");
                }
                ));
        }
    };
}

#if 1   // Alternate algorithm
void MIR_Helper_GetLifetimes_DetermineValueLifetime(
        ::MIR::TypeResolve& state, const ::MIR::Function& fcn,
        size_t bb_idx, size_t stmt_idx,  const ::MIR::LValue& lv,
        const ::std::vector<size_t>& block_offsets, const ::std::vector<bool>& use_bitmap,
        ValueLifetime& vl
        );

// ----------
// TODO: Improved algorithm
// 
// 1. Locate loops (such that a block can be checked for if it's part of a loop, relative to another block)
//  - This can also be used to determine if one bb is before another
// 2. Locate assignment operations (and inline assembly outputs) of locals
// 3. Run forwards until:
// - a jump to a visited block (inner loop)
// - a jump before the first known usage
// - a jump after the last known usage
// - an asignment of the value
// - a use-by-move

::MIR::ValueLifetimes MIR_Helper_GetLifetimes(::MIR::TypeResolve& state, const ::MIR::Function& fcn, bool dump_debug, const ::std::vector<bool>* mask/*=nullptr*/)
{
    TRACE_FUNCTION_F(state);

    size_t  statement_count = 0;
    ::std::vector<size_t>   block_offsets;
    block_offsets.reserve( fcn.blocks.size() );
    for(const auto& bb : fcn.blocks)
    {
        block_offsets.push_back(statement_count);
        statement_count += bb.statements.size() + 1;    // +1 for the terminator
    }
    block_offsets.push_back(statement_count);   // Store the final limit for later code to use.

    ::std::vector<ValueLifetime>    slot_lifetimes( fcn.locals.size(), ValueLifetime(statement_count) );

    // - Enumerate all read positions for each slot
    std::vector< std::vector<bool> > slot_read_bitmaps( fcn.locals.size() );
    {
        for(auto& b : slot_read_bitmaps)
            b.resize( statement_count );
        size_t  pos = 0;
        auto use_cb = [&](const ::MIR::LValue& tlv, ValUsage vu) {
            if( tlv.m_root.is_Local() )
            {
                if(vu != ValUsage::Write)
                    slot_read_bitmaps[tlv.m_root.as_Local()][pos] = true;
            }
            for(const auto& w : tlv.m_wrappers)
                if(w.is_Index())
                    slot_read_bitmaps[w.as_Index()][pos] = true;
            return false;
            };
        for(const auto& bb : fcn.blocks)
        {
            for(const auto& stmt : bb.statements)
            {
                visit_mir_lvalues(stmt, use_cb);
                pos ++;
            }
            visit_mir_lvalues(bb.terminator, use_cb);
            pos ++;
        }
    }

    // Enumerate direct assignments of variables (linear iteration of BB list)
    for(size_t bb_idx = 0; bb_idx < fcn.blocks.size(); bb_idx ++)
    {
        auto assigned_lvalue = [&](size_t bb_idx, size_t stmt_idx, const ::MIR::LValue& lv) {
                // NOTE: Fills the first statement after running, just to ensure that any assigned value has _a_ lifetime
                if( lv.is_Local() )
                {
                    auto de = lv.m_root.as_Local();
                    if( !mask || mask->at(de) )
                    {
                        MIR_Helper_GetLifetimes_DetermineValueLifetime(state, fcn, bb_idx, stmt_idx,  lv, block_offsets, slot_read_bitmaps[de], slot_lifetimes[de]);
                        slot_lifetimes[de].fill(block_offsets, bb_idx, stmt_idx, stmt_idx);
                    }
                }
            };

        const auto& bb = fcn.blocks[bb_idx];
        for(size_t stmt_idx = 0; stmt_idx < bb.statements.size(); stmt_idx ++)
        {
            state.set_cur_stmt(bb_idx, stmt_idx);
            const auto& stmt = bb.statements[stmt_idx];
            if( const auto* se = stmt.opt_Assign() )
            {
                // For assigned variables, determine how long that value will live
                assigned_lvalue(bb_idx, stmt_idx+1, se->dst);
            }
            else if( const auto* se = stmt.opt_Asm() )
            {
                for(const auto& e : se->outputs)
                {
                    assigned_lvalue(bb_idx, stmt_idx+1, e.second);
                }
            }
            else if( const auto* se = stmt.opt_Drop() )
            {
                // HACK: Mark values as valid wherever there's a drop (prevents confusion by simple validator)
                if( se->slot.m_wrappers.empty() && se->slot.m_root.is_Local() )
                {
                    auto de = se->slot.m_root.as_Local();
                    if( !mask || mask->at(de) )
                    {
                        slot_lifetimes[de].fill(block_offsets, bb_idx, stmt_idx,stmt_idx);
                    }
                }
            }
        }
        state.set_cur_stmt_term(bb_idx);

        // Only Call can assign a value
        TU_IFLET(::MIR::Terminator, bb.terminator, Call, te,
            assigned_lvalue(te.ret_block, 0, te.ret_val);
        )
    }

    // Dump out variable lifetimes.
    if( dump_debug )
    {
        for(size_t i = 0; i < slot_lifetimes.size(); i ++)
        {
            slot_lifetimes[i].dump_debug("_", i, block_offsets);
        }
    }


    ::MIR::ValueLifetimes   rv;
    rv.m_block_offsets = mv$(block_offsets);
    rv.m_slots.reserve( slot_lifetimes.size() );
    for(auto& lft : slot_lifetimes)
        rv.m_slots.push_back( ::MIR::ValueLifetime(mv$(lft.stmt_bitmap)) );
    return rv;
}
void MIR_Helper_GetLifetimes_DetermineValueLifetime(
        ::MIR::TypeResolve& mir_res, const ::MIR::Function& fcn,
        size_t bb_idx, size_t stmt_idx, // First statement in which the value is valid (after the assignment)
        const ::MIR::LValue& lv, const ::std::vector<size_t>& block_offsets, const ::std::vector<bool>& use_bitmap,
        ValueLifetime& vl
        )
{
    TRACE_FUNCTION_F(mir_res << lv << " assigned");
    // Walk the BB tree until:
    // - Loopback
    // - Assignment
    // - Drop

    struct State
    {
        const ::std::vector<size_t>&  m_block_offsets;
        ValueLifetime&  m_out_vl;

        ::std::vector<unsigned int> bb_history;
        size_t  last_read_ofs;  // Statement index
        bool    m_is_borrowed;

        State(const ::std::vector<size_t>& block_offsets, ValueLifetime& vl, size_t init_bb_idx, size_t init_stmt_idx):
            m_block_offsets(block_offsets),
            m_out_vl(vl),
            bb_history(),
            last_read_ofs(init_stmt_idx),
            m_is_borrowed(false)
        {
            bb_history.push_back(init_bb_idx);
        }
        State(State&& x):
            m_block_offsets(x.m_block_offsets),
            m_out_vl(x.m_out_vl),
            bb_history( mv$(x.bb_history) ),
            last_read_ofs( x.last_read_ofs ),
            m_is_borrowed( x.m_is_borrowed )
        {
        }
        State& operator=(State&& x) {
            this->bb_history = mv$(x.bb_history);
            this->last_read_ofs = x.last_read_ofs;
            this->m_is_borrowed = x.m_is_borrowed;
            return *this;
        }

        State clone() const {
            State   rv { m_block_offsets, m_out_vl, 0, last_read_ofs };
            rv.bb_history = bb_history;
            rv.m_is_borrowed = m_is_borrowed;
            return rv;
        }

        // Returns true if the variable has been borrowed
        bool is_borrowed() const {
            return this->m_is_borrowed;
        }

        void mark_borrowed(size_t stmt_idx) {
            if( ! m_is_borrowed )
            {
                m_is_borrowed = false;
                this->fill_to(stmt_idx);
            }
            m_is_borrowed = true;
        }
        void mark_read(size_t stmt_idx) {
            if( !m_is_borrowed )
            {
                this->fill_to(stmt_idx);
            }
            else
            {
                m_is_borrowed = false;
                this->fill_to(stmt_idx);
                m_is_borrowed = true;
            }
        }
        void fmt(::std::ostream& os) const {
            os << "BB" << bb_history.front() << "/" << last_read_ofs << "--";
            os << "[" << bb_history << "]";
        }
        void finalise(size_t stmt_idx)
        {
            if( m_is_borrowed )
            {
                m_is_borrowed = false;
                this->fill_to(stmt_idx);
                m_is_borrowed = true;
            }
        }
    private:
        void fill_to(size_t stmt_idx)
        {
            TRACE_FUNCTION_F(FMT_CB(ss, this->fmt(ss);));
            assert( !m_is_borrowed );
            assert(bb_history.size() > 0);
            if( bb_history.size() == 1 )
            {
                // only one block
                m_out_vl.fill(m_block_offsets, bb_history[0],  last_read_ofs, stmt_idx);
            }
            else
            {
                // First block.
                auto init_bb_idx = bb_history[0];
                auto limit_0 = m_block_offsets[init_bb_idx+1] - m_block_offsets[init_bb_idx] - 1;
                m_out_vl.fill(m_block_offsets, init_bb_idx,  last_read_ofs, limit_0);

                // Middle blocks
                for(size_t i = 1; i < bb_history.size()-1; i++)
                {
                    size_t bb_idx = bb_history[i];
                    assert(bb_idx+1 < m_block_offsets.size());
                    size_t limit = m_block_offsets[bb_idx+1] - m_block_offsets[bb_idx] - 1;
                    m_out_vl.fill(m_block_offsets, bb_idx, 0, limit);
                }

                // Last block
                auto bb_idx = bb_history.back();
                m_out_vl.fill(m_block_offsets, bb_idx,  0, stmt_idx);
            }

            last_read_ofs = stmt_idx;

            auto cur = this->bb_history.back();
            this->bb_history.clear();
            this->bb_history.push_back(cur);
        }
    };

    struct Runner
    {
        ::MIR::TypeResolve& m_mir_res;
        const ::MIR::Function& m_fcn;
        size_t m_init_bb_idx;
        size_t m_init_stmt_idx;
        const ::MIR::LValue& m_lv;
        const ::std::vector<size_t>& m_block_offsets;
        ValueLifetime& m_lifetimes;
        bool m_is_copy;

        ::std::vector<bool> m_visited_statements;

        ::std::vector<::std::pair<size_t, State>> m_states_to_do;

        Runner(::MIR::TypeResolve& mir_res, const ::MIR::Function& fcn, size_t init_bb_idx, size_t init_stmt_idx, const ::MIR::LValue& lv, const ::std::vector<size_t>& block_offsets, ValueLifetime& vl):
            m_mir_res(mir_res),
            m_fcn(fcn),
            m_init_bb_idx(init_bb_idx),
            m_init_stmt_idx(init_stmt_idx),
            m_lv(lv),
            m_block_offsets(block_offsets),
            m_lifetimes(vl),

            m_visited_statements( m_lifetimes.stmt_bitmap.size() )
        {
            ::HIR::TypeRef  tmp;
            m_is_copy = m_mir_res.m_resolve.type_is_copy(mir_res.sp, m_mir_res.get_lvalue_type(tmp, lv));
        }

        void run_block(size_t bb_idx, size_t stmt_idx, State state)
        {
            const auto& bb = m_fcn.blocks.at(bb_idx);
            assert(stmt_idx <= bb.statements.size());

            bool was_moved = false;
            bool was_updated = false;
            auto visit_cb = [&](const auto& lv, auto vu) {
                    if(lv.m_root == m_lv.m_root) {
                        switch(vu)
                        {
                        case ValUsage::Read:
                            DEBUG(m_mir_res << "Used");
                            state.mark_read(stmt_idx);
                            was_updated = true;
                            break;
                        case ValUsage::Move:
                            if( lv.m_wrappers.size() == m_lv.m_wrappers.size() )
                            {
                                DEBUG(m_mir_res << (m_is_copy ? "Read" : "Moved"));
                                state.mark_read(stmt_idx);
                                was_moved = ! m_is_copy;
                            }
                            else
                            {
                                DEBUG(m_mir_res << "Used (partial)");
                                state.mark_read(stmt_idx);
                                was_updated = true;
                            }
                            break;
                        case ValUsage::Borrow:
                            DEBUG(m_mir_res << "Borrowed");
                            state.mark_borrowed(stmt_idx);
                            was_updated = true;
                            break;
                        case ValUsage::Write:
                            // Don't care
                            break;
                        }
                    }
                    for(const auto& w : lv.m_wrappers)
                    {
                        if( w.is_Index() && m_lv.is_Local() && w.as_Index() == m_lv.as_Local() ) {
                            DEBUG(m_mir_res << "Index used");
                            state.mark_read(stmt_idx);
                            was_updated = true;
                        }
                    }
                    return false;
                    };

            for( ; stmt_idx < bb.statements.size(); stmt_idx ++)
            {
                const auto& stmt = bb.statements[stmt_idx];
                m_mir_res.set_cur_stmt(bb_idx, stmt_idx);
                m_visited_statements[ m_block_offsets.at(bb_idx) + stmt_idx ] = true;

                // Visit and see if the value is read (setting the read flag or end depending on if the value is Copy)
                was_updated = false;
                visit_mir_lvalues(stmt, visit_cb);
                if(was_updated || was_moved)
                {
                    DEBUG(m_mir_res << stmt);
                }

                if( was_moved )
                {
                    // Moved: Update read position and apply
                    DEBUG(m_mir_res << "Moved, return");
                    state.mark_read(stmt_idx);
                    state.finalise(stmt_idx);
                    return ;
                }

                TU_MATCH_HDRA( (stmt), {)
                TU_ARMA(Assign, se) {
                    if( se.dst == m_lv )
                    {
                        DEBUG(m_mir_res << "- Assigned to, return");
                        // Value assigned, just apply
                        state.finalise(stmt_idx);
                        return ;
                    }
                    }
                TU_ARMA(Drop, se) {
                    visit_mir_lvalue(se.slot, ValUsage::Read, visit_cb);
                    if( se.slot == m_lv )
                    {
                        // Value dropped, update read position and apply
                        DEBUG(m_mir_res << "- Dropped, return");
                        // - If it was borrowed, it can't still be borrowed here.
                        // TODO: Enable this once it's known to not cause mis-optimisation. It could currently.
                        //if( state.is_borrowed() ) {
                        //    state.clear_borrowed();
                        //}
                        state.mark_read(stmt_idx);
                        state.finalise(stmt_idx);
                        return ;
                    }
                    }
                TU_ARMA(Asm, se) {
                    // 
                    for(const auto& e : se.outputs)
                    {
                        if(e.second == m_lv) {
                            // Assigned, just apply
                            DEBUG(m_mir_res << "- Assigned (asm!), return");
                            state.finalise(stmt_idx);
                            return ;
                        }
                    }
                    }
                TU_ARMA(Asm2, se) {
                    for(const auto& p : se.params)
                    {
                        TU_MATCH_HDRA( (p), {)
                        TU_ARMA(Const, v) {}
                        TU_ARMA(Sym, v) {}
                        TU_ARMA(Reg, v) {
                            if(v.output) {
                                if(*v.output == m_lv) {
                                    // Assigned, just apply
                                    DEBUG(m_mir_res << "- Assigned (asm!), return");
                                    state.finalise(stmt_idx);
                                    return ;
                                }
                            }
                            }
                        }
                    }
                    }
                TU_ARMA(SetDropFlag, se) {
                    // Ignore
                    }
                TU_ARMA(ScopeEnd, se) {
                    // Ignore
                    }
                }
            }
            m_mir_res.set_cur_stmt_term(bb_idx);
            m_visited_statements[ m_block_offsets.at(bb_idx) + stmt_idx ] = true;

            was_updated = false;
            visit_mir_lvalues(bb.terminator, visit_cb);
            DEBUG(m_mir_res << bb.terminator << (was_updated ? " (used)" : ""));

            if( was_moved )
            {
                // Moved: Update read position and apply
                DEBUG(m_mir_res << "- Moved, return");
                state.mark_read(stmt_idx);
                state.finalise(stmt_idx);
                return ;
            }

            // Terminator
            TU_MATCH_HDRA( (bb.terminator), {)
            TU_ARMA(Incomplete, te) {
                // TODO: Isn't this a bug?
                DEBUG(m_mir_res << "Incomplete");
                state.finalise(stmt_idx);
                }
            TU_ARMA(Return, te) {
                DEBUG(m_mir_res << "Return");
                state.finalise(stmt_idx);
                }
            TU_ARMA(Diverge, te) {
                DEBUG(m_mir_res << "Diverge");
                state.finalise(stmt_idx);
                }
            TU_ARMA(Goto, te) {
                m_states_to_do.push_back( ::std::make_pair(te, mv$(state)) );
                }
            TU_ARMA(Panic, te) {
                m_states_to_do.push_back( ::std::make_pair(te.dst, mv$(state)) );
                }
            TU_ARMA(If, te) {
                m_states_to_do.push_back( ::std::make_pair(te.bb0, state.clone()) );
                m_states_to_do.push_back( ::std::make_pair(te.bb1, mv$(state)) );
                }
            TU_ARMA(Switch, te) {
                for(size_t i = 0; i < te.targets.size(); i ++)
                {
                    auto s = (i == te.targets.size()-1)
                        ? mv$(state)
                        : state.clone();
                    m_states_to_do.push_back( ::std::make_pair(te.targets[i], mv$(s)) );
                }
                }
            TU_ARMA(SwitchValue, te) {
                for(size_t i = 0; i < te.targets.size(); i ++)
                {
                    m_states_to_do.push_back( ::std::make_pair(te.targets[i], state.clone()) );
                }
                m_states_to_do.push_back( ::std::make_pair(te.def_target, mv$(state)) );
                }
            TU_ARMA(Call, te) {
                if( te.ret_val == m_lv )
                {
                    DEBUG(m_mir_res << "Assigned (Call), return");
                    // Value assigned, just apply
                    state.finalise(stmt_idx);
                    return ;
                }
                if( m_fcn.blocks.at(te.panic_block).statements.size() == 0 && m_fcn.blocks.at(te.panic_block).terminator.is_Diverge() ) {
                    // Shortcut: Don't create a new state if the panic target is Diverge
                }
                else {
                    m_states_to_do.push_back( ::std::make_pair(te.panic_block, state.clone()) );
                }
                m_states_to_do.push_back( ::std::make_pair(te.ret_block, mv$(state)) );
                }
            }
        }
    };

    Runner  runner(mir_res, fcn, bb_idx, stmt_idx, lv, block_offsets, vl);
    ::std::vector< ::std::pair<size_t,State>>   post_check_list;

    // TODO: Have a bitmap of visited statements. If a visted statement is hit, stop the current state
    // - Use the same rules as loopback.

    // Fill the first statement, to ensure that there is at least one bit set.
    runner.run_block(bb_idx, stmt_idx, State(block_offsets, vl, bb_idx, stmt_idx));

    while( ! runner.m_states_to_do.empty() )
    {
        auto bb_idx = runner.m_states_to_do.back().first;
        auto state = mv$(runner.m_states_to_do.back().second);
        runner.m_states_to_do.pop_back();

        DEBUG("state.bb_history=[" << state.bb_history << "], -> BB" << bb_idx);
        state.bb_history.push_back(bb_idx);

        if( runner.m_visited_statements.at( block_offsets.at(bb_idx) + 0 ) )
        {
            if( vl.stmt_bitmap.at( block_offsets.at(bb_idx) + 0) )
            {
                DEBUG("Looped (to already valid)");
                state.mark_read(0);
                state.finalise(0);
                continue ;
            }
            else if( state.is_borrowed() )
            {
                DEBUG("Looped (borrowed)");
                state.mark_read(0);
                state.finalise(0);
                continue ;
            }
            else
            {
                // Put this state elsewhere and check if the variable is known valid at that point.
                DEBUG("Looped (after last read), push for later");
                post_check_list.push_back( ::std::make_pair(bb_idx, mv$(state)) );
                continue ;
            }
        }

#if 0
        // TODO: Have a bitmap of if a BB mentions this value. If there are no unvisited BBs that mention this value, stop early.
        // - CATCH: The original BB contains a reference, but might not have been visited (if it was the terminating call that triggered)
        //  - Also, we don't want to give up early (if we loop back to the start of the first block)
        // - A per-statement bitmap would solve this. Return early if `!vl.stmt_bitmap & usage_stmt_bitmap == 0`
        // > Requires filling the bitmap as we go (for maximum efficiency)
        {
            bool found_non_visited = false;
            for(size_t i = 0; i < use_bitmap.size(); i ++)
            {
                // If a place where the value is used is not present in the output bitmap
                if( !vl.stmt_bitmap[i] && use_bitmap[i] )
                {
                    DEBUG("- Still used at +" << i);
                    found_non_visited = true;
                }
            }
            // If there were no uses of the variable that aren't covered by the lifetime bitmap
            if( ! found_non_visited )
            {
                // Terminate early
                DEBUG("Early terminate - All possible lifetimes covered");
                state.finalise(0);
                for(auto& s : runner.m_states_to_do)
                {
                    s.second.bb_history.push_back(bb_idx);
                    s.second.finalise(0);
                }
                return ;
            }
        }
#endif

        // Special case for when doing multiple runs on the same output
        if( vl.stmt_bitmap.at( block_offsets.at(bb_idx) + 0) )
        {
            DEBUG("Already valid in BB" << bb_idx);
            state.mark_read(0);
            state.finalise(0);
            continue;
        }
#if 0
        // TODO: Have a way of knowing if a state will never find a use (the negative of the above)
        // - Requires knowing for sure that a BB doesn't end up using the value.
        // - IDEA: Store a fork count and counts of Yes/No for each BB.
        //  > If ForkCount == No, the value isn't used in that branch.
        if( runner.m_bb_counts[bb_idx].visit_count > 0
                && runner.m_bb_counts[bb_idx].visit_count == runner.m_bb_counts[bb_idx].val_unused_count )
        {
            DEBUG("Target BB known to be not valid");
            runner.apply_state(state, 0);
            continue ;
        }
        runner.m_bb_counts[bb_idx].visit_count ++;
#endif

        runner.run_block(bb_idx, 0, mv$(state));
    }

    // Iterate while there are items in the post_check list
    while( !post_check_list.empty() )
    {
        bool change = false;
        for(auto it = post_check_list.begin(); it != post_check_list.end(); )
        {
            auto bb_idx = it->first;
            auto& state = it->second;
            // If the target of this loopback is valid, then the entire route to the loopback must have been valid
            if( vl.stmt_bitmap.at( block_offsets.at(bb_idx) + 0) )
            {
                change = true;
                DEBUG("Looped (now valid)");
                state.mark_read(0);
                state.finalise(0);

                it = post_check_list.erase(it);
            }
            else
            {
                ++ it;
            }
        }
        // Keep going while changes happen
        if( !change )
            break;
    }
}

#else

::MIR::ValueLifetimes MIR_Helper_GetLifetimes(::MIR::TypeResolve& state, const ::MIR::Function& fcn, bool dump_debug)
{
    TRACE_FUNCTION_F(state);
    // New algorithm notes:
    // ---
    // The lifetime of a value starts when it is written, and ends the last time it is read
    // - When a variable is read, end any existing lifetime and start a new one.
    // - When the value is read, update the end of its lifetime.
    // ---
    // A lifetime is a range in the call graph (with a start and end, including list of blocks)
    // - Representation: Bitmap with a bit per statement.
    // - Record the current block path in general state, along with known active lifetimes

    // TODO: If a value is borrowed, assume it lives forevermore
    // - Ideally there would be borrow tracking to determine its actual required lifetime.
    // - NOTE: This doesn't impact the borrows themselves, just the borrowee

    // TODO: Add a statement type StorageDead (or similar?) that indicates the point where a values scope ends

    // Scan through all possible paths in the graph (with loopback detection using a memory of the path)
    // - If a loop is detected, determine if there were changes to the lifetime set during that pass
    //  > Changes are noticed by recording in the state structure when it triggers a change in the lifetime
    //    map.
    struct Position
    {
        size_t path_index = 0; // index into the block path.
        unsigned int stmt_idx = 0;

        bool operator==(const Position& x) const {
            return path_index == x.path_index && stmt_idx == x.stmt_idx;
        }
    };
    struct ProtoLifetime
    {
        Position    start;
        Position    end;

        bool is_empty() const {
            return start == end;
        }
        bool is_borrowed() const {
            return this->end == Position { ~0u, ~0u };
        }
    };
    static unsigned NEXT_INDEX = 0;
    struct State
    {
        unsigned int index = 0;
        ::std::vector<unsigned int> block_path;
        ::std::vector<unsigned int> block_change_idx;
        unsigned int cur_change_idx = 0;

        // if read, update. If set, save and update
        ::std::vector<ProtoLifetime> tmp_ends;
        ::std::vector<ProtoLifetime> var_ends;

        State(const ::MIR::Function& fcn):
            tmp_ends( fcn.temporaries.size(), ProtoLifetime() ),
            var_ends( fcn.named_variables.size(), ProtoLifetime() )
        {
        }

        State clone() const {
            auto rv = *this;
            rv.index = ++NEXT_INDEX;
            return rv;
        }
    };
    NEXT_INDEX = 0;

    size_t  statement_count = 0;
    ::std::vector<size_t>   block_offsets;
    block_offsets.reserve( fcn.blocks.size() );
    for(const auto& bb : fcn.blocks)
    {
        block_offsets.push_back(statement_count);
        statement_count += bb.statements.size() + 1;    // +1 for the terminator
    }

    ::std::vector<ValueLifetime>    temporary_lifetimes( fcn.temporaries.size(), ValueLifetime(statement_count) );
    ::std::vector<ValueLifetime>    variable_lifetimes( fcn.named_variables.size(), ValueLifetime(statement_count) );

    struct BlockSeenLifetimes {
        bool m_has_state = false;
        const ::std::vector<size_t>& block_offsets;
        ::std::vector< ::std::vector<unsigned int> > tmp;
        ::std::vector< ::std::vector<unsigned int> > var;

        BlockSeenLifetimes(const ::std::vector<size_t>& block_offsets, const ::MIR::Function& fcn):
            block_offsets( block_offsets ),
            tmp( fcn.temporaries.size() ),
            var( fcn.named_variables.size() )
        {}

        bool has_state() const
        {
            return m_has_state;
        }

        bool try_merge(const State& val_state) const
        {
            // TODO: This logic isn't quite correct. Just becase a value's existing end is already marked as valid,
            // doesn't mean that we have no new information.
            // - Wait, doesn't it?
            auto try_merge_lft = [&](const ProtoLifetime& lft, const ::std::vector<unsigned int>& seen)->bool {
                if(lft.is_empty())  return false;
                // TODO: What should be done for borrow flagged values
                if(lft.is_borrowed())   return false;
                auto end_idx = block_offsets.at( val_state.block_path.at(lft.end.path_index) ) + lft.end.stmt_idx;

                auto it = ::std::find(seen.begin(), seen.end(), end_idx);
                return (it == seen.end());
                };
            for(size_t i = 0; i < val_state.tmp_ends.size(); i++)
            {
                if( try_merge_lft(val_state.tmp_ends[i], this->tmp[i]) )
                    return true;
            }
            for(size_t i = 0; i < val_state.var_ends.size(); i++)
            {
                if( try_merge_lft(val_state.var_ends[i], this->var[i]) )
                    return true;
            }
            return false;
        }

        bool merge(const State& val_state)
        {
            bool rv = false;
            auto merge_lft = [&](const ProtoLifetime& lft, ::std::vector<unsigned int>& seen)->bool {
                if(lft.is_empty())  return false;
                // TODO: What should be done for borrow flagged values
                if(lft.end == Position { ~0u, ~0u })    return false;
                auto end_idx = block_offsets.at( val_state.block_path.at(lft.end.path_index) ) + lft.end.stmt_idx;

                auto it = ::std::find(seen.begin(), seen.end(), end_idx);
                if( it == seen.end() )
                {
                    seen.push_back( end_idx );
                    return true;
                }
                else
                {
                    return false;
                }
                };
            for(size_t i = 0; i < val_state.tmp_ends.size(); i++)
            {
                rv |= merge_lft(val_state.tmp_ends[i], this->tmp[i]);
            }
            for(size_t i = 0; i < val_state.var_ends.size(); i++)
            {
                rv |= merge_lft(val_state.var_ends[i], this->var[i]);
            }
            m_has_state = true;
            return rv;
        }
    };
    ::std::vector<BlockSeenLifetimes>   block_seen_lifetimes( fcn.blocks.size(), BlockSeenLifetimes(block_offsets, fcn) );

    State   init_state(fcn);

    ::std::vector<::std::pair<unsigned int, State>> todo_queue;
    todo_queue.push_back(::std::make_pair( 0, mv$(init_state) ));

    while(!todo_queue.empty())
    {
        auto bb_idx = todo_queue.back().first;
        auto val_state = mv$(todo_queue.back().second);
        todo_queue.pop_back();
        state.set_cur_stmt(bb_idx, 0);

        // Fill alive time in the bitmap
        // TODO: Maybe also store the range (as a sequence of {block,start,end})
        auto add_lifetime_s = [&](State& val_state, const ::MIR::LValue& lv, const Position& start, const Position& end) {
            assert(start.path_index <= end.path_index);
            assert(start.path_index < end.path_index || start.stmt_idx <= end.stmt_idx);
            if(start.path_index == end.path_index && start.stmt_idx == end.stmt_idx)
                return;
            DEBUG("[add_lifetime] " << lv << " (" << start.path_index << "," << start.stmt_idx << ") -- (" << end.path_index << "," << end.stmt_idx << ")");
            ValueLifetime* lft;
            if(const auto* e = lv.opt_Temporary())
            {
                lft = &temporary_lifetimes[e->idx];
            }
            else if(const auto* e = lv.opt_Variable())
            {
                lft = &variable_lifetimes[*e];
            }
            else
            {
                MIR_TODO(state, "[add_lifetime] " << lv);
                return;
            }

            // Fill lifetime map for this temporary in the indicated range
            bool did_set = false;
            unsigned int j = start.stmt_idx;
            unsigned int i = start.path_index;
            while( i <= end.path_index && i < val_state.block_path.size() )
            {
                auto bb_idx = val_state.block_path.at(i);
                const auto& bb = fcn.blocks[bb_idx];
                MIR_ASSERT(state, j <= bb.statements.size(), "");
                MIR_ASSERT(state, bb_idx < block_offsets.size(), "");

                auto block_base = block_offsets.at(bb_idx);
                auto idx = block_base + j;
                if( !lft->stmt_bitmap.at(idx) )
                {
                    lft->stmt_bitmap[idx] = true;
                    did_set = true;
                }

                if( i == end.path_index && j == (end.stmt_idx != ~0u ? end.stmt_idx : bb.statements.size()) )
                    break;

                // If the current index is the terminator (one after the size)
                if(j == bb.statements.size())
                {
                    j = 0;
                    i++;
                }
                else
                {
                    j ++;
                }
            }

            // - If the above set a new bit, increment `val_state.cur_change_idx`
            if( did_set )
            {
                DEBUG("[add_lifetime] " << lv << " (" << start.path_index << "," << start.stmt_idx << ") -- (" << end.path_index << "," << end.stmt_idx << ") - New information");
                val_state.cur_change_idx += 1;
            }
            };
        auto add_lifetime = [&](const ::MIR::LValue& lv, const Position& start, const Position& end) {
            add_lifetime_s(val_state, lv, start, end);
            };

        auto apply_state = [&](State& state) {
            // Apply all changes in this state, just in case there was new information
            for(unsigned i = 0; i < fcn.temporaries.size(); i++)
                add_lifetime_s( state, ::MIR::LValue::make_Temporary({i}), state.tmp_ends[i].start, state.tmp_ends[i].end );
            for(unsigned i = 0; i < fcn.named_variables.size(); i++)
                add_lifetime_s( state, ::MIR::LValue::make_Variable({i}), state.var_ends[i].start, state.var_ends[i].end );
            };
        auto add_to_visit = [&](unsigned int new_bb_idx, State new_state) {
            auto& bb_memory_ent = block_seen_lifetimes[new_bb_idx];
            if( !bb_memory_ent.has_state() )
            {
                // No recorded state, needs to be visited
                DEBUG(state << " state" << new_state.index << " -> bb" << new_bb_idx << " (no existing state)");
            }
            else if( bb_memory_ent.try_merge(new_state) )
            {
                // This state has new information, needs to be visited
                DEBUG(state << " state" << new_state.index << " -> bb" << new_bb_idx << " (new info)");
            }
            else
            {
                // Skip
                // TODO: Acquire from the target block the actual end of any active lifetimes, then apply them.
                DEBUG(state << " state" << new_state.index << " -> bb" << new_bb_idx << " - No new state, no push");
                // - For all variables currently active, check if they're valid in the first statement of the target block.
                // - If so, mark as valid at the end of the current block
                auto bm_idx = block_offsets[new_bb_idx];
                Position    cur_pos;
                cur_pos.path_index = val_state.block_path.size() - 1;
                cur_pos.stmt_idx = fcn.blocks[bb_idx].statements.size();
                for(unsigned i = 0; i < fcn.temporaries.size(); i++) {
                    if( ! new_state.tmp_ends[i].is_empty() && temporary_lifetimes[i].stmt_bitmap[bm_idx] ) {
                        DEBUG("- tmp$" << i << " - Active in target, assume active");
                        new_state.tmp_ends[i].end = cur_pos;
                    }
                }
                for(unsigned i = 0; i < fcn.named_variables.size(); i++) {
                    if( ! new_state.var_ends[i].is_empty() && variable_lifetimes[i].stmt_bitmap[bm_idx] ) {
                        DEBUG("- var$" << i << " - Active in target, assume active");
                        new_state.var_ends[i].end = cur_pos;
                    }
                }
                // - Apply whatever state was still active
                apply_state(new_state);
                return ;
            }
            todo_queue.push_back(::std::make_pair( new_bb_idx, mv$(new_state) ));
            };

        // Compare this state to a composite list of lifetimes seen in this block
        // - Just compares the end of each proto lifetime
        {
            auto& bb_memory_ent = block_seen_lifetimes[bb_idx];
            bool had_state = bb_memory_ent.has_state();
            bool has_new = bb_memory_ent.merge(val_state);

            if( !has_new && had_state )
            {
                DEBUG(state << " state" << val_state.index << " - No new entry state");
                apply_state(val_state);

                continue ;
            }
        }

        // Check if this state has visited this block before, and if anything changed since last time
        {
            auto it = ::std::find(val_state.block_path.rbegin(), val_state.block_path.rend(), bb_idx);
            if( it != val_state.block_path.rend() )
            {
                auto idx = &*it - &val_state.block_path.front();
                if( val_state.block_change_idx[idx] == val_state.cur_change_idx )
                {
                    DEBUG(state << " " << val_state.index << " Loop and no change");
                    continue ;
                }
                else
                {
                    assert( val_state.block_change_idx[idx] < val_state.cur_change_idx );
                    DEBUG(state << " " << val_state.index << " --- Loop, " << val_state.cur_change_idx - val_state.block_change_idx[idx] << " changes");
                }
            }
            else
            {
                DEBUG(state << " " << val_state.index << " ---");
            }
            val_state.block_path.push_back(bb_idx);
            val_state.block_change_idx.push_back( val_state.cur_change_idx );
        }

        Position    cur_pos;
        cur_pos.path_index = val_state.block_path.size() - 1;
        cur_pos.stmt_idx = 0;
        auto lvalue_read = [&](const ::MIR::LValue& lv) {
            ProtoLifetime* slot;
            if(const auto* e = lv.opt_Temporary()) {
                slot = &val_state.tmp_ends.at(e->idx);
            }
            else if(const auto* e = lv.opt_Variable()) {
                slot = &val_state.var_ends.at(*e);
            }
            else {
                return ;
            }
            // Update the last read location
            //DEBUG("Update END " << lv << " to " << cur_pos);
            slot->end = cur_pos;
            };
        auto lvalue_set = [&](const ::MIR::LValue& lv) {
            ProtoLifetime* slot;
            if(const auto* e = lv.opt_Temporary()) {
                slot = &val_state.tmp_ends.at(e->idx);
            }
            else if(const auto* e = lv.opt_Variable()) {
                slot = &val_state.var_ends.at(*e);
            }
            else {
                return ;
            }
            // End whatever value was originally there, and insert this new one
            slot->end = cur_pos;
            add_lifetime(lv, slot->start, slot->end);
            slot->start = cur_pos;
            };
        auto lvalue_borrow = [&](const ::MIR::LValue& lv) {
            ProtoLifetime* slot;
            if(const auto* e = lv.opt_Temporary()) {
                slot = &val_state.tmp_ends.at(e->idx);
            }
            else if(const auto* e = lv.opt_Variable()) {
                slot = &val_state.var_ends.at(*e);
            }
            else {
                return ;
            }
            // TODO: Flag this value as currently being borrowed (a flag that never clears)
            slot->end = Position { ~0u, ~0u };
            };
        auto visit_lval_cb = [&](const auto& lv, ValUsage vu)->bool{
                if(vu == ValUsage::Read)
                    lvalue_read(lv);
                if(vu == ValUsage::Borrow)
                    lvalue_borrow(lv);
                if(vu == ValUsage::Write)
                    lvalue_set(lv);
                return false;
                };

        // Run statements
        for(const auto& stmt : fcn.blocks[bb_idx].statements)
        {
            auto stmt_idx = &stmt - &fcn.blocks[bb_idx].statements.front();
            cur_pos.stmt_idx = stmt_idx;
            state.set_cur_stmt(bb_idx, stmt_idx);
            DEBUG(state << " " << stmt);

            if( const auto* e = stmt.opt_Drop() )
            {
                visit_mir_lvalues(stmt, [&](const auto& lv, ValUsage vu)->bool{
                    if(vu == ValUsage::Read)
                        lvalue_read(lv);
                    return false;
                    });
                lvalue_read(e->slot);
                lvalue_set(e->slot);
            }
            else
            {
                visit_mir_lvalues(stmt, visit_lval_cb);
            }
        }
        cur_pos.stmt_idx = fcn.blocks[bb_idx].statements.size();

        state.set_cur_stmt_term(bb_idx);
        DEBUG(state << "TERM " << fcn.blocks[bb_idx].terminator);
        TU_MATCH(::MIR::Terminator, (fcn.blocks[bb_idx].terminator), (e),
        (Incomplete,
            // Should be impossible here.
            ),
        (Return,
            // End all active lifetimes at their previous location.
            apply_state(val_state);
            ),
        (Diverge,
            apply_state(val_state);
            ),
        (Goto,
            add_to_visit(e, mv$(val_state));
            ),
        (Panic,
            // What should be done here?
            ),
        (If,
            visit_mir_lvalue(e.cond, ValUsage::Read, visit_lval_cb);

            // Push blocks
            add_to_visit(e.bb0, val_state.clone());
            add_to_visit(e.bb1, mv$(val_state));
            ),
        (Switch,
            visit_mir_lvalue(e.val, ValUsage::Read, visit_lval_cb);
            ::std::set<unsigned int> tgts;
            for(const auto& tgt : e.targets)
                tgts.insert(tgt);

            for(const auto& tgt : tgts)
            {
                auto vs = (tgt == *tgts.rbegin() ? mv$(val_state) : val_state.clone());
                add_to_visit(tgt, mv$(vs));
            }
            ),
        (Call,
            if( const auto* f = e.fcn.opt_Value() )
                visit_mir_lvalue(*f, ValUsage::Read, visit_lval_cb);
            for(const auto& arg : e.args)
                if( const auto* e = arg.opt_LValue() )
                    visit_mir_lvalue(*e, ValUsage::Read, visit_lval_cb);

            // Push blocks (with return valid only in one)
            add_to_visit(e.panic_block, val_state.clone());

            // TODO: If the function returns !, don't follow the ret_block
            lvalue_set(e.ret_val);
            add_to_visit(e.ret_block, mv$(val_state));
            )
        )
    }

    // Dump out variable lifetimes.
    if( dump_debug )
    {
        for(unsigned int i = 0; i < temporary_lifetimes.size(); i ++)
        {
            temporary_lifetimes[i].dump_debug("tmp", i, block_offsets);
        }
        for(unsigned int i = 0; i < variable_lifetimes.size(); i ++)
        {
            variable_lifetimes[i].dump_debug("var", i, block_offsets);
        }
    }

    // Move lifetime bitmaps into the variable for the below code
    ::MIR::ValueLifetimes   rv;
    rv.m_block_offsets = mv$(block_offsets);
    rv.m_temporaries.reserve( temporary_lifetimes.size() );
    for(auto& lft : temporary_lifetimes)
        rv.m_temporaries.push_back( ::MIR::ValueLifetime(mv$(lft.stmt_bitmap)) );
    rv.m_variables.reserve( variable_lifetimes.size() );
    for(auto& lft : variable_lifetimes)
        rv.m_variables.push_back( ::MIR::ValueLifetime(mv$(lft.stmt_bitmap)) );

    return rv;
}
#endif
