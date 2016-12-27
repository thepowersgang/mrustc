/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/mir_builder.cpp
 * - MIR Building Helper
 */
#include <algorithm>
#include "from_hir.hpp"

// --------------------------------------------------------------------
// MirBuilder
// --------------------------------------------------------------------
MirBuilder::MirBuilder(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::Function::args_t& args, ::MIR::Function& output):
    m_root_span(sp),
    m_resolve(resolve),
    m_args(args),
    m_output(output),
    m_lang_Box(nullptr),
    m_block_active(false),
    m_result_valid(false),
    m_fcn_scope(*this, 0)
{
    if( resolve.m_crate.m_lang_items.count("owned_box") > 0 ) {
        m_lang_Box = &resolve.m_crate.m_lang_items.at("owned_box");
    }

    set_cur_block( new_bb_unlinked() );
    m_scopes.push_back( ScopeDef { sp } );
    m_scope_stack.push_back( 0 );

    m_scopes.push_back( ScopeDef { sp, ScopeType::make_Temporaries({}) } );
    m_scope_stack.push_back( 1 );

    m_variable_states.resize( output.named_variables.size(), VarState::Uninit );
}
MirBuilder::~MirBuilder()
{
    const auto& sp = m_root_span;
    if( block_active() )
    {
        if( has_result() )
        {
            push_stmt_assign( sp, ::MIR::LValue::make_Return({}), get_result(sp) );
        }
        terminate_scope( sp, ScopeHandle { *this, 1 } );
        terminate_scope( sp, mv$(m_fcn_scope) );
        end_block( ::MIR::Terminator::make_Return({}) );
    }
}

const ::HIR::TypeRef* MirBuilder::is_type_owned_box(const ::HIR::TypeRef& ty) const
{
    if( m_lang_Box )
    {
        if( ! ty.m_data.is_Path() ) {
            return nullptr;
        }
        const auto& te = ty.m_data.as_Path();

        if( ! te.path.m_data.is_Generic() ) {
            return nullptr;
        }
        const auto& pe = te.path.m_data.as_Generic();

        if( pe.m_path != *m_lang_Box ) {
            return nullptr;
        }
        // TODO: Properly assert?
        return &pe.m_params.m_types.at(0);
    }
    else
    {
        return nullptr;
    }
}

void MirBuilder::define_variable(unsigned int idx)
{
    DEBUG("DEFINE var" << idx  << ": " << m_output.named_variables.at(idx));
    for( auto scope_idx : ::reverse(m_scope_stack) )
    {
        auto& scope_def = m_scopes.at(scope_idx);
        TU_MATCH_DEF( ScopeType, (scope_def.data), (e),
        (
            ),
        (Variables,
            auto it = ::std::find(e.vars.begin(), e.vars.end(), idx);
            assert(it == e.vars.end());
            e.vars.push_back( idx );
            return ;
            ),
        (Split,
            BUG(Span(), "Variable " << idx << " introduced within a Split");
            )
        )
    }
    BUG(Span(), "Variable " << idx << " introduced with no Variable scope");
}
::MIR::LValue MirBuilder::new_temporary(const ::HIR::TypeRef& ty)
{
    unsigned int rv = m_output.temporaries.size();
    DEBUG("DEFINE tmp" << rv << ": " << ty);

    m_output.temporaries.push_back( ty.clone() );
    m_temporary_states.push_back( VarState::Uninit );
    assert(m_output.temporaries.size() == m_temporary_states.size());

    ScopeDef* top_scope = nullptr;
    for(unsigned int i = m_scope_stack.size(); i --; )
    {
        auto idx = m_scope_stack[i];
        if( m_scopes.at( idx ).data.is_Temporaries() ) {
            top_scope = &m_scopes.at(idx);
            break ;
        }
    }
    assert( top_scope );
    auto& tmp_scope = top_scope->data.as_Temporaries();
    tmp_scope.temporaries.push_back( rv );
    return ::MIR::LValue::make_Temporary({rv});
}
::MIR::LValue MirBuilder::lvalue_or_temp(const Span& sp, const ::HIR::TypeRef& ty, ::MIR::RValue val)
{
    TU_IFLET(::MIR::RValue, val, Use, e,
        return mv$(e);
    )
    else {
        auto temp = new_temporary(ty);
        push_stmt_assign( sp, ::MIR::LValue(temp.as_Temporary()), mv$(val) );
        return temp;
    }
}

::MIR::RValue MirBuilder::get_result(const Span& sp)
{
    if(!m_result_valid) {
        BUG(sp, "No value avaliable");
    }
    auto rv = mv$(m_result);
    m_result_valid = false;
    return rv;
}

::MIR::LValue MirBuilder::get_result_unwrap_lvalue(const Span& sp)
{
    auto rv = get_result(sp);
    TU_IFLET(::MIR::RValue, rv, Use, e,
        return mv$(e);
    )
    else {
        BUG(sp, "LValue expected, got RValue");
    }
}
::MIR::LValue MirBuilder::get_result_in_lvalue(const Span& sp, const ::HIR::TypeRef& ty)
{
    auto rv = get_result(sp);
    TU_IFLET(::MIR::RValue, rv, Use, e,
        return mv$(e);
    )
    else {
        auto temp = new_temporary(ty);
        push_stmt_assign( sp, ::MIR::LValue(temp.clone()), mv$(rv) );
        return temp;
    }
}
void MirBuilder::set_result(const Span& sp, ::MIR::RValue val)
{
    if(m_result_valid) {
        BUG(sp, "Pushing a result over an existing result");
    }
    m_result = mv$(val);
    m_result_valid = true;
}

void MirBuilder::push_stmt_assign(const Span& sp, ::MIR::LValue dst, ::MIR::RValue val)
{
    DEBUG(dst << " = " << val);
    ASSERT_BUG(sp, m_block_active, "Pushing statement with no active block");
    ASSERT_BUG(sp, dst.tag() != ::MIR::LValue::TAGDEAD, "");
    ASSERT_BUG(sp, val.tag() != ::MIR::RValue::TAGDEAD, "");

    TU_MATCHA( (val), (e),
    (Use,
        this->moved_lvalue(sp, e);
        ),
    (Constant,
        ),
    (SizedArray,
        this->moved_lvalue(sp, e.val);
        ),
    (Borrow,
        if( e.type == ::HIR::BorrowType::Owned ) {
            TODO(sp, "Move using &move");
            // Likely would require a marker that ensures that the memory isn't reused.
            this->moved_lvalue(sp, e.val);
        }
        else {
            // Doesn't move
        }
        ),
    (Cast,
        this->moved_lvalue(sp, e.val);
        ),
    (BinOp,
        switch(e.op)
        {
        case ::MIR::eBinOp::EQ:
        case ::MIR::eBinOp::NE:
        case ::MIR::eBinOp::GT:
        case ::MIR::eBinOp::GE:
        case ::MIR::eBinOp::LT:
        case ::MIR::eBinOp::LE:
            // Takes an implicit borrow... and only works on copy, so why is this block here?
            break;
        default:
            this->moved_lvalue(sp, e.val_l);
            this->moved_lvalue(sp, e.val_r);
            break;
        }
        ),
    (UniOp,
        this->moved_lvalue(sp, e.val);
        ),
    (DstMeta,
        // Doesn't move
        ),
    (DstPtr,
        // Doesn't move
        ),
    (MakeDst,
        // Doesn't move ptr_val
        this->moved_lvalue(sp, e.meta_val);
        ),
    (Tuple,
        for(const auto& val : e.vals)
            this->moved_lvalue(sp, val);
        ),
    (Array,
        for(const auto& val : e.vals)
            this->moved_lvalue(sp, val);
        ),
    (Variant,
        this->moved_lvalue(sp, e.val);
        ),
    (Struct,
        for(const auto& val : e.vals)
            this->moved_lvalue(sp, val);
        )
    )

    // Drop target if populated
    mark_value_assigned(sp, dst);
    m_output.blocks.at(m_current_block).statements.push_back( ::MIR::Statement::make_Assign({ mv$(dst), mv$(val) }) );
}
void MirBuilder::push_stmt_drop(const Span& sp, ::MIR::LValue val)
{
    ASSERT_BUG(sp, m_block_active, "Pushing statement with no active block");
    ASSERT_BUG(sp, val.tag() != ::MIR::LValue::TAGDEAD, "");

    if( lvalue_is_copy(sp, val) ) {
        // Don't emit a drop for Copy values
        return ;
    }

    DEBUG("DROP " << val);
    m_output.blocks.at(m_current_block).statements.push_back( ::MIR::Statement::make_Drop({ ::MIR::eDropKind::DEEP, mv$(val) }) );
}
void MirBuilder::push_stmt_drop_shallow(const Span& sp, ::MIR::LValue val)
{
    ASSERT_BUG(sp, m_block_active, "Pushing statement with no active block");
    ASSERT_BUG(sp, val.tag() != ::MIR::LValue::TAGDEAD, "");

    // TODO: Ensure that the type is a Box
    //if( lvalue_is_copy(sp, val) ) {
    //    // Don't emit a drop for Copy values
    //    return ;
    //}

    DEBUG("DROP shallow " << val);
    m_output.blocks.at(m_current_block).statements.push_back( ::MIR::Statement::make_Drop({ ::MIR::eDropKind::SHALLOW, mv$(val) }) );
}

void MirBuilder::mark_value_assigned(const Span& sp, const ::MIR::LValue& dst)
{
    TU_MATCH_DEF(::MIR::LValue, (dst), (e),
    (
        ),
    (Temporary,
        switch(get_temp_state(sp, e.idx))
        {
        case VarState::Uninit:
            break;
        case VarState::Dropped:
            // ERROR?
            break;
        case VarState::Moved:
        case VarState::MaybeMoved:
            // ERROR? Temporaries shouldn't be resassigned after becoming valid
            break;
        case VarState::InnerMoved:
            BUG(sp, "Reassigning inner-moved temporary - " << dst);
            break;
        case VarState::Init:
            // ERROR. Temporaries are single-assignment
            break;
        }
        set_temp_state(sp, e.idx, VarState::Init);
        ),
    (Return,
        // Don't drop.
        //m_return_valid = true;
        ),
    (Variable,
        // TODO: Ensure that slot is mutable (information is lost, assume true)
        switch( get_variable_state(sp, e) )
        {
        case VarState::Uninit:
        case VarState::Moved:
            break;
        case VarState::Dropped:
            // TODO: Is this an error? The variable has descoped.
            break;
        case VarState::Init:
            // Drop (if not Copy) - Copy check is done within push_stmt_drop
            push_stmt_drop( sp, dst.clone() );
            break;
        case VarState::InnerMoved:
            push_stmt_drop_shallow( sp, dst.clone() );
            break;
        case VarState::MaybeMoved:
            // TODO: Conditional drop
            break;
        }
        set_variable_state(sp, e, VarState::Init);
        )
    )
}

void MirBuilder::raise_variables(const Span& sp, const ::MIR::LValue& val, const ScopeHandle& scope)
{
    TRACE_FUNCTION_F(val);
    TU_MATCH_DEF(::MIR::LValue, (val), (e),
    (
        ),
    // TODO: This may not be correct, because it can change the drop points and ordering
    // HACK: Working around cases where values are dropped while the result is not yet used.
    (Deref,
        raise_variables(sp, *e.val, scope);
        ),
    (Field,
        raise_variables(sp, *e.val, scope);
        ),
    (Downcast,
        raise_variables(sp, *e.val, scope);
        ),
    // Actual value types
    (Variable,
        auto idx = e;
        auto scope_it = m_scope_stack.rbegin();
        while( scope_it != m_scope_stack.rend() )
        {
            auto& scope_def = m_scopes.at(*scope_it);

            TU_IFLET( ScopeType, scope_def.data, Variables, e,
                auto tmp_it = ::std::find( e.vars.begin(), e.vars.end(), idx );
                if( tmp_it != e.vars.end() )
                {
                    e.vars.erase( tmp_it );
                    DEBUG("Raise variable " << idx << " from " << *scope_it);
                    break ;
                }
            )
            // If the variable was defined above the desired scope (i.e. this didn't find it), return
            if( *scope_it == scope.idx )
                return ;
            ++scope_it;
        }
        if( scope_it == m_scope_stack.rend() )
        {
            // Temporary wasn't defined in a visible scope?
            return ;
        }
        ++scope_it;

        while( scope_it != m_scope_stack.rend() )
        {
            auto& scope_def = m_scopes.at(*scope_it);

            TU_IFLET( ScopeType, scope_def.data, Variables, e,
                e.vars.push_back( idx );
                DEBUG("- to " << *scope_it);
                return ;
            )
            ++scope_it;
        }

        DEBUG("- top");
        ),
    (Temporary,
        auto idx = e.idx;
        auto scope_it = m_scope_stack.rbegin();
        while( scope_it != m_scope_stack.rend() )
        {
            auto& scope_def = m_scopes.at(*scope_it);

            TU_IFLET( ScopeType, scope_def.data, Temporaries, e,
                auto tmp_it = ::std::find( e.temporaries.begin(), e.temporaries.end(), idx );
                if( tmp_it != e.temporaries.end() )
                {
                    e.temporaries.erase( tmp_it );
                    DEBUG("Raise temporary " << idx << " from " << *scope_it);
                    break ;
                }
            )
            
            // If the temporary was defined above the desired scope (i.e. this didn't find it), return
            if( *scope_it == scope.idx )
                return ;
            ++scope_it;
        }
        if( scope_it == m_scope_stack.rend() )
        {
            // Temporary wasn't defined in a visible scope?
            return ;
        }
        ++scope_it;

        while( scope_it != m_scope_stack.rend() )
        {
            auto& scope_def = m_scopes.at(*scope_it);

            TU_IFLET( ScopeType, scope_def.data, Temporaries, e,
                e.temporaries.push_back( idx );
                DEBUG("- to " << *scope_it);
                return ;
            )
            ++scope_it;
        }

        DEBUG("- top");
        )
    )
}
void MirBuilder::raise_variables(const Span& sp, const ::MIR::RValue& rval, const ScopeHandle& scope)
{
    TU_MATCHA( (rval), (e),
    (Use,
        this->raise_variables(sp, e, scope);
        ),
    (Constant,
        ),
    (SizedArray,
        this->raise_variables(sp, e.val, scope);
        ),
    (Borrow,
        // TODO: Wait, is this valid?
        this->raise_variables(sp, e.val, scope);
        ),
    (Cast,
        this->raise_variables(sp, e.val, scope);
        ),
    (BinOp,
        this->raise_variables(sp, e.val_l, scope);
        this->raise_variables(sp, e.val_r, scope);
        ),
    (UniOp,
        this->raise_variables(sp, e.val, scope);
        ),
    (DstMeta,
        this->raise_variables(sp, e.val, scope);
        ),
    (DstPtr,
        this->raise_variables(sp, e.val, scope);
        ),
    (MakeDst,
        this->raise_variables(sp, e.ptr_val, scope);
        this->raise_variables(sp, e.meta_val, scope);
        ),
    (Tuple,
        for(const auto& val : e.vals)
            this->raise_variables(sp, val, scope);
        ),
    (Array,
        for(const auto& val : e.vals)
            this->raise_variables(sp, val, scope);
        ),
    (Variant,
        this->raise_variables(sp, e.val, scope);
        ),
    (Struct,
        for(const auto& val : e.vals)
            this->raise_variables(sp, val, scope);
        )
    )
}

void MirBuilder::set_cur_block(unsigned int new_block)
{
    ASSERT_BUG(Span(), !m_block_active, "Updating block when previous is active");
    ASSERT_BUG(Span(), new_block < m_output.blocks.size(), "Invalid block ID being started - " << new_block);
    ASSERT_BUG(Span(), m_output.blocks[new_block].terminator.is_Incomplete(), "Attempting to resume a completed block - BB" << new_block);
    DEBUG("BB" << new_block << " START");
    m_current_block = new_block;
    m_block_active = true;
}
void MirBuilder::end_block(::MIR::Terminator term)
{
    if( !m_block_active ) {
        BUG(Span(), "Terminating block when none active");
    }
    DEBUG("BB" << m_current_block << " END -> " << term);
    m_output.blocks.at(m_current_block).terminator = mv$(term);
    m_block_active = false;
    m_current_block = 0;
}
::MIR::BasicBlockId MirBuilder::pause_cur_block()
{
    if( !m_block_active ) {
        BUG(Span(), "Pausing block when none active");
    }
    DEBUG("BB" << m_current_block << " PAUSE");
    m_block_active = false;
    auto rv = m_current_block;
    m_current_block = 0;
    return rv;
}
::MIR::BasicBlockId MirBuilder::new_bb_linked()
{
    auto rv = new_bb_unlinked();
    DEBUG("BB" << rv);
    end_block( ::MIR::Terminator::make_Goto(rv) );
    set_cur_block(rv);
    return rv;
}
::MIR::BasicBlockId MirBuilder::new_bb_unlinked()
{
    auto rv = m_output.blocks.size();
    DEBUG("BB" << rv);
    m_output.blocks.push_back({});
    return rv;
}

ScopeHandle MirBuilder::new_scope_var(const Span& sp)
{
    unsigned int idx = m_scopes.size();
    m_scopes.push_back( ScopeDef {sp, ScopeType::make_Variables({})} );
    m_scope_stack.push_back( idx );
    DEBUG("START (var) scope " << idx);
    return ScopeHandle { *this, idx };
}
ScopeHandle MirBuilder::new_scope_temp(const Span& sp)
{
    unsigned int idx = m_scopes.size();
    m_scopes.push_back( ScopeDef {sp, ScopeType::make_Temporaries({})} );
    m_scope_stack.push_back( idx );
    DEBUG("START (temp) scope " << idx);
    return ScopeHandle { *this, idx };
}
ScopeHandle MirBuilder::new_scope_split(const Span& sp)
{
    unsigned int idx = m_scopes.size();
    m_scopes.push_back( ScopeDef {sp, ScopeType::make_Split({})} );
    m_scopes.back().data.as_Split().arms.push_back( {} );
    m_scope_stack.push_back( idx );
    DEBUG("START (split) scope " << idx);
    return ScopeHandle { *this, idx };
}
ScopeHandle MirBuilder::new_scope_loop(const Span& sp)
{
    unsigned int idx = m_scopes.size();
    m_scopes.push_back( ScopeDef {sp, ScopeType::make_Loop({})} );
    m_scope_stack.push_back( idx );
    DEBUG("START (loop) scope " << idx);
    return ScopeHandle { *this, idx };
}
void MirBuilder::terminate_scope(const Span& sp, ScopeHandle scope, bool emit_cleanup/*=true*/)
{
    DEBUG("DONE scope " << scope.idx << " - " << (emit_cleanup ? "CLEANUP" : "NO CLEANUP"));
    // 1. Check that this is the current scope (at the top of the stack)
    if( m_scope_stack.empty() || m_scope_stack.back() != scope.idx )
    {
        DEBUG("- m_scope_stack = [" << m_scope_stack << "]");
        auto it = ::std::find( m_scope_stack.begin(), m_scope_stack.end(), scope.idx );
        if( it == m_scope_stack.end() )
            BUG(sp, "Terminating scope not on the stack - scope " << scope.idx);
        BUG(sp, "Terminating scope " << scope.idx << " when not at top of stack, " << (m_scope_stack.end() - it - 1) << " scopes in the way");
    }

    auto& scope_def = m_scopes.at(scope.idx);
    ASSERT_BUG( sp, scope_def.complete == false, "Terminating scope which is already terminated" );

    if( emit_cleanup )
    {
        // 2. Emit drops for all non-moved variables (share with below)
        drop_scope_values(scope_def);
    }

    // 3. Pop scope (last because `drop_scope_values` uses the stack)
    m_scope_stack.pop_back();

    complete_scope(scope_def);
}
void MirBuilder::terminate_scope_early(const Span& sp, const ScopeHandle& scope)
{
    DEBUG("EARLY scope " << scope.idx);

    // 1. Ensure that this block is in the stack
    auto it = ::std::find( m_scope_stack.begin(), m_scope_stack.end(), scope.idx );
    if( it == m_scope_stack.end() ) {
        BUG(sp, "Early-terminating scope not on the stack");
    }
    unsigned int slot = it - m_scope_stack.begin();

    bool is_conditional = false;
    for(unsigned int i = m_scope_stack.size(); i-- > slot; )
    {
        auto idx = m_scope_stack[i];
        auto& scope_def = m_scopes.at( idx );

        // If a conditional block is hit, prevent full termination of the rest
        if( scope_def.data.is_Split() || scope_def.data.is_Loop() )
            is_conditional = true;

        if( !is_conditional ) {
            DEBUG("Complete scope " << idx);
            drop_scope_values(scope_def);
            m_scope_stack.pop_back();
            complete_scope(scope_def);
        }
        else {
            // Mark patial within this scope?
            DEBUG("Drop part of scope " << idx);

            // Emit drops for dropped values within this scope
            drop_scope_values(scope_def);
            // Inform the scope that it's been early-exited
            TU_IFLET( ScopeType, scope_def.data, Split, e,
                e.arms.back().has_early_terminated = true;
            )
        }
    }
}
void MirBuilder::end_split_arm(const Span& sp, const ScopeHandle& handle, bool reachable)
{
    ASSERT_BUG(sp, handle.idx < m_scopes.size(), "");
    auto& sd = m_scopes.at( handle.idx );
    ASSERT_BUG(sp, sd.data.is_Split(), "");
    auto& sd_split = sd.data.as_Split();
    ASSERT_BUG(sp, !sd_split.arms.empty(), "");

    sd_split.arms.back().always_early_terminated = /*sd_split.arms.back().has_early_terminated &&*/ !reachable;

    // HACK: If this arm's end is reachable, convert InnerMoved (shallow drop) variable states to Moved
    // - I'm not 100% sure this is the correct place for calling drop.
    if( reachable )
    {
        auto& vss = sd_split.arms.back().var_states;
        for(unsigned int i = 0; i < vss.size(); i ++ )
        {
            auto& vs = vss[i];
            if( vs == VarState::InnerMoved ) {
                // Emit the shallow drop
                push_stmt_drop_shallow( sp, ::MIR::LValue::make_Variable(i) );
                vs = VarState::Moved;
            }
        }
    }

    sd_split.arms.push_back( {} );
}
void MirBuilder::end_split_arm_early(const Span& sp)
{
    // Terminate all scopes until a split is found.
    while( ! m_scope_stack.empty() && ! (m_scopes.at( m_scope_stack.back() ).data.is_Split() || m_scopes.at( m_scope_stack.back() ).data.is_Loop()) )
    {
        auto& scope_def = m_scopes[m_scope_stack.back()];
        // Fully drop the scope
        DEBUG("Complete scope " << m_scope_stack.size()-1);
        drop_scope_values(scope_def);
        m_scope_stack.pop_back();
        complete_scope(scope_def);
    }

    if( !m_scope_stack.empty() && m_scopes.at( m_scope_stack.back() ).data.is_Split() )
    {
        auto& sd = m_scopes[ m_scope_stack.back() ];
        auto& sd_split = sd.data.as_Split();
        sd_split.arms.back().has_early_terminated = true;

        const auto& vss = sd_split.arms.back().var_states;
        for(unsigned int i = 0; i < vss.size(); i ++ )
        {
            auto& vs = vss[i];
            if( vs == VarState::InnerMoved ) {
                // Emit the shallow drop
                push_stmt_drop_shallow( sp, ::MIR::LValue::make_Variable(i) );
                //vs = VarState::Dropped;
            }
        }
    }
}
void MirBuilder::complete_scope(ScopeDef& sd)
{
    sd.complete = true;

    TU_MATCHA( (sd.data), (e),
    (Temporaries,
        DEBUG("Temporaries " << e.temporaries);
        ),
    (Variables,
        DEBUG("Variables " << e.vars);
        ),
    (Loop,
        DEBUG("Loop");
        ),
    (Split,
        )
    )

    // No macro for better debug output.
    if( sd.data.is_Split() )
    {
        auto& e = sd.data.as_Split();

        assert( e.arms.size() > 1 );
        TRACE_FUNCTION_F("Split - " << (e.arms.size() - 1) << " arms");
        e.arms.pop_back();

        struct StateMerger
        {
            static VarState merge_state(const Span& sp, VarState new_state, VarState old_state)
            {
                switch(old_state)
                {
                case VarState::Uninit:
                    switch( new_state )
                    {
                    case VarState::Uninit:
                        BUG(sp, "Variable state changed from Uninit to Uninit (wut?)");
                        break;
                    case VarState::Init:
                        // TODO: MaybeInit?
                        return VarState::MaybeMoved;
                    case VarState::MaybeMoved:
                        return VarState::MaybeMoved;
                    case VarState::Moved:
                        return VarState::Uninit;
                    case VarState::InnerMoved:
                        TODO(sp, "Handle InnerMoved in Split scope (Init:arm.var_states)");
                        break;
                    case VarState::Dropped:
                        BUG(sp, "Dropped value in arm");
                        break;
                    }
                    BUG(sp, "Override from Uninit to " << new_state);
                    break;
                case VarState::Init:
                    switch( new_state )
                    {
                    case VarState::Uninit:
                        // TODO: MaybeInit?
                        return VarState::MaybeMoved;
                    case VarState::Init:
                        return VarState::Init;
                    case VarState::MaybeMoved:
                        return VarState::MaybeMoved;
                    case VarState::Moved:
                        return VarState::MaybeMoved;
                    case VarState::InnerMoved:
                        TODO(sp, "Handle InnerMoved in Split scope (Init:arm.var_states)");
                        break;
                    case VarState::Dropped:
                        BUG(sp, "Dropped value in arm");
                        break;
                    }
                    break;
                case VarState::InnerMoved:
                    // Need to tag for conditional shallow drop? Or just do that at the end of the split?
                    // - End of the split means that the only optional state is outer drop.
                    TODO(sp, "Handle InnerMoved in Split scope (new_states)");
                    break;
                case VarState::MaybeMoved:
                    // Already optional, don't change
                    return VarState::MaybeMoved;
                case VarState::Moved:
                    switch( new_state )
                    {
                    case VarState::Uninit:
                        // Wut?
                        break;
                    case VarState::Init:
                        // Wut? Reinited?
                        return VarState::MaybeMoved;
                    case VarState::MaybeMoved:
                        return VarState::MaybeMoved;
                    case VarState::Moved:
                        return VarState::Moved;
                    case VarState::InnerMoved:
                        TODO(sp, "Handle InnerMoved in Split scope (Moved:arm.var_states)");
                        break;
                    case VarState::Dropped:
                        BUG(sp, "Dropped value in arm");
                        break;
                    }
                    break;
                case VarState::Dropped:
                    TODO(sp, "How can an arm drop a value?");
                    break;
                }
                throw "";
            }
        };

        // 1. Make a bitmap of changed states in all arms
        size_t var_count = 0;
        size_t tmp_count = 0;
        ::std::set<unsigned int> changed_vars;
        ::std::set<unsigned int> changed_tmps;
        const SplitArm* first_arm = nullptr;
        for(const auto& arm : e.arms)
        {
            if( arm.always_early_terminated )
                continue ;
            for(unsigned int i = 0; i < arm.changed_var_states.size(); i++)
                if( arm.changed_var_states[i] )
                    changed_vars.insert( i );
            for(unsigned int i = 0; i < arm.changed_tmp_states.size(); i++)
                if( arm.changed_tmp_states[i] )
                    changed_tmps.insert( i );
            if( !first_arm )
                first_arm = &arm;
            
            var_count = ::std::max(var_count, arm.var_states.size());
            tmp_count = ::std::max(tmp_count, arm.tmp_states.size());
        }
        
        if( !first_arm )
        {
            DEBUG("No arms yeilded");
            return ;
        }

        ::std::vector<VarState> new_var_states { var_count };
        ::std::vector<VarState> new_tmp_states { tmp_count };
        
        // 2. Build up the final composite state of the first arm
        for(unsigned int i : changed_vars)
        {
            if( i < first_arm->changed_var_states.size() && first_arm->changed_var_states[i] )
            {
                new_var_states[i] = first_arm->var_states[i];
            }
            else
            {
                new_var_states[i] = get_variable_state(sd.span, i);
            }
        }
        for(unsigned int i : changed_tmps)
        {
            if( i < first_arm->changed_tmp_states.size() && first_arm->changed_tmp_states[i] )
            {
                new_tmp_states[i] = first_arm->tmp_states[i];
            }
            else
            {
                new_tmp_states[i] = get_temp_state(sd.span, i);
            }
        }
        
        // 3. Compare the rest of the arms
        for(const auto& arm : e.arms)
        {
            if( arm.always_early_terminated )
                continue ;
            if( &arm == first_arm )
                continue ;
            DEBUG("><");
            for(unsigned int i : changed_vars)
            {
                DEBUG("- VAR" << i);
                auto new_state = ((i < arm.var_states.size() && arm.changed_var_states[i]) ? arm.var_states[i] : get_variable_state(sd.span, i));
                new_var_states[i] = StateMerger::merge_state(sd.span, new_state, new_var_states[i]);
            }
            for(unsigned int i : changed_tmps)
            {
                DEBUG("- TMP" << i);
                auto new_state = ((i < arm.tmp_states.size() && arm.changed_tmp_states[i]) ? arm.tmp_states[i] : get_temp_state(sd.span, i));
                new_tmp_states[i] = StateMerger::merge_state(sd.span, new_state, new_tmp_states[i]);
            }
        }

        // 4. Apply changes
        for(unsigned int i : changed_vars)
        {
            // - NOTE: This scope should be off the stack now, so this call will get the original state
            auto old_state = get_variable_state(sd.span, i);
            auto new_state = new_var_states[i];
            DEBUG("var" << i << " old_state = " << old_state << ", new_state = " << new_state);
            set_variable_state(sd.span, i, new_state);
        }
        for(unsigned int i : changed_tmps)
        {
            // - NOTE: This scope should be off the stack now, so this call will get the original state
            auto old_state = get_temp_state(sd.span, i);
            auto new_state = new_tmp_states[i];
            DEBUG("tmp" << i << " old_state = " << old_state << ", new_state = " << new_state);
            set_temp_state(sd.span, i, new_state);
        }
    }
}

void MirBuilder::with_val_type(const Span& sp, const ::MIR::LValue& val, ::std::function<void(const ::HIR::TypeRef&)> cb) const
{
    TU_MATCH(::MIR::LValue, (val), (e),
    (Variable,
        cb( m_output.named_variables.at(e) );
        ),
    (Temporary,
        cb( m_output.temporaries.at(e.idx) );
        ),
    (Argument,
        ASSERT_BUG(sp, e.idx < m_args.size(), "Argument number out of range");
        cb( m_args.at(e.idx).second );
        ),
    (Static,
        TU_MATCHA( (e.m_data), (pe),
        (Generic,
            ASSERT_BUG(sp, pe.m_params.m_types.empty(), "Path params on static");
            const auto& s = m_resolve.m_crate.get_static_by_path(sp, pe.m_path);
            cb( s.m_type );
            ),
        (UfcsKnown,
            TODO(sp, "Static - UfcsKnown - " << e);
            ),
        (UfcsUnknown,
            BUG(sp, "Encountered UfcsUnknown in Static - " << e);
            ),
        (UfcsInherent,
            TODO(sp, "Static - UfcsInherent - " << e);
            )
        )
        ),
    (Return,
        TODO(sp, "Return");
        ),
    (Field,
        with_val_type(sp, *e.val, [&](const auto& ty){
            TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty.m_data), (te),
            (
                BUG(sp, "Field access on unexpected type - " << ty);
                ),
            (Array,
                cb( *te.inner );
                ),
            (Slice,
                cb( *te.inner );
                ),
            (Path,
                ASSERT_BUG(sp, te.binding.is_Struct(), "Field on non-Struct - " << ty);
                const auto& str = *te.binding.as_Struct();
                TU_MATCHA( (str.m_data), (se),
                (Unit,
                    BUG(sp, "Field on unit-like struct - " << ty);
                    ),
                (Tuple,
                    ASSERT_BUG(sp, e.field_index < se.size(),
                        "Field index out of range in tuple-struct " << ty << " - " << e.field_index << " > " << se.size());
                    const auto& fld = se[e.field_index];
                    if( monomorphise_type_needed(fld.ent) ) {
                        auto sty = monomorphise_type(sp, str.m_params, te.path.m_data.as_Generic().m_params, fld.ent);
                        m_resolve.expand_associated_types(sp, sty);
                        cb(sty);
                    }
                    else {
                        cb(fld.ent);
                    }
                    ),
                (Named,
                    ASSERT_BUG(sp, e.field_index < se.size(),
                        "Field index out of range in struct " << ty << " - " << e.field_index << " > " << se.size());
                    const auto& fld = se[e.field_index].second;
                    if( monomorphise_type_needed(fld.ent) ) {
                        auto sty = monomorphise_type(sp, str.m_params, te.path.m_data.as_Generic().m_params, fld.ent);
                        m_resolve.expand_associated_types(sp, sty);
                        cb(sty);
                    }
                    else {
                        cb(fld.ent);
                    }
                    )
                )
                ),
            (Tuple,
                ASSERT_BUG(sp, e.field_index < te.size(), "Field index out of range in tuple " << e.field_index << " >= " << te.size());
                cb( te[e.field_index] );
                )
            )
            });
        ),
    (Deref,
        with_val_type(sp, *e.val, [&](const auto& ty){
            TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty.m_data), (te),
            (
                BUG(sp, "Deref on unexpected type - " << ty);
                ),
            (Path,
                if( const auto* inner_ptr = this->is_type_owned_box(ty) )
                {
                    cb( *inner_ptr );
                }
                else {
                    BUG(sp, "Deref on unexpected type - " << ty);
                }
                ),
            (Pointer,
                cb(*te.inner);
                ),
            (Borrow,
                cb(*te.inner);
                )
            )
            });
        ),
    (Index,
        with_val_type(sp, *e.val, [&](const auto& ty){
            TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty.m_data), (te),
            (
                BUG(sp, "Index on unexpected type - " << ty);
                ),
            (Slice,
                cb(*te.inner);
                ),
            (Array,
                cb(*te.inner);
                )
            )
            });
        ),
    (Downcast,
        with_val_type(sp, *e.val, [&](const auto& ty){
            TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty.m_data), (te),
            (
                BUG(sp, "Downcast on unexpected type - " << ty);
                ),
            (Path,
                //ASSERT_BUG(sp, !te.binding.is_Unbound(), "Unbound path " << ty << " encountered");
                ASSERT_BUG(sp, te.binding.is_Enum(), "Downcast on non-Enum - " << ty << " for " << val);
                const auto& enm = *te.binding.as_Enum();
                const auto& variants = enm.m_variants;
                ASSERT_BUG(sp, e.variant_index < variants.size(), "Variant index out of range");
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
                    ::HIR::TypeRef  tup( mv$(tys) );
                    m_resolve.expand_associated_types(sp, tup);
                    cb(tup);
                    ),
                (Struct,
                    // HACK! Create tuple.
                    ::std::vector< ::HIR::TypeRef>  tys;
                    for(const auto& fld : ve)
                        tys.push_back( monomorphise_type(sp, enm.m_params, te.path.m_data.as_Generic().m_params, fld.second.ent) );
                    ::HIR::TypeRef  tup( mv$(tys) );
                    m_resolve.expand_associated_types(sp, tup);
                    cb(tup);
                    )
                )
                )
            )
            });
        )
    )
}

bool MirBuilder::lvalue_is_copy(const Span& sp, const ::MIR::LValue& val) const
{
    int rv = 0;
    with_val_type(sp, val, [&](const auto& ty){
        DEBUG("[lvalue_is_copy] ty="<<ty);
        rv = (m_resolve.type_is_copy(sp, ty) ? 2 : 1);
        });
    assert(rv != 0);
    return rv == 2;
}

VarState MirBuilder::get_variable_state(const Span& sp, unsigned int idx) const
{
    for( auto scope_idx : ::reverse(m_scope_stack) )
    {
        const auto& scope_def = m_scopes.at(scope_idx);
        TU_MATCH_DEF( ScopeType, (scope_def.data), (e),
        (
            ),
        (Variables,
            auto it = ::std::find(e.vars.begin(), e.vars.end(), idx);
            if( it != e.vars.end() ) {
                // If controlled by this block, exit early (won't find it elsewhere)
                break ;
            }
            ),
        (Split,
            const auto& cur_arm = e.arms.back();
            if( idx < cur_arm.changed_var_states.size() && cur_arm.changed_var_states[idx] )
            {
                assert( idx < cur_arm.var_states.size() );
                return cur_arm.var_states[idx];
            }
            )
        )
    }

    ASSERT_BUG(sp, idx < m_variable_states.size(), "Variable " << idx << " out of range for state table");
    return m_variable_states[idx];
}
void MirBuilder::set_variable_state(const Span& sp, unsigned int idx, VarState state)
{
    for( auto scope_idx : ::reverse(m_scope_stack) )
    {
        auto& scope_def = m_scopes.at(scope_idx);
        if( scope_def.data.is_Variables() )
        {
            const auto& e = scope_def.data.as_Variables();
            auto it = ::std::find(e.vars.begin(), e.vars.end(), idx);
            if( it != e.vars.end() ) {
                break ;
            }
        }
        else if( scope_def.data.is_Split() )
        {
            auto& e = scope_def.data.as_Split();
            auto& cur_arm = e.arms.back();
            if( idx >= cur_arm.changed_var_states.size() ) {
                cur_arm.changed_var_states.resize( idx + 1 );
                cur_arm.var_states.resize( idx + 1 );
            }
            assert( idx < cur_arm.var_states.size() );
            cur_arm.changed_var_states[idx] = true;
            cur_arm.var_states[idx] = state;
            return ;
        }
        else
        {
        }
    }

    ASSERT_BUG(sp, idx < m_variable_states.size(), "Variable " << idx << " out of range for state table");
    m_variable_states[idx] = state;
}
VarState MirBuilder::get_temp_state(const Span& sp, unsigned int idx) const
{
    for( auto scope_idx : ::reverse(m_scope_stack) )
    {
        const auto& scope_def = m_scopes.at(scope_idx);
        if( scope_def.data.is_Temporaries() )
        {
            const auto& e = scope_def.data.as_Temporaries();
            auto it = ::std::find(e.temporaries.begin(), e.temporaries.end(), idx);
            if( it != e.temporaries.end() ) {
                break ;
            }
        }
        else if( scope_def.data.is_Split() )
        {
            const auto& e = scope_def.data.as_Split();
            const auto& cur_arm = e.arms.back();
            if( idx < cur_arm.changed_tmp_states.size() && cur_arm.changed_tmp_states[idx] )
            {
                assert( idx < cur_arm.tmp_states.size() );
                return cur_arm.tmp_states[idx];
            }
        }
    }

    ASSERT_BUG(sp, idx < m_temporary_states.size(), "Temporary " << idx << " out of range for state table");
    return m_temporary_states[idx];
}
void MirBuilder::set_temp_state(const Span& sp, unsigned int idx, VarState state)
{
    for( auto scope_idx : ::reverse(m_scope_stack) )
    {
        auto& scope_def = m_scopes.at(scope_idx);
        if( scope_def.data.is_Temporaries() )
        {
            const auto& e = scope_def.data.as_Temporaries();
            auto it = ::std::find(e.temporaries.begin(), e.temporaries.end(), idx);
            if( it != e.temporaries.end() ) {
                break ;
            }
        }
        else if( scope_def.data.is_Split() )
        {
            auto& e = scope_def.data.as_Split();
            auto& cur_arm = e.arms.back();
            if( idx >= cur_arm.changed_tmp_states.size() ) {
                cur_arm.changed_tmp_states.resize( idx + 1 );
                cur_arm.tmp_states.resize( idx + 1 );
            }
            assert( idx < cur_arm.tmp_states.size() );
            cur_arm.changed_tmp_states[idx] = true;
            cur_arm.tmp_states[idx] = state;
            return ;
        }
    }

    ASSERT_BUG(sp, idx < m_temporary_states.size(), "Temporary " << idx << " out of range for state table");
    m_temporary_states[idx] = state;
}

void MirBuilder::drop_scope_values(const ScopeDef& sd)
{
    TU_MATCHA( (sd.data), (e),
    (Temporaries,
        for(auto tmp_idx : ::reverse(e.temporaries))
        {
            switch( get_temp_state(sd.span, tmp_idx) )
            {
            case VarState::Uninit:
                DEBUG("Temporary " << tmp_idx << " Uninit");
                break;
            case VarState::Dropped:
                DEBUG("Temporary " << tmp_idx << " Dropped");
                break;
            case VarState::Moved:
                DEBUG("Temporary " << tmp_idx << " Moved");
                break;
            case VarState::Init:
                push_stmt_drop( sd.span, ::MIR::LValue::make_Temporary({ tmp_idx }) );
                set_temp_state(sd.span, tmp_idx, VarState::Dropped);
                break;
            case VarState::InnerMoved:
                push_stmt_drop_shallow( sd.span, ::MIR::LValue::make_Temporary({ tmp_idx }) );
                set_temp_state(sd.span, tmp_idx, VarState::Dropped);
                break;
            case VarState::MaybeMoved:
                //BUG(sd.span, "Optionally moved temporary? - " << tmp_idx);
                break;
            }
        }
        ),
    (Variables,
        for(auto var_idx : ::reverse(e.vars))
        {
            switch( get_variable_state(sd.span, var_idx) )
            {
            case VarState::Uninit:
            case VarState::Dropped:
            case VarState::Moved:
                break;
            case VarState::Init:
                push_stmt_drop( sd.span, ::MIR::LValue::make_Variable(var_idx) );
                break;
            case VarState::InnerMoved:
                push_stmt_drop_shallow( sd.span, ::MIR::LValue::make_Variable(var_idx) );
                set_variable_state(sd.span, var_idx, VarState::Dropped);
                break;
            case VarState::MaybeMoved:
                //TODO(sd.span, "Include drop flags");
                // TODO: Drop flags
                break;
            }
        }
        ),
    (Split,
        // No values, controls parent
        ),
    (Loop,
        // No values
        )
    )
}

void MirBuilder::moved_lvalue(const Span& sp, const ::MIR::LValue& lv)
{
    TRACE_FUNCTION_F(lv);
    TU_MATCHA( (lv), (e),
    (Variable,
        if( !lvalue_is_copy(sp, lv) ) {
            set_variable_state(sp, e, VarState::Moved);
        }
        ),
    (Temporary,
        if( !lvalue_is_copy(sp, lv) ) {
            set_temp_state(sp, e.idx, VarState::Moved);
        }
        ),
    (Argument,
        //TODO(sp, "Mark argument as moved");
        ),
    (Static,
        //TODO(sp, "Static - Assert that type is Copy");
        ),
    (Return,
        BUG(sp, "Read of return value");
        ),
    (Field,
        if( lvalue_is_copy(sp, lv) ) {
        }
        else {
            // TODO: Partial moves.
            moved_lvalue(sp, *e.val);
        }
        ),
    (Deref,
        if( lvalue_is_copy(sp, lv) ) {
        }
        else {
            // HACK: If the dereferenced type is a Box ("owned_box") then hack in move and shallow drop
            if( this->m_lang_Box )
            {
                bool is_box = false;
                with_val_type(sp, *e.val, [&](const auto& ty){
                    DEBUG("ty = " << ty);
                    is_box = this->is_type_owned_box(ty);
                    });
                if( is_box )
                {
                    ::MIR::LValue   inner_lv;
                    // 1. If the inner lvalue isn't a slot with move information, move out of the lvalue into a temporary (with standard temp scope)
                    TU_MATCH_DEF( ::MIR::LValue, (*e.val), (ei),
                    (
                        with_val_type(sp, *e.val, [&](const auto& ty){ inner_lv = this->new_temporary(ty); });
                        this->push_stmt_assign(sp, inner_lv.clone(), ::MIR::RValue( mv$(*e.val) ));
                        *e.val = inner_lv.clone();
                        ),
                    (Variable,
                        inner_lv = ::MIR::LValue(ei);
                        ),
                    (Temporary,
                        inner_lv = ::MIR::LValue(ei);
                        ),
                    (Argument,
                        inner_lv = ::MIR::LValue(ei);
                        )
                    )
                    // 2. Mark the slot as requiring only a shallow drop
                    TU_MATCH_DEF( ::MIR::LValue, (inner_lv), (ei),
                    (
                        BUG(sp, "Box move out of invalid LValue " << inner_lv << " - should have been moved");
                        ),
                    (Variable,
                        set_variable_state(sp, ei, VarState::InnerMoved);
                        ),
                    (Temporary,
                        set_temp_state(sp, ei.idx, VarState::InnerMoved);
                        ),
                    (Argument,
                        TODO(sp, "Mark arg " << ei.idx << " for shallow drop");
                        )
                    )
                    // Early return!
                    return ;
                }
            }
            BUG(sp, "Move out of deref with non-Copy values - &move? - " << lv << " : " << FMT_CB(ss, this->with_val_type(sp, lv, [&](const auto& ty){ss<<ty;});) );
            moved_lvalue(sp, *e.val);
        }
        ),
    (Index,
        if( lvalue_is_copy(sp, lv) ) {
        }
        else {
            BUG(sp, "Move out of index with non-Copy values - Partial move?");
            moved_lvalue(sp, *e.val);
        }
        moved_lvalue(sp, *e.idx);
        ),
    (Downcast,
        // TODO: What if the inner is Copy? What if the inner is a hidden pointer?
        moved_lvalue(sp, *e.val);
        )
    )
}

// --------------------------------------------------------------------

ScopeHandle::~ScopeHandle()
{
    if( idx != ~0u )
    {
        try {
            ASSERT_BUG(Span(), m_builder.m_scopes.size() > idx, "Scope invalid");
            ASSERT_BUG(Span(), m_builder.m_scopes.at(idx).complete, "Scope " << idx << " not completed");
        }
        catch(...) {
            abort();
        }
    }
}

::std::ostream& operator<<(::std::ostream& os, VarState x)
{
    switch(x)
    {
    #define _(V)    case VarState::V:   os << #V;   break;
    _(Uninit)
    _(Init)
    _(MaybeMoved)
    _(Moved)
    _(InnerMoved)
    _(Dropped)
    #undef _
    }
    return os;
}
