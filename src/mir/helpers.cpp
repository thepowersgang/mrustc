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

void ::MIR::TypeResolve::fmt_pos(::std::ostream& os) const
{
    os << this->m_path << " BB" << this->bb_idx << "/";
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
    fmt_pos(os);
    cb(os);
    os << ::std::endl;
    abort();
    //throw CheckFailure {};
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
const ::HIR::TypeRef& ::MIR::TypeResolve::get_lvalue_type(::HIR::TypeRef& tmp, const ::MIR::LValue& val) const
{
    TU_MATCH(::MIR::LValue, (val), (e),
    (Variable,
        MIR_ASSERT(*this, e < m_fcn.named_variables.size(), val << " out of range (" << m_fcn.named_variables.size() << ")");
        return m_fcn.named_variables.at(e);
        ),
    (Temporary,
        MIR_ASSERT(*this, e.idx < m_fcn.temporaries.size(), val << " out of range (" << m_fcn.temporaries.size() << ")");
        return m_fcn.temporaries.at(e.idx);
        ),
    (Argument,
        MIR_ASSERT(*this, e.idx < m_args.size(), val << " out of range (" << m_args.size() << ")");
        return m_args.at(e.idx).second;
        ),
    (Static,
        return get_static_type(tmp,  e);
        ),
    (Return,
        return m_ret_type;
        ),
    (Field,
        const auto& ty = this->get_lvalue_type(tmp, *e.val);
        TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty.m_data), (te),
        (
            MIR_BUG(*this, "Field access on unexpected type - " << ty);
            ),
        // Array and Slice use LValue::Field when the index is constant and known-good
        (Array,
            return *te.inner;
            ),
        (Slice,
            return *te.inner;
            ),
        (Tuple,
            MIR_ASSERT(*this, e.field_index < te.size(), "Field index out of range in tuple " << e.field_index << " >= " << te.size());
            return te[e.field_index];
            ),
        (Path,
            MIR_ASSERT(*this, te.binding.is_Struct(), "Field on non-Struct - " << ty);
            const auto& str = *te.binding.as_Struct();
            auto monomorph = [&](const auto& ty)->const auto& {
                if( monomorphise_type_needed(ty) ) {
                    tmp = monomorphise_type(sp, str.m_params, te.path.m_data.as_Generic().m_params, ty);
                    m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                }
                else {
                    return ty;
                }
                };
            TU_MATCHA( (str.m_data), (se),
            (Unit,
                MIR_BUG(*this, "Field on unit-like struct - " << ty);
                ),
            (Tuple,
                MIR_ASSERT(*this, e.field_index < se.size(), "Field index out of range in tuple-struct " << te.path);
                return monomorph(se[e.field_index].ent);
                ),
            (Named,
                MIR_ASSERT(*this, e.field_index < se.size(), "Field index out of range in struct " << te.path);
                return monomorph(se[e.field_index].second.ent);
                )
            )
            )
        )
        ),
    (Deref,
        const auto& ty = this->get_lvalue_type(tmp, *e.val);
        TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty.m_data), (te),
        (
            MIR_BUG(*this, "Deref on unexpected type - " << ty);
            ),
        (Path,
            if( const auto* inner_ptr = this->is_type_owned_box(ty) )
            {
                return *inner_ptr;
            }
            else {
                MIR_BUG(*this, "Deref on unexpected type - " << ty);
            }
            ),
        (Pointer,
            return *te.inner;
            ),
        (Borrow,
            return *te.inner;
            )
        )
        ),
    (Index,
        const auto& ty = this->get_lvalue_type(tmp, *e.val);
        TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty.m_data), (te),
        (
            MIR_BUG(*this, "Index on unexpected type - " << ty);
            ),
        (Slice,
            return *te.inner;
            ),
        (Array,
            return *te.inner;
            )
        )
        ),
    (Downcast,
        const auto& ty = this->get_lvalue_type(tmp, *e.val);
        TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty.m_data), (te),
        (
            MIR_BUG(*this, "Downcast on unexpected type - " << ty);
            ),
        (Path,
            MIR_ASSERT(*this, te.binding.is_Enum() || te.binding.is_Union(), "Downcast on non-Enum");
            if( te.binding.is_Enum() )
            {
                const auto& enm = *te.binding.as_Enum();
                const auto& variants = enm.m_variants;
                MIR_ASSERT(*this, e.variant_index < variants.size(), "Variant index out of range");
                const auto& variant = variants[e.variant_index];
                // TODO: Make data variants refer to associated types (unify enum and struct handling)
                TU_MATCHA( (variant.second), (ve),
                (Value,
                    ),
                (Unit,
                    ),
                (Tuple,
                    // HACK! Create tuple.
                    ::std::vector< ::HIR::TypeRef>  tys;
                    for(const auto& fld : ve)
                        tys.push_back( monomorphise_type(sp, enm.m_params, te.path.m_data.as_Generic().m_params, fld.ent) );
                    tmp = ::HIR::TypeRef( mv$(tys) );
                    m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                    ),
                (Struct,
                    // HACK! Create tuple.
                    ::std::vector< ::HIR::TypeRef>  tys;
                    for(const auto& fld : ve)
                        tys.push_back( monomorphise_type(sp, enm.m_params, te.path.m_data.as_Generic().m_params, fld.second.ent) );
                    tmp = ::HIR::TypeRef( mv$(tys) );
                    m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                    )
                )
            }
            else
            {
                const auto& unm = *te.binding.as_Union();
                MIR_ASSERT(*this, e.variant_index < unm.m_variants.size(), "Variant index out of range");
                const auto& variant = unm.m_variants[e.variant_index];
                const auto& var_ty = variant.second.ent;

                if( monomorphise_type_needed(var_ty) ) {
                    tmp = monomorphise_type(sp, unm.m_params, te.path.m_data.as_Generic().m_params, variant.second.ent);
                    m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                }
                else {
                    return var_ty;
                }
            }
            )
        )
        )
    )
    throw "";
}

::HIR::TypeRef MIR::TypeResolve::get_const_type(const ::MIR::Constant& c) const
{
    TU_MATCHA( (c), (e),
    (Int,
        return e.t;
        ),
    (Uint,
        return e.t;
        ),
    (Float,
        return e.t;
        ),
    (Bool,
        return ::HIR::CoreType::Bool;
        ),
    (Bytes,
        return ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, ::HIR::TypeRef::new_array( ::HIR::CoreType::U8, e.size() ) );
        ),
    (StaticString,
        return ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, ::HIR::CoreType::Str );
        ),
    (Const,
        MonomorphState  p;
        auto v = m_resolve.get_value(this->sp, e.p, p, /*signature_only=*/true);
        if( const auto* ve = v.opt_Constant() ) {
            const auto& ty = (*ve)->m_type;
            if( monomorphise_type_needed(ty) )
                MIR_TODO(*this, "get_const_type - Monomorphise type " << ty);
            else
                return ty.clone();
        }
        else {
            MIR_BUG(*this, "get_const_type - Not a constant");
        }
        ),
    (ItemAddr,
        MIR_TODO(*this, "get_const_type - Get type for constant `" << c << "`");
        )
    )
    throw "";
}
const ::HIR::TypeRef* ::MIR::TypeResolve::is_type_owned_box(const ::HIR::TypeRef& ty) const
{
    return m_resolve.is_type_owned_box(ty);
}

// --------------------------------------------------------------------
// MIR_Helper_GetLifetimes
// --------------------------------------------------------------------
namespace {
    enum class ValUsage {
        Read,
        Write,
        Borrow,
    };

    bool visit_mir_lvalue(const ::MIR::LValue& lv, ValUsage u, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        if( cb(lv, u) )
            return true;
        TU_MATCHA( (lv), (e),
        (Variable,
            ),
        (Argument,
            ),
        (Temporary,
            ),
        (Static,
            ),
        (Return,
            ),
        (Field,
            return visit_mir_lvalue(*e.val, u, cb);
            ),
        (Deref,
            return visit_mir_lvalue(*e.val, ValUsage::Read, cb);
            ),
        (Index,
            bool rv = false;
            rv |= visit_mir_lvalue(*e.val, u, cb);
            rv |= visit_mir_lvalue(*e.idx, ValUsage::Read, cb);
            return rv;
            ),
        (Downcast,
            return visit_mir_lvalue(*e.val, u, cb);
            )
        )
        return false;
    }

    bool visit_mir_lvalue(const ::MIR::Param& p, ValUsage u, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        if( const auto* e = p.opt_LValue() )
        {
            return visit_mir_lvalue(*e, u, cb);
        }
        else
        {
            return false;
        }
    }

    bool visit_mir_lvalues(const ::MIR::RValue& rval, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        bool rv = false;
        TU_MATCHA( (rval), (se),
        (Use,
            rv |= visit_mir_lvalue(se, ValUsage::Read, cb);
            ),
        (Constant,
            ),
        (SizedArray,
            rv |= visit_mir_lvalue(se.val, ValUsage::Read, cb);
            ),
        (Borrow,
            rv |= visit_mir_lvalue(se.val, ValUsage::Borrow, cb);
            ),
        (Cast,
            rv |= visit_mir_lvalue(se.val, ValUsage::Read, cb);
            ),
        (BinOp,
            rv |= visit_mir_lvalue(se.val_l, ValUsage::Read, cb);
            rv |= visit_mir_lvalue(se.val_r, ValUsage::Read, cb);
            ),
        (UniOp,
            rv |= visit_mir_lvalue(se.val, ValUsage::Read, cb);
            ),
        (DstMeta,
            rv |= visit_mir_lvalue(se.val, ValUsage::Read, cb);
            ),
        (DstPtr,
            rv |= visit_mir_lvalue(se.val, ValUsage::Read, cb);
            ),
        (MakeDst,
            rv |= visit_mir_lvalue(se.ptr_val, ValUsage::Read, cb);
            rv |= visit_mir_lvalue(se.meta_val, ValUsage::Read, cb);
            ),
        (Tuple,
            for(auto& v : se.vals)
                rv |= visit_mir_lvalue(v, ValUsage::Read, cb);
            ),
        (Array,
            for(auto& v : se.vals)
                rv |= visit_mir_lvalue(v, ValUsage::Read, cb);
            ),
        (Variant,
            rv |= visit_mir_lvalue(se.val, ValUsage::Read, cb);
            ),
        (Struct,
            for(auto& v : se.vals)
                rv |= visit_mir_lvalue(v, ValUsage::Read, cb);
            )
        )
        return rv;
    }

    bool visit_mir_lvalues(const ::MIR::Statement& stmt, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        bool rv = false;
        TU_MATCHA( (stmt), (e),
        (Assign,
            rv |= visit_mir_lvalues(e.src, cb);
            rv |= visit_mir_lvalue(e.dst, ValUsage::Write, cb);
            ),
        (Asm,
            for(auto& v : e.inputs)
                rv |= visit_mir_lvalue(v.second, ValUsage::Read, cb);
            for(auto& v : e.outputs)
                rv |= visit_mir_lvalue(v.second, ValUsage::Write, cb);
            ),
        (SetDropFlag,
            ),
        (Drop,
            // Well, it mutates...
            rv |= visit_mir_lvalue(e.slot, ValUsage::Write, cb);
            ),
        (ScopeEnd,
            )
        )
        return rv;
    }

    /*
    void visit_mir_lvalues_mut(::MIR::Terminator& term, ::std::function<bool(::MIR::LValue& , ValUsage)> cb)
    {
        TU_MATCHA( (term), (e),
        (Incomplete,
            ),
        (Return,
            ),
        (Diverge,
            ),
        (Goto,
            ),
        (Panic,
            ),
        (If,
            visit_mir_lvalue_mut(e.cond, ValUsage::Read, cb);
            ),
        (Switch,
            visit_mir_lvalue_mut(e.val, ValUsage::Read, cb);
            ),
        (Call,
            if( e.fcn.is_Value() ) {
                visit_mir_lvalue_mut(e.fcn.as_Value(), ValUsage::Read, cb);
            }
            for(auto& v : e.args)
                visit_mir_lvalue_mut(v, ValUsage::Read, cb);
            visit_mir_lvalue_mut(e.ret_val, ValUsage::Write, cb);
            )
        )
    }

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
}
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

    struct ValueLifetime
    {
        ::std::vector<bool> stmt_bitmap;
        ValueLifetime(size_t stmt_count):
            stmt_bitmap(stmt_count)
        {}

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
            auto try_merge_lft = [&](const ProtoLifetime& lft, const ::std::vector<unsigned int>& seen)->bool {
                if(lft.is_empty())  return false;
                // TODO: What should be done for borrow flagged values
                if(lft.end == Position { ~0u, ~0u })    return false;
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
                DEBUG(state << " " << new_state.index << " -> bb" << new_bb_idx << " (no existing state)");
            }
            else if( bb_memory_ent.try_merge(new_state) )
            {
                // This state has new information, needs to be visited
                DEBUG(state << " " << new_state.index << " -> bb" << new_bb_idx << " (new info)");
            }
            else
            {
                // Skip
                DEBUG(state << " " << new_state.index << " No new state before push (to bb" << new_bb_idx << "), applying");
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
                DEBUG(state << " " << val_state.index << " No new entry state");
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
