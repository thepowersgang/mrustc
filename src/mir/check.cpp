/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/check.cpp
 * - MIR Correctness validation
 */
#include "main_bindings.hpp"
#include "mir.hpp"
#include <hir/visitor.hpp>
#include <hir_typeck/static.hpp>

namespace {
    const unsigned int STMT_TERM = ~0u;
    
    #define MIR_BUG(state, ...) ( (state).print_bug( [&](auto& _os){_os << __VA_ARGS__; } ) )
    #define MIR_ASSERT(state, cnd, ...) do { if( !(cnd) ) (state).print_bug( [&](auto& _os){_os << "ASSERT " #cnd " failed - " << __VA_ARGS__; } ); } while(0)
    #define MIR_TODO(state, ...) ( (state).print_todo( [&](auto& _os){_os << __VA_ARGS__; } ) )
    
    struct MirCheckFailure {
    };
    
    struct State {
        typedef ::std::vector< ::std::pair< ::HIR::Pattern, ::HIR::TypeRef> >   t_args;
        
        const StaticTraitResolve& resolve;
        const ::HIR::ItemPath& path;
        const ::MIR::Function& fcn;
        const t_args&   args;
        const ::HIR::TypeRef&   ret_type;
        
        unsigned int bb_idx = 0;
        unsigned int stmt_idx = 0;
        const ::HIR::SimplePath*    m_lang_Box;
        
        Span    sp;
        
        State(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, const ::MIR::Function& fcn, const t_args& args, const ::HIR::TypeRef& ret_type):
            resolve(resolve),
            path(path),
            fcn(fcn),
            args(args),
            ret_type(ret_type)
        {
            if( resolve.m_crate.m_lang_items.count("owned_box") > 0 ) {
                m_lang_Box = &resolve.m_crate.m_lang_items.at("owned_box");
            }
        }
        
        void set_cur_stmt(unsigned int bb_idx, unsigned int stmt_idx) {
            this->bb_idx = bb_idx;
            this->stmt_idx = stmt_idx;
        }
        void set_cur_stmt_term(unsigned int bb_idx) {
            this->bb_idx = bb_idx;
            this->stmt_idx = STMT_TERM;
        }
        
        void print_bug(::std::function<void(::std::ostream& os)> cb) const {
            print_msg("ERROR", cb);
        }
        void print_todo(::std::function<void(::std::ostream& os)> cb) const {
            print_msg("TODO", cb);
        }
        void print_msg(const char* tag, ::std::function<void(::std::ostream& os)> cb) const
        {
            auto& os = ::std::cerr;
            os << "MIR " << tag << ": " << this->path << " BB" << this->bb_idx << "/";
            if( this->stmt_idx == STMT_TERM ) {
                os << "TERM";
            }
            else {
                os << this->stmt_idx;
            }
            os << ": ";
            cb(os);
            os << ::std::endl;
            // TODO: Throw something? Or mark an error so the rest of the function can be checked.
            throw MirCheckFailure {};
        }
        
        const ::HIR::TypeRef& get_lvalue_type(::HIR::TypeRef& tmp, const ::MIR::LValue& val) const
        {
            TU_MATCH(::MIR::LValue, (val), (e),
            (Variable,
                return this->fcn.named_variables.at(e);
                ),
            (Temporary,
                return this->fcn.temporaries.at(e.idx);
                ),
            (Argument,
                return this->args.at(e.idx).second;
                ),
            (Static,
                TU_MATCHA( (e.m_data), (pe),
                (Generic,
                    MIR_ASSERT(*this, pe.m_params.m_types.empty(), "Path params on static");
                    const auto& s = resolve.m_crate.get_static_by_path(sp, pe.m_path);
                    return s.m_type;
                    ),
                (UfcsKnown,
                    MIR_TODO(*this, "LValue::Static - UfcsKnown - " << e);
                    ),
                (UfcsUnknown,
                    MIR_BUG(*this, "Encountered UfcsUnknown in LValue::Static - " << e);
                    ),
                (UfcsInherent,
                    MIR_TODO(*this, "LValue::Static - UfcsInherent - " << e);
                    )
                )
                ),
            (Return,
                return this->ret_type;
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
                    TU_MATCHA( (str.m_data), (se),
                    (Unit,
                        MIR_BUG(*this, "Field on unit-like struct - " << ty);
                        ),
                    (Tuple,
                        MIR_ASSERT(*this, e.field_index < se.size(), "Field index out of range in tuple-struct");
                        const auto& fld = se[e.field_index];
                        if( monomorphise_type_needed(fld.ent) ) {
                            tmp = monomorphise_type(sp, str.m_params, te.path.m_data.as_Generic().m_params, fld.ent);
                            this->resolve.expand_associated_types(sp, tmp);
                            return tmp;
                        }
                        else {
                            return fld.ent;
                        }
                        ),
                    (Named,
                        MIR_ASSERT(*this, e.field_index < se.size(), "Field index out of range in struct");
                        const auto& fld = se[e.field_index].second;
                        if( monomorphise_type_needed(fld.ent) ) {
                            tmp = monomorphise_type(sp, str.m_params, te.path.m_data.as_Generic().m_params, fld.ent);
                            this->resolve.expand_associated_types(sp, tmp);
                            return tmp;
                        }
                        else {
                            return fld.ent;
                        }
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
                    MIR_ASSERT(*this, te.binding.is_Enum(), "Downcast on non-Enum");
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
                        return tmp;
                        ),
                    (Struct,
                        // HACK! Create tuple.
                        ::std::vector< ::HIR::TypeRef>  tys;
                        for(const auto& fld : ve)
                            tys.push_back( monomorphise_type(sp, enm.m_params, te.path.m_data.as_Generic().m_params, fld.second.ent) );
                        tmp = ::HIR::TypeRef( mv$(tys) );
                        return tmp;
                        )
                    )
                    )
                )
                )
            )
            throw "";
        }
        
        const ::HIR::TypeRef* is_type_owned_box(const ::HIR::TypeRef& ty) const
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
    };
}

void MIR_Validate(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, const ::MIR::Function& fcn, const State::t_args& args, const ::HIR::TypeRef& ret_type)
{
    Span    sp;
    State   state { resolve, path, fcn, args, ret_type };
    // Validation rules:

    // [CFA] = Control Flow Analysis
    // - [CFA] All code paths from bb0 must end with either a return or a diverge (or loop)
    //  - Requires checking the links between basic blocks, with a bitmap to catch loops/multipath
    {
        bool returns = false;
        ::std::vector<bool> visited_bbs( fcn.blocks.size() );
        ::std::vector<unsigned int> to_visit_blocks;
        to_visit_blocks.push_back(0);
        while( to_visit_blocks.size() > 0 )
        {
            auto block = to_visit_blocks.back();
            to_visit_blocks.pop_back();
            assert(block < fcn.blocks.size());
            if( visited_bbs[block] ) {
                continue ;
            }
            visited_bbs[block] = true;
            
            
            state.set_cur_stmt_term(block);
            
            #define PUSH_BB(idx, desc)  do {\
                if( !(idx < fcn.blocks.size() ) )   MIR_BUG(state,  "Invalid target block - " << desc << " bb" << idx);\
                if( visited_bbs[idx] == false ) {\
                    to_visit_blocks.push_back(idx); \
                }\
                } while(0)
            TU_MATCH(::MIR::Terminator, (fcn.blocks[block].terminator), (e),
            (Incomplete,
                MIR_BUG(state,  "Encounterd `Incomplete` block in control flow");
                ),
            (Return,
                returns = true;
                ),
            (Diverge,
                //can_panic = true;
                ),
            (Goto,
                PUSH_BB(e, "Goto");
                ),
            (Panic,
                PUSH_BB(e.dst, "Panic");
                ),
            (If,
                PUSH_BB(e.bb0, "If true");
                PUSH_BB(e.bb1, "If false");
                ),
            (Switch,
                for(unsigned int i = 0; i < e.targets.size(); i++ ) {
                    PUSH_BB(e.targets[i], "Switch V" << i);
                }
                ),
            (Call,
                PUSH_BB(e.ret_block, "Call ret");
                PUSH_BB(e.panic_block, "Call panic");
                )
            )
            #undef PUSH_BB
        }
        if( !returns ) {
            DEBUG("- Function doesn't return.");
        }
    }
    
    // [ValState] = Value state tracking (use after move, uninit, ...)
    // - [ValState] No drops or usage of uninitalised values (Uninit, Moved, or Dropped)
    // - [ValState] Temporaries are write-once.
    //  - Requires maintaining state information for all variables/temporaries with support for loops
    //  - 

    // [Flat] = Basic checks (just iterates BBs)
    // - [Flat] Types must be valid (correct type for slot etc.)
    //  - Simple check of all assignments/calls/...
    {
        for(unsigned int bb_idx = 0; bb_idx < fcn.blocks.size(); bb_idx ++)
        {
            const auto& bb = fcn.blocks[bb_idx];
            for(unsigned int stmt_idx = 0; stmt_idx < bb.statements.size(); stmt_idx ++)
            {
                const auto& stmt = bb.statements[stmt_idx];
                state.set_cur_stmt(bb_idx, stmt_idx);
                
                switch( stmt.tag() )
                {
                case ::MIR::Statement::TAGDEAD:
                    throw "";
                case ::MIR::Statement::TAG_Assign: {
                    const auto& a = stmt.as_Assign();
                    
                    auto check_type = [&](const auto& src_ty) {
                        ::HIR::TypeRef  tmp;
                        const auto& dst_ty = state.get_lvalue_type(tmp, a.dst);
                        if( src_ty == ::HIR::TypeRef::new_diverge() ) {
                        }
                        else if( src_ty == dst_ty ) {
                        }
                        else {
                            MIR_BUG(state,  "Type mismatch, destination is " << dst_ty << ", source is " << src_ty);
                        }
                        };
                    TU_MATCH(::MIR::RValue, (a.src), (e),
                    (Use,
                        ::HIR::TypeRef  tmp;
                        check_type( state.get_lvalue_type(tmp, e) );
                        ),
                    (Constant,
                        // TODO: Check constant types.
                        ::HIR::TypeRef  tmp;
                        const auto& dst_ty = state.get_lvalue_type(tmp, a.dst);
                        TU_MATCH( ::MIR::Constant, (e), (c),
                        (Int,
                            bool good = false;
                            if( dst_ty.m_data.is_Primitive() ) {
                                switch( dst_ty.m_data.as_Primitive() ) {
                                case ::HIR::CoreType::I8:
                                case ::HIR::CoreType::I16:
                                case ::HIR::CoreType::I32:
                                case ::HIR::CoreType::I64:
                                case ::HIR::CoreType::Isize:
                                    good = true;
                                    break;
                                default:
                                    break;
                                }
                            }
                            if( !good ) {
                                MIR_BUG(state,  "Type mismatch, destination is " << dst_ty << ", source is a signed integer");
                            }
                            ),
                        (Uint,
                            bool good = false;
                            if( dst_ty.m_data.is_Primitive() ) {
                                switch( dst_ty.m_data.as_Primitive() ) {
                                case ::HIR::CoreType::U8:
                                case ::HIR::CoreType::U16:
                                case ::HIR::CoreType::U32:
                                case ::HIR::CoreType::U64:
                                case ::HIR::CoreType::Usize:
                                case ::HIR::CoreType::Char:
                                    good = true;
                                    break;
                                default:
                                    break;
                                }
                            }
                            if( !good ) {
                                MIR_BUG(state,  "Type mismatch, destination is " << dst_ty << ", source is an unsigned integer");
                            }
                            ),
                        (Float,
                            bool good = false;
                            if( dst_ty.m_data.is_Primitive() ) {
                                switch( dst_ty.m_data.as_Primitive() ) {
                                case ::HIR::CoreType::F32:
                                case ::HIR::CoreType::F64:
                                    good = true;
                                    break;
                                default:
                                    break;
                                }
                            }
                            if( !good ) {
                                MIR_BUG(state,  "Type mismatch, destination is " << dst_ty << ", source is a floating point value");
                            }
                            ),
                        (Bool,
                            check_type( ::HIR::TypeRef(::HIR::CoreType::Bool) );
                            ),
                        (Bytes,
                            // TODO: Check result (could be either &[u8; N] or &[u8])
                            ),
                        (StaticString,
                            check_type( ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, ::HIR::CoreType::Str) );
                            ),
                        (Const,
                            // TODO: Check result type against type of const
                            ),
                        (ItemAddr,
                            // TODO: Check result type against pointer to item type
                            )
                        )
                        ),
                    (SizedArray,
                        // TODO: Check that return type is an array
                        // TODO: Check that the input type is Copy
                        ),
                    (Borrow,
                        // TODO: Check return type
                        ),
                    (Cast,
                        // TODO: Check return type
                        // TODO: Check suitability of source type (COMPLEX)
                        ),
                    (BinOp,
                        ::HIR::TypeRef  tmp_l, tmp_r;
                        const auto& ty_l = state.get_lvalue_type(tmp_l, e.val_l);
                        const auto& ty_r = state.get_lvalue_type(tmp_r, e.val_r);
                        // TODO: Check that operation is valid on these types
                        switch( e.op )
                        {
                        case ::MIR::eBinOp::BIT_SHR:
                        case ::MIR::eBinOp::BIT_SHL:
                            break;
                        default:
                            // Check argument types are equal
                            if( ty_l != ty_r )
                                MIR_BUG(state, "Type mismatch in binop, " << ty_l << " != " << ty_r);
                        }
                        // TODO: Check return type
                        ),
                    (UniOp,
                        // TODO: Check that operation is valid on this type
                        // TODO: Check return type
                        ),
                    (DstMeta,
                        // TODO: Ensure that the input type is a: Generic, Array, or DST
                        // TODO: Check return type
                        ),
                    (MakeDst,
                        ),
                    (Tuple,
                        // TODO: Check return type
                        ),
                    (Array,
                        // TODO: Check return type
                        ),
                    (Struct,
                        // TODO: Check return type
                        )
                    )
                    } break;
                case ::MIR::Statement::TAG_Drop:
                    // TODO: Anything need checking here?
                    break;
                }
            }
            
            state.set_cur_stmt_term(bb_idx);
            TU_MATCH(::MIR::Terminator, (bb.terminator), (e),
            (Incomplete,
                ),
            (Return,
                // TODO: Check if the function can return (i.e. if its return type isn't an empty type)
                ),
            (Diverge,
                ),
            (Goto,
                ),
            (Panic,
                ),
            (If,
                // Check that condition lvalue is a bool
                ::HIR::TypeRef  tmp;
                const auto& ty = state.get_lvalue_type(tmp, e.cond);
                if( ty != ::HIR::CoreType::Bool ) {
                    MIR_BUG(state, "Type mismatch in `If` - expected bool, got " << ty);
                }
                ),
            (Switch,
                // Check that the condition is an enum
                ),
            (Call,
                // Typecheck arguments and return value
                )
            )
        }
    }
}

namespace {
    // TODO: Create visitor that handles setting up a StaticTraitResolve?
    class OuterVisitor:
        public ::HIR::Visitor
    {
        StaticTraitResolve  m_resolve;
    public:
        OuterVisitor(const ::HIR::Crate& crate):
            m_resolve(crate)
        {}
        
        // NOTE: This is left here to ensure that any expressions that aren't handled by higher code cause a failure
        void visit_expr(::HIR::ExprPtr& exp) override {
            BUG(Span(), "visit_expr hit in OuterVisitor");
        }
        
        void visit_type(::HIR::TypeRef& ty) override
        {
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
                this->visit_type( *e.inner );
                DEBUG("Array size " << ty);
                if( e.size ) {
                    MIR_Validate(m_resolve, ::HIR::ItemPath(), *e.size.m_mir, {}, ::HIR::TypeRef(::HIR::CoreType::Usize));
                }
            )
            else {
                ::HIR::Visitor::visit_type(ty);
            }
        }

        // ------
        // Code-containing items
        // ------
        void visit_function(::HIR::ItemPath p, ::HIR::Function& item) override {
            auto _ = this->m_resolve.set_item_generics(item.m_params);
            if( item.m_code ) {
                DEBUG("Function code " << p);
                MIR_Validate(m_resolve, p, *item.m_code.m_mir, item.m_args, item.m_return);
            }
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override {
            if( item.m_value ) {
                DEBUG("`static` value " << p);
                MIR_Validate(m_resolve, p, *item.m_value.m_mir, {}, item.m_type);
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override {
            if( item.m_value ) {
                DEBUG("`const` value " << p);
                MIR_Validate(m_resolve, p, *item.m_value.m_mir, {}, item.m_type);
            }
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            auto _ = this->m_resolve.set_item_generics(item.m_params);
            
            // TODO: Use a different type depding on repr()
            auto enum_type = ::HIR::TypeRef(::HIR::CoreType::Isize);
            
            for(auto& var : item.m_variants)
            {
                TU_IFLET(::HIR::Enum::Variant, var.second, Value, e,
                    // TODO: Get the repr type
                    MIR_Validate(m_resolve, p + var.first, *e.expr.m_mir, {}, enum_type);
                )
            }
        }
        
        // Boilerplate
        void visit_trait(::HIR::ItemPath p, ::HIR::Trait& item) override {
            auto _ = this->m_resolve.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_trait(p, item);
        }
        void visit_type_impl(::HIR::TypeImpl& impl) override {
            auto _ = this->m_resolve.set_impl_generics(impl.m_params);
            ::HIR::Visitor::visit_type_impl(impl);
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override {
            auto _ = this->m_resolve.set_impl_generics(impl.m_params);
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
        }
    };
}

// --------------------------------------------------------------------

void MIR_CheckCrate(/*const*/ ::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}
