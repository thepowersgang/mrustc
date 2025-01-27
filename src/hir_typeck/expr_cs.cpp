/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/expr_cs.cpp
 * - Constraint Solver type inferrence
 */
#include "main_bindings.hpp"
#include <hir/expr.hpp>
#include <hir/hir.hpp>
#include <hir/visitor.hpp>
#include <algorithm>    // std::find_if

#include <hir_typeck/static.hpp>
#include "helpers.hpp"
#include "expr_visit.hpp"
#include "expr_cs.hpp"
#include "hir_conv/main_bindings.hpp"

namespace {
    inline HIR::ExprNodeP mk_exprnodep(HIR::ExprNode* en, ::HIR::TypeRef ty){ en->m_res_type = mv$(ty); return HIR::ExprNodeP(en); }

    inline ::HIR::SimplePath get_parent_path(const ::HIR::SimplePath& sp) {
        return sp.parent();
    }
    inline ::HIR::GenericPath get_parent_path(const ::HIR::GenericPath& gp) {
        return ::HIR::GenericPath( get_parent_path(gp.m_path), gp.m_params.clone() );
    }

    bool type_contains_impl_placeholder(const ::HIR::TypeRef& t) {
        struct HasPlaceholder {};
        struct V: public HIR::Visitor {
            void visit_constgeneric(const HIR::ConstGeneric& v) {
                if( v.is_Generic() && v.as_Generic().is_placeholder() ) {
                    throw HasPlaceholder {};
                }
            }
            void visit_path_params(HIR::PathParams& pp) override {
                for(const auto& v : pp.m_values) {
                    visit_constgeneric(v);
                }
                HIR::Visitor::visit_path_params(pp);
            }
            void visit_type(HIR::TypeRef& ty) override {
                if( ty.data().is_Generic() && ty.data().as_Generic().is_placeholder() ) {
                    throw HasPlaceholder {};
                }
                if(const auto* e = ty.data().opt_Array() ) {
                    if( const auto* ase = e->size.opt_Unevaluated() ) {
                        visit_constgeneric(*ase);
                    }
                }
                HIR::Visitor::visit_type(ty);
            }
        } v;
        try {
            v.visit_type(const_cast<HIR::TypeRef&>(t));
            return false;
        }
        catch(const HasPlaceholder& ) {
            return true;
        }
    }

    struct MonomorphEraseHrls: public Monomorphiser {
        ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& g) const override {
            return ::HIR::TypeRef(g);
        }
        ::HIR::ConstGeneric get_value(const Span& sp, const ::HIR::GenericRef& g) const override {
            return g;
        }
        ::HIR::LifetimeRef get_lifetime(const Span& sp, const ::HIR::GenericRef& g) const override {
            if( g.group() == 3 ) {
                return HIR::LifetimeRef();
            } else {
                return HIR::LifetimeRef(g.binding);
            }
        }
    };
}
#define NEWNODE(TY, SP, CLASS, ...)  mk_exprnodep(new HIR::ExprNode##CLASS(SP ,## __VA_ARGS__), TY)

namespace {

    // -----------------------------------------------------------------------
    // Revisit Class
    //
    // Handles visiting nodes during inferrence passes
    // -----------------------------------------------------------------------
    // TODO: Convert these to `Revisitor` instances
    class ExprVisitor_Revisit:
        public ::HIR::ExprVisitor
    {
        Context& context;
        bool m_completed;
        /// Tells vistors that inferrence has stalled, and that they can take
        /// more extreme actions (e.g. ignoring an ambigious inherent method
        /// and using a trait method instead)
        bool m_is_fallback;
    public:
        ExprVisitor_Revisit(Context& context, bool fallback=false):
            context(context),
            m_completed(false),
            m_is_fallback(fallback)
        {}

        bool node_completed() const {
            return m_completed;
        }

        void visit(::HIR::ExprNode_Block& node) override {

            const auto is_diverge = [&](const ::HIR::TypeRef& rty)->bool {
                const auto& ty = this->context.get_type(rty);
                // TODO: Search the entire type for `!`? (What about pointers to it? or Option/Result?)
                // - A correct search will search for unconditional (ignoring enums with a non-! variant) non-rawptr instances of ! in the type
                return ty.data().is_Diverge();
                };

            assert( !node.m_nodes.empty() );
            const auto& last_ty = this->context.get_type( node.m_nodes.back()->m_res_type );
            DEBUG("_Block: last_ty = " << last_ty);

            bool diverges = false;
            // NOTE: If the final statement in the block diverges, mark this as diverging
            if(const auto* e = last_ty.data().opt_Infer())
            {
                switch(e->ty_class)
                {
                case ::HIR::InferClass::Integer:
                case ::HIR::InferClass::Float:
                    diverges = false;
                    break;
                default:
                    this->context.possible_equate_type_unknown(node.span(), node.m_res_type, Context::IvarUnknownType::From);
                    return ;
                }
            }
            else if( is_diverge(last_ty) ) {
                diverges = true;
            }
            else {
                diverges = false;
            }
            // If a statement in this block diverges
            if( diverges ) {
                DEBUG("_Block: diverges, yield !");
                this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_diverge());
            }
            else {
                DEBUG("_Block: doesn't diverge but doesn't yield a value, yield ()");
                this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
            }
            this->m_completed = true;
        }
        void visit(::HIR::ExprNode_ConstBlock& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Asm& node) override {
            // TODO: Revisit for validation
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Asm2& node) override {
            // TODO: Revisit for validation
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Return& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Yield& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Let& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Loop& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_LoopControl& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Match& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_If& node) override {
            no_revisit(node);
        }

        void visit(::HIR::ExprNode_Assign& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_BinOp& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_UniOp& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Borrow& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_RawBorrow& node) override {
            no_revisit(node);
        }
        void bad_cast(const Span& sp, const ::HIR::TypeRef& src_ty, const ::HIR::TypeRef& tgt_ty, const char* where) {
            ERROR(sp, E0000, "Invalid cast [" << where << "]:\n"
                << "from " << this->context.m_ivars.fmt_type(src_ty) << "\n"
                << " to  " << this->context.m_ivars.fmt_type(tgt_ty));
        }
        void visit(::HIR::ExprNode_Cast& node) override {
            const auto& sp = node.span();
            const auto& tgt_ty = this->context.get_type(node.m_res_type);
            const auto& src_ty = this->context.get_type(node.m_value->m_res_type);
            TRACE_FUNCTION_F(src_ty << " as " << tgt_ty);

            if( this->context.m_ivars.types_equal(src_ty, tgt_ty) ) {
                this->m_completed = true;
                return ;
            }

            TU_MATCH_HDRA( (tgt_ty.data()), {)
            TU_ARMA(Infer, e) {
                // Can't know anything
                //this->m_completed = true;
                DEBUG("- Target type is still _");
                }
            TU_ARMA(Diverge, e) {
                BUG(sp, "");
                }
            TU_ARMA(Primitive, e) {
                // Don't have anything to contribute
                // EXCEPT: `char` can only be casted from `u8` (but what about no-op casts?)
                // - Hint the input (default) to be `u8`
                if( e == ::HIR::CoreType::Char )
                {
                    if(this->m_is_fallback)
                    {
                        this->context.equate_types(sp, src_ty, ::HIR::CoreType::U8);
                        this->m_completed = true;
                    }
                    else if( !this->context.get_type(src_ty).data().is_Infer() )
                    {
                        this->m_completed = true;
                    }
                    else
                    {
                        // Not fallback, and still infer - not complete
                    }
                }
                else
                {
                    this->m_completed = true;
                }
                }
            TU_ARMA(Path, e) {
                this->context.equate_types_coerce(sp, tgt_ty, node.m_value);
                this->m_completed = true;
                return ;
                }
            TU_ARMA(Generic, e) {
                TODO(sp, "_Cast Generic");
                }
            TU_ARMA(TraitObject, e) {
                bad_cast(sp, src_ty, tgt_ty, "dst");
                }
            TU_ARMA(ErasedType, e) {
                bad_cast(sp, src_ty, tgt_ty, "dst");
                }
            TU_ARMA(Array, e) {
                bad_cast(sp, src_ty, tgt_ty, "dst");
                }
            TU_ARMA(Slice, e) {
                bad_cast(sp, src_ty, tgt_ty, "dst");
                }
            TU_ARMA(Tuple, e) {
                bad_cast(sp, src_ty, tgt_ty, "dst");
                }
            TU_ARMA(Borrow, e) {
                // Emit a coercion and delete this revisit
                this->context.equate_types_coerce(sp, tgt_ty, node.m_value);
                this->m_completed = true;
                return ;
                }
            TU_ARMA(Pointer, e) {
                const auto& ity = this->context.get_type(e.inner);
                TU_MATCH_HDRA( (src_ty.data()), {)
                default:
                    ERROR(sp, E0000, "Invalid cast to pointer from " << src_ty);
                case ::HIR::TypeData::TAG_Function:
                case ::HIR::TypeData::TAG_NamedFunction:
                    // TODO: What is the valid set? *const () and *const u8 at least are allowed
                    if( ity == ::HIR::TypeRef::new_unit() || ity == ::HIR::CoreType::U8 || ity == ::HIR::CoreType::I8 ) {
                        this->m_completed = true;
                    }
                    else if( ity.data().is_Infer() ) {
                        // Keep around.
                    }
                    else {
                        //ERROR(sp, E0000, "Invalid cast to " << this->context.m_ivars.fmt_type(tgt_ty) << " from " << src_ty);
                        // TODO: Only allow thin pointers? `c_void` is used in 1.74 libstd
                        this->m_completed = true;
                    }
                TU_ARMA(Primitive, s_e) {
                    switch(s_e)
                    {
                    case ::HIR::CoreType::Bool:
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::F32:
                    case ::HIR::CoreType::F64:
                        ERROR(sp, E0000, "Invalid cast to pointer from " << src_ty);
                    default:
                        break;
                    }
                    // NOTE: Can't be to a fat pointer though - This is checked by the later pass (once all types are known and thus sized-ness is known)
                    this->m_completed = true;
                    }
                TU_ARMA(Infer, s_e) {
                    switch( s_e.ty_class )
                    {
                    case ::HIR::InferClass::Float:
                        ERROR(sp, E0000, "Invalid cast to pointer from floating point literal");
                    case ::HIR::InferClass::Integer:
                        this->context.equate_types(sp, src_ty, ::HIR::TypeRef(::HIR::CoreType::Usize));
                        this->m_completed = true;
                        break;
                    case ::HIR::InferClass::None:
                        break;
                    }
                    }
                TU_ARMA(Borrow, s_e) {
                    // Check class (destination must be weaker) and type
                    if( !(s_e.type >= e.type) ) {
                        ERROR(sp, E0000, "Invalid cast from " << src_ty << " to " << tgt_ty);
                    }
                    const auto& src_inner = this->context.get_type(s_e.inner);

                    // This is also a semi-coercion point, so need to run the same sort of rules

                    // If either the source or the destination inner tyes are infer - add back rules
                    if( const auto* s_e_i = src_inner.data().opt_Infer() )
                    {
                        this->context.possible_equate_ivar(sp, s_e_i->index, ity, Context::PossibleTypeSource::UnsizeTo);
                    }
                    if( const auto* d_e_i = ity.data().opt_Infer() )
                    {
                        this->context.possible_equate_ivar(sp, d_e_i->index, s_e.inner, Context::PossibleTypeSource::UnsizeFrom);
                    }

                    // If this looks like `&mut [?; N]` -> `*mut ?` then do a possible equate between the two types
                    if( src_inner.data().is_Array() ) {
                        if( const auto* s_e_i = this->context.get_type(src_inner.data().as_Array().inner).data().opt_Infer() ) {
                            this->context.possible_equate_ivar(sp, s_e_i->index, ity, Context::PossibleTypeSource::UnsizeTo);
                            return ;
                        }
                    }

                    // NOTE: &mut T -> *mut U where T: Unsize<U> is allowed
                    // TODO: Wouldn't this be better served by a coercion point?

                    if( src_inner.data().is_Infer() || ity.data().is_Infer() )
                    {
                        // Either side is infer, keep going
                    }
                    // - NOTE: Crude, and likely to break if ether inner isn't known.
                    else if( src_inner.data().is_Array() && src_inner.data().as_Array().inner == ity )
                    {
                        // Allow &[T; n] -> *const T - Convert into two casts
                        auto ty = ::HIR::TypeRef::new_pointer(e.type, src_inner.clone());
                        node.m_value = NEWNODE(ty.clone(), sp, _Cast, mv$(node.m_value), ty.clone());
                        this->m_completed = true;
                    }
                    else
                    {
                        bool found = this->context.m_resolve.find_trait_impls(sp, this->context.m_resolve.m_lang_Unsize, ::HIR::PathParams(e.inner.clone()), s_e.inner, [](auto , auto){ return true; });
                        if( found ) {
                            auto ty = ::HIR::TypeRef::new_borrow(e.type, e.inner.clone());
                            node.m_value = NEWNODE(ty.clone(), sp, _Unsize, mv$(node.m_value), ty.clone());
                            this->context.add_trait_bound(sp, s_e.inner, this->context.m_resolve.m_lang_Unsize, ::HIR::PathParams(e.inner.clone()));
                        }
                        else {
                            this->context.equate_types(sp, e.inner, s_e.inner);
                        }
                        this->m_completed = true;
                    }
                    }
                TU_ARMA(Pointer, s_e) {
                    // Allow with no link?
                    // TODO: In some rare cases, this ivar could be completely
                    // unrestricted. If in fallback mode
                    const auto& dst_inner = this->context.get_type(e.inner);
                    const auto& src_inner = this->context.get_type(s_e.inner);
                    if( dst_inner.data().is_Infer() )
                    {
                        // NOTE: Don't equate on fallback, to avoid cast chains breaking
                        // - Instead, leave the bounds present (which will hopefully get used by ivar_poss)
                        ::std::vector<HIR::TypeRef>   tys;
                        tys.push_back( dst_inner.clone() );
                        tys.push_back( src_inner.clone() );
                        this->context.possible_equate_ivar_bounds(sp, dst_inner.data().as_Infer().index, std::move(tys));
                        return ;
                    }
                    else if( src_inner.data().is_Infer() )
                    {
                        if( !this->m_is_fallback )
                        {
                            ::std::vector<HIR::TypeRef>   tys;
                            tys.push_back( dst_inner.clone() );
                            tys.push_back( src_inner.clone() );
                            this->context.possible_equate_ivar_bounds(sp, src_inner.data().as_Infer().index, std::move(tys));
                            return ;
                        }
                        this->context.equate_types(sp, dst_inner, src_inner);
                    }
                    else
                    {
                    }
                    this->m_completed = true;
                    }
                }
                }
            TU_ARMA(Function, e) {
                // NOTE: Valid if it's causing a fn item -> fn pointer coercion
                TU_MATCH_HDRA( (src_ty.data()), {)
                default:
                    bad_cast(sp, src_ty, tgt_ty, "fcn src");
                TU_ARMA(Closure, s_e) {
                    auto pp = e.hrls.make_empty_params(true);
                    auto ms = MonomorphHrlsOnly(pp);
                    // Valid cast here, downstream code will check if its a non-capturing closure
                    if( s_e.node->m_args.size() != e.m_arg_types.size() )
                        bad_cast(sp, src_ty, tgt_ty, "fcn nargs");
                    this->context.equate_types(sp, ms.monomorph_type(sp, e.m_rettype), s_e.node->m_return);
                    for(size_t i = 0; i < e.m_arg_types.size(); i++)
                        this->context.equate_types(sp, ms.monomorph_type(sp, e.m_arg_types[i]), s_e.node->m_args[i].second);
                    this->m_completed = true;
                    }
                TU_ARMA(Function, s_e) {
                    // Check that the ABI and unsafety is correct
                    if( s_e.m_abi != e.m_abi || (s_e.is_unsafe && s_e.is_unsafe != e.is_unsafe) || s_e.m_arg_types.size() != e.m_arg_types.size() )
                        bad_cast(sp, src_ty, tgt_ty, "fcn nargs");
                    this->context.equate_types(sp, tgt_ty, src_ty);
                    this->m_completed = true;
                    }
                TU_ARMA(NamedFunction, f) {
                    auto ft = context.m_resolve.expand_associated_types(sp, HIR::TypeRef(f.decay(sp)));
                    const auto& s_e = ft.data().as_Function();
                    // Check that the ABI and unsafety is correct
                    if( s_e.m_abi != e.m_abi || (s_e.is_unsafe && s_e.is_unsafe != e.is_unsafe) || s_e.m_arg_types.size() != e.m_arg_types.size() )
                        bad_cast(sp, src_ty, tgt_ty, "fcn nargs");

                    this->context.equate_types(sp, e.m_rettype, s_e.m_rettype);
                    for(size_t i = 0; i < e.m_arg_types.size(); i++)
                        this->context.equate_types(sp, e.m_arg_types[i], s_e.m_arg_types[i]);
                    this->m_completed = true;
                    }
                }
                }
            TU_ARMA(NamedFunction, e) {
                BUG(sp, "Attempting to cast to a named-function type - impossible");
                }
            TU_ARMA(Closure, e) {
                BUG(sp, "Attempting to cast to a closure type - impossible");
                }
            TU_ARMA(Generator, e) {
                BUG(sp, "Attempting to cast to a generator type - impossible");
                }
            }
        }
        void visit(::HIR::ExprNode_Unsize& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Index& node) override {
            const auto& lang_Index = this->context.m_crate.get_lang_item_path(node.span(), "index");    // TODO: Pre-load
            const auto& val_ty = this->context.get_type(node.m_value->m_res_type);
            //const auto& idx_ty = this->context.get_type(node.m_index->m_res_type);
            const auto& idx_ty = this->context.get_type(node.m_cache.index_ty);
            TRACE_FUNCTION_F(&node << " Index: val=" << val_ty << ", idx=" << idx_ty << "");

            this->context.possible_equate_type_unknown(node.span(), node.m_res_type, Context::IvarUnknownType::From);

            // NOTE: Indexing triggers autoderef
            unsigned int deref_count = 0;
            ::HIR::TypeRef  tmp_type;   // Temporary type used for handling Deref
            const auto* current_ty = &node.m_value->m_res_type;
            ::std::vector< ::HIR::TypeRef>  deref_res_types;

            // TODO: (CHECK) rustc doesn't use the index value type when finding the indexable item, mrustc does.
            ::HIR::PathParams   trait_pp;
            trait_pp.m_types.push_back( idx_ty.clone() );
            do {
                const auto& ty = this->context.get_type(*current_ty);
                DEBUG("(Index): (: " << ty << ")[: " << trait_pp.m_types[0] << "]");
                if( ty.data().is_Infer() ) {
                    return ;
                }

                ::HIR::TypeRef  possible_index_type;
                ::HIR::TypeRef  possible_res_type;
                unsigned int count = 0;
                bool rv = this->context.m_resolve.find_trait_impls(node.span(), lang_Index, trait_pp, ty, [&](auto impl, auto cmp) {
                    DEBUG("[visit(_Index)] cmp=" << cmp << " - " << impl);
                    possible_res_type = impl.get_type("Output", {});
                    count += 1;
                    if( cmp == ::HIR::Compare::Equal ) {
                        return true;
                    }
                    possible_index_type = impl.get_trait_ty_param(0);
                    return false;
                    });
                if( rv ) {
                    // If a non-fuzzy impl was found, but there was no result type - then the result must be opaque
                    if( possible_res_type == HIR::TypeRef() ) {
                        possible_res_type = ::HIR::TypeRef::new_path(
                            ::HIR::Path(ty.clone(), HIR::GenericPath(lang_Index, mv$(trait_pp)), "Output"),
                            HIR::TypePathBinding::make_Opaque({})
                            );
                    }
                    // TODO: Node's result type could be an &-ptr?
                    this->context.equate_types(node.span(), node.m_res_type,  possible_res_type);
                    break;
                }
                else if( count == 1 ) {
                    assert( possible_index_type != ::HIR::TypeRef() );
                    this->context.equate_types_assoc(node.span(), node.m_res_type,  lang_Index, mv$(trait_pp), ty, "Output", false);
                    break;
                }
                else if( count > 1 ) {
                    // Multiple fuzzy matches, don't keep dereferencing until we know.
                    current_ty = nullptr;
                    break;
                }
                else {
                    // Either no matches, or multiple fuzzy matches
                }

                deref_count += 1;
                current_ty = this->context.m_resolve.autoderef(node.span(), ty,  tmp_type);
                if( current_ty )
                    deref_res_types.push_back( current_ty->clone() );
            } while( current_ty );

            if( current_ty )
            {
                DEBUG("Found impl on type " << *current_ty << " with " << deref_count << " derefs");
                assert( deref_count == deref_res_types.size() );
                for(auto& ty_r : deref_res_types)
                {
                    auto ty = mv$(ty_r);

                    node.m_value = this->context.create_autoderef( mv$(node.m_value), mv$(ty) );
                    context.m_ivars.get_type(node.m_value->m_res_type);
                }

                m_completed = true;
            }
        }
        void visit(::HIR::ExprNode_Deref& node) override {
            const auto& ty = this->context.get_type(node.m_value->m_res_type);
            TRACE_FUNCTION_F("Deref: ty=" << ty);

            TU_MATCH_HDRA( (ty.data()), {)
            default: {
                const auto& op_trait = this->context.m_crate.get_lang_item_path(node.span(), "deref");
                this->context.equate_types_assoc(node.span(), node.m_res_type,  op_trait, {}, node.m_value->m_res_type.clone(), "Target");
                }
            TU_ARMA(Infer, e) {
                // Keep trying
                this->context.possible_equate_type_unknown(node.span(), node.m_res_type, Context::IvarUnknownType::From);
                return ;
                }
            TU_ARMA(Borrow, e) {
                // - Not really needed, but this is cheaper.
                this->context.equate_types(node.span(), node.m_res_type, e.inner);
                }
            TU_ARMA(Pointer, e) {
                // TODO: Figure out if this node is in an unsafe block.
                this->context.equate_types(node.span(), node.m_res_type, e.inner);
                }
            }
            this->m_completed = true;
        }
        void visit_emplace_129(::HIR::ExprNode_Emplace& node) {
            const auto& sp = node.span();
            const auto& exp_ty = this->context.get_type(node.m_res_type);
            const auto& data_ty = this->context.get_type(node.m_value->m_res_type);
            const auto& placer_ty = this->context.get_type(node.m_place->m_res_type);
            const auto& lang_Boxed = this->context.m_lang_Box;
            TRACE_FUNCTION_F("exp_ty=" << exp_ty << ", data_ty=" << data_ty << ", placer_ty" << placer_ty);
            ASSERT_BUG(sp, node.m_type == ::HIR::ExprNode_Emplace::Type::Boxer, "1.29 mode with non-box _Emplace node");
            ASSERT_BUG(sp, placer_ty == ::HIR::TypeRef::new_unit(), "1.29 mode with box in syntax - placer type is " << placer_ty);

            ASSERT_BUG(sp, !lang_Boxed.components().empty(), "`owned_box` not present when `box` operator used");

            // NOTE: `owned_box` shouldn't point to anything but a struct
            const auto& str = this->context.m_crate.get_struct_by_path(sp, lang_Boxed);
            // TODO: Store this type to avoid having to construct it every pass
            auto p = ::HIR::GenericPath(lang_Boxed, {data_ty.clone()});
            if(TARGETVER_LEAST_1_54) {
                p.m_params.m_types.push_back( MonomorphStatePtr(nullptr, &p.m_params,nullptr).monomorph_type(sp,str.m_params.m_types.at(1).m_default) );
                this->context.add_ivars(p.m_params.m_types.back());
            }
            auto boxed_ty = ::HIR::TypeRef::new_path( mv$(p), &str );

            // TODO: is there anyting special about this node that might need revisits?

            context.equate_types(sp, exp_ty, boxed_ty);
            this->m_completed = true;
        }
        void visit_emplace_119(::HIR::ExprNode_Emplace& node) {
            const auto& sp = node.span();
            const auto& exp_ty = this->context.get_type(node.m_res_type);
            const auto& data_ty = this->context.get_type(node.m_value->m_res_type);
            const auto& placer_ty = this->context.get_type(node.m_place->m_res_type);
            auto node_ty = node.m_type;
            TRACE_FUNCTION_F("_Emplace: exp_ty=" << exp_ty << ", data_ty=" << data_ty << ", placer_ty" << placer_ty);

            switch(node_ty)
            {
            case ::HIR::ExprNode_Emplace::Type::Noop:
                BUG(sp, "No-op _Emplace in typeck?");
                break;
            case ::HIR::ExprNode_Emplace::Type::Placer: {
                if( placer_ty.data().is_Infer() )
                {
                    // Can't do anything, the place is still unknown
                    DEBUG("Place unknown, wait");
                    //this->context.equate_types_to_shadow(sp, placer_ty);
                    this->context.possible_equate_type_unknown(sp, data_ty, Context::IvarUnknownType::To);
                    return ;
                }

                // Where P = `placer_ty` and D = `data_ty`
                // Result type is <<P as Placer<D>>::Place as InPlace<D>>::Owner
                const auto& lang_Placer = this->context.m_crate.get_lang_item_path(sp, "placer_trait");
                const auto& lang_InPlace = this->context.m_crate.get_lang_item_path(sp, "in_place_trait");
                // - Bound P: Placer<D>
                this->context.equate_types_assoc(sp, {}, lang_Placer, HIR::PathParams(data_ty.clone()), placer_ty, "");
                // - 
                auto place_ty = ::HIR::TypeRef::new_path( ::HIR::Path(placer_ty.clone(), ::HIR::GenericPath(lang_Placer, ::HIR::PathParams(data_ty.clone())), "Place"), {} );
                this->context.equate_types_assoc(sp, node.m_res_type, lang_InPlace, HIR::PathParams(data_ty.clone()), place_ty, "Owner");
                break; }
            case ::HIR::ExprNode_Emplace::Type::Boxer: {
                const ::HIR::TypeRef* inner_ty;
                if( exp_ty.data().is_Infer() ) {
                    // If the expected result type is still an ivar, nothing can be done

                    // HACK: Add a possibility of the result type being ``Box<`data_ty`>``
                    // - This only happens if the `owned_box` lang item is present and this node is a `box` operation
                    const auto& lang_Boxed = this->context.m_lang_Box;
                    if( ! lang_Boxed.components().empty() )
                    {
                        // NOTE: `owned_box` shouldn't point to anything but a struct
                        const auto& str = this->context.m_crate.get_struct_by_path(sp, lang_Boxed);
                        // TODO: Store this type to avoid having to construct it every pass
                        auto boxed_ty = ::HIR::TypeRef::new_path( ::HIR::GenericPath(lang_Boxed, {data_ty.clone()}), &str );
                        this->context.possible_equate_ivar( sp, exp_ty.data().as_Infer().index, boxed_ty, Context::PossibleTypeSource::CoerceFrom );
                    }
                    this->context.possible_equate_type_unknown(sp, data_ty, Context::IvarUnknownType::To);
                    return ;
                }
                // Assert that the expected result is a Path::Generic type.
                if( ! exp_ty.data().is_Path() ) {
                    ERROR(sp, E0000, "box/in can only produce GenericPath types, got " << exp_ty);
                }
                const auto& path = exp_ty.data().as_Path().path;
                if( ! path.m_data.is_Generic() ) {
                    ERROR(sp, E0000, "box/in can only produce GenericPath types, got " << exp_ty);
                }
                const auto& gpath = path.m_data.as_Generic();

                if( gpath.m_params.m_types.size() > 0 )
                {
                    // TODO: If there's only one, check if it's a valid coercion target, if not don't bother making the coercion.

                    // Take a copy of the type with all type parameters replaced with new ivars
                    auto newpath = ::HIR::GenericPath(gpath.m_path);
                    for( const auto& t : gpath.m_params.m_types )
                    {
                        (void)t;
                        newpath.m_params.m_types.push_back( this->context.m_ivars.new_ivar_tr() );
                    }
                    auto newty = ::HIR::TypeRef::new_path( mv$(newpath), exp_ty.data().as_Path().binding.clone() );

                    // Turn this revisit into a coercion point with the new result type
                    // - Mangle this node to be a passthrough to a copy of itself.

                    node.m_value = ::HIR::ExprNodeP( new ::HIR::ExprNode_Emplace(node.span(), node.m_type, mv$(node.m_place), mv$(node.m_value)) );
                    node.m_type = ::HIR::ExprNode_Emplace::Type::Noop;
                    node.m_value->m_res_type = mv$(newty);
                    inner_ty = &node.m_value->m_res_type;

                    this->context.equate_types_coerce(sp, exp_ty, node.m_value);
                }
                else
                {
                    inner_ty = &exp_ty;
                }

                // Insert a trait bound on the result type to impl `Placer/Boxer`
                this->context.equate_types_assoc(sp, data_ty, this->context.m_crate.get_lang_item_path(sp, "boxed_trait"), {}, *inner_ty, "Data");
                break; }
            }

            this->m_completed = true;
        }
        void visit(::HIR::ExprNode_Emplace& node) override {
            if(TARGETVER_MOST_1_19)
                return visit_emplace_119(node);
            return visit_emplace_129(node);
        }

        void visit(::HIR::ExprNode_TupleVariant& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_CallPath& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_CallValue& node) override {
            //const auto& sp = node.span();
            const auto& ty_o = this->context.get_type(node.m_value->m_res_type);
            TRACE_FUNCTION_F("CallValue: ty=" << ty_o);

            //this->context.possible_equate_type_coerce_from_unknown(node.span(), node.m_res_type);
            this->context.possible_equate_type_unknown(node.span(), node.m_res_type, Context::IvarUnknownType::Bound);
            // - Shadow (prevent ivar guessing) every parameter
            for( const auto& arg_ty : node.m_arg_ivars ) {
                this->context.possible_equate_type_unknown(node.span(), arg_ty, Context::IvarUnknownType::To);
            }

            if( ty_o.data().is_Infer() ) {
                // - Don't even bother
                return ;
            }

            const auto& lang_FnOnce = this->context.m_resolve.m_lang_FnOnce;

            // 1. Create a param set with a single tuple (of all argument types)
            ::HIR::PathParams   trait_pp;
            {
                ::std::vector< ::HIR::TypeRef>  arg_types;
                for(const auto& arg_ty : node.m_arg_ivars) {
                    arg_types.push_back( this->context.get_type(arg_ty).clone() );
                }
                trait_pp.m_types.push_back( ::HIR::TypeRef( mv$(arg_types) ) );
            }

            unsigned int deref_count = 0;
            ::HIR::TypeRef  tmp_type;   // for autoderef
            const auto* ty_p = &ty_o;

            bool keep_looping = false;
            do  // while( keep_looping );
            {
                // Reset at the start of each loop
                keep_looping = false;

                const auto& ty = *ty_p;
                DEBUG("- ty = " << ty);
                if( const auto* e = ty.data().opt_Closure() )
                {
                    for( const auto& arg : e->node->m_args )
                        node.m_arg_types.push_back(arg.second.clone());
                    node.m_arg_types.push_back(e->node->m_return.clone());
                    node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::Unknown;
                }
                else if( ty.data().is_Function() || ty.data().is_NamedFunction() )
                {
                    HIR::TypeRef    tmp_ft;
                    const auto* e = ty.data().opt_Function();
                    if( !e ) {
                        tmp_ft = this->context.m_resolve.expand_associated_types(node.span(), ty.data().as_NamedFunction().decay(node.span()));
                        e = &tmp_ft.data().as_Function();
                    }
                    auto hrls = e->hrls.make_empty_params(true);
                    auto m = MonomorphHrlsOnly(hrls);
                    for( const auto& arg : e->m_arg_types ) {
                        node.m_arg_types.push_back(m.monomorph_type(node.span(), arg));
                    }
                    if( e->is_variadic ) {
                        for(size_t i = e->m_arg_types.size(); i < node.m_args.size(); i ++) {
                            node.m_arg_types.push_back( node.m_arg_ivars[i].clone() );
                        }
                    }
                    node.m_arg_types.push_back(m.monomorph_type(node.span(), e->m_rettype));
                    node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::Fn;
                }
                else if( ty.data().is_Infer() )
                {
                    // No idea yet
                    return ;
                }
                else if( const auto* e = ty.data().opt_Borrow() )
                {
                    deref_count++;
                    ty_p = &this->context.get_type(e->inner);
                    DEBUG("Deref " << ty << " -> " << *ty_p);
                    keep_looping = true;
                    continue;
                }
                // TODO: If autoderef is possible, do it and continue. Only look for impls once autoderef fails
                else
                {
                    ::HIR::TypeRef  fcn_args_tup;
                    ::HIR::TypeRef  fcn_ret;

                    // TODO: Use `find_trait_impls` instead of two different calls
                    // - This will get the TraitObject impl search too

                    // Locate an impl of FnOnce (exists for all other Fn* traits)
                    // TODO: Sometimes there's impls that just forward for wrappers, which can lead to incorrect rules
                    // e.g. `&mut _` (where `_ = Box<...>`) later will pick the FnMut impl for `&mut T: FnMut` - but Box doesn't have those forwarding impls
                    // - Maybe just keep applying auto-deref until it's no longer possible?
                    unsigned int count = 0;
                    this->context.m_resolve.find_trait_impls(node.span(), lang_FnOnce, trait_pp, ty, [&](auto impl, auto cmp)->bool {
                        // TODO: Don't accept if too fuzzy
                        count++;

                        auto tup = impl.get_trait_ty_param(0);
                        if (!tup.data().is_Tuple())
                            ERROR(node.span(), E0000, "FnOnce expects a tuple argument, got " << tup);
                        MonomorphHrlsOnly(HIR::PathParams()).monomorph_type(node.span(), tup);
                        fcn_args_tup = mv$(tup);

                        fcn_ret = impl.get_type("Output", {});
                        DEBUG("[visit:_CallValue] fcn_args_tup=" << fcn_args_tup << ", fcn_ret=" << fcn_ret);
                        return cmp == ::HIR::Compare::Equal;
                    });
                    DEBUG("Found " << count << " impls of FnOnce");
                    if(count > 1) {
                        return;
                    }
                    if( count == 1 )
                    {
                        this->context.equate_types_assoc(node.span(), node.m_res_type, lang_FnOnce, HIR::PathParams(fcn_args_tup.clone()), ty, "Output");

                        // If the return type wasn't found in the impls, emit it as a UFCS
                        if(fcn_ret == ::HIR::TypeRef())
                        {
                            fcn_ret = ::HIR::TypeRef::new_path(::HIR::Path(::HIR::Path::Data::make_UfcsKnown({
                                ty.clone(),
                                // - Clone argument tuple, as it's stolen into cache below
                                ::HIR::GenericPath(lang_FnOnce, ::HIR::PathParams(fcn_args_tup.clone())),
                                "Output",
                                {}
                                })),
                                {}
                                );
                        }
                    }
                    else if( const auto* e = ty.data().opt_Borrow() )
                    {
                        deref_count++;
                        ty_p = &this->context.get_type(e->inner);
                        DEBUG("Deref " << ty << " -> " << *ty_p);
                        keep_looping = true;
                        continue;
                    }
                    else
                    {
                        if( !ty.data().is_Generic() )
                        {
                            bool found = this->context.m_resolve.find_trait_impls_crate(node.span(), lang_FnOnce, trait_pp, ty, [&](auto impl, auto cmp)->bool {
                                if (cmp == ::HIR::Compare::Fuzzy)
                                    TODO(node.span(), "Handle fuzzy match - " << impl);

                                auto tup = impl.get_trait_ty_param(0);
                                if (!tup.data().is_Tuple())
                                    ERROR(node.span(), E0000, "FnOnce expects a tuple argument, got " << tup);
                                fcn_args_tup = mv$(tup);
                                fcn_ret = impl.get_type("Output", {});
                                ASSERT_BUG(node.span(), fcn_ret != ::HIR::TypeRef(), "Impl didn't have a type for Output - " << impl);
                                return true;
                            });
                            if (found) {
                                // Fill cache and leave the TU_MATCH
                                node.m_arg_types = std::move(fcn_args_tup.get_unique().as_Tuple());
                                node.m_arg_types.push_back(mv$(fcn_ret));
                                node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::Unknown;
                                break; // leaves TU_MATCH
                            }
                        }
                        if( const auto* next_ty_p = this->context.m_resolve.autoderef(node.span(), ty, tmp_type) )
                        {
                            DEBUG("Deref (autoderef) " << ty << " -> " << *next_ty_p);
                            deref_count++;
                            ty_p = next_ty_p;
                            keep_looping = true;
                            continue;
                        }

                        // Didn't find anything. Error?
                        ERROR(node.span(), E0000, "Unable to find an implementation of Fn*" << trait_pp << " for " << this->context.m_ivars.fmt_type(ty));
                    }

                    node.m_arg_types = mv$(fcn_args_tup.get_unique().as_Tuple());
                    node.m_arg_types.push_back(mv$(fcn_ret));
                }
            } while( keep_looping );

            if( deref_count > 0 )
            {
                ty_p = &ty_o;
                while(deref_count-- > 0)
                {
                    ty_p = this->context.m_resolve.autoderef(node.span(), *ty_p, tmp_type);
                    assert(ty_p);
                    node.m_value = this->context.create_autoderef( mv$(node.m_value), ty_p->clone() );
                }
            }

            ASSERT_BUG(node.span(), node.m_arg_types.size() == node.m_args.size() + 1,
                "Malformed cache in CallValue: " << node.m_arg_types.size() << " != 1+"<<node.m_args.size());
            for(unsigned int i = 0; i < node.m_args.size(); i ++)
            {
                this->context.equate_types(node.span(), node.m_arg_types[i], node.m_arg_ivars[i]);
            }
            this->context.equate_types(node.span(), node.m_res_type, node.m_arg_types.back());
            this->m_completed = true;
        }
        void visit(::HIR::ExprNode_CallMethod& node) override {
            const auto& sp = node.span();

            const auto& ty = this->context.get_type(node.m_value->m_res_type);
            TRACE_FUNCTION_F("(CallMethod) {" << this->context.m_ivars.fmt_type(ty) << "}." << node.m_method << node.m_params
                << "(" << FMT_CB(os, for( const auto& arg_node : node.m_args ) os << this->context.m_ivars.fmt_type(arg_node->m_res_type) << ", ";) << ")"
                << " -> " << this->context.m_ivars.fmt_type(node.m_res_type)
                );

            // Make sure that no mentioned types are inferred until this method is known
            this->context.possible_equate_type_unknown(node.span(), node.m_res_type, Context::IvarUnknownType::From);
            for( const auto& arg_node : node.m_args ) {
                this->context.possible_equate_type_unknown(node.span(), arg_node->m_res_type, Context::IvarUnknownType::To);
            }

            // Using autoderef, locate this method on the type
            // TODO: Obtain a list of avaliable methods at that level?
            // - If running in a mode after stablise (before defaults), fall
            // back to trait if the inherent is still ambigious.
            ::std::vector<::std::pair<TraitResolution::AutoderefBorrow, ::HIR::Path>> possible_methods;
            unsigned int deref_count = this->context.m_resolve.autoderef_find_method(node.span(), node.m_traits, node.m_trait_param_ivars, ty, node.m_method,  possible_methods);
        try_again:
            if( deref_count != ~0u )
            {
                DEBUG("possible_methods = " << possible_methods);
                // HACK: In fallback mode, remove inherent impls from bounded ivars
                if( ty.data().is_Infer() && this->m_is_fallback )
                {
                    auto new_end = std::remove_if(possible_methods.begin(), possible_methods.end(), [](const auto& e){ return e.second.m_data.is_UfcsInherent(); });
                    if( new_end != possible_methods.begin() ) {
                        possible_methods.erase(new_end, possible_methods.end());
                    }
                }
                if( possible_methods.empty() )
                {
                    //ERROR(sp, E0000, "Could not find method `" << method_name << "` on type `" << top_ty << "`");
                    ERROR(sp, E0000, "No applicable methods for {" << this->context.m_ivars.fmt_type(ty) << "}." << node.m_method);
                }
                if( possible_methods.size() > 1 )
                {
                    // TODO: What do do when there's multiple possibilities?
                    // - Should use available information to strike them down
                    // > Try and equate the return type and the arguments, if any fail then move on to the next possibility?
                    // > ONLY if those arguments/return are generic
                    //
                    // Possible causes of multiple entries
                    // - Multiple distinct traits with the same method
                    //   > If `self` is concretely known, this is an error (and shouldn't happen in well-formed code).
                    // - Multiple inherent methods on a type
                    //   > These would have to have different type parmeters
                    // - Multiple trait bounds (same trait, different type params)
                    //   > Guess at the type params, then discard if there's a conflict?
                    //   > De-duplicate same traits?
                    //
                    //
                    // So: To be able to prune the list, we need to check the type parameters for the trait/type/impl 

                    // Remove anything except for the highest autoref level
                    for(auto it_1 = possible_methods.begin(); it_1 != possible_methods.end(); ++ it_1)
                    {
                        if( it_1->first != possible_methods.front().first )
                        {
                            it_1 = possible_methods.erase(it_1) - 1;
                        }
                    }
                    // De-duplcate traits in this list.
                    // - If the self type and the trait name are the same, replace with an entry using placeholder
                    //   ivars (node.m_trait_param_ivars)
                    for(auto it_1 = possible_methods.begin(); it_1 != possible_methods.end(); ++ it_1)
                    {
                        // Only consider trait impls (UfcsKnown path)
                        if( !it_1->second.m_data.is_UfcsKnown() )
                            continue;

                        auto& e1 = it_1->second.m_data.as_UfcsKnown();
                        for(auto it_2 = it_1 + 1; it_2 != possible_methods.end(); ++ it_2)
                        {
                            if( !it_2->second.m_data.is_UfcsKnown() )
                                continue;
                            // If it's a complete duplicate, immediately ignore.
                            if( it_2->second == it_1->second ) {
                                it_2 = possible_methods.erase(it_2) - 1;
                                continue ;
                            }
                            const auto& e2 = it_2->second.m_data.as_UfcsKnown();

                            // TODO: If the trait is the same, but the type differs, pick the first?
                            if( e1.trait == e2.trait ) {
                                // NOTE: The trait is identical, but the full path comparison above failed. Ergo the type is different.
                                DEBUG("Duplicate trait, different type - " << e1.trait << " for " << e1.type << " or " << e2.type << ", picking the first");
                                it_2 = possible_methods.erase(it_2) - 1;
                                continue ;
                            }
                            if( e1.type != e2.type )
                                continue;
                            // Compare the simplepath.
                            if( e1.trait.m_path != e2.trait.m_path )
                                continue;
                            assert( !(e1.trait.m_params == e2.trait.m_params) );

                            DEBUG("Duplicate trait in possible_methods - " << it_1->second << " and " << it_2->second);

                            // Remove the second entry, after re-creating the params using the ivar list
                            // TODO: If `Into<Foo>` and `Into<_>` is seen, we want to pick the solid type, BUT
                            //       For `Into<Foo>` and `Into<Bar>` this needs to be collapsed into `Into<_>` and propagated
                            //if( e1.trait .compare_with_placeholders(sp, e2.trait, context.m_ivars.callback_resolve_infer()) == ::HIR::Compare::Unequal )
                            {
                                auto& ivars = node.m_trait_param_ivars;
                                unsigned int n_params = e1.trait.m_params.m_types.size();
                                while(ivars.size() < n_params) {
                                    ivars.push_back(context.m_ivars.new_ivar());
                                }
                                assert(n_params <= ivars.size());
                                ::HIR::PathParams   trait_params;
                                trait_params.m_types.reserve( n_params );
                                for(unsigned int i = 0; i < n_params; i++) {
                                    trait_params.m_types.push_back( ::HIR::TypeRef::new_infer(ivars[i], ::HIR::InferClass::None) );
                                    //ASSERT_BUG(sp, m_ivars.get_type( trait_params.m_types.back() ).m_data.as_Infer().index == ivars[i], "A method selection ivar was bound");
                                }
                                if(e1.trait.m_params.m_values.size() > 0) {
                                    TODO(sp, "Populate infer for value generics");
                                }
                                // If one of these was already using the placeholder ivars, then maintain the one with the palceholders
                                if( e1.trait.m_params != trait_params )
                                {
                                    e1.trait.m_params = mv$(trait_params);
                                }
                            }

                            it_2 = possible_methods.erase(it_2) - 1;
                        }
                    }

                    // If tjhis is fallback mode, and we're in a trait impl - grab the trait
                    if( possible_methods.size() > 1 && this->context.m_resolve.current_trait_path() )
                    {
                        const auto& tp = *this->context.m_resolve.current_trait_path();
                        auto found = possible_methods.end();
                        bool had_inherent = false;
                        for(auto it = possible_methods.begin(); it != possible_methods.end(); ++it) {
                            had_inherent |= it->second.m_data.is_UfcsInherent();
                            if( it->second.m_data.is_UfcsKnown() && it->second.m_data.as_UfcsKnown().trait.m_path == tp.m_path ) {
                                found = it;
                            }
                        }
                        if( !had_inherent && found != possible_methods.end() ) {
                            DEBUG("Multiple options - Restricted to just current trait");
                            possible_methods.erase(found+1, possible_methods.end());
                            possible_methods.erase(possible_methods.begin(), found);
                        }
                    }

                    DEBUG("possible_methods = " << possible_methods);
                }
                assert( !possible_methods.empty() );
                if( possible_methods.size() != 1 && possible_methods.front().second.m_data.is_UfcsKnown() )
                {
                    DEBUG("- Multiple options, deferring");
                    // TODO: If the type is fully known, then this is an error.
                    return;
                }
                auto& ad_borrow = possible_methods.front().first;
                auto& fcn_path = possible_methods.front().second;
                DEBUG("- deref_count = " << deref_count << ", fcn_path = " << fcn_path);

                node.m_method_path = mv$(fcn_path);
                // NOTE: Steals the params from the node
                TU_MATCH(::HIR::Path::Data, (node.m_method_path.m_data), (e),
                (Generic,
                    ),
                (UfcsUnknown,
                    ),
                (UfcsKnown,
                    e.params = mv$(node.m_params);
                    //fix_param_count(sp, this->context, node.m_method_path, fcn.m_params, e.params);
                    ),
                (UfcsInherent,
                    e.params = mv$(node.m_params);
                    //fix_param_count(sp, this->context, node.m_method_path, fcn.m_params, e.params);
                    )
                )

                // TODO: If this is ambigious, and it's an inherent, and in fallback mode - fall down to the next trait method.
                if( !typecheck::visit_call_populate_cache(this->context, node.span(), node.m_method_path, node.m_cache) )
                {
                    // Move the params back
                    TU_MATCH(::HIR::Path::Data, (node.m_method_path.m_data), (e),
                    (Generic, ),
                    (UfcsUnknown, ),
                    (UfcsKnown,
                        node.m_params = mv$(e.params);
                        ),
                    (UfcsInherent,
                        node.m_params = mv$(e.params);
                        )
                    )
                    if( this->m_is_fallback && node.m_method_path.m_data.is_UfcsInherent() )
                    {
                        unsigned n_remove = 1;
                        while( n_remove < possible_methods.size() && possible_methods[n_remove].second.m_data.is_UfcsInherent() )
                        {
                            n_remove += 1;
                        }
                        if( n_remove < possible_methods.size() )
                        {
                            possible_methods.erase(possible_methods.begin() + n_remove);
                            DEBUG("Inference stall (remove " << n_remove << ") try again with " << possible_methods.front().second);
                            goto try_again;
                        }
                        else
                        {
                            DEBUG("AMBIGUOUS and removed all " << n_remove << " possibilities");
                        }
                    }
                    else
                    {
                        DEBUG("- AMBIGUOUS - Trying again later");
                    }
                    return ;
                }
                DEBUG("> m_method_path = " << node.m_method_path);

                assert( node.m_cache.m_arg_types.size() >= 1);

                if( node.m_args.size()+1 != node.m_cache.m_arg_types.size() - 1 ) {
                    ERROR(node.span(), E0000, "Incorrect number of arguments to " << node.m_method_path
                        << " - exp " << node.m_cache.m_arg_types.size()-2 << " got " << node.m_args.size());
                }
                DEBUG("- fcn_path=" << node.m_method_path);

                // --- Check and equate self/arguments/return
                DEBUG("node.m_cache.m_arg_types = " << node.m_cache.m_arg_types);
                // NOTE: `Self` is equated after autoderef and autoref
                for(unsigned int i = 0; i < node.m_args.size(); i ++)
                {
                    // 1+ because it's a method call (#0 is Self)
                    DEBUG("> ARG " << i << " : " << node.m_cache.m_arg_types[1+i]);
                    this->context.equate_types_coerce(sp, node.m_cache.m_arg_types[1+i], node.m_args[i]);
                }
                DEBUG("> Ret : " << node.m_cache.m_arg_types.back());
                this->context.equate_types(sp, node.m_res_type,  node.m_cache.m_arg_types.back());

                // Add derefs
                if( deref_count > 0 )
                {
                    assert( deref_count < (1<<16) );    // Just some sanity.
                    DEBUG("- Inserting " << deref_count << " dereferences");
                    // Get dereferencing!
                    auto& node_ptr = node.m_value;
                    ::HIR::TypeRef  tmp_ty;
                    const ::HIR::TypeRef*   cur_ty = &node_ptr->m_res_type;
                    while( deref_count-- )
                    {
                        auto span = node_ptr->span();
                        cur_ty = this->context.m_resolve.autoderef(span, *cur_ty, tmp_ty);
                        assert(cur_ty);
                        auto ty = cur_ty->clone();

                        node.m_value = this->context.create_autoderef( mv$(node.m_value), mv$(ty) );
                    }
                }

                // Autoref
                if( ad_borrow != TraitResolution::AutoderefBorrow::None )
                {
                    ::HIR::BorrowType   bt = ::HIR::BorrowType::Shared;
                    switch(ad_borrow)
                    {
                    case TraitResolution::AutoderefBorrow::None:    throw "";
                    case TraitResolution::AutoderefBorrow::Shared:   bt = ::HIR::BorrowType::Shared; break;
                    case TraitResolution::AutoderefBorrow::Unique:   bt = ::HIR::BorrowType::Unique; break;
                    case TraitResolution::AutoderefBorrow::Owned :   bt = ::HIR::BorrowType::Owned ; break;
                    }

                    auto ty = ::HIR::TypeRef::new_borrow(bt, node.m_value->m_res_type.clone());
                    DEBUG("- Ref (cmd) " << &*node.m_value << " -> " << ty);
                    auto span = node.m_value->span();
                    node.m_value = NEWNODE(mv$(ty), span, _Borrow,  bt, mv$(node.m_value) );
                }
                else
                {
                }

                // Equate the type for `self` (to ensure that Self's type params infer correctly)
                this->context.equate_types(sp, node.m_cache.m_arg_types[0], node.m_value->m_res_type);

                this->m_completed = true;
            }
        }
        void visit(::HIR::ExprNode_Field& node) override {
            const auto& field_name = node.m_field;
            TRACE_FUNCTION_F("(Field) name=" << field_name << ", ty = " << this->context.m_ivars.fmt_type(node.m_value->m_res_type));

            //this->context.possible_equate_type_unknown(node.span(), node.m_res_type, Context::IvarUnknownType::From);
            this->context.possible_equate_type_unknown(node.span(), node.m_res_type, Context::IvarUnknownType::Bound);

            ::HIR::TypeRef  out_type;

            // Using autoderef, locate this field
            unsigned int deref_count = 0;
            ::HIR::TypeRef  tmp_type;   // Temporary type used for handling Deref
            const auto* current_ty = &node.m_value->m_res_type;
            ::std::vector< ::HIR::TypeRef>  deref_res_types;

            // TODO: autoderef_find_field?
            do {
                const auto& ty = this->context.m_ivars.get_type(*current_ty);
                if( ty.data().is_Infer() ) {
                    DEBUG("Hit ivar, returning early");
                    return ;
                }
                if(ty.data().is_Path() && ty.data().as_Path().binding.is_Unbound()) {
                    DEBUG("Hit unbound path, returning early");
                    return ;
                }
                if( this->context.m_resolve.find_field(node.span(), ty, field_name, out_type) ) {
                    this->context.equate_types(node.span(), node.m_res_type, out_type);
                    break;
                }

                deref_count += 1;
                current_ty = this->context.m_resolve.autoderef(node.span(), ty,  tmp_type);
                if( current_ty )
                    deref_res_types.push_back( current_ty->clone() );
            } while(current_ty);

            if( !current_ty )
            {
                ERROR(node.span(), E0000, "Couldn't find the field " << field_name << " in " << this->context.m_ivars.fmt_type(node.m_value->m_res_type));
            }

            assert( deref_count == deref_res_types.size() );
            for(unsigned int i = 0; i < deref_res_types.size(); i ++ )
            {
                auto ty = mv$(deref_res_types[i]);
                DEBUG("- Deref " << &*node.m_value << " -> " << ty);
                if( node.m_value->m_res_type.data().is_Array() ) {
                    BUG(node.span(), "Field access from array/slice?");
                }
                node.m_value = NEWNODE(mv$(ty), node.span(), _Deref,  mv$(node.m_value));
                context.m_ivars.get_type(node.m_value->m_res_type);
            }

            m_completed = true;
        }

        void visit(::HIR::ExprNode_Literal& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_UnitVariant& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_PathValue& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Variable& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_ConstParam& node) override {
            no_revisit(node);
        }

        void visit(::HIR::ExprNode_StructLiteral& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Tuple& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_ArrayList& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_ArraySized& node) override {
            no_revisit(node);
        }

        void visit(::HIR::ExprNode_Closure& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Generator& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_GeneratorWrapper& node) override {
            no_revisit(node);
        }
    private:
        void no_revisit(::HIR::ExprNode& node) {
            BUG(node.span(), "Node revisit unexpected - " << typeid(node).name());
        }
    };  // class ExprVisitor_Revisit

    // -----------------------------------------------------------------------
    // Post-inferrence visitor
    //
    // Saves the inferred types into the HIR expression tree, and ensures that
    // all types were inferred.
    // -----------------------------------------------------------------------
    class ExprVisitor_Apply:
        public ::HIR::ExprVisitorDef
    {
        const Context& context;
        const HMTypeInferrence& ivars;
    public:
        ExprVisitor_Apply(const Context& context):
            context(context),
            ivars(context.m_ivars)
        {
        }
        void visit_node_ptr(::HIR::ExprPtr& node_ptr)
        {
            auto& node = *node_ptr;
            const char* node_ty = typeid(node).name();

            TRACE_FUNCTION_FR(&node << " " << &node << " " << node_ty << " : " << node.m_res_type, node_ty);
            this->check_type_resolved_top(node.span(), node.m_res_type);
            DEBUG(node_ty << " : = " << node.m_res_type);

            node_ptr->visit(*this);

            for( auto& ty : node_ptr.m_bindings )
                this->check_type_resolved_top(node.span(), ty);

            for( auto& ty : node_ptr.m_erased_types )
                this->check_type_resolved_top(node.span(), ty);

            for(auto& ent : context.m_erased_type_aliases)
            {
                auto t = ent.second.our_type.clone();
                check_type_resolved(node.span(), t, t);
                // If the type is for the same type alias, then ignore
                if( t.data().is_ErasedType() && t.data().as_ErasedType().m_inner.is_Alias() && t.data().as_ErasedType().m_inner.as_Alias().inner.get() == ent.first ) {
                    continue;
                }
                // TODO: Enforce/validate that the parmas match this function's params, then convert method-level to type-level
                // - Get the path params used to construct this path in the first place, and then do a `clone_ty_with`
                auto ty = clone_ty_with(node.span(), t, [&](const HIR::TypeRef& tpl, HIR::TypeRef& out_ty)->bool  {
                    for(size_t i = 0; i < ent.second.params.m_types.size(); i++ ) {
                        if( tpl == ent.second.params.m_types[i] ) {
                            out_ty = HIR::TypeRef(ent.first->generics.m_types.at(i).m_name, i);
                            return true;
                        }
                    }
                    return false;
                    });
                {
                    auto p = ent.first->generics.make_nop_params(0);
                    MonomorphStatePtr(nullptr, &p, nullptr).monomorph_type(node.span(), ty);
                }
                if( ent.first->type == HIR::TypeRef() ) {
                    DEBUG("type " << ent.first->path << " = " << ty);
                    ent.first->type = std::move(ty);
                }
                else {
                    if( ent.first->type != ty ) {
                        ERROR(node.span(), E0000, "Disagreement on type for " << ent.first->path << ": " << ent.first->type << " or " << ty);
                    }
                }
            }
        }
        void visit_node_ptr(::HIR::ExprNodeP& node_ptr) override {
            auto& node = *node_ptr;
            const char* node_ty = typeid(node).name();
            TRACE_FUNCTION_FR(&node_ptr << " " << &node << " " << node_ty << " : " << node.m_res_type, &node << " " << node_ty);
            this->check_type_resolved_top(node.span(), node.m_res_type);
            DEBUG(node_ty << " : = " << node.m_res_type);
            ::HIR::ExprVisitorDef::visit_node_ptr(node_ptr);
        }

        void visit_pattern(const Span& sp, ::HIR::Pattern& pat) override {
            TU_MATCH_HDRA( (pat.m_data), { )
            default:
                break;
            TU_ARMA(Value, e) {
                TU_IFLET( ::HIR::Pattern::Value, (e.val), Named, ve,
                    this->check_type_resolved_path(sp, ve.path);
                )
                }
            TU_ARMA(Range, e) {
                if( e.end && e.start->is_Named() ) {
                    this->check_type_resolved_path(sp, e.start->as_Named().path);
                }
                if( e.end && e.end->is_Named() ) {
                    this->check_type_resolved_path(sp, e.end->as_Named().path);
                }
                }
            TU_ARMA(PathValue, e) {
                this->check_type_resolved_path(sp, e.path);
                }
            TU_ARMA(PathTuple, e) {
                this->check_type_resolved_path(sp, e.path);
                }
            TU_ARMA(PathNamed, e) {
                this->check_type_resolved_path(sp, e.path);
                }
            }
            ::HIR::ExprVisitorDef::visit_pattern(sp, pat);
        }

        void visit(::HIR::ExprNode_Block& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            if( node.m_value_node )
            {
                check_types_equal(node.span(), node.m_res_type, node.m_value_node->m_res_type);
            }
            // If the last node diverges (yields `!`) then this block can yield `!` (or anything)
            else if( ! node.m_nodes.empty() && node.m_nodes.back()->m_res_type == ::HIR::TypeRef::new_diverge() )
            {
            }
            else
            {
                // Non-diverging (empty, or with a non-diverging last node) blocks must yield `()`
                check_types_equal(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
            }
        }

        void visit(::HIR::ExprNode_Let& node) override {
            this->check_type_resolved_top(node.span(), node.m_type);
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_Closure& node) override {
            for(auto& arg : node.m_args)
                this->check_type_resolved_top(node.span(), arg.second);
            this->check_type_resolved_top(node.span(), node.m_return);
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_Generator& node) override {
            //for(auto& arg : node.m_args)
            //    this->check_type_resolved_top(node.span(), arg.second);
            this->check_type_resolved_top(node.span(), node.m_return);
            this->check_type_resolved_top(node.span(), node.m_yield_ty);
            this->check_type_resolved_top(node.span(), node.m_resume_ty);
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_GeneratorWrapper& node) override {
            BUG(node.span(), "");
        }

        void visit_callcache(const Span&sp, ::HIR::ExprCallCache& cache)
        {
            for(auto& ty : cache.m_arg_types)
                this->check_type_resolved_top(sp, ty);
        }
        void visit(::HIR::ExprNode_CallPath& node) override {
            this->visit_callcache(node.span(), node.m_cache);

            this->check_type_resolved_path(node.span(), node.m_path);
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_CallMethod& node) override {
            this->visit_callcache(node.span(), node.m_cache);

            this->check_type_resolved_path(node.span(), node.m_method_path);
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_CallValue& node) override {

            for(auto& ty : node.m_arg_types) {
                this->check_type_resolved_top(node.span(), ty);
            }

            {
                const auto& ty = context.get_type(node.m_value->m_res_type);
                if( /*const auto* e =*/ ty.data().opt_Closure() )
                {
                    node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::Unknown;
                }
                else if( /*const auto* e =*/ ty.data().opt_Function() )
                {
                    node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::Fn;
                }
                else
                {
                    // 1. Create a param set with a single tuple (of all argument types)
                    ::HIR::PathParams   trait_pp;
                    {
                        ::std::vector< ::HIR::TypeRef>  arg_types;
                        for(const auto& arg_ty : node.m_arg_ivars) {
                            arg_types.push_back( this->context.get_type(arg_ty).clone() );
                        }
                        trait_pp.m_types.push_back( ::HIR::TypeRef( mv$(arg_types) ) );
                    }

                    // 3. Locate the most permissive implemented Fn* trait (Fn first, then FnMut, then assume just FnOnce)
                    // NOTE: Borrowing is added by the expansion to CallPath
                    if( this->context.m_resolve.find_trait_impls(node.span(), this->context.m_resolve.m_lang_Fn, trait_pp, ty, [&](auto impl, auto cmp) {
                        //ASSERT_BUG(node.span(), cmp == ::HIR::Compare::Equal, "");
                        return true;
                        }) )
                    {
                        DEBUG("-- Using Fn");
                        node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::Fn;
                    }
                    else if( this->context.m_resolve.find_trait_impls(node.span(), this->context.m_resolve.m_lang_FnMut, trait_pp, ty, [&](auto impl, auto cmp) {
                        //ASSERT_BUG(node.span(), cmp == ::HIR::Compare::Equal, "Fuzzy FnMut" << trait_pp << " impl?! " << ty);
                        return true;
                        }) )
                    {
                        DEBUG("-- Using FnMut");
                        node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::FnMut;
                    }
                    else
                    {
                        DEBUG("-- Using FnOnce (default)");
                        node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::FnOnce;
                    }
                }
            }

            ::HIR::ExprVisitorDef::visit(node);
        }

        void visit(::HIR::ExprNode_PathValue& node) override {
            this->check_type_resolved_path(node.span(), node.m_path);
        }
        void visit(::HIR::ExprNode_UnitVariant& node) override {
            this->check_type_resolved_genericpath(node.span(), node.m_path);
        }
        void visit(::HIR::ExprNode_StructLiteral& node) override {
            this->check_type_resolved_genericpath(node.span(), node.m_real_path);
            for(auto& ty : node.m_value_types) {
                if( ty != ::HIR::TypeRef() ) {
                    this->check_type_resolved_top(node.span(), ty);
                }
            }

#if 1
            const auto& sp = node.span();
            const auto& ty_path = node.m_real_path;
            const auto& ty = node.m_res_type;
            ASSERT_BUG(sp, ty.data().is_Path(), "Result type of _StructLiteral isn't Path");
            const ::HIR::t_struct_fields* fields_ptr = nullptr;
            TU_MATCH_HDRA( (ty.data().as_Path().binding), {)
            TU_ARMA(Unbound, e) {}
            TU_ARMA(Opaque, e) {}
            TU_ARMA(Enum, e) {
                const auto& var_name = ty_path.m_path.components().back();
                const auto& enm = *e;
                auto idx = enm.find_variant(var_name);
                ASSERT_BUG(sp, idx != SIZE_MAX, "");
                ASSERT_BUG(sp, enm.m_data.is_Data(), "");
                const auto& var = enm.m_data.as_Data()[idx];

                const auto& str = *var.type.data().as_Path().binding.as_Struct();
                ASSERT_BUG(sp, var.is_struct, "Struct literal for enum on non-struct variant");
                fields_ptr = &str.m_data.as_Named();
                }
            TU_ARMA(Union, e) {
                fields_ptr = &e->m_variants;
                ASSERT_BUG(node.span(), node.m_values.size() > 0, "Union with no values");
                ASSERT_BUG(node.span(), node.m_values.size() == 1, "Union with multiple values");
                ASSERT_BUG(node.span(), !node.m_base_value, "Union can't have a base value");
                }
            TU_ARMA(ExternType, e) {
                BUG(sp, "ExternType in StructLiteral");
                }
            TU_ARMA(Struct, e) {
                if( e->m_data.is_Unit() )
                {
                    ASSERT_BUG(node.span(), node.m_values.size() == 0, "Values provided for unit-like struct");
                    ASSERT_BUG(node.span(), ! node.m_base_value, "Values provided for unit-like struct");
                    return ;
                }

                ASSERT_BUG(node.span(), e->m_data.is_Named(), "StructLiteral not pointing to a braced struct, instead " << e->m_data.tag_str() << " - " << ty);
                fields_ptr = &e->m_data.as_Named();
                }
            }
            ASSERT_BUG(node.span(), fields_ptr, "Didn't get field for path in _StructLiteral - " << ty);
            const ::HIR::t_struct_fields& fields = *fields_ptr;
            for(const auto& fld : fields) {
                DEBUG(fld.first << ": " << fld.second.ent);
            }
#endif

            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_TupleVariant& node) override {
            this->check_type_resolved_pp(node.span(), node.m_path.m_params, ::HIR::TypeRef());
            for(auto& ty : node.m_arg_types) {
                if( ty != ::HIR::TypeRef() ) {
                    this->check_type_resolved_top(node.span(), ty);
                }
            }

            ::HIR::ExprVisitorDef::visit(node);
        }

        void visit(::HIR::ExprNode_Literal& node) override {
            TU_MATCH(::HIR::ExprNode_Literal::Data, (node.m_data), (e),
            (Integer,
                ASSERT_BUG(node.span(), node.m_res_type.data().is_Primitive(), "Integer _Literal didn't return primitive - " << node.m_res_type);
                e.m_type = node.m_res_type.data().as_Primitive();
                ),
            (Float,
                ASSERT_BUG(node.span(), node.m_res_type.data().is_Primitive(), "Float Literal didn't return primitive - " << node.m_res_type);
                e.m_type = node.m_res_type.data().as_Primitive();
                ),
            (Boolean,
                ),
            (ByteString,
                ),
            (String,
                )
            )
        }

        void visit(::HIR::ExprNode_Cast& node) override {
            this->check_type_resolved_top(node.span(), node.m_dst_type);
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_Unsize& node) override {
            this->check_type_resolved_top(node.span(), node.m_dst_type);
            ::HIR::ExprVisitorDef::visit(node);
        }

    private:
        void check_type_resolved_top(const Span& sp, ::HIR::TypeRef& ty) const {
            check_type_resolved(sp, ty, ty);
            ty = this->context.m_resolve.expand_associated_types(sp, mv$(ty));
            DEBUG(ty);
        }

        void check_type_resolved_constgeneric(const Span& sp, ::HIR::ConstGeneric& v, const ::HIR::TypeRef& top_type) const
        {
            if(v.is_Infer())
            {
                auto val = ivars.get_value(v).clone();
                ASSERT_BUG(sp, !val.is_Infer(), "Failure to infer " << v << " in " << top_type);
                v = std::move(val);
            }
        }

        void check_type_resolved_pp(const Span& sp, ::HIR::PathParams& pp, const ::HIR::TypeRef& top_type) const {
            for(auto& ty : pp.m_types)
                check_type_resolved(sp, ty, top_type);
            for(auto& val : pp.m_values)
                check_type_resolved_constgeneric(sp, val, top_type);
        }
        void check_type_resolved_path(const Span& sp, ::HIR::Path& path) const {
            auto tmp = ::HIR::TypeRef::new_path(path.clone(), {});
            check_type_resolved_path(sp, path, tmp);
            TU_MATCH(::HIR::Path::Data, (path.m_data), (pe),
            (Generic,
                for(auto& ty : pe.m_params.m_types)
                    ty = this->context.m_resolve.expand_associated_types(sp, mv$(ty));
                ),
            (UfcsInherent,
                pe.type = this->context.m_resolve.expand_associated_types(sp, mv$(pe.type));
                for(auto& ty : pe.params.m_types)
                    ty = this->context.m_resolve.expand_associated_types(sp, mv$(ty));
                for(auto& ty : pe.impl_params.m_types)
                    ty = this->context.m_resolve.expand_associated_types(sp, mv$(ty));
                ),
            (UfcsKnown,
                pe.type = this->context.m_resolve.expand_associated_types(sp, mv$(pe.type));
                for(auto& ty : pe.params.m_types)
                    ty = this->context.m_resolve.expand_associated_types(sp, mv$(ty));
                for(auto& ty : pe.trait.m_params.m_types)
                    ty = this->context.m_resolve.expand_associated_types(sp, mv$(ty));
                ),
            (UfcsUnknown,
                throw "";
                )
            )
        }
        void check_type_resolved_path(const Span& sp, ::HIR::Path& path, const ::HIR::TypeRef& top_type) const {
            TU_MATCH(::HIR::Path::Data, (path.m_data), (pe),
            (Generic,
                check_type_resolved_pp(sp, pe.m_params, top_type);
                ),
            (UfcsInherent,
                check_type_resolved(sp, pe.type, top_type);
                check_type_resolved_pp(sp, pe.params, top_type);
                check_type_resolved_pp(sp, pe.impl_params, top_type);
                ),
            (UfcsKnown,
                check_type_resolved(sp, pe.type, top_type);
                check_type_resolved_pp(sp, pe.trait.m_params, top_type);
                check_type_resolved_pp(sp, pe.params, top_type);
                ),
            (UfcsUnknown,
                ERROR(sp, E0000, "UfcsUnknown " << path << " left in " << top_type);
                )
            )
        }
        void check_type_resolved_genericpath(const Span& sp, ::HIR::GenericPath& path) const {
            auto tmp = ::HIR::TypeRef::new_path(path.clone(), {});
            check_type_resolved_pp(sp, path.m_params, tmp);
        }
        void check_type_resolved(const Span& sp, ::HIR::TypeRef& ty, const ::HIR::TypeRef& top_type) const {
            class InnerVisitor:
                public HIR::Visitor
            {
                const ExprVisitor_Apply& parent;
                const Span& sp;
                const ::HIR::TypeRef& top_type;

            public:
                InnerVisitor(const ExprVisitor_Apply& parent, const Span& sp, const ::HIR::TypeRef& top_type)
                    :parent(parent)
                    ,sp(sp)
                    ,top_type(top_type)
                {
                }

                void visit_path(::HIR::Path& path, HIR::Visitor::PathContext pc) override
                {
                    if( path.m_data.is_UfcsUnknown() )
                        ERROR(sp, E0000, "UfcsUnknown " << path << " left in " << top_type);
                    ::HIR::Visitor::visit_path(path, pc);
                }
                void visit_constgeneric(::HIR::ConstGeneric& v) override
                {
                    if(v.is_Infer())
                    {
                        auto val = parent.ivars.get_value(v).clone();
                        DEBUG(v << " -> " << val);
                        v = std::move(val);
                    }
                }
                void visit_type(::HIR::TypeRef& ty) override
                {
                    if( ty.data().is_Infer() )
                    {
                        auto new_ty = parent.ivars.get_type(ty).clone();
                        DEBUG(ty << " -> " << new_ty);
                        // - Move over before checking, so that the source type mentions the correct ivar
                        ty = mv$(new_ty);
                        if( ty.data().is_Infer() ) {
                            ERROR(sp, E0000, "Failed to infer type " << ty << " in "  << top_type);
                        }
                    }

                    ::HIR::Visitor::visit_type(ty);

                    if( ty.data().is_Array() )
                    {
                        auto& size = ty.data_mut().as_Array().size;
                        if(size.is_Unevaluated()) {
                            if(size.as_Unevaluated().is_Evaluated()) {
                                DEBUG("Known size: " << size);
                                size = size.as_Unevaluated().as_Evaluated()->read_usize(0);
                            }
                        }
                    }
                }
            };

            InnerVisitor v(*this, sp, top_type);
            v.visit_type(ty);
        }

        void check_types_equal(const Span& sp, const ::HIR::TypeRef& l, const ::HIR::TypeRef& r) const
        {
            DEBUG(sp << " - " << l << " == " << r);
            if( r.data().is_Diverge() ) {
                // Diverge on the right is always valid
                // - NOT on the left, because `!` can become everything, but nothing can become `!`
            }
            else if( l != r ) {
                ERROR(sp, E0000, "Type mismatch\n - " << l << "\n!= " << r);
            }
            else {
                // All good
            }
        }
    }; // class ExprVisitor_Apply

    class ExprVisitor_Print:
        public ::HIR::ExprVisitor
    {
        const Context& context;
        ::std::ostream& m_os;
    public:
        ExprVisitor_Print(const Context& context, ::std::ostream& os):
            context(context),
            m_os(os)
        {}

        void visit(::HIR::ExprNode_Block& node) override {
            m_os << "_Block {" << context.m_ivars.fmt_type(node.m_nodes.back()->m_res_type) << "}";
        }
        void visit(::HIR::ExprNode_ConstBlock& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Asm& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Asm2& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Return& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Yield& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Let& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Loop& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_LoopControl& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Match& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_If& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Assign& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_BinOp& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_UniOp& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Borrow& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_RawBorrow& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Cast& node) override {
            m_os << "_Cast {" << context.m_ivars.fmt_type(node.m_value->m_res_type) << "}";
        }
        void visit(::HIR::ExprNode_Unsize& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Index& node) override {
            m_os << "_Index {" << fmt_res_ty(*node.m_value) << "}[{" << fmt_res_ty(*node.m_index) << "}]";
        }
        void visit(::HIR::ExprNode_Deref& node) override {
            m_os << "_Deref {" << fmt_res_ty(*node.m_value) << "}";
        }
        void visit(::HIR::ExprNode_Emplace& node) override {
            m_os << "_Emplace(" << fmt_res_ty(*node.m_value) << " in " << fmt_res_ty(*node.m_place) << ")";
        }

        void visit(::HIR::ExprNode_TupleVariant& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_CallPath& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_CallValue& node) override {
            m_os << "_CallValue {" << fmt_res_ty(*node.m_value) << "}(";
            for(const auto& arg : node.m_args)
                m_os << "{" << fmt_res_ty(*arg) << "}, ";
            m_os << ")";
        }
        void visit(::HIR::ExprNode_CallMethod& node) override {
            m_os << "_CallMethod {" << fmt_res_ty(*node.m_value) << "}." << node.m_method << "(";
            for(const auto& arg : node.m_args)
                m_os << "{" << fmt_res_ty(*arg) << "}, ";
            m_os << ")";
        }
        void visit(::HIR::ExprNode_Field& node) override {
            m_os << "_Field {" << fmt_res_ty(*node.m_value) << "}." << node.m_field;
        }

        void visit(::HIR::ExprNode_Literal& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_UnitVariant& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_PathValue& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Variable& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_ConstParam& node) override {
            no_revisit(node);
        }

        void visit(::HIR::ExprNode_StructLiteral& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Tuple& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_ArrayList& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_ArraySized& node) override {
            no_revisit(node);
        }

        void visit(::HIR::ExprNode_Closure& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Generator& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_GeneratorWrapper& node) override {
            no_revisit(node);
        }
    private:
        HMTypeInferrence::FmtType fmt_res_ty(const ::HIR::ExprNode& n) {
            return context.m_ivars.fmt_type(n.m_res_type);
        }
        void no_revisit(::HIR::ExprNode& n) {
            throw "";
        }
    }; // class ExprVisitor_Print
}


void Context::dump() const {
    DEBUG("--- Variables");
    for(unsigned int i = 0; i < m_bindings.size(); i ++)
    {
        DEBUG(i << " " << m_bindings[i].name << ": " << this->m_ivars.fmt_type(m_bindings[i].ty));
    }
    DEBUG("--- Ivars");
    m_ivars.dump();
    DEBUG("--- CS Context - " << link_coerce.size() << " Coercions, " << link_assoc.size() << " associated, " << to_visit.size() << " nodes, " << adv_revisits.size() << " callbacks");
    for(const auto& vp : link_coerce) {
        const auto& v = *vp;
        //DEBUG(v);
        DEBUG("R" << v.rule_idx << " " << this->m_ivars.fmt_type(v.left_ty) << " := " << v.right_node_ptr << " " << &**v.right_node_ptr << " (" << this->m_ivars.fmt_type((*v.right_node_ptr)->m_res_type) << ")");
    }
    for(const auto& v : link_assoc) {
        DEBUG(v);
    }
    for(const auto& v : to_visit) {
        DEBUG(&*v << " " << FMT_CB(os, { ExprVisitor_Print ev(*this, os); v->visit(ev); }) << " -> " << this->m_ivars.fmt_type(v->m_res_type));
    }
    for(const auto& v : adv_revisits) {
        DEBUG(FMT_CB(ss, v->fmt(ss);));
    }
    DEBUG("---");
}

void Context::equate_types(const Span& sp, const ::HIR::TypeRef& li, const ::HIR::TypeRef& ri) {

    if( li == ri || this->m_ivars.get_type(li) == this->m_ivars.get_type(ri) ) {
        DEBUG(li << " == " << ri);
        return ;
    }

    // Instantly apply equality
    TRACE_FUNCTION_F(li << " == " << ri);

    ASSERT_BUG(sp, !type_contains_impl_placeholder(ri), "Type contained an impl placeholder parameter - " << ri);
    ASSERT_BUG(sp, !type_contains_impl_placeholder(li), "Type contained an impl placeholder parameter - " << li);

    ::HIR::TypeRef  l_tmp;
    ::HIR::TypeRef  r_tmp;
    const auto& l_t = this->m_resolve.expand_associated_types(sp, this->m_ivars.get_type(li), l_tmp);
    const auto& r_t = this->m_resolve.expand_associated_types(sp, this->m_ivars.get_type(ri), r_tmp);

    // Strip HRLs, just in case
    MonomorphEraseHrls().monomorph_type(sp, l_t);
    MonomorphEraseHrls().monomorph_type(sp, r_t);

    if( l_t.data().is_Diverge() && !r_t.data().is_Infer() ) {
        return ;
    }
    if( r_t.data().is_Diverge() && !l_t.data().is_Infer() ) {
        return;
    }

    equate_types_inner(sp, l_t, r_t);
}

void Context::equate_types_inner(const Span& sp, const ::HIR::TypeRef& li, const ::HIR::TypeRef& ri) {

    if( li == ri || this->m_ivars.get_type(li) == this->m_ivars.get_type(ri) ) {
        return ;
    }

    // Check if the type contains a replacable associated type
    ::HIR::TypeRef  l_tmp;
    ::HIR::TypeRef  r_tmp;
    const auto& l_t = (li.data().is_Infer() ? this->m_resolve.expand_associated_types(sp, this->m_ivars.get_type(li), l_tmp) : li);
    const auto& r_t = (ri.data().is_Infer() ? this->m_resolve.expand_associated_types(sp, this->m_ivars.get_type(ri), r_tmp) : ri);
    if( l_t == r_t ) {
        return ;
    }

    // If either side is still a UfcsKnown after `expand_associated_types`, then emit an assoc bound instead of damaging ivars
    if(const auto* r_e = r_t.data().opt_Path())
    {
        if(const auto* rpe = r_e->path.m_data.opt_UfcsKnown())
        {
            if( r_e->binding.is_Unbound() ) {
                this->equate_types_assoc(sp, l_t,  rpe->trait.m_path, rpe->trait.m_params.clone(), rpe->type,  rpe->item.c_str(), false);
                return ;
            }
        }
    }
    if(const auto* l_e = l_t.data().opt_Path())
    {
        if(const auto* lpe = l_e->path.m_data.opt_UfcsKnown())
        {
            if( l_e->binding.is_Unbound() ) {
                this->equate_types_assoc(sp, r_t,  lpe->trait.m_path, lpe->trait.m_params.clone(), lpe->type,  lpe->item.c_str(), false);
                return ;
            }
        }
    }

#if 1
    if( const auto* et = r_t.data().opt_ErasedType() )
    {
        if( const auto* ee = et->m_inner.opt_Alias() )
        {
            // HACK: Only propagate type information backwards if this isn't an ivar
            // - This logic seems to work, but isn't strictly speaking the right logic
            if( !l_t.data().is_Infer() && ee->inner->is_public_to(m_resolve.m_vis_path) ) {
                if( this->m_erased_type_aliases.count(ee->inner.get()) == 0 ) {
                    this->m_erased_type_aliases.insert(std::make_pair( ee->inner.get(), Context::TaitEntry { ee->params, l_t.clone() } ));
                }
                else {
                    equate_types_inner(sp, l_t, this->m_erased_type_aliases.at(ee->inner.get()).our_type);
                }
                return ;
            }
        }
    }
#endif
    if( const auto* et = l_t.data().opt_ErasedType() )
    {
        if( const auto* ee = et->m_inner.opt_Alias() )
        {
            if( ee->inner->is_public_to(m_resolve.m_vis_path) ) {
                if( this->m_erased_type_aliases.count(ee->inner.get()) == 0 ) {
                    this->m_erased_type_aliases.insert(std::make_pair( ee->inner.get(), Context::TaitEntry { ee->params, r_t.clone() } ));
                }
                else {
                    equate_types_inner(sp, this->m_erased_type_aliases.at(ee->inner.get()).our_type, r_t);
                }
                return ;
            }
        }
    }

    auto set_ivar = [&](const HIR::TypeRef& dst, const HIR::TypeRef& src) {
        auto ivar_idx = dst.data().as_Infer().index;
        if( ivar_idx < m_ivars_sized.size() && m_ivars_sized.at(ivar_idx) ) {
            this->require_sized(sp, src);
        }
        if( visit_ty_with(src, [&](const HIR::TypeRef& ity){ return ity == dst; }) ) {
            DEBUG("Start of a loop detected: rewrite");
            // Ensure that there's an unexpanded ATY in here (containing the ivar)
            // Replace the ATY with a new ivar
            // Equate this ivar with the updated type
            // Add an ATY equality rule for the new ivar
            // - `_0 = Ty< <_0 as Foo>::Type >`
            // becomes
            // - `_0 = Ty<_1>`
            // - `<_0 as Foo>::Type = _1`
            auto new_src = clone_ty_with(sp, src, [&](const HIR::TypeRef& tpl, HIR::TypeRef& out_ty)->bool {
                if( tpl.data().is_Path() && tpl.data().as_Path().binding.is_Unbound() ) {
                    if( visit_ty_with(src, [&](const HIR::TypeRef& ity){ return ity == dst; }) ) {
                        const auto& pe = tpl.data().as_Path().path.m_data.as_UfcsKnown();
                        out_ty = this->m_ivars.new_ivar_tr();
                        this->equate_types_assoc(sp, out_ty, pe.trait.m_path, pe.trait.m_params.clone(), pe.type, pe.item.c_str(), false);
                        return true;
                    }
                    else {
                    }
                }
                return false;
                });
            ASSERT_BUG(sp, !visit_ty_with(new_src, [&](const HIR::TypeRef& ity){ return ity == dst; }), "");
            this->m_ivars.set_ivar_to(ivar_idx, std::move(new_src));
        }
        else {
            this->m_ivars.set_ivar_to(ivar_idx, src.clone());
        }
        };

    DEBUG("- l_t = " << l_t << ", r_t = " << r_t);
    if(const auto* r_e = r_t.data().opt_Infer())
    {
        if(const auto* l_e = l_t.data().opt_Infer())
        {
            // If both are infer, unify the two ivars (alias right to point to left)
            // TODO: Unify sized flags

            if( (r_e->index < m_ivars_sized.size() && m_ivars_sized.at(r_e->index))
              ||(l_e->index < m_ivars_sized.size() && m_ivars_sized.at(l_e->index))
                )
            {
                this->require_sized(sp, l_t);
                this->require_sized(sp, r_t);
            }

            this->m_ivars.ivar_unify(l_e->index, r_e->index);
        }
        else {
            // Righthand side is infer, alias it to the left
            set_ivar(r_t, l_t);
        }
    }
    else
    {
        if(/*const auto* l_e =*/ l_t.data().opt_Infer())
        {
            // Lefthand side is infer, alias it to the right
            set_ivar(l_t, r_t);
        }
        else {
            // Helper function for Path and TraitObject
            auto equality_typeparams = [&](const ::HIR::PathParams& l, const ::HIR::PathParams& r) {
                    if( l.m_types.size() != r.m_types.size() ) {
                        ERROR(sp, E0000, "Type mismatch in path params (type count) `" << l << "` and `" << r << "`");
                    }
                    for(unsigned int i = 0; i < l.m_types.size(); i ++)
                    {
                        this->equate_types_inner(sp, l.m_types[i], r.m_types[i]);
                    }

                    if( l.m_values.size() != r.m_values.size() ) {
                        ERROR(sp, E0000, "Type mismatch in path params (value count) `" << l << "` and `" << r << "`");
                    }
                    for(unsigned int i = 0; i < l.m_values.size(); i ++)
                    {
                        this->equate_values(sp, l.m_values[i], r.m_values[i]);
                    }
                };
            auto equality_path = [&](const ::HIR::Path& l, const ::HIR::Path& r)->bool {
                if( l.m_data.tag() != r.m_data.tag() ) {
                    return false;
                }
                TU_MATCH_HDRA( (l.m_data, r.m_data), {)
                TU_ARMA(Generic, lpe, rpe) {
                    if( lpe.m_path != rpe.m_path ) {
                        return false;
                    }
                    equality_typeparams(lpe.m_params, rpe.m_params);
                    }
                TU_ARMA(UfcsInherent, lpe, rpe) {
                    equality_typeparams(lpe.params, rpe.params);
                    if( lpe.item != rpe.item )
                        return false;
                    this->equate_types_inner(sp, lpe.type, rpe.type);
                    }
                TU_ARMA(UfcsKnown, lpe, rpe) {
                    if( lpe.trait.m_path != rpe.trait.m_path || lpe.item != rpe.item )
                        return false;
                    equality_typeparams(lpe.trait.m_params, rpe.trait.m_params);
                    equality_typeparams(lpe.params, rpe.params);
                    this->equate_types_inner(sp, lpe.type, rpe.type);
                    }
                TU_ARMA(UfcsUnknown, lpe, rpe) {
                    // TODO: If the type is fully known, locate a suitable trait item
                    equality_typeparams(lpe.params, rpe.params);
                    if( lpe.item != rpe.item )
                        return false;
                    this->equate_types_inner(sp, lpe.type, rpe.type);
                    }
                }
                return true;
                };

            // If either side is !, return early
            // TODO: Should ! end up in an ivar?
            #if 1
            if( l_t.data().is_Diverge() && r_t.data().is_Diverge() ) {
                return ;
            }
            /*else if( l_t.data().is_Diverge() ) {
                if(const auto* l_e = li.data().opt_Infer()) {
                    this->m_ivars.set_ivar_to(l_e->index, r_t.clone());
                }
                return ;
            }*/
            else if( r_t.data().is_Diverge() ) {
                if(const auto* r_e = ri.data().opt_Infer()) {
                    this->m_ivars.set_ivar_to(r_e->index, l_t.clone());
                }
                return ;
            }
            else {
            }
            #else
            if( l_t.m_data.is_Diverge() || r_t.m_data.is_Diverge() ) {
                return ;
            }
            #endif

            if( l_t.data().tag() != r_t.data().tag() ) {
                ERROR(sp, E0000, "Type mismatch between " << this->m_ivars.fmt_type(l_t) << " and " << this->m_ivars.fmt_type(r_t));
            }
            TU_MATCH_HDRA( (l_t.data(), r_t.data()), {)
            TU_ARMA(Infer, l_e, r_e) {
                throw "";
                }
            TU_ARMA(Diverge, l_e, r_e) {
                // ignore?
                }
            TU_ARMA(Primitive, l_e, r_e) {
                if( l_e != r_e ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                }
            TU_ARMA(Path, l_e, r_e) {
                if( !equality_path(l_e.path, r_e.path) ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                }
            TU_ARMA(Generic, l_e, r_e) {
                if( l_e.binding != r_e.binding ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                }
            TU_ARMA(TraitObject, l_e, r_e) {
                if( l_e.m_trait.m_path.m_path != r_e.m_trait.m_path.m_path ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                equality_typeparams(l_e.m_trait.m_path.m_params, r_e.m_trait.m_path.m_params);
                for(auto it_l = l_e.m_trait.m_type_bounds.begin(), it_r = r_e.m_trait.m_type_bounds.begin(); it_l != l_e.m_trait.m_type_bounds.end(); it_l++, it_r++ ) {
                    if( it_l->first != it_r->first ) {
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - associated bounds differ");
                    }
                    this->equate_types_inner(sp, it_l->second.type, it_r->second.type);
                }
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
                }
            TU_ARMA(ErasedType, l_e, r_e) {
                if( l_e.m_inner.tag() != r_e.m_inner.tag() ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - different erased class");
                }
                TU_MATCH_HDRA( (l_e.m_inner, r_e.m_inner), {)
                TU_ARMA(Fcn, lee, ree) {
                    ASSERT_BUG(sp, lee.m_origin != ::HIR::SimplePath(), "ErasedType " << l_t << " wasn't bound to its origin");
                    ASSERT_BUG(sp, ree.m_origin != ::HIR::SimplePath(), "ErasedType " << r_t << " wasn't bound to its origin");
                    if( !equality_path(lee.m_origin, ree.m_origin) ) {
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - different source");
                    }
                    }
                TU_ARMA(Alias, lee, ree) {
                    if( lee.inner != ree.inner ) {
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - different source");
                    }
                    equality_typeparams(lee.params, ree.params);
                    }
                TU_ARMA(Known, lee, ree) {
                    equate_types_inner(sp, lee, ree);
                    }
                }
                }
            TU_ARMA(Array, l_e, r_e) {
                this->equate_types_inner(sp, l_e.inner, r_e.inner);
                if( l_e.size != r_e.size ) {
                    if( (l_e.size.is_Unevaluated() && l_e.size.as_Unevaluated().is_Infer()) || TU_TEST1(r_e.size, Unevaluated, .is_Infer()) ) {
                        // Handle one side being fully-known
                        if( !l_e.size.is_Unevaluated() ) {
                            assert(l_e.size.is_Known());
                            assert(r_e.size.is_Unevaluated());
                            this->equate_values(sp, HIR::EncodedLiteralPtr(EncodedLiteral::make_usize(l_e.size.as_Known())), r_e.size.as_Unevaluated());
                        }
                        else if( !r_e.size.is_Unevaluated() ) {
                            assert(l_e.size.is_Unevaluated());
                            assert(r_e.size.is_Known());
                            this->equate_values(sp, l_e.size.as_Unevaluated(), HIR::EncodedLiteralPtr(EncodedLiteral::make_usize(r_e.size.as_Known())));
                        }
                        else {
                            this->equate_values(sp, l_e.size.as_Unevaluated(), r_e.size.as_Unevaluated());
                        }
                    }
                    else {
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - sizes differ");
                    }
                }
                }
            TU_ARMA(Slice, l_e, r_e) {
                this->equate_types_inner(sp, l_e.inner, r_e.inner);
                }
            TU_ARMA(Tuple, l_e, r_e) {
                if( l_e.size() != r_e.size() ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - Tuples are of different length");
                }
                for(unsigned int i = 0; i < l_e.size(); i ++)
                {
                    this->equate_types_inner(sp, l_e[i], r_e[i]);
                }
                }
            TU_ARMA(Borrow, l_e, r_e) {
                if( l_e.type != r_e.type ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - Borrow classes differ");
                }
                this->equate_types_inner(sp, l_e.inner, r_e.inner);
                }
            TU_ARMA(Pointer, l_e, r_e) {
                if( l_e.type != r_e.type ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - Pointer mutability differs");
                }
                this->equate_types_inner(sp, l_e.inner, r_e.inner);
                }
            TU_ARMA(NamedFunction, l_e, r_e) {
                if( !equality_path(l_e.path, r_e.path) ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                }
            TU_ARMA(Function, l_e, r_e) {
                if( l_e.is_unsafe != r_e.is_unsafe
                    || l_e.m_abi != r_e.m_abi
                    || l_e.m_arg_types.size() != r_e.m_arg_types.size()
                    )
                {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                // TODO: HRLs
                this->equate_types_inner(sp, l_e.m_rettype, r_e.m_rettype);
                for(unsigned int i = 0; i < l_e.m_arg_types.size(); i ++ ) {
                    this->equate_types_inner(sp, l_e.m_arg_types[i], r_e.m_arg_types[i]);
                }
                }
            TU_ARMA(Closure, l_e, r_e) {
                if( l_e.node != r_e.node ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                }
            TU_ARMA(Generator, l_e, r_e) {
                if( l_e.node != r_e.node )
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
            }
        }
    }
}

void Context::equate_values(const Span& sp, const ::HIR::ConstGeneric& rl, const ::HIR::ConstGeneric& rr)
{
    const auto& l = this->m_ivars.get_value(rl);
    const auto& r = this->m_ivars.get_value(rr);
    if( l != r ) {
        DEBUG(l << " != " << r);
        if(l.is_Infer()) {
            if(r.is_Infer()) {
                // Unify ivars
                this->m_ivars.ivar_val_unify(l.as_Infer().index, r.as_Infer().index);
            }
            else {
                this->m_ivars.set_ivar_val_to(l.as_Infer().index, r.clone());
            }
        }
        else {
            if(r.is_Infer()) {
                this->m_ivars.set_ivar_val_to(r.as_Infer().index, l.clone());
            }
            else {
                // TODO: What about unevaluated values due to type inference?
                ERROR(sp, E0000, "Value mismatch between " << l << " and " << r);
            }
        }
    }
    else {
        DEBUG(l << " == " << r);
    }
}

void Context::add_binding_inner(const Span& sp, const ::HIR::PatternBinding& pb, ::HIR::TypeRef type)
{
    assert( pb.is_valid() );
    switch( pb.m_type )
    {
    case ::HIR::PatternBinding::Type::Move:
        this->add_var( sp, pb.m_slot, pb.m_name, mv$(type) );
        break;
    case ::HIR::PatternBinding::Type::Ref:
        this->add_var( sp, pb.m_slot, pb.m_name, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, mv$(type)) );
        break;
    case ::HIR::PatternBinding::Type::MutRef:
        this->add_var( sp, pb.m_slot, pb.m_name, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Unique, mv$(type)) );
        break;
    }
}

// NOTE: Mutates the pattern to add ivars to contained paths
void Context::handle_pattern(const Span& sp, ::HIR::Pattern& pat, const ::HIR::TypeRef& type)
{
    TRACE_FUNCTION_F("pat = " << pat << ", type = " << type);

    // TODO: 1.29 includes "match ergonomics" which allows automatic insertion of borrow/deref when matching
    // - Handling this will make pattern matching slightly harder (all patterns needing revisist)
    // - BUT: New bindings will still be added as usualin this pass.
    // - Any use of `&` (or `ref`?) in the pattern disables match ergonomics for the entire pattern.
    //   - Does `box` also do this disable?
    //
    //
    // - Add a counter to each pattern indicting how many implicit borrows/derefs are applied.
    // - When this function is called, check if the pattern is eligable for pattern auto-ref/deref
    // - Detect if the pattern uses & or ref. If it does, then invoke the existing code
    // - Otherwise, register a revisit for the pattern

    // 1. Determine if this pattern can apply auto-ref/deref
    if( pat.m_data.is_Any() ) {
        // `_` pattern, no destructure/match, so no auto-ref/deref
        // - TODO: Does this do auto-borrow too?
        for(const auto& pb : pat.m_bindings) {
            this->add_binding_inner(sp, pb, type.clone());
        }
        return ;
    }

    // NOTE: Even if the top-level is a binding, and even if the top-level type is fully known, match ergonomics
    // still applies.
    if( TARGETVER_LEAST_1_29 ) {
        // There's not a `&` or `ref` in the pattern, and we're targeting 1.29
        // - Run the match ergonomics handler
        // TODO: Default binding mode can be overridden back to "move" with `mut`

        struct MatchErgonomicsRevisit:
            public Revisitor
        {
            Span    sp;
            ::HIR::TypeRef  m_outer_ty;
            ::HIR::Pattern& m_pattern;
            ::HIR::PatternBinding::Type m_outer_mode;

            mutable ::std::vector<::HIR::TypeRef>   m_temp_ivars;
            mutable ::HIR::TypeRef  m_possible_type;

            MatchErgonomicsRevisit(Span sp, ::HIR::TypeRef outer, ::HIR::Pattern& pat, ::HIR::PatternBinding::Type binding_mode=::HIR::PatternBinding::Type::Move):
                sp(mv$(sp)), m_outer_ty(mv$(outer)),
                m_pattern(pat),
                m_outer_mode(binding_mode)
            {}

            const Span& span() const override {
                return sp;
            }
            void fmt(::std::ostream& os) const override {
                os << "MatchErgonomicsRevisit { " << m_pattern << " : " << m_outer_ty << " }";
            }
            bool revisit(Context& context, bool is_fallback_mode) override {
                TRACE_FUNCTION_F("Match ergonomics - " << m_pattern << " : " << m_outer_ty << (is_fallback_mode ? " (fallback)": ""));
                m_outer_ty = context.m_resolve.expand_associated_types(sp, mv$(m_outer_ty));
                return this->revisit_inner_real(context, m_pattern, m_outer_ty, m_outer_mode, is_fallback_mode);
            }
            // TODO: Recurse into inner patterns, creating new revisitors?
            // - OR, could just recurse on it.
            // 
            // Recusring incurs costs on every iteration, but is less expensive the first time around
            // New revisitors are cheaper when inferrence takes multiple iterations, but takes longer first time.
            bool revisit_inner(Context& context, ::HIR::Pattern& pattern, const ::HIR::TypeRef& type, ::HIR::PatternBinding::Type binding_mode) const
            {
                if( !revisit_inner_real(context, pattern, type, binding_mode, false) )
                {
                    DEBUG("Add revisit for " << pattern << " : " << type << "(mode = " << (int)binding_mode << ")");
                    context.add_revisit_adv( box$(( MatchErgonomicsRevisit { sp, type.clone(), pattern, binding_mode } )) );
                }
                return true;
            }
            ::HIR::TypeRef get_possible_type_val(Context& context, ::HIR::Pattern::Value& pv) const
            {
                TU_MATCH_HDR( (pv), {)
                TU_ARM(pv, Integer, ve) {
                    if( ve.type == ::HIR::CoreType::Str ) {
                        return context.m_ivars.new_ivar_tr(::HIR::InferClass::Integer);
                    }
                    return ve.type;
                    }
                TU_ARM(pv, Float, ve) {
                    if( ve.type == ::HIR::CoreType::Str ) {
                        return context.m_ivars.new_ivar_tr(::HIR::InferClass::Float);
                    }
                    return ve.type;
                    }
                TU_ARM(pv, String, ve) {
                    return ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, ::HIR::CoreType::Str);
                    }
                TU_ARM(pv, ByteString, ve) {
                    // TODO: ByteString patterns can match either &[u8] or &[u8; N]
#if 0
                    return ::HIR::TypeRef::new_borrow(
                            ::HIR::BorrowType::Shared,
                            ::HIR::TypeRef::new_slice(::HIR::CoreType::U8)
                            );
#else
                    return ::HIR::TypeRef();
#endif
                    }
                TU_ARM(pv, Named, ve) {
                    DEBUG("TODO: Look up the path and get the type: " << ve.path);
                    if( ve.binding ) {
                        return ve.binding->m_type.clone();
                    }
                    else if( ve.path.m_data.is_Generic() ) {
                        TODO(sp, "Look up pattern value: " << ve.path);
                    }
                    else {
                        return ::HIR::TypeRef();
                    }
                    }
                }
                throw "";
            }
            ::HIR::TypeRef get_possible_type_inner(Context& context, ::HIR::Pattern& pattern) const
            {
                ::HIR::TypeRef  possible_type;
                // Get a potential type from the pattern, and set as a possibility.
                // - Note, this is only if no derefs were applied
                TU_MATCH_HDR( (pattern.m_data), { )
                TU_ARM(pattern.m_data, Any, pe) {
                    // No type information.
                    }
                TU_ARM(pattern.m_data, Value, pe) {
                    possible_type = get_possible_type_val(context, pe.val);
                    }
                TU_ARM(pattern.m_data, Range, pe) {
                    if(pe.start)    possible_type = get_possible_type_val(context, *pe.start);
                    if( possible_type == ::HIR::TypeRef() ) {
                        if(pe.end)  possible_type = get_possible_type_val(context, *pe.end);
                    }
                    else {
                        // TODO: Check that the type from .end matches .start
                    }
                    }
                TU_ARM(pattern.m_data, Box, pe) {
                    // TODO: Get type info (Box<_>) ?
                    // - Is this possible? Shouldn't a box pattern disable ergonomics?
                    }
                TU_ARM(pattern.m_data, Ref, pe) {
                    BUG(sp, "Match ergonomics - & pattern");
                    }
                TU_ARM(pattern.m_data, Tuple, e) {
                    // Get type info `(T, U, ...)`
                    if( m_temp_ivars.size() != e.sub_patterns.size() ) {
                        for(size_t i = 0; i < e.sub_patterns.size(); i ++)
                            m_temp_ivars.push_back( context.m_ivars.new_ivar_tr() );
                    }
                    decltype(m_temp_ivars)  tuple;
                    for(const auto& ty : m_temp_ivars)
                        tuple.push_back(ty.clone());
                    possible_type = ::HIR::TypeRef( ::std::move(tuple) );
                    }
                TU_ARM(pattern.m_data, SplitTuple, pe) {
                    // Can't get type information, tuple size is unkown
                    }
                TU_ARM(pattern.m_data, Slice, e) {
                    // Can be either a [T] or [T; n]. Can't provide a hint
                    // - Can provide the hint if not behind a borrow.
                    }
                TU_ARM(pattern.m_data, SplitSlice, pe) {
                    // Can be either a [T] or [T; n]. Can't provide a hint
                    }
                TU_ARM(pattern.m_data, PathValue, e) {
                    TU_MATCH_HDRA( (e.binding), {)
                    TU_ARMA(Unbound, _)
                        BUG(sp, "");
                    TU_ARMA(Struct, be) {
                        auto& p = e.path.m_data.as_Generic();
                        assert(be);
                        context.add_ivars_params( p.m_params );
                        possible_type = ::HIR::TypeRef::new_path(p.clone(), ::HIR::TypePathBinding(be));
                        }
                    TU_ARMA(Enum, be) {
                        auto& p = e.path.m_data.as_Generic();
                        assert(be.ptr);
                        context.add_ivars_params( p.m_params );
                        possible_type = ::HIR::TypeRef::new_path(get_parent_path(p), ::HIR::TypePathBinding(be.ptr));
                        }
                    }
                    }
                TU_ARM(pattern.m_data, PathTuple, e) {
                    TU_MATCH_HDRA( (e.binding), {)
                    TU_ARMA(Unbound, _)
                        BUG(sp, "");
                    TU_ARMA(Struct, be) {
                        auto& p = e.path.m_data.as_Generic();
                        assert(be);
                        context.add_ivars_params( p.m_params );
                        possible_type = ::HIR::TypeRef::new_path(p.clone(), ::HIR::TypePathBinding(be));
                        }
                    TU_ARMA(Enum, be) {
                        auto& p = e.path.m_data.as_Generic();
                        assert(be.ptr);
                        context.add_ivars_params( p.m_params );
                        possible_type = ::HIR::TypeRef::new_path(get_parent_path(p), ::HIR::TypePathBinding(be.ptr));
                        }
                    }
                    }
                TU_ARM(pattern.m_data, PathNamed, e) {
                    TU_MATCH_HDRA( (e.binding), {)
                    TU_ARMA(Unbound, _)
                        BUG(sp, "");
                    TU_ARMA(Struct, be) {
                        auto& p = e.path.m_data.as_Generic();
                        assert(be);
                        context.add_ivars_params( p.m_params );
                        possible_type = ::HIR::TypeRef::new_path(p.clone(), ::HIR::TypePathBinding(be));
                        }
                    TU_ARMA(Enum, be) {
                        auto& p = e.path.m_data.as_Generic();
                        assert(be.ptr);
                        context.add_ivars_params( p.m_params );
                        possible_type = ::HIR::TypeRef::new_path(get_parent_path(p), ::HIR::TypePathBinding(be.ptr));
                        }
                    }
                    }
            
                TU_ARM(pattern.m_data, Or, e) {
                    for(auto& subpat : e) {
                        possible_type = get_possible_type_inner(context, subpat);
                        if( possible_type != HIR::TypeRef() )
                            break ;
                    }
                    }
                }
                return possible_type;
            }
            const ::HIR::TypeRef& get_possible_type(Context& context, ::HIR::Pattern& pattern) const
            {
                if( m_possible_type == ::HIR::TypeRef() )
                {
                    m_possible_type = get_possible_type_inner(context, pattern);
                }
                return m_possible_type;
            }
            bool revisit_inner_real(Context& context, ::HIR::Pattern& pattern, const ::HIR::TypeRef& type, ::HIR::PatternBinding::Type binding_mode, bool is_fallback) const
            {
                TRACE_FUNCTION_F(pattern << " : " << type);

                // Binding applies to the raw input type (not after dereferencing)
                for(auto& pb : pattern.m_bindings )
                {
                    // - Binding present, use the current binding mode
                    if( pb.m_type == ::HIR::PatternBinding::Type::Move && !pb.m_mutable )
                    {
                        pb.m_type = binding_mode;
                    }
                    ::HIR::TypeRef  tmp;
                    const ::HIR::TypeRef* binding_type = nullptr;
                    switch(pb.m_type)
                    {
                    case ::HIR::PatternBinding::Type::Move:
                        binding_type = &type;
                        break;
                    case ::HIR::PatternBinding::Type::MutRef:
                        // NOTE: Needs to deref and borrow to get just `&mut T` (where T isn't a &mut T)
                        binding_type = &(tmp = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Unique, type.clone()));
                        break;
                    case ::HIR::PatternBinding::Type::Ref:
                        // NOTE: Needs to deref and borrow to get just `&mut T` (where T isn't a &mut T)
                        binding_type = &(tmp = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, type.clone()));
                        break;
                    default:
                        TODO(sp, "Assign variable type using mode " << (int)binding_mode << " and " << type);
                    }
                    assert(binding_type);
                    context.equate_types(sp, context.get_var(sp, pb.m_slot), *binding_type);
                }

                // For `_` patterns, there's nothing to match, so they just succeed with no derefs
                if( pattern.m_data.is_Any() )
                {
                    return true;
                }

                if( auto* pe = pattern.m_data.opt_Ref() )
                {
                    // Require a &-ptr (hard requirement), then visit sub-pattern
                    auto inner_ty = context.m_ivars.new_ivar_tr();
                    auto new_ty = ::HIR::TypeRef::new_borrow( pe->type, inner_ty.clone() );
                    context.equate_types(sp, type, new_ty);

                    return this->revisit_inner( context, *pe->sub, inner_ty, ::HIR::PatternBinding::Type::Move );
                }
                if(auto* pe = pattern.m_data.opt_Or())
                {
                    bool rv = true;
                    for(auto& subpat : *pe)
                    {
                        rv &= this->revisit_inner(context, subpat, type, binding_mode);
                    }
                    return rv;
                }

                // If the type is a borrow, then count derefs required for the borrow
                // - If the first non-borrow inner is an ivar, return false
                unsigned n_deref = 0;
                ::HIR::BorrowType   bt = ::HIR::BorrowType::Owned;
                const auto* ty_p = &context.get_type(type);
                while( const auto* te = ty_p->data().opt_Borrow() ) {
                    DEBUG("bt " << bt << ", " << te->type);
                    bt = ::std::min(bt, te->type);
                    ty_p = &context.get_type( te->inner );
                    n_deref ++;
                }
                DEBUG("- " << n_deref << " derefs of class " << bt << " to get " << *ty_p);
                if( ty_p->data().is_Infer() || TU_TEST1(ty_p->data(), Path, .binding.is_Unbound()) )
                {
                    // Still pure infer, can't do anything
                    // - What if it's a literal?

                    // TODO: Don't do fallback if the ivar is marked as being hard blocked
                    if( const auto* te = ty_p->data().opt_Infer() )
                    {
                        if( te->index < context.possible_ivar_vals.size()
                            && context.possible_ivar_vals[te->index].force_disable
                            )
                        {
                            MatchErgonomicsRevisit::disable_possibilities_on_bindings(sp, context, pattern);
                            return false;
                        }
                    }

                    // If there's no dereferences done, then add a possible unsize type
                    const ::HIR::TypeRef& possible_type = get_possible_type(context, pattern);
                    if( possible_type != ::HIR::TypeRef() )
                    {
                        DEBUG("n_deref = " << n_deref << ", possible_type = " << possible_type);
                        const ::HIR::TypeRef* possible_type_p = &possible_type;
                        // Unwrap borrows as many times as we've already dereferenced
                        for(size_t i = 0; i < n_deref && possible_type_p; i ++) {
                            if( const auto* te = possible_type_p->data().opt_Borrow() ) {
                                possible_type_p = &te->inner;
                            }
                            else {
                                possible_type_p = nullptr;
                            }
                        }
                        if( possible_type_p )
                        {
                            const auto& possible_type = *possible_type_p;
                            if( const auto* te = ty_p->data().opt_Infer() )
                            {
                                context.possible_equate_ivar(sp, te->index, possible_type, Context::PossibleTypeSource::UnsizeTo);
                            }
                            else if( is_fallback )
                            {
                                DEBUG("Fallback equate " << possible_type);
                                context.equate_types(sp, *ty_p, possible_type);
                            }
                            else
                            {
                            }
                        }
                    }

                    // Visit all inner bindings and disable coercion fallbacks on them.
                    MatchErgonomicsRevisit::disable_possibilities_on_bindings(sp, context, pattern, /*is_top_level=*/true);
                    return false;
                }
                if( ty_p->data().is_Primitive() && ty_p->data().as_Primitive() == HIR::CoreType::Str ) {
                    // Can't match on `str`, so unwrap it?
                    // - Unwrapping happens in Pattern::Value handling
                }
                const auto& ty = *ty_p;

                // Here we have a known type and binding mode for this pattern
                // - Time to handle this pattern then recurse into sub-patterns

                // Store the deref count in the pattern.
                pattern.m_implicit_deref_count = n_deref;
                // Determine the new binding mode from the borrow type
                switch(bt)
                {
                case ::HIR::BorrowType::Owned:
                    // No change
                    break;
                case ::HIR::BorrowType::Unique:
                    switch(binding_mode)
                    {
                    case ::HIR::PatternBinding::Type::Move:
                    case ::HIR::PatternBinding::Type::MutRef:
                        binding_mode = ::HIR::PatternBinding::Type::MutRef;
                        break;
                    case ::HIR::PatternBinding::Type::Ref:
                        // No change
                        break;
                    }
                    break;
                case ::HIR::BorrowType::Shared:
                    binding_mode = ::HIR::PatternBinding::Type::Ref;
                    break;
                }

                bool rv = false;
                TU_MATCH_HDR( (pattern.m_data), { )
                TU_ARM(pattern.m_data, Ref, pe) {
                    BUG(sp, "Match ergonomics - `&` pattern already handled");
                    }
                TU_ARM(pattern.m_data, Or, pe) {
                    BUG(sp, "Match ergonomics - `|` pattern already handled");
                    }
                TU_ARM(pattern.m_data, Any, pe) {
                    // no-op
                    rv = true;
                    }
                TU_ARM(pattern.m_data, Value, pe) {
                    // no-op?
                    if( pe.val.is_Named() ) {
                        // TODO: If the value is a borrow, then unwind borrows.
                        ASSERT_BUG(sp, pe.val.as_Named().binding, pattern);
                        //const auto& cval = pe.val.as_Named().binding->m_value_res;
                        const auto& ty = pe.val.as_Named().binding->m_type;
                        if( ty.data().is_Borrow() ) {
                            ASSERT_BUG(sp, pattern.m_implicit_deref_count >= 1, "");
                            pattern.m_implicit_deref_count -= 1;
                        }
                    }
                    else if( pe.val.is_String() || pe.val.is_ByteString() ) {
                        ASSERT_BUG(sp, pattern.m_implicit_deref_count >= 1, "");
                        pattern.m_implicit_deref_count -= 1;
                    }
                    rv = true;
                    }
                TU_ARM(pattern.m_data, Range, pe) {
                    // no-op?
                    rv = true;
                    }
                TU_ARM(pattern.m_data, Box, pe) {
                    // Box<T>
                    if( TU_TEST2(ty.data(), Path, .path.m_data, Generic, .m_path == context.m_lang_Box) )
                    {
                        const auto& path = ty.data().as_Path().path.m_data.as_Generic();
                        const auto& inner = path.m_params.m_types.at(0);
                        rv = this->revisit_inner(context, *pe.sub, inner, binding_mode);
                    }
                    else
                    {
                        TODO(sp, "Match ergonomics - box pattern - Non Box<T> type: " << ty);
                        //auto inner = this->m_ivars.new_ivar_tr();
                        //this->handle_pattern_direct_inner(sp, *e.sub, inner);
                        //::HIR::GenericPath  path { m_lang_Box, ::HIR::PathParams(mv$(inner)) };
                        //this->equate_types( sp, type, ::HIR::TypeRef::new_path(mv$(path), ::HIR::TypePathBinding(&m_crate.get_struct_by_path(sp, m_lang_Box))) );
                    }
                    }
                TU_ARM(pattern.m_data, Tuple, e) {
                    if( !ty.data().is_Tuple() ) {
                        ERROR(sp, E0000, "Matching a non-tuple with a tuple pattern - " << ty);
                    }
                    const auto& te = ty.data().as_Tuple();
                    if( e.sub_patterns.size() != te.size() ) {
                        ERROR(sp, E0000, "Tuple pattern with an incorrect number of fields, expected " << e.sub_patterns.size() << "-tuple, got " << ty);
                    }

                    rv = true;
                    for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                        rv &= this->revisit_inner(context, e.sub_patterns[i], te[i], binding_mode);
                    }
                TU_ARM(pattern.m_data, SplitTuple, pe) {
                    if( !ty.data().is_Tuple() ) {
                        ERROR(sp, E0000, "Matching a non-tuple with a tuple pattern - " << ty);
                    }
                    const auto& te = ty.data().as_Tuple();
                    if( pe.leading.size() + pe.trailing.size() > te.size() ) {
                        ERROR(sp, E0000, "Split-tuple pattern with an incorrect number of fields, expected at most " << (pe.leading.size() + pe.trailing.size()) << "-tuple, got " << te.size());
                    }
                    pe.total_size = te.size();
                    rv = true;
                    for(size_t i = 0; i < pe.leading.size(); i++)
                        rv &= this->revisit_inner(context, pe.leading[i], te[i], binding_mode);
                    for(size_t i = 0; i < pe.trailing.size(); i++)
                        rv &= this->revisit_inner(context, pe.trailing[i], te[te.size() - pe.trailing.size() + i], binding_mode);
                    }
                TU_ARM(pattern.m_data, Slice, e) {
                    const ::HIR::TypeRef*   slice_inner;
                    if(const auto* te = ty.data().opt_Slice()) {
                        slice_inner = &te->inner;
                    }
                    else if(const auto* te = ty.data().opt_Array() ) {
                        slice_inner = &te->inner;
                        // Equate the array size
                        context.equate_types(sp, ty, ::HIR::TypeRef::new_array(slice_inner->clone(), e.sub_patterns.size()));
                    }
                    else {
                        ERROR(sp, E0000, "Matching a non-array/slice with a slice pattern - " << ty);
                    }
                    rv = true;
                    for(auto& sub : e.sub_patterns)
                        rv |= this->revisit_inner(context, sub, *slice_inner, binding_mode);
                    }
                TU_ARM(pattern.m_data, SplitSlice, pe) {
                    const ::HIR::TypeRef*   slice_inner;
                    if(const auto* te = ty.data().opt_Slice()) {
                        slice_inner = &te->inner;
                    }
                    else if(const auto* te = ty.data().opt_Array() ) {
                        slice_inner = &te->inner;
                    }
                    else {
                        ERROR(sp, E0000, "Matching a non-array/slice with a slice pattern - " << ty);
                    }
                    rv = true;
                    for(auto& sub : pe.leading)
                        rv |= this->revisit_inner(context, sub, *slice_inner, binding_mode);
                    if(pe.extra_bind.is_valid())
                    {
                        ::HIR::TypeRef  binding_ty_inner = ::HIR::TypeRef::new_slice( slice_inner->clone() );
                        // TODO: Do arrays get bound as arrays?
                        if( ty.data().is_Array() ) {
                            size_t size_sub = pe.leading.size() + pe.trailing.size();
                            binding_ty_inner = ::HIR::TypeRef::new_array(slice_inner->clone(), ty.data().as_Array().size.as_Known() - size_sub);
                            //TODO(sp, "SplitSlice extra bind with array: " << pe.extra_bind << " on " << ty);
                        }
                        ::HIR::TypeRef  binding_ty;
                        if( pe.extra_bind.m_type == ::HIR::PatternBinding::Type::Move )
                        {
                            pe.extra_bind.m_type = binding_mode;
                        }
                        switch(pe.extra_bind.m_type)
                        {
                        case ::HIR::PatternBinding::Type::Move:
                            // Only valid for an array?
                            ASSERT_BUG(sp, ty.data().is_Array(), "Non-array SplitSlize move bind");
                            binding_ty = mv$(binding_ty_inner);
                            break;
                        case ::HIR::PatternBinding::Type::Ref:
                            binding_ty = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, mv$(binding_ty_inner));
                            break;
                        case ::HIR::PatternBinding::Type::MutRef:
                            binding_ty = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Unique, mv$(binding_ty_inner));
                            break;
                        }
                        context.equate_types(sp, context.get_var(sp, pe.extra_bind.m_slot), binding_ty);
                    }
                    for(auto& sub : pe.trailing)
                        rv |= this->revisit_inner(context, sub, *slice_inner, binding_mode);
                    }
                TU_ARM(pattern.m_data, PathValue, e) {
                    context.equate_types( sp, ty, get_possible_type_inner(context, pattern) );
                    
                    TU_MATCH_HDRA( (e.binding), { )
                    TU_ARMA(Unbound, be)    throw "";
                    TU_ARMA(Struct, be) {
                        const auto& str = *be;
                        ASSERT_BUG(sp, str.m_data.is_Unit(), "PathValue used on non-unit struct variant");
                        }
                    TU_ARMA(Enum, be) {
                        const auto& enm = *be.ptr;
                        if( const auto* ee = enm.m_data.opt_Data() )
                        {
                            ASSERT_BUG(sp, be.var_idx < ee->size(), "");
                            const auto& var = (*ee)[be.var_idx];
                            ASSERT_BUG(sp, var.type == HIR::TypeRef::new_unit(), "EnumValue used on non-value enum variant");
                        }
                        }
                    }
                    rv = true;
                    }
                TU_ARM(pattern.m_data, PathTuple, e) {
                    context.equate_types( sp, ty, get_possible_type_inner(context, pattern) );
                    
                    const auto& sd = ::HIR::pattern_get_tuple(sp, e.path, e.binding);

                    auto ms = MonomorphStatePtr(nullptr, &e.path.m_data.as_Generic().m_params, nullptr);
                    ::HIR::TypeRef  tmp;
                    auto maybe_monomorph = [&](const ::HIR::TypeRef& field_type)->const ::HIR::TypeRef& {
                        return (monomorphise_type_needed(field_type)
                                ? (tmp = context.m_resolve.expand_associated_types(sp, ms.monomorph_type(sp, field_type)))
                                : field_type
                                );
                        };

                    e.total_size = sd.size();

                    rv = true;
                    for( unsigned int i = 0; i < e.leading.size(); i ++ )
                    {
                        /*const*/ auto& sub_pat = e.leading[i];
                        const auto& var_ty = maybe_monomorph(sd[i].ent);
                        rv &= this->revisit_inner(context, sub_pat, var_ty, binding_mode);
                    }
                    for( unsigned int i = 0; i < e.trailing.size(); i ++ )
                    {
                        /*const*/ auto& sub_pat = e.trailing[i];
                        const auto& var_ty = maybe_monomorph(sd[sd.size() - e.trailing.size() + i].ent);
                        rv &= this->revisit_inner(context, sub_pat, var_ty, binding_mode);
                    }
                    }
                TU_ARM(pattern.m_data, PathNamed, e) {
                    context.equate_types( sp, ty, get_possible_type_inner(context, pattern) );

                    //if( ! e.is_wildcard() )
                    if( e.sub_patterns.empty() )
                    {
                        // TODO: Check the field count?
                        rv = true;
                    }
                    else
                    {
                        const auto& sd = ::HIR::pattern_get_named(sp, e.path, e.binding);

                        auto ms = MonomorphStatePtr(nullptr, &e.path.m_data.as_Generic().m_params, nullptr);
                        ::HIR::TypeRef  tmp;
                        auto maybe_monomorph = [&](const ::HIR::TypeRef& field_type)->const ::HIR::TypeRef& {
                            return (monomorphise_type_needed(field_type)
                                    ? (tmp = context.m_resolve.expand_associated_types(sp, ms.monomorph_type(sp, field_type)))
                                    : field_type
                                    );
                            };

                        rv = true;
                        for( auto& field_pat : e.sub_patterns )
                        {
                            unsigned int f_idx = ::std::find_if( sd.begin(), sd.end(), [&](const auto& x){ return x.first == field_pat.first; } ) - sd.begin();
                            if( f_idx == sd.size() ) {
                                ERROR(sp, E0000, "Struct " << e.path << " doesn't have a field " << field_pat.first);
                            }
                            const ::HIR::TypeRef& field_type = maybe_monomorph(sd[f_idx].second.ent);
                            rv &= this->revisit_inner(context, field_pat.second, field_type, binding_mode);
                        }
                    }
                    }
                }
                return rv;
            }

            static void disable_possibilities_on_bindings(const Span& sp, Context& context, const ::HIR::Pattern& pat, bool is_top_level=false)
            {
                if( !is_top_level ) {
                    for(const auto& pb : pat.m_bindings ) {
                        context.possible_equate_type_unknown(sp, context.get_var(sp, pb.m_slot), Context::IvarUnknownType::Bound);
                    }
                }
                TU_MATCH_HDRA( (pat.m_data), {)
                TU_ARMA(Any, e) {
                    }
                TU_ARMA(Value, e) {
                    }
                TU_ARMA(Range, e) {
                    }
                TU_ARMA(Box, e) {
                    disable_possibilities_on_bindings(sp, context, *e.sub);
                    }
                TU_ARMA(Ref, e) {
                    disable_possibilities_on_bindings(sp, context, *e.sub);
                    }
                TU_ARMA(Tuple, e) {
                    for(auto& subpat : e.sub_patterns)
                        disable_possibilities_on_bindings(sp, context, subpat);
                    }
                TU_ARMA(SplitTuple, e) {
                    for(auto& subpat : e.leading) {
                        disable_possibilities_on_bindings(sp, context, subpat);
                    }
                    for(auto& subpat : e.trailing) {
                        disable_possibilities_on_bindings(sp, context, subpat);
                    }
                    }
                TU_ARMA(Slice, e) {
                    for(auto& sub : e.sub_patterns)
                        disable_possibilities_on_bindings(sp, context, sub);
                    }
                TU_ARMA(SplitSlice, e) {
                    for(auto& sub : e.leading)
                        disable_possibilities_on_bindings(sp, context, sub);
                    if( e.extra_bind.is_valid() ) {
                        context.possible_equate_type_unknown(sp, context.get_var(sp, e.extra_bind.m_slot), Context::IvarUnknownType::Bound);
                    }
                    for(auto& sub : e.trailing)
                        disable_possibilities_on_bindings(sp, context, sub);
                    }

                // - Enums/Structs
                TU_ARMA(PathValue, e) {
                    }
                TU_ARMA(PathTuple, e) {
                    for(auto& subpat : e.leading)
                        disable_possibilities_on_bindings(sp, context, subpat);
                    for(auto& subpat : e.trailing)
                        disable_possibilities_on_bindings(sp, context, subpat);
                    }
                TU_ARMA(PathNamed, e) {
                    for(auto& field_pat : e.sub_patterns)
                        disable_possibilities_on_bindings(sp, context, field_pat.second);
                    }
            
                TU_ARMA(Or, e) {
                    for(auto& subpat : e)
                        disable_possibilities_on_bindings(sp, context, subpat);
                    }
                }
            }
            static void create_bindings(const Span& sp, Context& context, ::HIR::Pattern& pat)
            {
                for(const auto& pb : pat.m_bindings ) {
                    context.add_var( sp, pb.m_slot, pb.m_name, context.m_ivars.new_ivar_tr() );
                    // TODO: Ensure that there's no more bindings below this?
                    // - I'll leave the option open, MIR generation should handle cases where there's multiple borrows
                    //   or moves.
                }
                TU_MATCH_HDRA( (pat.m_data), { )
                TU_ARMA(Any, e) {
                    }
                TU_ARMA(Value, e) {
                    }
                TU_ARMA(Range, e) {
                    }
                TU_ARMA(Box, e) {
                    create_bindings(sp, context, *e.sub);
                    }
                TU_ARMA(Ref, e) {
                    create_bindings(sp, context, *e.sub);
                    }
                TU_ARMA(Tuple, e) {
                    for(auto& subpat : e.sub_patterns)
                        create_bindings(sp, context, subpat);
                    }
                TU_ARMA(SplitTuple, e) {
                    for(auto& subpat : e.leading) {
                        create_bindings(sp, context, subpat);
                    }
                    for(auto& subpat : e.trailing) {
                        create_bindings(sp, context, subpat);
                    }
                    }
                TU_ARMA(Slice, e) {
                    for(auto& sub : e.sub_patterns)
                        create_bindings(sp, context, sub);
                    }
                TU_ARMA(SplitSlice, e) {
                    for(auto& sub : e.leading)
                        create_bindings(sp, context, sub);
                    if( e.extra_bind.is_valid() ) {
                        const auto& pb = e.extra_bind;
                        context.add_var( sp, pb.m_slot, pb.m_name, context.m_ivars.new_ivar_tr() );
                    }
                    for(auto& sub : e.trailing)
                        create_bindings(sp, context, sub);
                    }

                // - Enums/Structs
                TU_ARMA(PathValue, e) {
                    }
                TU_ARMA(PathTuple, e) {
                    for(auto& subpat : e.leading)
                        create_bindings(sp, context, subpat);
                    for(auto& subpat : e.trailing)
                        create_bindings(sp, context, subpat);
                    }
                TU_ARMA(PathNamed, e) {
                    for(auto& field_pat : e.sub_patterns)
                        create_bindings(sp, context, field_pat.second);
                    }
            
                TU_ARMA(Or, e) {
                    assert(e.size() > 0);
                    create_bindings(sp, context, e[0]);
                    // TODO: Ensure that the other arms have the same binding set
                    }
                }
            }
        };
        // - Create variables, assigning new ivars for all of them.
        MatchErgonomicsRevisit::create_bindings(sp, *this, pat);
        // - Add a revisit for the outer pattern (saving the current target type as well as the pattern)
        DEBUG("Handle match ergonomics - " << pat << " with " << type);
        this->add_revisit_adv( box$(( MatchErgonomicsRevisit { sp, type.clone(), pat } )) );
        return ;
    }

    // ---
    this->handle_pattern_direct_inner(sp, pat, type);
}

void Context::handle_pattern_direct_inner(const Span& sp, ::HIR::Pattern& pat, const ::HIR::TypeRef& type)
{
    TRACE_FUNCTION_F("pat = " << pat << ", type = " << type);

    for(const auto& pb : pat.m_bindings ) {
        this->add_binding_inner(sp, pb, type.clone());
    }

    struct H {
        static void handle_value(Context& context, const Span& sp, const ::HIR::TypeRef& type, const ::HIR::Pattern::Value& val) {
            TU_MATCH(::HIR::Pattern::Value, (val), (v),
            (Integer,
                DEBUG("Integer " << ::HIR::TypeRef(v.type));
                // TODO: Apply an ivar bound? (Require that this ivar be an integer?)
                if( v.type != ::HIR::CoreType::Str ) {
                    context.equate_types(sp, type, ::HIR::TypeRef(v.type));
                }
                ),
            (Float,
                DEBUG("Float " << ::HIR::TypeRef(v.type));
                // TODO: Apply an ivar bound? (Require that this ivar be a float?)
                if( v.type != ::HIR::CoreType::Str ) {
                    context.equate_types(sp, type, ::HIR::TypeRef(v.type));
                }
                ),
            (String,
                context.equate_types(sp, type, ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, ::HIR::TypeRef(::HIR::CoreType::Str) ));
                ),
            (ByteString,
                // NOTE: Matches both &[u8] and &[u8; N], so doesn't provide type information
                // TODO: Check the type.
                ),
            (Named,
                // TODO: Get type of the value and equate it
                )
            )
        }

        static ::HIR::TypeRef get_path_type(Context& context, const Span& sp, ::HIR::Path& path, const ::HIR::Pattern::PathBinding& binding)
        {
            TU_MATCH_HDRA( (binding), {)
            TU_ARMA(Unbound, _)
                BUG(sp, "");
            TU_ARMA(Struct, be) {
                auto& p = path.m_data.as_Generic();
                assert(be);
                context.add_ivars_params( p.m_params );
                return ::HIR::TypeRef::new_path(p.clone(), ::HIR::TypePathBinding(be));
                }
            TU_ARMA(Enum, be) {
                auto& p = path.m_data.as_Generic();
                assert(be.ptr);
                context.add_ivars_params( p.m_params );
                return ::HIR::TypeRef::new_path(get_parent_path(p), ::HIR::TypePathBinding(be.ptr));
                }
            }
            throw "";
        }
    };

    //
    TU_MATCH_HDRA( (pat.m_data), {)
    TU_ARMA(Any, e) {
        // Just leave it, the pattern says nothing
        }
    TU_ARMA(Value, e) {
        H::handle_value(*this, sp, type, e.val);
        }
    TU_ARMA(Range, e) {
        if(e.start) H::handle_value(*this, sp, type, *e.start);
        if(e.end  ) H::handle_value(*this, sp, type, *e.end);
        }
    TU_ARMA(Box, e) {
        if( m_lang_Box == ::HIR::SimplePath() )
            ERROR(sp, E0000, "Use of `box` pattern without the `owned_box` lang item");
        const auto& ty = this->get_type(type);
        // Two options:
        // 1. Enforce that the current type must be "owned_box"
        // 2. Make a new ivar for the inner and emit an associated type bound on Deref

        // Taking option 1 for now
        if(const auto* te = ty.data().opt_Path())
        {
            if( TU_TEST1(te->path.m_data, Generic, .m_path == m_lang_Box) ) {
                // Box<T>
                const auto& inner = te->path.m_data.as_Generic().m_params.m_types.at(0);
                this->handle_pattern_direct_inner(sp, *e.sub, inner);
                break ;
            }
        }

        auto inner = this->m_ivars.new_ivar_tr();
        this->handle_pattern_direct_inner(sp, *e.sub, inner);
        ::HIR::GenericPath  path { m_lang_Box, ::HIR::PathParams(mv$(inner)) };
        this->equate_types( sp, type, ::HIR::TypeRef::new_path(mv$(path), ::HIR::TypePathBinding(&m_crate.get_struct_by_path(sp, m_lang_Box))) );
        }
    TU_ARMA(Ref, e) {
        const auto& ty = this->get_type(type);
        if(const auto* te = ty.data().opt_Borrow())
        {
            if( te->type != e.type ) {
                ERROR(sp, E0000, "Pattern-type mismatch, &-ptr mutability mismatch");
            }
            this->handle_pattern_direct_inner(sp, *e.sub, te->inner);
        }
        else {
            auto inner = this->m_ivars.new_ivar_tr();
            this->handle_pattern_direct_inner(sp, *e.sub, inner);
            this->equate_types(sp, type, ::HIR::TypeRef::new_borrow( e.type, mv$(inner) ));
        }
        }
    TU_ARMA(Tuple, e) {
        const auto& ty = this->get_type(type);
        if(const auto* tep = ty.data().opt_Tuple())
        {
            const auto& te = *tep;
            if( e.sub_patterns.size() != te.size() ) {
                ERROR(sp, E0000, "Tuple pattern with an incorrect number of fields, expected " << e.sub_patterns.size() << "-tuple, got " << ty);
            }

            for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                this->handle_pattern_direct_inner(sp, e.sub_patterns[i], te[i] );
        }
        else {

            ::std::vector< ::HIR::TypeRef>  sub_types;
            for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ ) {
                sub_types.push_back( this->m_ivars.new_ivar_tr() );
                this->handle_pattern_direct_inner(sp, e.sub_patterns[i], sub_types[i] );
            }
            this->equate_types(sp, ty, ::HIR::TypeRef( mv$(sub_types) ));
        }
        }
    TU_ARMA(SplitTuple, e) {
        const auto& ty = this->get_type(type);
        if(const auto* tep = ty.data().opt_Tuple())
        {
            const auto& te = *tep;
            // - Should have been checked in AST resolve
            ASSERT_BUG(sp, e.leading.size() + e.trailing.size() <= te.size(), "Invalid field count for split tuple pattern");

            unsigned int tup_idx = 0;
            for(auto& subpat : e.leading) {
                this->handle_pattern_direct_inner(sp, subpat, te[tup_idx++]);
            }
            tup_idx = te.size() - e.trailing.size();
            for(auto& subpat : e.trailing) {
                this->handle_pattern_direct_inner(sp, subpat, te[tup_idx++]);
            }

            // TODO: Should this replace the pattern with a non-split?
            // - Changing the address of the pattern means that the below revisit could fail.
            e.total_size = te.size();
        }
        else {
            if( !ty.data().is_Infer() ) {
                ERROR(sp, E0000, "Tuple pattern on non-tuple");
            }

            ::std::vector<::HIR::TypeRef>   leading_tys;
            leading_tys.reserve(e.leading.size());
            for(auto& subpat : e.leading) {
                leading_tys.push_back( this->m_ivars.new_ivar_tr() );
                this->handle_pattern_direct_inner(sp, subpat, leading_tys.back());
            }
            ::std::vector<::HIR::TypeRef>   trailing_tys;
            for(auto& subpat : e.trailing) {
                trailing_tys.push_back( this->m_ivars.new_ivar_tr() );
                this->handle_pattern_direct_inner(sp, subpat, trailing_tys.back());
            }

            struct SplitTuplePatRevisit:
                public Revisitor
            {
                Span    sp;
                ::HIR::TypeRef  m_outer_ty;
                ::std::vector<::HIR::TypeRef>   m_leading_tys;
                ::std::vector<::HIR::TypeRef>   m_trailing_tys;
                unsigned int& m_pat_total_size;

                SplitTuplePatRevisit(Span sp, ::HIR::TypeRef outer, ::std::vector<::HIR::TypeRef> leading, ::std::vector<::HIR::TypeRef> trailing, unsigned int& pat_total_size):
                    sp(mv$(sp)), m_outer_ty(mv$(outer)),
                    m_leading_tys( mv$(leading) ), m_trailing_tys( mv$(trailing) ),
                    m_pat_total_size(pat_total_size)
                {}

                const Span& span() const override {
                    return sp;
                }
                void fmt(::std::ostream& os) const override {
                    os << "SplitTuplePatRevisit { " << m_outer_ty << " = (" << m_leading_tys << ", ..., " << m_trailing_tys << ") }";
                }
                bool revisit(Context& context, bool is_fallback) override {
                    const auto& ty = context.get_type(m_outer_ty);
                    if(ty.data().is_Infer()) {
                        return false;
                    }
                    else if(const auto* tep = ty.data().opt_Tuple()) {
                        const auto& te = *tep;
                        if( te.size() < m_leading_tys.size() + m_trailing_tys.size() )
                            ERROR(sp, E0000, "Tuple pattern too large for tuple");
                        for(unsigned int i = 0; i < m_leading_tys.size(); i ++)
                            context.equate_types(sp, te[i], m_leading_tys[i]);
                        unsigned int ofs = te.size() - m_trailing_tys.size();
                        for(unsigned int i = 0; i < m_trailing_tys.size(); i ++)
                            context.equate_types(sp, te[ofs+i], m_trailing_tys[i]);
                        m_pat_total_size = te.size();
                        return true;
                    }
                    else {
                        ERROR(sp, E0000, "Tuple pattern on non-tuple - " << ty);
                    }
                }
            };

            // Register a revisit and wait until the tuple is known - then bind through.
            this->add_revisit_adv( box$(( SplitTuplePatRevisit { sp, ty.clone(), mv$(leading_tys), mv$(trailing_tys), e.total_size } )) );
        }
        }
    TU_ARMA(Slice, e) {
        const auto& ty = this->get_type(type);
        TU_MATCH_HDRA( (ty.data()), {)
        default:
            ERROR(sp, E0000, "Slice pattern on non-array/-slice - " << ty);
        TU_ARMA(Slice, te) {
            for(auto& sub : e.sub_patterns)
                this->handle_pattern_direct_inner(sp, sub, te.inner );
            }
        TU_ARMA(Array, te) {
            for(auto& sub : e.sub_patterns)
                this->handle_pattern_direct_inner(sp, sub, te.inner );
            }
        TU_ARMA(Infer, te) {
            auto inner = this->m_ivars.new_ivar_tr();
            for(auto& sub : e.sub_patterns)
                this->handle_pattern_direct_inner(sp, sub, inner);

            struct SlicePatRevisit:
                public Revisitor
            {
                Span    sp;
                ::HIR::TypeRef  inner;
                ::HIR::TypeRef  type;
                unsigned int size;

                SlicePatRevisit(Span sp, ::HIR::TypeRef inner, ::HIR::TypeRef type, unsigned int size):
                    sp(mv$(sp)), inner(mv$(inner)), type(mv$(type)), size(size)
                {}

                const Span& span() const override {
                    return sp;
                }
                void fmt(::std::ostream& os) const override { os << "SlicePatRevisit { " << inner << ", " << type << ", " << size; }
                bool revisit(Context& context, bool is_fallback) override {
                    const auto& ty = context.get_type(type);
                    TU_MATCH_HDRA( (ty.data()), {)
                    default:
                        ERROR(sp, E0000, "Slice pattern on non-array/-slice - " << ty);
                    TU_ARMA(Infer, te) {
                        return false;
                        }
                    TU_ARMA(Slice, te) {
                        context.equate_types(sp, te.inner, inner);
                        return true;
                        }
                    TU_ARMA(Array, te) {
                        if( te.size.as_Known() != size ) {
                            ERROR(sp, E0000, "Slice pattern on an array if differing size");
                        }
                        context.equate_types(sp, te.inner, inner);
                        return true;
                        }
                    }
                    throw "unreachable"; //UNREACHABLE();
                }
            };
            this->add_revisit_adv( box$(( SlicePatRevisit { sp, mv$(inner), ty.clone(), static_cast<unsigned int>(e.sub_patterns.size()) } )) );
            }
        }
        }
    TU_ARMA(SplitSlice, e) {
        ::HIR::TypeRef  inner;
        unsigned int min_len = e.leading.size() + e.trailing.size();
        const auto& ty = this->get_type(type);
        TU_MATCH_HDRA( (ty.data()), {)
        default:
            ERROR(sp, E0000, "SplitSlice pattern on non-array/-slice - " << ty);
        TU_ARMA(Slice, te) {
            // Slice - Fetch inner and set new variable also be a slice
            // - TODO: Better new variable handling.
            inner = te.inner.clone();
            if( e.extra_bind.is_valid() ) {
                this->add_binding_inner( sp, e.extra_bind, ty.clone() );
            }
            }
        TU_ARMA(Array, te) {
            inner = te.inner.clone();
            if( te.size.as_Known() < min_len ) {
                ERROR(sp, E0000, "Slice pattern on an array smaller than the pattern");
            }
            unsigned extra_len = te.size.as_Known() - min_len;

            if( e.extra_bind.is_valid() ) {
                this->add_binding_inner( sp, e.extra_bind, ::HIR::TypeRef::new_array(inner.clone(), extra_len) );
            }
            }
        TU_ARMA(Infer, te) {
            inner = this->m_ivars.new_ivar_tr();
            ::HIR::TypeRef  var_ty;
            if( e.extra_bind.is_valid() ) {
                var_ty = this->m_ivars.new_ivar_tr();
                this->add_binding_inner( sp, e.extra_bind, var_ty.clone() );
            }

            struct SplitSlicePatRevisit:
                public Revisitor
            {
                Span    sp;
                // Inner type
                ::HIR::TypeRef  inner;
                // Outer ivar (should be either Slice or Array)
                ::HIR::TypeRef  type;
                // Binding type (if not default value)
                ::HIR::TypeRef  var_ty;
                unsigned int min_size;

                SplitSlicePatRevisit(Span sp, ::HIR::TypeRef inner, ::HIR::TypeRef type, ::HIR::TypeRef var_ty, unsigned int size):
                    sp(mv$(sp)), inner(mv$(inner)), type(mv$(type)), var_ty(mv$(var_ty)), min_size(size)
                {}

                const Span& span() const override {
                    return sp;
                }
                void fmt(::std::ostream& os) const override { os << "SplitSlice inner=" << inner << ", outer=" << type << ", binding="<<var_ty<<", " << min_size; }
                bool revisit(Context& context, bool is_fallback) override {
                    const auto& ty = context.get_type(this->type);
                    TU_MATCH_HDRA( (ty.data()), {)
                    default:
                        ERROR(sp, E0000, "Slice pattern on non-array/-slice - " << ty);
                    TU_ARMA(Infer, te) {
                        return false;
                        }
                    TU_ARMA(Slice, te) {
                        // Slice - Equate inners
                        context.equate_types(this->sp, this->inner, te.inner);
                        if( this->var_ty != ::HIR::TypeRef() ) {
                            context.equate_types(this->sp, this->var_ty, ty);
                        }
                        }
                    TU_ARMA(Array, te) {
                        // Array - Equate inners and check size
                        context.equate_types(this->sp, this->inner, te.inner);
                        if( te.size.as_Known() < this->min_size ) {
                            ERROR(sp, E0000, "Slice pattern on an array smaller than the pattern");
                        }
                        unsigned extra_len = te.size.as_Known() - this->min_size;

                        if( this->var_ty != ::HIR::TypeRef() ) {
                            context.equate_types(this->sp, this->var_ty, ::HIR::TypeRef::new_array(this->inner.clone(), extra_len) );
                        }
                        }
                    }
                    return true;
                }
            };
            // Callback
            this->add_revisit_adv( box$(( SplitSlicePatRevisit { sp, inner.clone(), ty.clone(), mv$(var_ty), min_len } )) );
            }
        }

        for(auto& sub : e.leading)
            this->handle_pattern_direct_inner( sp, sub, inner );
        for(auto& sub : e.trailing)
            this->handle_pattern_direct_inner( sp, sub, inner );
        }

    // - Enums/Structs
    TU_ARMA(PathValue, e) {
        this->equate_types( sp, type, H::get_path_type(*this, sp, e.path, e.binding) );
        TU_MATCH_HDRA( (e.binding), {)
        TU_ARMA(Unbound, _)
            BUG(sp, "");
        TU_ARMA(Struct, str) {
            assert( str->m_data.is_Unit() );
            }
        TU_ARMA(Enum, be) {
            if( const auto* ee = be.ptr->m_data.opt_Data() )
            {
                ASSERT_BUG(sp, be.var_idx < ee->size(), "");
                const auto& var = (*ee)[be.var_idx];
                if( var.type.data().is_Tuple() && var.type.data().as_Tuple().size() == 0 ) {
                    // All good
                }
                else {
                    // TODO: Error here due to invalid variant type
                }
            }
            }
        }
        }
    TU_ARMA(PathTuple, e) {
        this->equate_types( sp, type, H::get_path_type(*this, sp, e.path, e.binding) );
        
        const auto& sd = ::HIR::pattern_get_tuple(sp, e.path, e.binding);

        auto ms = MonomorphStatePtr(nullptr, &e.path.m_data.as_Generic().m_params, nullptr);
        ::HIR::TypeRef tmp;
        auto maybe_monomorph = [&](const ::HIR::TypeRef& ty)->const ::HIR::TypeRef& {
            if( monomorphise_type_needed(ty) ) {
                return (tmp = ms.monomorph_type(sp, ty));
            }
            else {
                return ty;
            }
            };
        if( e.is_split ) {
            ASSERT_BUG(sp, e.leading.size() + e.trailing.size() <= sd.size(),
                "PathTuple size mismatch, expected at most " << sd.size() << " fields but got " << e.leading.size() + e.trailing.size());
        }
        else {
            ASSERT_BUG(sp, e.leading.size() == sd.size(), "PathTuple size mismatch, expected " << sd.size() << " fields but got " << e.leading.size());
            assert(e.trailing.size() == 0);
        }
        e.total_size = sd.size();

        for( size_t i = 0; i < e.leading.size(); i ++ )
        {
            /*const*/ auto& sub_pat = e.leading[i];
            this->handle_pattern_direct_inner(sp, sub_pat, maybe_monomorph(sd[i].ent));
        }
        for( size_t i = 0; i < e.trailing.size(); i ++ )
        {
            /*const*/ auto& sub_pat = e.trailing[i];
            this->handle_pattern_direct_inner(sp, sub_pat, maybe_monomorph(sd[sd.size() - e.trailing.size() + i].ent));
        }
        }
    TU_ARMA(PathNamed, e) {
        this->equate_types( sp, type, H::get_path_type(*this, sp, e.path, e.binding) );

        if( e.is_wildcard() )
            return ;

        const auto& sd = ::HIR::pattern_get_named(sp, e.path, e.binding);

        auto ms = MonomorphStatePtr(nullptr, &e.path.m_data.as_Generic().m_params, nullptr);

        for( auto& field_pat : e.sub_patterns )
        {
            unsigned int f_idx = ::std::find_if( sd.begin(), sd.end(), [&](const auto& x){ return x.first == field_pat.first; } ) - sd.begin();
            if( f_idx == sd.size() ) {
                ERROR(sp, E0000, "Struct " << e.path << " doesn't have a field " << field_pat.first);
            }
            const ::HIR::TypeRef& field_type = sd[f_idx].second.ent;
            if( monomorphise_type_needed(field_type) ) {
                auto field_type_mono = ms.monomorph_type(sp, field_type);
                this->handle_pattern_direct_inner(sp, field_pat.second, field_type_mono);
            }
            else {
                this->handle_pattern_direct_inner(sp, field_pat.second, field_type);
            }
        }
        }
            
    TU_ARMA(Or, e) {
        for(auto& subpat : e)
            this->handle_pattern_direct_inner(sp, subpat, type);
        }
    }
}
void Context::equate_types_coerce(const Span& sp, const ::HIR::TypeRef& l, ::HIR::ExprNodeP& node_ptr)
{
    this->m_ivars.get_type(l);
    // - Just record the equality
    this->link_coerce.push_back(std::make_unique<Coercion>(Coercion {
        this->next_rule_idx ++,
        l.clone(), &node_ptr
        }));
    DEBUG("++ " << *this->link_coerce.back());
    this->m_ivars.mark_change();
}
void Context::possible_equate_type_unknown(const Span& sp, const ::HIR::TypeRef& ty, Context::IvarUnknownType src)
{
    TU_MATCH_HDRA( (this->get_type(ty).data()), {)
    default:
        // TODO: Shadow sub-types too
    TU_ARMA(Path, e) {
        TU_MATCH_HDRA( (e.path.m_data), {)
        default:
            // TODO: Ufcs?
        TU_ARMA(Generic, pe) {
            for(const auto& sty : pe.m_params.m_types)
                this->possible_equate_type_unknown(sp, sty, src);
        }
        }
        }
    TU_ARMA(Tuple, e) {
        for(const auto& sty : e)
            this->possible_equate_type_unknown(sp, sty, src);
        }
    TU_ARMA(Borrow, e) {
        this->possible_equate_type_unknown(sp, e.inner, src);
        }
    TU_ARMA(Array, e) {
        this->possible_equate_type_unknown(sp, e.inner, src);
        }
    TU_ARMA(Slice, e) {
        this->possible_equate_type_unknown(sp, e.inner, src);
        }
    TU_ARMA(Closure, e) {
        for(const auto& aty : e.node->m_args)
            this->possible_equate_type_unknown(sp, aty.second, src);
        this->possible_equate_type_unknown(sp, e.node->m_return, src);
        }
    TU_ARMA(Infer, e) {
        this->possible_equate_ivar_unknown(sp, e.index, src);
        }
    }
}
void Context::equate_types_assoc(const Span& sp, const ::HIR::TypeRef& l,  const ::HIR::SimplePath& trait, ::HIR::PathParams pp, const ::HIR::TypeRef& impl_ty, const char *name, bool is_op)
{
    for(const auto& a : this->link_assoc)
    {
        if( a.left_ty != l )
            continue ;
        if( a.trait != trait )
            continue ;
        if( a.params != pp )
            continue ;
        if( a.impl_ty != impl_ty )
            continue ;
        if( a.name != name )
            continue ;
        if( a.is_operator != is_op )
            continue ;

        DEBUG("(DUPLICATE " << a << ")");
        return ;
    }
    this->link_assoc.push_back(Associated {
        this->next_rule_idx ++,
        sp,
        MonomorphEraseHrls().monomorph_type(sp, l, true),

        trait.clone(),
        MonomorphEraseHrls().monomorph_path_params(sp, pp, true),
        MonomorphEraseHrls().monomorph_type(sp, impl_ty, true),
        name,
        is_op
        });
    DEBUG("++ " << this->link_assoc.back());
    this->m_ivars.mark_change();
}
void Context::add_revisit(::HIR::ExprNode& node) {
    this->to_visit.push_back( &node );
}
void Context::add_revisit_adv(::std::unique_ptr<Revisitor> ent_ptr) {
    this->adv_revisits.push_back( mv$(ent_ptr) );
}
void Context::require_sized(const Span& sp, const ::HIR::TypeRef& ty_)
{
    const auto& ty = m_ivars.get_type(ty_);
    TRACE_FUNCTION_F(ty_ << " -> " << ty);
    if( m_resolve.type_is_sized(sp, ty) == ::HIR::Compare::Unequal )
    {
        ERROR(sp, E0000, "Unsized type not valid here - " << ty);
    }
    if( const auto* e = ty.data().opt_Infer() )
    {
        switch(e->ty_class)
        {
        case ::HIR::InferClass::Integer:
        case ::HIR::InferClass::Float:
            // Has to be.
            break;
        default:
            // TODO: Flag for future checking
            ASSERT_BUG(sp, e->index != ~0u, "Unbound ivar " << ty);
            if(e->index >= m_ivars_sized.size())
                m_ivars_sized.resize(e->index+1);
            m_ivars_sized.at(e->index) = true;
            break;
        }
    }
    else if( const auto* e = ty.data().opt_Path() )
    {
        const ::HIR::GenericParams* params_def = nullptr;
        TU_MATCHA( (e->binding), (pb),
        (Unbound,
            // TODO: Add a trait check rule
            params_def = nullptr;
            ),
        (Opaque,
            // Already checked by type_is_sized
            params_def = nullptr;
            ),
        (ExternType,
            static ::HIR::GenericParams empty_params;
            params_def = &empty_params;
            ),
        (Enum,
            params_def = &pb->m_params;
            ),
        (Union,
            params_def = &pb->m_params;
            ),
        (Struct,
            params_def = &pb->m_params;

            if( pb->m_struct_markings.dst_type == ::HIR::StructMarkings::DstType::Possible )
            {
                // Check sized-ness of the unsized param
                this->require_sized( sp, e->path.m_data.as_Generic().m_params.m_types.at(pb->m_struct_markings.unsized_param) );
            }
            )
        )

        if( params_def )
        {
            const auto& gp_tys = e->path.m_data.as_Generic().m_params.m_types;
            for(size_t i = 0; i < gp_tys.size(); i ++)
            {
                if(params_def->m_types.at(i).m_is_sized)
                {
                    this->require_sized( sp, gp_tys[i] );
                }
            }
        }
    }
    else if( const auto* e = ty.data().opt_Tuple() )
    {
        // All entries in a tuple must be Sized
        for(const auto& ity : *e)
        {
            this->require_sized( sp, ity );
        }
    }
    else if( const auto* e = ty.data().opt_Array() )
    {
        // Inner type of an array must be sized
        this->require_sized(sp, e->inner);
    }
}

std::ostream& operator<<(std::ostream& os, const Context::PossibleTypeSource& x) {
    switch(x)
    {
    case Context::PossibleTypeSource::UnsizeTo:  os << "UnsizeTo";   break;
    case Context::PossibleTypeSource::CoerceTo:  os << "CoerceTo";   break;
    case Context::PossibleTypeSource::UnsizeFrom:  os << "UnsizeFrom";   break;
    case Context::PossibleTypeSource::CoerceFrom:  os << "CoerceFrom";   break;
    }
    return os;
}

Context::IVarPossible* Context::get_ivar_possibilities(const Span& sp, unsigned int ivar_index)
{
    {
        const auto& real_ty = m_ivars.get_type(ivar_index);
        if( !TU_TEST1(real_ty.data(), Infer, .index == ivar_index) )
        {
            DEBUG("IVar " << ivar_index << " is actually " << real_ty);
            return nullptr;
        }
    }

    if( ivar_index >= possible_ivar_vals.size() ) {
        possible_ivar_vals.resize( ivar_index + 1 );
    }
    return &possible_ivar_vals[ivar_index];
}
void Context::possible_equate_ivar(const Span& sp, unsigned int ivar_index, const ::HIR::TypeRef& raw_t, PossibleTypeSource src)
{
    const auto& t = this->m_ivars.get_type(raw_t);
    DEBUG(ivar_index << " " << src << " " << raw_t << " " << t);
    auto* entp = get_ivar_possibilities(sp, ivar_index);
    if(!entp)
        return;
    auto& ent = *entp;

    switch(src)
    {
    case PossibleTypeSource::UnsizeTo:  ent.types_coerce_to.push_back(IVarPossible::CoerceTy(t.clone(), false));   break;
    case PossibleTypeSource::CoerceTo:  ent.types_coerce_to.push_back(IVarPossible::CoerceTy(t.clone(), true ));   break;
    case PossibleTypeSource::UnsizeFrom:  ent.types_coerce_from.push_back(IVarPossible::CoerceTy(t.clone(), false));   break;
    case PossibleTypeSource::CoerceFrom:  ent.types_coerce_from.push_back(IVarPossible::CoerceTy(t.clone(), true ));   break;
    }

    // Tag ivars embedded in `raw_t` to prevent them from being guessed unless no other option
    if( !t.data().is_Infer() )
    {
        switch(src)
        {
        case PossibleTypeSource::UnsizeTo:
        case PossibleTypeSource::CoerceTo:
            possible_equate_type_unknown(sp, t, IvarUnknownType::To);
            break;
        case PossibleTypeSource::UnsizeFrom:
        case PossibleTypeSource::CoerceFrom:
            possible_equate_type_unknown(sp, t, IvarUnknownType::From);
            break;
        }
    }
}
void Context::possible_equate_ivar_bounds(const Span& sp, unsigned int ivar_index, std::vector< ::HIR::TypeRef> types)
{
    // Obtain the entry (and returning early if already known)
    auto* entp = get_ivar_possibilities(sp, ivar_index);
    if(!entp)
        return ;
    auto& ent = *entp;

    // Determine if this ivar is in the list of possibilities
    bool has_self = false;
    for(auto it = types.begin(); it != types.end(); )
    {
        auto& e = *it;
        ASSERT_BUG(sp, !type_contains_impl_placeholder(e), "Type contained an impl placeholder parameter - " << e);
        e = m_ivars.get_type(e).clone();
        if( TU_TEST1(e.data(), Infer, .index == ivar_index) )
        {
            has_self = true;
            it = types.erase(it);
        }
        else
        {
            ++ it;
        }
    }

    if( ent.has_bounded )
    {
        // Get the union of the bound set and this

        // TODO: If `ent.bounds_include_self` was set, then accept check if it's still set?
        ent.bounds_include_self |= has_self;
        if( ent.bounds_include_self )
        {
            // Accept everything in `types`
            for(auto& ty : types)
            {
                if( std::find(ent.bounded.begin(), ent.bounded.end(), ty) == ent.bounded.end() )
                {
                    ent.bounded.push_back(::std::move(ty));
                }
            }
        }
        else
        {
            // For each existing bound
            for(auto it_existing = ent.bounded.begin(); it_existing != ent.bounded.end(); )
            {
                // Remove if it can't be found in the incoming set
                if( std::find(types.begin(), types.end(), *it_existing) != types.end() )
                {
                    ++ it_existing;
                }
                else
                {
                    it_existing = ent.bounded.erase(it_existing);
                }
            }
        }
        DEBUG(ivar_index << " bounded as [" << ent.bounded << "], union from [" << types << "] has_self=" << ent.bounds_include_self);
    }
    else
    {
        ent.has_bounded = true;
        ent.bounds_include_self = has_self;
        ent.bounded = std::move(types);
        DEBUG(ivar_index << " bounded as [" << ent.bounded << "] has_self=" << has_self);
    }
}

std::ostream& operator<<(std::ostream& os, const Context::IvarUnknownType& x) {
    switch(x)
    {
    case Context::IvarUnknownType::To:      os << "To";     break;
    case Context::IvarUnknownType::From:    os << "From";   break;
    case Context::IvarUnknownType::Bound:   os << "Bound";  break;
    }
    return os;
}
void Context::possible_equate_ivar_unknown(const Span& sp, unsigned int ivar_index, IvarUnknownType src)
{
    DEBUG(ivar_index << " = ?? (" << src << ")");
    ASSERT_BUG(sp, m_ivars.get_type(ivar_index).data().is_Infer(), "possible_equate_ivar_unknown on known ivar");

    if( ivar_index >= possible_ivar_vals.size() ) {
        possible_ivar_vals.resize( ivar_index + 1 );
    }
    auto& ent = possible_ivar_vals[ivar_index];
    switch(src)
    {
    case IvarUnknownType::To:       ent.force_no_to = true;     break;
    case IvarUnknownType::From:     ent.force_no_from = true;   break;
    case IvarUnknownType::Bound:    ent.force_disable = true;   break;
    }
}

void Context::add_var(const Span& sp, unsigned int index, const RcString& name, ::HIR::TypeRef type) {
    DEBUG("(" << index << " " << name << " : " << type << ")");
    assert(index != ~0u);
    ASSERT_BUG(sp, type != HIR::TypeRef(), "Unset ivar in variable type");
    if( m_bindings.size() <= index )
        m_bindings.resize(index+1);
    if( m_bindings[index].name == "" ) {
        m_bindings[index] = Binding { name, mv$(type) };
        // NOTE: Disabled to support unsized locals (1.74)
        //this->require_sized(sp, m_bindings[index].ty);
    }
    else {
        ASSERT_BUG(sp, m_bindings[index].name == name, "");
        this->equate_types(sp, m_bindings[index].ty, type);
    }
}

const ::HIR::TypeRef& Context::get_var(const Span& sp, unsigned int idx) const {
    if( idx < this->m_bindings.size() ) {
        ASSERT_BUG(sp, this->m_bindings[idx].ty != HIR::TypeRef(), "Local #" << idx << " `" << this->m_bindings[idx].name << "` with no populated type");
        return this->m_bindings[idx].ty;
    }
    else {
        BUG(sp, "get_var - Binding index out of range - " << idx << " >=" << this->m_bindings.size());
    }
}

::HIR::ExprNodeP Context::create_autoderef(::HIR::ExprNodeP val_node, ::HIR::TypeRef ty_dst) const
{
    const auto& span = val_node->span();
    const auto& ty_src = val_node->m_res_type;
    // Special case for going Array->Slice, insert _Unsize instead of _Deref
    if( get_type(ty_src).data().is_Array() )
    {
        ASSERT_BUG(span, ty_dst.data().is_Slice(), "Array should only ever autoderef to Slice");

        // Would emit borrow+unsize+deref, but that requires knowing the borrow class.
        // HACK: Emit an invalid _Unsize op that is fixed once usage type is known.
        auto ty_dst_c = ty_dst.clone();
        val_node = NEWNODE( mv$(ty_dst), span, _Unsize,  mv$(val_node), mv$(ty_dst_c) );
        DEBUG("- Unsize " << &*val_node << " -> " << val_node->m_res_type);
    }
    else {
        val_node = NEWNODE( mv$(ty_dst), span, _Deref,  mv$(val_node) );
        DEBUG("- Deref " << &*val_node << " -> " << val_node->m_res_type);
    }

    return val_node;
}


namespace {
    void add_coerce_borrow(Context& context, ::HIR::ExprNodeP& orig_node_ptr, const ::HIR::TypeRef& des_borrow_inner, ::std::function<void(::HIR::ExprNodeP& n)> cb)
    {
        auto borrow_type = context.m_ivars.get_type(orig_node_ptr->m_res_type).data().as_Borrow().type;

        // Since this function operates on destructured &-ptrs, the dereferences have to be added behind a borrow
        ::HIR::ExprNodeP*   node_ptr_ptr = &orig_node_ptr;

        #if 1
        // If the coercion is of a block, apply the mutation to the inner node
        ASSERT_BUG(Span(), orig_node_ptr, "Null node pointer passed to `add_coerce_borrow`");
        while( auto* p = dynamic_cast< ::HIR::ExprNode_Block*>(&**node_ptr_ptr) )
        {
            DEBUG("- Moving into block");
            assert( p->m_value_node );
            // Block result and the inner node's result must be the same type
            ASSERT_BUG( p->span(), context.m_ivars.types_equal(p->m_res_type, p->m_value_node->m_res_type),
                "Block and result mismatch - " << context.m_ivars.fmt_type(p->m_res_type) << " != " << context.m_ivars.fmt_type(p->m_value_node->m_res_type));
            // - Override the the result type to the desired result
            p->m_res_type = ::HIR::TypeRef::new_borrow(borrow_type, des_borrow_inner.clone());
            node_ptr_ptr = &p->m_value_node;
        }
        #endif
        auto& node_ptr = *node_ptr_ptr;
        const auto& src_type = context.m_ivars.get_type(node_ptr->m_res_type);

        // - If the pointed node is a borrow operation, add the dereferences within its value
        if( auto* p = dynamic_cast< ::HIR::ExprNode_Borrow*>(&*node_ptr) )
        {
            // Set the result of the borrow operation to the output type
            node_ptr->m_res_type = ::HIR::TypeRef::new_borrow(borrow_type, des_borrow_inner.clone());

            node_ptr_ptr = &p->m_value;
        }
        // - Otherwise, create a new borrow operation behind which the dereferences happen
        else
        {
            DEBUG("- Coercion node isn't a borrow, adding one");
            auto span = node_ptr->span();
            const auto& src_inner_ty = src_type.data().as_Borrow().inner;

            // NOTE: The type here is for _after_ `cb` has been called
            auto inner_ty_ref = ::HIR::TypeRef::new_borrow(borrow_type, des_borrow_inner.clone());

            // 1. Dereference (resulting in the dereferenced input type)
            node_ptr = NEWNODE(src_inner_ty.clone(), span, _Deref,  mv$(node_ptr));
            DEBUG("- Deref " << &*node_ptr << " -> " << node_ptr->m_res_type);
            // 2. Borrow (resulting in the referenced output type)
            node_ptr = NEWNODE(mv$(inner_ty_ref), span, _Borrow,  borrow_type, mv$(node_ptr));
            DEBUG("- Borrow " << &*node_ptr << " -> " << node_ptr->m_res_type);

            // - Set node pointer reference to point into the new borrow op
            node_ptr_ptr = &dynamic_cast< ::HIR::ExprNode_Borrow&>(*node_ptr).m_value;
        }

        cb(*node_ptr_ptr);

        context.m_ivars.mark_change();
    }

    enum CoerceResult {
        Unknown,    // Coercion still unknown.
        Equality,   // Types should be equated
        Fail,   // Equality would fail
        Custom, // An op was emitted, and rule is complete
        Unsize, // Emits an _Unsize op
    };

    // TODO: Add a (two?) callback(s) that handle type equalities (and possible equalities) so this function doesn't have to mutate the context
    CoerceResult check_unsize_tys(
        const Context& context, const Span& sp, const ::HIR::TypeRef& dst_raw, const ::HIR::TypeRef& src_raw,
        Context* context_mut, ::HIR::ExprNodeP* node_ptr_ptr=nullptr
        )
    {
        const auto& dst = context.m_ivars.get_type(dst_raw);
        const auto& src = context.m_ivars.get_type(src_raw);
        TRACE_FUNCTION_F("dst=" << dst << ", src=" << src);

        // If the types are already equal, no operation is required
        if( context.m_ivars.types_equal(dst, src) ) {
            DEBUG("Equal");
            return CoerceResult::Equality;
        }

        // Impossibilities
        if( src.data().is_Slice() )
        {
            // [T] can't unsize to anything
            DEBUG("Slice can't unsize");
            if( dst.data().is_Slice() || dst.data().is_Infer() ) {
                return CoerceResult::Equality;
            }
            else {
                return CoerceResult::Fail;
            }
        }

        // Handle ivars specially
        if(dst.data().is_Infer() && src.data().is_Infer())
        {
            // If both are literals, equate
            if( dst.data().as_Infer().is_lit() && src.data().as_Infer().is_lit() )
            {
                DEBUG("Literal ivars");
                return CoerceResult::Equality;
            }
            if( context_mut )
            {
                context_mut->possible_equate_ivar(sp, src.data().as_Infer().index, dst, Context::PossibleTypeSource::UnsizeTo);
                context_mut->possible_equate_ivar(sp, dst.data().as_Infer().index, src, Context::PossibleTypeSource::UnsizeFrom);
            }
            DEBUG("Both ivars");
            return CoerceResult::Unknown;
        }
        else if(const auto* dep = dst.data().opt_Infer())
        {
            // Literal from a primtive has to be equal
            if( dep->is_lit() && src.data().is_Primitive() )
            {
                DEBUG("Literal with primitive");
                return CoerceResult::Equality;
            }
            if( context_mut )
            {
                context_mut->possible_equate_ivar(sp, dep->index, src, Context::PossibleTypeSource::UnsizeFrom);
                // Disable inner parts of the source? (E.g. if it's a closure)
                if( src.data().is_Closure() ) {
                    context_mut->possible_equate_type_unknown(sp, src, Context::IvarUnknownType::To);
                }
            }
            DEBUG("Dst ivar");
            return CoerceResult::Unknown;
        }
        else if(const auto* sep = src.data().opt_Infer())
        {
            if(sep->is_lit())
            {
                if( !dst.data().is_TraitObject())
                {
                    // Literal to anything other than a trait object must be an equality
                    DEBUG("Literal with primitive");
                    return CoerceResult::Equality;
                }
                else
                {
                    // Fall through
                }
            }
            else
            {
                if( context_mut )
                {
                    context_mut->possible_equate_ivar(sp, sep->index, dst, Context::PossibleTypeSource::UnsizeTo);
                }
                DEBUG("Src is ivar (" << src << "), return Unknown");
                return CoerceResult::Unknown;
            }
        }
        else
        {
            // Neither side is an ivar, keep going.
        }

        // If either side is an unbound path, then return Unknown
        if( TU_TEST1(src.data(), Path, .binding.is_Unbound()) )
        {
            DEBUG("Source unbound path");
            return CoerceResult::Unknown;
        }
        if( TU_TEST1(dst.data(), Path, .binding.is_Unbound()) )
        {
            DEBUG("Destination unbound path");
            return CoerceResult::Unknown;
        }

        // Array unsize (quicker than going into deref search)
        if(dst.data().is_Slice() && src.data().is_Array())
        {
            if( context_mut )
            {
                context_mut->equate_types(sp, dst.data().as_Slice().inner, src.data().as_Array().inner);
            }
            if(node_ptr_ptr)
            {
                // TODO: Insert deref (instead of leading to a _Unsize op)
            }
            else
            {
                // Just return Unsize
            }
            DEBUG("Array => Slice");
            return CoerceResult::Unsize;
        }

        // Shortcut: Types that can't deref coerce (and can't coerce here because the target isn't a TraitObject)
        if( ! dst.data().is_TraitObject() )
        {
            if( src.data().is_Generic() )
            {
            }
            else if( src.data().is_Path() )
            {
            }
            else if( src.data().is_Borrow() )
            {
            }
            else
            {
                DEBUG("Target isn't a trait object, and sources can't Deref");
                return CoerceResult::Equality;
            }
        }

        // Deref coercions
        // - If right can be dereferenced to left
        if(node_ptr_ptr)
        {
            DEBUG("-- Deref coercions");
            ::HIR::TypeRef  tmp_ty;
            const ::HIR::TypeRef*   out_ty_p = &src;
            unsigned int count = 0;
            ::std::vector< ::HIR::TypeRef>  types;
            while( (out_ty_p = context.m_resolve.autoderef(sp, *out_ty_p, tmp_ty)) )
            {
                const auto& out_ty = context.m_ivars.get_type(*out_ty_p);
                DEBUG("From? " << out_ty);
                count += 1;

                if( const auto* sep = out_ty.data().opt_Infer() )
                {
                    if( !sep->is_lit() )
                    {
                        // Hit a _, so can't keep going
                        if( context_mut )
                        {
                            // Could also be any deref chain of the destination type
                            ::HIR::TypeRef  tmp_ty2;
                            const ::HIR::TypeRef* d_ty_p = &dst;
                            //context_mut->possible_equate_ivar(sp, sep->index, dst, Context::PossibleTypeSource::UnsizeTo);
                            for(unsigned int i = 0; (d_ty_p = context.m_resolve.autoderef(sp, *d_ty_p, tmp_ty2)) && i < count - 1; i ++)
                            {
                            }
                            if( d_ty_p )
                            {
                                // TODO: This should be a `DerefTo` (can't do other unsizings?)
                                context_mut->possible_equate_ivar(sp, sep->index, *d_ty_p, Context::PossibleTypeSource::UnsizeTo);
                            }
                            else
                            {
                                // No type available, why?
                            }
                        }
                        DEBUG("Src derefs to ivar (" << src << "), return Unknown");
                        return CoerceResult::Unknown;
                    }
                    // Literal infer, keep going (but remember how many times we dereferenced?)
                }

                if( TU_TEST1(out_ty.data(), Generic, .is_placeholder()) )
                {
                    DEBUG("Src derefed to a placeholder generic type (" << out_ty << "), return Unknown");
                    return CoerceResult::Unknown;
                }

                if( TU_TEST1(out_ty.data(), Path, .binding.is_Unbound()) )
                {
                    DEBUG("Src derefed to unbound type (" << out_ty << "), return Unknown");
                    return CoerceResult::Unknown;
                }

                types.push_back( out_ty.clone() );

                // Types aren't equal
                if( context.m_ivars.types_equal(dst, out_ty) == false )
                {
                    // Check if they can be considered equivalent.
                    // - E.g. a fuzzy match, or both are slices/arrays
                    if( dst.data().tag() != out_ty.data().tag() )
                    {
                        DEBUG("Different types");
                        continue ;
                    }

                    if( dst.data().is_Slice() )
                    {
                        if(context_mut)
                        {
                            context_mut->equate_types(sp, dst, out_ty);
                        }
                    }
                    else if( dst.data().is_Borrow() )
                    {
                        DEBUG("Borrow, continue");
                        continue ;
                    }
                    else
                    {
                        if( dst .compare_with_placeholders(sp, out_ty, context.m_ivars.callback_resolve_infer()) == ::HIR::Compare::Unequal ) {
                            DEBUG("Same tag, but not fuzzy match");
                            continue ;
                        }
                        DEBUG("Same tag and fuzzy match - assuming " << dst << " == " << out_ty);
                        if( context_mut )
                        {
                            context_mut->equate_types(sp, dst, out_ty);
                        }
                    }
                }

                if( context_mut && node_ptr_ptr )
                {
                    auto& node_ptr = *node_ptr_ptr;
                    add_coerce_borrow(*context_mut, node_ptr, types.back(), [&](auto& node_ptr)->void {
                        // node_ptr = node that yeilds ty_src
                        assert( count == types.size() );
                        for(unsigned int i = 0; i < types.size(); i ++ )
                        {
                            auto span = node_ptr->span();
                            // TODO: Replace with a call to context.create_autoderef to handle cases where the below assertion would fire.
                            ASSERT_BUG(span, !node_ptr->m_res_type.data().is_Array(), "Array->Slice shouldn't be in deref coercions");
                            auto ty = mv$(types[i]);
                            node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Deref( mv$(span), mv$(node_ptr) ));
                            DEBUG("- Deref " << &*node_ptr << " -> " << ty);
                            node_ptr->m_res_type = mv$(ty);
                            context.m_ivars.get_type(node_ptr->m_res_type);
                        }
                        });
                }

                return CoerceResult::Custom;
            }
            // Either ran out of deref, or hit a _
            DEBUG("No applicable deref coercions");
        }

        // Trait objects
        if(const auto* dep = dst.data().opt_TraitObject())
        {
            if(const auto* sep = src.data().opt_TraitObject())
            {
                DEBUG("TraitObject => TraitObject");
                // Ensure that the trait list in the destination is a strict subset of the source

                // TODO: Equate these two trait paths
                if( dep->m_trait.m_path.m_path != sep->m_trait.m_path.m_path )
                {
                    // Trait mismatch!
                    #if 1   // 1.74: `trait_upcasting` feature
                    return CoerceResult::Unsize;
                    #endif
                    return CoerceResult::Equality;
                }
                const auto& tys_d = dep->m_trait.m_path.m_params.m_types;
                const auto& tys_s = sep->m_trait.m_path.m_params.m_types;
                if( context_mut )
                {
                    for(size_t i = 0; i < tys_d.size(); i ++)
                    {
                        context_mut->equate_types(sp, tys_d[i], tys_s.at(i));
                    }
                }

                // 2. Destination markers must be a strict subset
                for(const auto& mt : dep->m_markers)
                {
                    // TODO: Fuzzy match
                    bool found = false;
                    for(const auto& omt : sep->m_markers) {
                        if( omt == mt ) {
                            found = true;
                            break;
                        }
                    }
                    if( !found ) {
                        // Return early.
                        return CoerceResult::Equality;
                    }
                }

                return CoerceResult::Unsize;
            }
            else
            {
                const auto& trait = dep->m_trait.m_path;

                // Check for trait impl
                if( trait.m_path != ::HIR::SimplePath() )
                {
                    // Just call equate_types_assoc to add the required bounds.
                    if( context_mut )
                    {
                        auto pp = dep->m_trait.m_hrtbs ? dep->m_trait.m_hrtbs->make_empty_params(true) : HIR::PathParams();
                        MonomorphHrlsOnly   ms(pp);
                        for(const auto& tyb : dep->m_trait.m_type_bounds)
                        {
                            context_mut->equate_types_assoc(sp, tyb.second.type,  trait.m_path, ms.monomorph_path_params(sp, trait.m_params, true), src, tyb.first.c_str(), false);
                        }
                        if( dep->m_trait.m_type_bounds.empty() )
                        {
                            context_mut->add_trait_bound(sp, src,  trait.m_path, ms.monomorph_path_params(sp, trait.m_params, true));
                        }
                    }
                    else
                    {
                        // Check that the trait is implemented (so this only returns `Unsize` if the rule would be valid - use for check_ivar_poss)
                        if( !context.m_resolve.find_trait_impls(sp, trait.m_path, trait.m_params, src, [&](auto _impl_ref, auto _cmp) { return true; }) ) {
                            return CoerceResult::Equality;
                        }
                    }
                }

                if( context_mut )
                {
                    for(const auto& marker : dep->m_markers)
                    {
                        context_mut->add_trait_bound(sp, src,  marker.m_path, marker.m_params.clone());
                    }
                }
                else
                {
                    // TODO: Should this check?
                }

                // Add _Unsize operator
                return CoerceResult::Unsize;
            }
        }

        // Find an Unsize impl?
        struct H {
            static bool type_is_bounded(const ::HIR::TypeRef& ty)
            {
                if( ty.data().is_Generic() ) {
                    return true;
                }
                else if( TU_TEST1(ty.data(), Path, .binding.is_Opaque()) ) {
                    return true;
                }
                else {
                    return false;
                }
            }
        };
        if( H::type_is_bounded(src) )
        {
            DEBUG("Search for `Unsize<" << dst << ">` impl for `" << src << "`");

            ImplRef best_impl;
            unsigned int count = 0;

            ::HIR::PathParams   pp { dst.clone() };
            bool found = context.m_resolve.find_trait_impls(sp, context.m_resolve.m_lang_Unsize, pp, src, [&best_impl,&count,&context](auto impl, auto cmp){
                    DEBUG("[check_unsize_tys] Found impl " << impl << (cmp == ::HIR::Compare::Fuzzy ? " (fuzzy)" : ""));
                    if( !impl.overlaps_with(context.m_crate, best_impl) )
                    {
                        // No overlap, count it as a new possibility
                        if( count == 0 )
                            best_impl = mv$(impl);
                        count ++;
                    }
                    else if( impl.more_specific_than(best_impl) )
                    {
                        best_impl = mv$(impl);
                    }
                    else
                    {
                        // Less specific
                    }
                    // TODO: Record the best impl (if fuzzy) and equate params
                    return cmp != ::HIR::Compare::Fuzzy;
                    });
            if( found )
            {
                return CoerceResult::Unsize;
            }
            else if( count == 1 )
            {
                auto pp = best_impl.get_trait_params();
                DEBUG("Fuzzy, best was Unsize" << pp);
                if( context_mut )
                {
                    context_mut->equate_types(sp, dst, pp.m_types.at(0));
                }
                return CoerceResult::Unsize;
            }
            else
            {
                // TODO: Fuzzy?
                //context.equate_types(sp, e.inner, s_e.inner);
                DEBUG("Multiple impls for bounded unsize");
            }
        }

        // Path types
        if( src.data().tag() == dst.data().tag() )
        {
            TU_MATCH_HDRA( (src.data(), dst.data()), {)
            default:
                return CoerceResult::Equality;
            TU_ARMA(Path, se, de) {
                if( se.binding.tag() == de.binding.tag() )
                {
                    TU_MATCHA( (se.binding, de.binding), (sbe, dbe),
                    (Unbound,
                        // Don't care
                        ),
                    (Opaque,
                        // Handled above in bounded
                        ),
                    (ExternType,
                        // Must be equal
                        if( sbe == dbe )
                        {
                            return CoerceResult::Equality;
                        }
                        ),
                    (Enum,
                        // Must be equal
                        if( sbe == dbe )
                        {
                            return CoerceResult::Equality;
                        }
                        ),
                    (Union,
                        if( sbe == dbe )
                        {
                            // Must be equal
                            return CoerceResult::Equality;
                        }
                        ),
                    (Struct,
                        if( sbe == dbe )
                        {
                            const auto& sm = sbe->m_struct_markings;
                            if( sm.dst_type == ::HIR::StructMarkings::DstType::Possible )
                            {
                                DEBUG("Possible DST");
                                const auto& p_src = se.path.m_data.as_Generic().m_params;
                                const auto& p_dst = de.path.m_data.as_Generic().m_params;
                                const auto& isrc = p_src.m_types.at(sm.unsized_param);
                                const auto& idst = p_dst.m_types.at(sm.unsized_param);
                                auto rv = check_unsize_tys(context, sp, idst, isrc, context_mut, nullptr);
                                switch(rv)
                                {
                                case CoerceResult::Fail:
                                case CoerceResult::Unknown:
                                    break;
                                default:
                                    if(context_mut) {
                                        for(size_t i = 0; i < p_src.m_types.size(); i++) {
                                            if( i != sm.unsized_param ) {
                                                context_mut->equate_types(sp, p_dst.m_types.at(i), p_src.m_types[i]);
                                            }
                                        }
                                        for(size_t i = 0; i < p_src.m_values.size(); i++) {
                                            if( i != sm.unsized_param ) {
                                                context_mut->equate_values(sp, p_dst.m_values.at(i), p_src.m_values[i]);
                                            }
                                        }
                                    }
                                    break;
                                }
                                return rv;
                            }
                            else
                            {
                                // Must be equal
                                return CoerceResult::Equality;
                            }
                        }
                        )
                    )
                }
                }
            }
        }

        // If the destination is an Unbound path, return Unknown
        if( TU_TEST1(dst.data(), Path, .binding.is_Unbound()) )
        {
            DEBUG("Unbound destination");
            return CoerceResult::Unknown;
        }


        DEBUG("Reached end of check_unsize_tys, return Equality");
        // TODO: Determine if this unsizing could ever happen.
        return CoerceResult::Equality;
    }

    /// Checks if two types can be a valid coercion
    //
    // General rules:
    // - CoerceUnsized generics/associated types can only involve generics/associated types
    // - CoerceUnsized structs only go between themselves (and either recurse or unsize a parameter)
    // - No other path can implement CoerceUnsized
    // - Pointers do unsizing (and maybe casting)
    // - All other types equate
    CoerceResult check_coerce_tys(
        const Context& context, const Span& sp, const ::HIR::TypeRef& dst, const ::HIR::TypeRef& src_r,
        Context* context_mut=nullptr, ::HIR::ExprNodeP* node_ptr_ptr=nullptr
        )
    {
        auto src = src_r.clone_shallow();
        TRACE_FUNCTION_F(dst << " := " << src);
        // If the types are equal, then return equality
        if( context.m_ivars.types_equal(dst, src) ) {
            return CoerceResult::Equality;
        }
        // If either side is a literal, then can't Coerce
        if( TU_TEST1(dst.data(), Infer, .is_lit()) ) {
            if(!src.data().is_Diverge()) {
                return CoerceResult::Equality;
            }
        }
        // Nothing but `!` can become `!` (reverse does not hold, `!` can become anything)
        if( dst.data().is_Diverge() ) {
            return CoerceResult::Equality;
        }
        if( TU_TEST1(src.data(), Infer, .is_lit()) ) {
            return CoerceResult::Equality;
        }

        // TODO: If the destination is bounded to be Sized, equate and return.
        // If both sides are `_`, then can't know about coerce yet
        if( dst.data().is_Infer() && src.data().is_Infer() ) {
            // Add possibilities both ways
            if(context_mut)
            {
                context_mut->possible_equate_ivar(sp, src.data().as_Infer().index, dst, Context::PossibleTypeSource::CoerceTo);
                context_mut->possible_equate_ivar(sp, dst.data().as_Infer().index, src, Context::PossibleTypeSource::CoerceFrom);
            }
            return CoerceResult::Unknown;
        }

        struct H {
            static bool type_is_bounded(const ::HIR::TypeRef& ty)
            {
                if( ty.data().is_Generic() ) {
                    return true;
                }
                else if( TU_TEST1(ty.data(), Path, .binding.is_Opaque()) ) {
                    return true;
                }
                else {
                    return false;
                }
            }
            static ::HIR::TypeRef make_pruned(Context& context, const ::HIR::TypeRef& ty)
            {
                const auto& binding = ty.data().as_Path().binding;
                const auto& sm = binding.as_Struct()->m_struct_markings;
                ::HIR::GenericPath gp = ty.data().as_Path().path.m_data.as_Generic().clone();
                assert(sm.coerce_param != ~0u);
                gp.m_params.m_types.at(sm.coerce_param) = context.m_ivars.new_ivar_tr();
                return ::HIR::TypeRef::new_path(mv$(gp), binding.as_Struct());
            }
        };
        // A CoerceUnsized generic/aty/erased on one side
        // - If other side is an ivar, do a possible equality and return Unknown
        // - If impl is found, emit _Unsize
        // - Else, equate and return
        // TODO: Should ErasedType be counted here? probably not.
        if( H::type_is_bounded(src) || H::type_is_bounded(dst) )
        {
            const auto& lang_CoerceUnsized = context.m_crate.get_lang_item_path(sp, "coerce_unsized");  // TODO: Pre-load
            // `CoerceUnsized<U> for T` means `T -> U`

            ::HIR::PathParams   pp { dst.clone() };

            // PROBLEM: This can false-negative leading to the types being falsely equated.

            bool fuzzy_match = false;
            ImplRef  best_impl;
            bool found = context.m_resolve.find_trait_impls(sp, lang_CoerceUnsized, pp, src, [&](auto impl, auto cmp)->bool {
                DEBUG("[check_coerce] cmp=" << cmp << ", impl=" << impl);
                // TODO: Allow fuzzy match if it's the only matching possibility?
                // - Recorded for now to know if there could be a matching impl later
                if( cmp == ::HIR::Compare::Fuzzy ) {
                    fuzzy_match = true;
                    if( impl.more_specific_than(best_impl) ) {
                        best_impl = mv$(impl);
                    }
                    else {
                        TODO(sp, "Equal specificity impls");
                    }
                }
                return cmp == ::HIR::Compare::Equal;
                });
            // - Concretely found - emit the _Unsize op and remove this rule
            if( found )
            {
                return CoerceResult::Unsize;
            }
            if( fuzzy_match )
            {
                DEBUG("- best_impl = " << best_impl);
                return CoerceResult::Unknown;
            }
            DEBUG("- No CoerceUnsized impl found");
        }

        // CoerceUnsized struct paths
        // - If one side is an ivar, create a type-pruned version of the other
        // - Recurse/unsize inner value
        if( src.data().is_Infer() && TU_TEST2(dst.data(), Path, .binding, Struct, ->m_struct_markings.coerce_unsized != ::HIR::StructMarkings::Coerce::None) )
        {
            if(context_mut)
            {
#if 0
                auto new_src = H::make_pruned(context, dst);
                context_mut->equate_types(sp, src, new_src);
#else
                context_mut->possible_equate_ivar(sp, src.data().as_Infer().index, dst, Context::PossibleTypeSource::CoerceTo);
            }
#endif
            // TODO: Avoid needless loop return
            return CoerceResult::Unknown;
        }
        if( dst.data().is_Infer() && TU_TEST2(src.data(), Path, .binding, Struct, ->m_struct_markings.coerce_unsized != ::HIR::StructMarkings::Coerce::None) )
        {
            if(context_mut)
            {
#if 0
                auto new_dst = H::make_pruned(context, src);
                context_mut->equate_types(sp, dst, new_dst);
#else
                context_mut->possible_equate_ivar(sp, dst.data().as_Infer().index, src, Context::PossibleTypeSource::CoerceFrom);
#endif
            }
            // TODO: Avoid needless loop return
            return CoerceResult::Unknown;
        }
        if( TU_TEST1(dst.data(), Path, .binding.is_Struct()) && TU_TEST1(src.data(), Path, .binding.is_Struct()) )
        {
            const auto& spbe = src.data().as_Path().binding.as_Struct();
            const auto& dpbe = dst.data().as_Path().binding.as_Struct();
            if( spbe != dpbe )
            {
                // TODO: Error here? (equality in caller will cause an error)
                return CoerceResult::Equality;
            }
            const auto& sm = spbe->m_struct_markings;
            // Has to be equal?
            if( sm.coerce_unsized == ::HIR::StructMarkings::Coerce::None )
                return CoerceResult::Equality;

            // Equate all parameters that aren't the unsizing param
            const auto& pp_dst = dst.data().as_Path().path.m_data.as_Generic().m_params;
            const auto& pp_src = src.data().as_Path().path.m_data.as_Generic().m_params;
            DEBUG(pp_dst << " = " << pp_src);
            assert(pp_dst.m_types.size() == pp_src.m_types.size());
            for(size_t i = 0; i < pp_src.m_types.size(); i ++ ) {
                if( i == sm.coerce_param )
                    continue ;
                if(context_mut)
                {
                    context_mut->equate_types(sp, pp_dst.m_types.at(i), pp_src.m_types.at(i));
                }
            }
            assert(pp_dst.m_values.size() == pp_src.m_values.size());
            for(size_t i = 0; i < pp_src.m_values.size(); i ++ ) {
                TODO(sp, "Handle values in CoerceUnsized");
            }
            // Check coercion/unsizing of the target type
            const auto& idst = pp_dst.m_types.at(sm.coerce_param);
            const auto& isrc = pp_src.m_types.at(sm.coerce_param);
            switch( sm.coerce_unsized )
            {
            case ::HIR::StructMarkings::Coerce::None:
                throw "";
            case ::HIR::StructMarkings::Coerce::Passthrough:
                DEBUG("Passthough CoerceUnsized");
                // TODO: Force emitting `_Unsize` instead of anything else
                return check_coerce_tys(context, sp, idst, isrc, context_mut, nullptr);
            case ::HIR::StructMarkings::Coerce::Pointer:
                DEBUG("Pointer CoerceUnsized");
                return check_unsize_tys(context, sp, idst, isrc, context_mut, nullptr);
            }
        }

        // If either side is an unbound UFCS, can't know yet
        if( TU_TEST1(dst.data(), Path, .binding.is_Unbound()) || TU_TEST1(src.data(), Path, .binding.is_Unbound()) )
        {
            return CoerceResult::Unknown;
        }

        // Any other type, check for pointer
        // - If not a pointer, return Equality
        if(const auto* sep = src.data().opt_Infer())
        {
            const auto& se = *sep;
            ASSERT_BUG(sp, ! dst.data().is_Infer(), "Already handled?");

            // Add a disable flag to all ivars within the `dst` type
            // - This should prevent early guessing
            if(context_mut)
            {
                context_mut->possible_equate_type_unknown(sp, dst, Context::IvarUnknownType::From);
            }

            // If the other side is a pointer
            if( dst.data().is_Pointer() || dst.data().is_Borrow() )
            {
                if(context_mut)
                {
                    context_mut->possible_equate_ivar(sp, se.index, dst, Context::PossibleTypeSource::CoerceTo);
                }
                return CoerceResult::Unknown;
            }
            // Not a pointer (handled just above), and not a CoerceUnsized struct (handled farther above)
            // - Could be a generic with CoerceUnsized
            // HACK: Composite types may hit issues with `!` within them, primtives don't have this issue?
            // - Unsure why, but if the below is unconditional then there's a type error with `Result<!,...>` and `Try`
            else if( dst.data().is_Primitive() )
            {
                return CoerceResult::Equality;
            }
            else
            {
                if(context_mut)
                {
                    context_mut->possible_equate_ivar(sp, se.index, dst, Context::PossibleTypeSource::UnsizeFrom);
                }
                return CoerceResult::Unknown;
            }
        }
        else if( src.data().is_Diverge() ) {
            if( const auto* dep = dst.data().opt_Infer() )
            {
                if(context_mut)
                {
                    context_mut->possible_equate_ivar(sp, dep->index, src, Context::PossibleTypeSource::UnsizeFrom);
                }
                return CoerceResult::Unknown;
            }
            else {
                // Downstream just handles this
                //return CoerceResult::Unsize;
                return CoerceResult::Custom;
            }
        }
        else if(const auto* sep = src.data().opt_Pointer())
        {
            const auto& se = *sep;
            if( const auto* dep = dst.data().opt_Infer() )
            {
                if(context_mut)
                {
                    context_mut->possible_equate_ivar(sp, dep->index, src, Context::PossibleTypeSource::CoerceFrom);
                }
                return CoerceResult::Unknown;
            }
            else if( const auto* dep = dst.data().opt_Pointer() )
            {
                // Check strength reduction
                if( dep->type < se.type )
                {
                    if( node_ptr_ptr )
                    {
                        // > Convert `src` to `src as *mut SI`
                        auto new_type = ::HIR::TypeRef::new_pointer(dep->type, se.inner.clone());

                        // If the coercion is of a block, do the reborrow on the last node of the block
                        // - Cleans up the dumped MIR and prevents needing a reborrow elsewhere.
                        // - TODO: Alter the block's result types
                        ::HIR::ExprNodeP* npp = node_ptr_ptr;   // Note: Node pointer can be null (when checking)
                        while( auto* p = dynamic_cast< ::HIR::ExprNode_Block*>(npp->get()) )
                        {
                            DEBUG("- Propagate to the last node of a _Block");
                            ASSERT_BUG( p->span(), context.m_ivars.types_equal(p->m_res_type, p->m_value_node->m_res_type),
                                "Block and result mismatch - " << context.m_ivars.fmt_type(p->m_res_type) << " != " << context.m_ivars.fmt_type(p->m_value_node->m_res_type));
                            if( !context.m_ivars.types_equal(p->m_res_type, src) ) {
                                DEBUG("Block and result mismatch - " << context.m_ivars.fmt_type(p->m_res_type) << " != " << context.m_ivars.fmt_type(src));
                                return CoerceResult::Unknown;
                            }
                            if(context_mut)
                            {
                                p->m_res_type = dst.clone();
                            }
                            npp = &p->m_value_node;
                            ASSERT_BUG(sp, *npp, "Null node pointer on block");
                        }
                        ::HIR::ExprNodeP& node_ptr = *npp;

                        if(context_mut)
                        {
                            // Add cast down
                            auto span = node_ptr->span();
                            // *<inner>
                            DEBUG("- NEWNODE _Cast -> " << new_type);
                            node_ptr = NEWNODE( new_type.clone(), span, _Cast,  mv$(node_ptr), new_type.clone() );
                            context.m_ivars.get_type(node_ptr->m_res_type);

                            context_mut->m_ivars.mark_change();
                        }

                        // Continue on with coercion (now that node_ptr is updated)
                        switch( check_unsize_tys(context, sp, dep->inner, se.inner, context_mut, &node_ptr) )
                        {
                        case CoerceResult::Fail:
                            return CoerceResult::Fail;
                        case CoerceResult::Unknown:
                            // Add new coercion at the new inner point
                            if( &node_ptr != node_ptr_ptr )
                            {
                                DEBUG("Unknown check_unsize_tys after autoderef - " << dst << " := " << node_ptr->m_res_type);
                                if(context_mut)
                                {
                                    context_mut->equate_types_coerce(sp, dst, node_ptr);
                                }
                                return CoerceResult::Custom;
                            }
                            else
                            {
                                return CoerceResult::Unknown;
                            }
                        case CoerceResult::Custom:
                            return CoerceResult::Custom;
                        case CoerceResult::Equality:
                            if(context_mut)
                            {
                                context_mut->equate_types(sp, dep->inner, se.inner);
                            }
                            return CoerceResult::Custom;
                        case CoerceResult::Unsize:
                            if(context_mut)
                            {
                                DEBUG("- NEWNODE _Unsize " << &node_ptr << " " << &*node_ptr << " -> " << dst);
                                auto span = node_ptr->span();
                                node_ptr = NEWNODE( dst.clone(), span, _Unsize,  mv$(node_ptr), dst.clone() );
                            }
                            return CoerceResult::Custom;
                        }
                        throw "";
                    }
                    else
                    {
                        //TODO(sp, "Borrow strength reduction with no node pointer - " << src << " -> " << dst);
                        DEBUG("Pointer strength reduction with no node pointer - " << src << " -> " << dst);
                        return CoerceResult::Unsize;
                    }
                }
                else if( dep->type == se.type ) {
                    // Valid.
                }
                else {
                    //ERROR(sp, E0000, "Type mismatch between " << dst << " and " << src << " - Borrow classes differ");
                    // TODO: return CoerceResult::Failed? (indicating that it failed outright, don't even try)
                    return CoerceResult::Equality;
                }
                ASSERT_BUG(sp, dep->type == se.type, "Pointer strength mismatch");

                // Call unsizing code
                return check_unsize_tys(context, sp, dep->inner, se.inner, context_mut, node_ptr_ptr);
            }
            else
            {
                // TODO: Error here? (leave to caller)
                return CoerceResult::Equality;
            }
        }
        else if(const auto* sep = src.data().opt_Borrow())
        {
            const auto& se = *sep;
            if( const auto* dep = dst.data().opt_Infer() )
            {
                if(context_mut)
                {
                    context_mut->possible_equate_ivar(sp, dep->index, src, Context::PossibleTypeSource::CoerceFrom);
                }
                return CoerceResult::Unknown;
            }
            else if( const auto* dep = dst.data().opt_Pointer() )
            {
                // Add cast to the pointer (if valid strength reduction)
                // Call unsizing code on casted value
                
                // Borrows can coerce to pointers while reducing in strength
                // - Shared < Unique. If the destination is not weaker or equal to the source, it's an error
                if( !(dep->type <= se.type) ) {
                    ERROR(sp, E0000, "Type mismatch between " << dst << " and " << src << " - Mutability not compatible");
                }

                // Add downcast
                switch( check_unsize_tys(context, sp, dep->inner, se.inner, context_mut, node_ptr_ptr) )
                {
                case CoerceResult::Fail:
                    return CoerceResult::Fail;
                case CoerceResult::Unknown:
                    return CoerceResult::Unknown;
                case CoerceResult::Custom:
                    return CoerceResult::Custom;
                case CoerceResult::Equality:
                    if(node_ptr_ptr && context_mut)
                    {
                        auto& node_ptr = *node_ptr_ptr;
                        {
                            DEBUG("- NEWNODE _Cast " << &*node_ptr << " -> " << dst);
                            auto span = node_ptr->span();
                            node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Cast( mv$(span), mv$(node_ptr), dst.clone() ));
                            node_ptr->m_res_type = dst.clone();
                        }
                    }

                    if(context_mut)
                    {
                        context_mut->equate_types(sp, dep->inner, se.inner);
                    }
                    return CoerceResult::Custom;
                case CoerceResult::Unsize:
                    if(node_ptr_ptr && context_mut)
                    {
                        auto& node_ptr = *node_ptr_ptr;
                        auto dst_b = ::HIR::TypeRef::new_borrow(se.type, dep->inner.clone());
                        DEBUG("- NEWNODE _Unsize " << &*node_ptr << " -> " << dst_b);
                        {
                            auto span = node_ptr->span();
                            node_ptr = NEWNODE( dst_b.clone(), span, _Unsize,  mv$(node_ptr), dst_b.clone() );
                        }

                        DEBUG("- NEWNODE _Cast " << &*node_ptr << " -> " << dst);
                        {
                            auto span = node_ptr->span();
                            node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Cast( mv$(span), mv$(node_ptr), dst.clone() ));
                            node_ptr->m_res_type = dst.clone();
                        }
                    }
                    return CoerceResult::Custom;
                }
                throw "";
            }
            else if( const auto* dep = dst.data().opt_Borrow() )
            {
                // Check strength reduction
                if( dep->type < se.type )
                {
                    if( node_ptr_ptr )
                    {
                        // > Goes from `src` -> `*src` -> `&`dep->type` `*src`
                        const auto inner_ty = se.inner.clone();
                        auto dst_bt = dep->type;
                        auto new_type = ::HIR::TypeRef::new_borrow(dst_bt, inner_ty.clone());

                        // If the coercion is of a block, do the reborrow on the last node of the block
                        // - Cleans up the dumped MIR and prevents needing a reborrow elsewhere.
                        // - TODO: Alter the block's result types
                        {
                            ::HIR::ExprNodeP* npp = node_ptr_ptr;
                            while( auto* p = dynamic_cast< ::HIR::ExprNode_Block*>(npp->get()) )
                            {
                                if( !context.m_ivars.types_equal(p->m_res_type, src) ) {
                                    DEBUG("(borrow) Block and result mismatch - " << context.m_ivars.fmt_type(p->m_res_type) << " != " << context.m_ivars.fmt_type(src));
                                    return CoerceResult::Unknown;
                                }
                                npp = &p->m_value_node;
                                ASSERT_BUG(sp, *npp, "Null node pointer in block");
                            }
                        }
                        ::HIR::ExprNodeP* npp = node_ptr_ptr;
                        while( auto* p = dynamic_cast< ::HIR::ExprNode_Block*>(npp->get()) )
                        {
                            DEBUG("- Propagate borrow coercion to the last node of a _Block: " << context.m_ivars.fmt_type(p->m_res_type));
                            ASSERT_BUG( p->span(), context.m_ivars.types_equal(p->m_res_type, p->m_value_node->m_res_type),
                                "(borrow) Block and result mismatch - " << context.m_ivars.fmt_type(p->m_res_type) << " != " << context.m_ivars.fmt_type(p->m_value_node->m_res_type));
                            ASSERT_BUG( p->span(), context.m_ivars.types_equal(p->m_res_type, src),
                                "(borrow) Block and result mismatch - " << context.m_ivars.fmt_type(p->m_res_type) << " != " << context.m_ivars.fmt_type(src) );
                            if(context_mut)
                            {
                                p->m_res_type = dst.clone();
                            }
                            npp = &p->m_value_node;
                        }
                        ::HIR::ExprNodeP& node_ptr = *npp;

                        if(context_mut)
                        {
                            // Add cast down
                            auto span = node_ptr->span();
                            // *<inner>
                            DEBUG("- Deref -> " << inner_ty);
                            node_ptr = NEWNODE( inner_ty.clone(), span, _Deref,  mv$(node_ptr) );
                            context.m_ivars.get_type(node_ptr->m_res_type);
                            // &*<inner>
                            DEBUG("- Borrow -> " << new_type);
                            node_ptr = NEWNODE( mv$(new_type) , span, _Borrow,  dst_bt, mv$(node_ptr) );
                            context.m_ivars.get_type(node_ptr->m_res_type);

                            context_mut->m_ivars.mark_change();
                        }

                        // Continue on with coercion (now that node_ptr is updated)
                        switch( check_unsize_tys(context, sp, dep->inner, se.inner, context_mut, &node_ptr) )
                        {
                        case CoerceResult::Fail:
                            return CoerceResult::Fail;
                        case CoerceResult::Unknown:
                            // Add new coercion at the new inner point
                            if( &node_ptr != node_ptr_ptr )
                            {
                                DEBUG("Unknown check_unsize_tys after autoderef - " << dst << " := " << node_ptr->m_res_type);
                                if(context_mut)
                                {
                                    context_mut->equate_types_coerce(sp, dst, node_ptr);
                                }
                                return CoerceResult::Custom;
                            }
                            else
                            {
                                return CoerceResult::Unknown;
                            }
                        case CoerceResult::Custom:
                            return CoerceResult::Custom;
                        case CoerceResult::Equality:
                            if(context_mut)
                            {
                                context_mut->equate_types(sp, dep->inner, se.inner);
                            }
                            return CoerceResult::Custom;
                        case CoerceResult::Unsize:
                            if(context_mut)
                            {
                                DEBUG("- NEWNODE _Unsize " << &node_ptr << " " << &*node_ptr << " -> " << dst);
                                auto span = node_ptr->span();
                                node_ptr = NEWNODE( dst.clone(), span, _Unsize,  mv$(node_ptr), dst.clone() );
                            }
                            return CoerceResult::Custom;
                        }
                        throw "";
                    }
                    else
                    {
                        //TODO(sp, "Borrow strength reduction with no node pointer - " << src << " -> " << dst);
                        DEBUG("Borrow strength reduction with no node pointer - " << src << " -> " << dst);
                        return CoerceResult::Unsize;
                    }
                }
                else if( dep->type == se.type ) {
                    // Valid.
                }
                else {
                    //ERROR(sp, E0000, "Type mismatch between " << dst << " and " << src << " - Borrow classes differ");
                    // TODO: return CoerceResult::Failed? (indicating that it failed outright, don't even try)
                    return CoerceResult::Equality;
                }
                ASSERT_BUG(sp, dep->type == se.type, "Borrow strength mismatch");

                // Call unsizing code
                return check_unsize_tys(context, sp, dep->inner, se.inner, context_mut, node_ptr_ptr);
            }
            else
            {
                // TODO: Error here?
                return CoerceResult::Equality;
            }
        }
        else if( src.data().is_Closure() )
        {
            const auto& se = src.data().as_Closure();
            if( dst.data().is_Function() )
            {
                const auto& de = dst.data().as_Function();
                if(node_ptr_ptr)
                {
                    auto& node_ptr = *node_ptr_ptr;
                    auto span = node_ptr->span();
                    if( de.m_abi != ABI_RUST ) {
                        ERROR(span, E0000, "Cannot use closure for extern function pointer");
                    }
                    if( de.m_arg_types.size() != se.node->m_args.size() ) {
                        ERROR(span, E0000, "Mismatched argument count coercing closure to fn(...)");
                    }
                    if(context_mut)
                    {
                        auto pp = de.hrls.make_empty_params(true);
                        MonomorphHrlsOnly   ms(pp);
                        for(size_t i = 0; i < de.m_arg_types.size(); i++)
                        {
                            context_mut->equate_types(sp, ms.monomorph_type(sp, de.m_arg_types[i]), se.node->m_args[i].second);
                        }
                        context_mut->equate_types(sp, ms.monomorph_type(sp, de.m_rettype), se.node->m_return);
                        node_ptr = NEWNODE( dst.clone(), span, _Cast,  mv$(node_ptr), dst.clone() );
                    }
                }
                return CoerceResult::Custom;
            }
            else if( const auto* dep = dst.data().opt_Infer() )
            {
                if(context_mut)
                {
                    // Prevent inferrence of argument/return types
                    for(const auto& at : se.node->m_args)
                        context_mut->possible_equate_type_unknown(sp, at.second, Context::IvarUnknownType::To);
                    context_mut->possible_equate_type_unknown(sp, se.node->m_return, Context::IvarUnknownType::Bound);
                    // Add as a possiblity
                    context_mut->possible_equate_ivar(sp, dep->index, src, Context::PossibleTypeSource::CoerceFrom);
                }
                return CoerceResult::Unknown;
            }
            else
            {
                return CoerceResult::Equality;
            }
        }
        else if( const auto* se = src.data().opt_NamedFunction() )
        {
            if( const auto* de = dst.data().opt_Function() )
            {
                auto ft = context.m_resolve.expand_associated_types(sp, HIR::TypeRef(se->decay(sp)));
                const auto* se = &ft.data().as_Function();

                // ABI must match
                if( se->m_abi != de->m_abi )
                    return CoerceResult::Equality;
                // const can be removed
                //if( se->is_const != de->is_const && de->is_const ) // Error going TO a const function pointer
                //    return CoerceResult::Equality;
                // unsafe can be added
                if( se->is_unsafe != de->is_unsafe && se->is_unsafe ) // Error going FROM an unsafe function pointer
                    return CoerceResult::Equality;
                // argument/return types must match
                if( se->m_arg_types.size() != de->m_arg_types.size() )
                    return CoerceResult::Equality;

                if(context_mut)
                {
                    auto& node_ptr = *node_ptr_ptr;
                    auto span = node_ptr->span();

                    auto s_pp = se->hrls.make_empty_params(true);
                    MonomorphHrlsOnly   s_ms(s_pp);
                    auto d_pp = de->hrls.make_empty_params(true);
                    MonomorphHrlsOnly   d_ms(d_pp);
                    for(size_t i = 0; i < de->m_arg_types.size(); i++)
                    {
                        context_mut->equate_types(sp, d_ms.monomorph_type(span, de->m_arg_types[i]), s_ms.monomorph_type(span, se->m_arg_types[i]));
                    }
                    context_mut->equate_types(sp, d_ms.monomorph_type(span, de->m_rettype), s_ms.monomorph_type(span, se->m_rettype));
                    node_ptr = NEWNODE( dst.clone(), span, _Cast,  mv$(node_ptr), dst.clone() );
                }
                return CoerceResult::Custom;
            }
            // Function pointers can coerce safety
            else if( const auto* dep = dst.data().opt_Infer() )
            {
                if(context_mut)
                {
                    context_mut->possible_equate_ivar(sp, dep->index, src, Context::PossibleTypeSource::UnsizeFrom);
                }
                return CoerceResult::Unknown;
            }
            else
            {
                return CoerceResult::Equality;
            }
        }
        else if( const auto* se = src.data().opt_Function() )
        {
            if( const auto* de = dst.data().opt_Function() )
            {

                auto& node_ptr = *node_ptr_ptr;
                auto span = node_ptr->span();
                DEBUG("Function pointer coercion");
                // ABI must match
                if( se->m_abi != de->m_abi )
                    return CoerceResult::Equality;
                // const can be removed
                //if( se->is_const != de->is_const && de->is_const ) // Error going TO a const function pointer
                //    return CoerceResult::Equality;
                // unsafe can be added
                if( se->is_unsafe != de->is_unsafe && se->is_unsafe ) // Error going FROM an unsafe function pointer
                    return CoerceResult::Equality;
                // argument/return types must match
                if( de->m_arg_types.size() != se->m_arg_types.size() )
                    return CoerceResult::Equality;
                if(context_mut)
                {
                    auto s_pp = se->hrls.make_empty_params(true);
                    MonomorphHrlsOnly   s_ms(s_pp);
                    auto d_pp = de->hrls.make_empty_params(true);
                    MonomorphHrlsOnly   d_ms(d_pp);
                    for(size_t i = 0; i < de->m_arg_types.size(); i++)
                    {
                        context_mut->equate_types(sp, d_ms.monomorph_type(span, de->m_arg_types[i]), s_ms.monomorph_type(span, se->m_arg_types[i]));
                    }
                    context_mut->equate_types(sp, d_ms.monomorph_type(span, de->m_rettype), s_ms.monomorph_type(span, se->m_rettype));
                    node_ptr = NEWNODE( dst.clone(), span, _Cast,  mv$(node_ptr), dst.clone() );
                }
                return CoerceResult::Custom;
            }
            // Function pointers can coerce safety
            else if( const auto* dep = dst.data().opt_Infer() )
            {
                if(context_mut)
                {
                    context_mut->possible_equate_ivar(sp, dep->index, src, Context::PossibleTypeSource::UnsizeFrom);
                }
                return CoerceResult::Unknown;
            }
            else
            {
                return CoerceResult::Equality;
            }
        }
        else
        {
            // TODO: ! should be handled above or in caller?
            return CoerceResult::Equality;
        }
    }
    bool check_coerce(Context& context, const Context::Coercion& v)
    {
        ::HIR::ExprNodeP& node_ptr = *v.right_node_ptr;
        const auto& sp = node_ptr->span();
        const auto& ty_dst = context.m_ivars.get_type(v.left_ty);
        const auto& ty_src = context.m_ivars.get_type(node_ptr->m_res_type);
        TRACE_FUNCTION_FR(
                v << " - " << context.m_ivars.fmt_type(ty_dst) << " := " << context.m_ivars.fmt_type(ty_src),
                v << " - " << context.m_ivars.fmt_type(v.left_ty) << " := " << context.m_ivars.fmt_type(node_ptr->m_res_type)
                );

        // NOTE: Coercions can happen on comparisons, which means that checking for Sized isn't valid (because you can compare unsized types)

        switch( check_coerce_tys(context, sp, ty_dst, ty_src, &context, &node_ptr) )
        {
        case CoerceResult::Fail:
            return false;
        case CoerceResult::Unknown:
            DEBUG("Unknown - keep");
            return false;
        case CoerceResult::Custom:
            DEBUG("Custom - Completed");
            return true;
        case CoerceResult::Equality:
            DEBUG("Trigger equality - Completed");
            context.equate_types(sp, ty_dst, ty_src);
            return true;
        case CoerceResult::Unsize:
            DEBUG("Add _Unsize " << &*node_ptr << " -> " << ty_dst);
            auto span = node_ptr->span();
            node_ptr = NEWNODE( ty_dst.clone(), span, _Unsize,  mv$(node_ptr), ty_dst.clone() );
            return true;
        }
        throw "";
    }

    bool check_associated(Context& context, const Context::Associated& v)
    {
        const auto& sp = v.span;
        TRACE_FUNCTION_F(v);

        ::HIR::TypeRef  output_type;

        struct H {
            static bool type_is_num(const ::HIR::TypeRef& t) {
                TU_MATCH_HDRA( (t.data()), {)
                default:
                    return false;
                TU_ARMA(Primitive, e) {
                    switch(e)
                    {
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::Char:
                        return false;
                    default:
                        return true;
                    }
                    }
                TU_ARMA(Infer, e) {
                    return e.is_lit();
                    }
                }
                throw "unreachable";
            }
        };

        // MAGIC! Have special handling for operator overloads
        if( v.is_operator ) {
            if( v.params.m_types.size() == 0 )
            {
                // Uni ops = If the value is a primitive, the output is the same type
                const auto& ty = context.get_type(v.impl_ty);
                const auto& res = context.get_type(v.left_ty);
                if( H::type_is_num(ty) ) {
                    DEBUG("- Magic inferrence link for uniops on numerics");
                    context.equate_types(sp, res, ty);
                }
            }
            else if( v.params.m_types.size() == 1 )
            {
                // Binary operations - If both types are primitives, the output is the lefthand side
                // NOTE: Comparison ops don't (currently) see this
                const auto& left = v.impl_ty; // yes, impl = LHS of binop
                const auto& right = v.params.m_types.at(0);
                const auto& res = v.left_ty;
                if( H::type_is_num(context.get_type(left)) && H::type_is_num(context.get_type(right)) )
                {
                    DEBUG("- Magic inferrence link for binops on numerics");
                    if( v.name == "" )
                    {
                        // Comparison op, output already known to be `bool`
                    }
                    else
                    {
                        context.equate_types(sp, res, left);
                    }
                    if( v.trait == context.m_crate.get_lang_item_path_opt("shl") || v.trait == context.m_crate.get_lang_item_path_opt("shr") )
                    {
                        // Shifts can have mismatched types on each side.
                    }
                    else
                    {
                        // NOTE: This only holds if not a shift
                        context.equate_types(sp, left, right);
                    }
                    if( context.get_type(left).data().is_Infer() && context.get_type(right).data().is_Infer() && context.get_type(res).data().is_Infer() ) {
                        context.possible_equate_type_unknown(sp, right, Context::IvarUnknownType::To);
                        DEBUG("> All are infer, skip");
                        return false;
                    }
                }

                context.possible_equate_type_unknown(sp, right, Context::IvarUnknownType::To);
            }
            else
            {
                BUG(sp, "Associated type rule with `is_operator` set but an incorrect parameter count");
            }
        }

        // If the output type is present, prevent it from being guessed
        // - This generates an exact equation.
        if( v.left_ty != ::HIR::TypeRef() )
        {
            //context.equate_types_bounded(sp, v.left_ty, {});
            context.possible_equate_type_unknown(sp, v.left_ty, Context::IvarUnknownType::Bound);
        }

        // If the impl type is an unbounded ivar, and there's no trait args - don't bother searching
        if( const auto* e = context.m_ivars.get_type(v.impl_ty).data().opt_Infer() )
        {
            // TODO: ?
            if( !e->is_lit() && v.params.m_types.empty() )
            {
                return false;
            }

            // If the type is completely unbounded, then any lookup will fail.
            // - Disable inference on the type params (as a future impl will add bounds)
            if( !e->is_lit() )
            {
                for(const auto& t : v.params.m_types)
                {
                    //context.equate_types_bounded(sp, t, {});
                    context.possible_equate_type_unknown(sp, t, Context::IvarUnknownType::To);
                }
                return false;
            }
        }

        // Locate applicable trait impl
        unsigned int count = 0;
        DEBUG("Searching for impl " << v.trait << v.params << " for " << context.m_ivars.fmt_type(v.impl_ty));
        struct Possibility {
            ::HIR::TypeRef  impl_ty;
            ::HIR::PathParams   params;
            ImplRef  impl_ref;
        };
        ::std::vector<Possibility>  possible_impls;
        try {
        bool found = context.m_resolve.find_trait_impls(sp, v.trait, v.params,  v.impl_ty,
            [&](ImplRef impl, HIR::Compare cmp) {
                DEBUG("[check_associated] Found cmp=" << cmp << " " << impl);
                if( v.name != "" ) {
                    // TODO: Are params needed for these ATY bounds?
                    auto out_ty_o = impl.get_type(v.name.c_str(), {});
                    if( out_ty_o == ::HIR::TypeRef() )
                    {
                        out_ty_o = ::HIR::TypeRef::new_path(::HIR::Path( v.impl_ty.clone(), ::HIR::GenericPath(v.trait, v.params.clone()), v.name, ::HIR::PathParams() ), {});
                    }
                    out_ty_o = context.m_resolve.expand_associated_types(sp, mv$(out_ty_o));

                    // TODO: if this is an unbound UfcsUnknown, treat as a fuzzy match.
                    // - Shouldn't compare_with_placeholders do that?

                    // - If we're looking for an associated type, allow it to eliminate impossible impls
                    //  > This makes `let v: usize = !0;` work without special cases
                    auto cmp2 = v.left_ty.compare_with_placeholders(sp, out_ty_o, context.m_ivars.callback_resolve_infer());
                    if( cmp2 == ::HIR::Compare::Unequal ) {
                        DEBUG("[check_associated] - (fail) known result can't match (" << context.m_ivars.fmt_type(v.left_ty) << " and " << context.m_ivars.fmt_type(out_ty_o) << ")");
                        return false;
                    }
                    // if solid or fuzzy, leave as-is
                    output_type = mv$( out_ty_o );
                    DEBUG("[check_associated] cmp = " << cmp << " (2) out=" << output_type);
                }
                if( cmp == ::HIR::Compare::Equal ) {
                    // NOTE: Sometimes equal can be returned when it's not 100% equal (TODO)
                    // - Equate the types
                    auto itp = impl.get_trait_params();
                    ASSERT_BUG(sp, v.params.m_types.size() == itp.m_types.size(), "Parameter count mismatch between impl and rule: r=" << v.params << " i=" << itp);
                    for(unsigned int i = 0; i < v.params.m_types.size(); i ++)
                    {
                        context.equate_types(sp, v.params.m_types[i], itp.m_types[i]);
                    }
                    for(unsigned int i = 0; i < v.params.m_values.size(); i ++)
                    {
                        context.equate_values(sp, v.params.m_values[i], itp.m_values[i]);
                    }
                    return true;
                }
                else {
                    count += 1;
                    DEBUG("[check_associated] - (possible) " << impl);

                    auto impl_ty = impl.get_impl_type();
                    auto impl_params = impl.get_trait_params();

                    impl_ty = context.m_resolve.expand_associated_types(sp, std::move(impl_ty));
                    for(auto& t : impl_params.m_types) {
                        t = context.m_resolve.expand_associated_types(sp, mv$(t));
                    }

                    if( possible_impls.empty() ) {
                        DEBUG("[check_associated] First - " << impl);
                        possible_impls.push_back({ std::move(impl_ty), std::move(impl_params), std::move(impl) });
                    }
                    // If there is an existing impl, determine if this is part of the same specialisation tree
                    // - If more specific, replace. If less, ignore.
                    // NOTE: `overlaps_with` (should be) reflective
                    else
                    {
                        bool was_used = false;
                        for(auto& possible_impl : possible_impls)
                        {
                            const auto& best_impl = possible_impl.impl_ref;
                            // TODO: Handle duplicates (from overlapping bounds)
                            if( impl.overlaps_with(context.m_crate, best_impl) )
                            {
                                DEBUG("[check_associated] - Overlaps with existing - " << best_impl);
                                // if not more specific than the existing best, ignore.
                                if( ! impl.more_specific_than(best_impl) )
                                {
                                    DEBUG("[check_associated] - Less specific than existing");
                                    // NOTE: This picks the _least_ specific impl
                                    possible_impl.impl_ty = ::std::move(impl_ty);
                                    possible_impl.params = ::std::move(impl_params);
                                    possible_impl.impl_ref = ::std::move(impl);
                                    count -= 1;
                                }
                                // If the existing best is not more specific than the new one, use the new one
                                else if( ! best_impl.more_specific_than(impl) )
                                {
                                    DEBUG("[check_associated] - More specific than existing - " << best_impl);
                                    count -= 1;
                                }
                                else
                                {
                                    // Supposedly, `more_specific_than` should be reflexive...
                                    DEBUG("[check_associated] > Neither is more specific. Error?");
                                }
                                was_used = true;
                                break;
                            }
                            else
                            {
                                // Disjoint impls.
                                DEBUG("[check_associated] Disjoint with " << best_impl);
                            }

                            // Edge case: Might be just outright identical
                            if( possible_impl.impl_ty == impl_ty && possible_impl.params == impl_params )
                            {
                                auto t1 = v.name == "" ? HIR::TypeRef() : possible_impl.impl_ref.get_type(v.name.c_str(), {});
                                auto t2 = v.name == "" ? HIR::TypeRef() : impl.get_type(v.name.c_str(), {});
                                if(v.name == "" || t1 == t2 || t2 == HIR::TypeRef())
                                {
                                    DEBUG("[check_associated] HACK: Same type and params, and ATY matches or this impl doesn't have it");
                                    was_used = true;
                                    count -= 1;
                                    break;
                                }
                                else if( t1 == HIR::TypeRef() )
                                {
                                    DEBUG("[check_associated] - Same type and params, and has an ATY (while original doesn't)");
                                    // NOTE: This picks the _least_ specific impl
                                    possible_impl.impl_ty = ::std::move(impl_ty);
                                    possible_impl.params = ::std::move(impl_params);
                                    possible_impl.impl_ref = ::std::move(impl);
                                    was_used = true;
                                    count -= 1;
                                    break;
                                }
                                else
                                {
                                    DEBUG("[check_associated] HACK: Same type and params, but ATY mismatch - "
                                        << possible_impl.impl_ref.get_type(v.name.c_str(), {}) << " != " << impl.get_type(v.name.c_str(), {}));
                                }
                            }
                        }
                        if( !was_used )
                        {
                            DEBUG("[check_associated] Add new possible: " << impl);
                            possible_impls.push_back({ ::std::move(impl_ty), ::std::move(impl_params), ::std::move(impl) });
                        }
                    }

                    return false;
                }
            });
        if( found ) {
            // Fully-known impl
            DEBUG("Fully-known impl located");
            if( v.name != "" ) {
                // Stop this from just pushing the same rule again.
                if( output_type.data().is_Path() && output_type.data().as_Path().path.m_data.is_UfcsKnown() )
                {
                    const auto& te = output_type.data().as_Path();
                    const auto& pe = te.path.m_data.as_UfcsKnown();
                    // If the target type is unbound, and is this rule exactly, don't return success
                    if( te.binding.is_Unbound() && pe.type == v.impl_ty && pe.item == v.name && pe.trait.m_path == v.trait && pe.trait.m_params == v.params)
                    {
                        DEBUG("Would re-create the same rule, returning unconsumed");
                        return false;
                    }
                }
                context.equate_types(sp, v.left_ty, output_type);
            }
            // TODO: Any equating of type params?
            return true;
        }
        else if( count == 0 ) {
            // No applicable impl
            // - TODO: This should really only fire when there isn't an impl. But it currently fires when _
            if( v.name == "" )
                DEBUG("No impl of " << v.trait << context.m_ivars.fmt(v.params) << " for " << context.m_ivars.fmt_type(v.impl_ty));
            else
                DEBUG("No impl of " << v.trait << context.m_ivars.fmt(v.params) << " for " << context.m_ivars.fmt_type(v.impl_ty)
                    << " with " << v.name << " = " << context.m_ivars.fmt_type(v.left_ty));

            const auto& ty = context.get_type(v.impl_ty);
            bool is_known = !ty.data().is_Infer() && !(ty.data().is_Path() && ty.data().as_Path().binding.is_Unbound());
            //bool is_known = !context.m_ivars.type_contains_ivars(v.impl_ty);
            //for(const auto& t : v.params.m_types)
            //    is_known &= !context.m_ivars.type_contains_ivars(t);
            if( !is_known )
            {
                // There's still an ivar (or an unbound UFCS), keep trying
                return false;
            }
            else if( v.trait == context.m_resolve.m_lang_Unsize )
            {
                // TODO: Detect if this was a compiler-generated bound, or was actually in the code.

                ASSERT_BUG(sp, v.params.m_types.size() == 1, "Incorrect number of parameters for Unsize");
                const auto& src_ty = context.get_type(v.impl_ty);
                const auto& dst_ty = context.get_type(v.params.m_types[0]);

                context.equate_types(sp, dst_ty, src_ty);
                return true;
            }
            else
            {
                if( v.name == "" )
                    ERROR(sp, E0000, "Failed to find an impl of " << v.trait << context.m_ivars.fmt(v.params) << " for " << context.m_ivars.fmt_type(v.impl_ty));
                else
                    ERROR(sp, E0000, "Failed to find an impl of " << v.trait << context.m_ivars.fmt(v.params) << " for " << context.m_ivars.fmt_type(v.impl_ty)
                        << " with " << v.name << " = " << context.m_ivars.fmt_type(v.left_ty));
            }
        }
        else if( count == 1 ) {
            auto& possible_impl_ty = possible_impls.at(0).impl_ty;
            auto& possible_params = possible_impls.at(0).params;
            auto& best_impl = possible_impls.at(0).impl_ref;
            DEBUG("Only one impl " << v.trait << context.m_ivars.fmt(possible_params) << " for " << context.m_ivars.fmt_type(possible_impl_ty)
                << " - out=" << output_type);
            // - If there are any magic params in the impl, don't use it yet.
            //  > Ideally, there should be a match_test_generics to resolve the magic impls.
            DEBUG("> best_impl=" << best_impl);
            if( best_impl.has_magic_params() ) {
                // Pick this impl, and evaluate it (expanding the magic params out)
                // - Equate `v.impl_ty` and `best_impl`'s type...
                //   - We expect an ivar from `v.impl_ty` to be matched against some sort of known type (struct, tuple, array, ...)
                //   - When that happens, allocate new ivars for the magic params in that type and assign.
                struct Matcher
                    : public HIR::MatchGenerics
                    , public Monomorphiser
                {
                    Context& m_context;
                    mutable ::std::map<HIR::GenericRef, HIR::TypeRef>    m_types;
                    mutable ::std::map<HIR::GenericRef, HIR::ConstGeneric>   m_values;

                    Matcher(Context& context): m_context(context) {}

                    ::HIR::Compare cmp_type(const Span& sp, const ::HIR::TypeRef& ty_l, const ::HIR::TypeRef& ty_r, HIR::t_cb_resolve_type resolve_cb) override {
                        const auto& l = (ty_l.data().is_Infer() ? resolve_cb.get_type(sp, ty_l) : ty_l);
                        const auto& r = (ty_r.data().is_Infer() ? resolve_cb.get_type(sp, ty_r) : ty_r);
                        if( ty_r.data().is_Generic() && ty_r.data().as_Generic().group() == HIR::GENERIC_Placeholder ) {
                            BUG(sp, "Assigning into a placeholder? should have been known");
                        }
                        if( l.data().is_Infer() && !r.data().is_Infer() ) {
                            // Monomorph the RHS, assigning new ivars to each impl param
                            auto new_ty = this->monomorph_type(sp, r, true);
                            m_context.equate_types(sp, l, new_ty);
                            return ::HIR::Compare::Equal;
                        }
                        return HIR::MatchGenerics::cmp_type(sp, ty_l, ty_r, resolve_cb);
                    }

                    ::HIR::Compare match_ty(const ::HIR::GenericRef& g, const ::HIR::TypeRef& ty, HIR::t_cb_resolve_type resolve_cb) override {
                        if( ty.data().is_Generic() && ty.data().as_Generic() == g ) {
                            return ::HIR::Compare::Equal;
                        }
                        TODO(Span(), "match_ty - " << g << " = " << ty);
                    }
                    ::HIR::Compare match_val(const ::HIR::GenericRef& g, const ::HIR::ConstGeneric& v) override {
                        if( v.is_Generic() && v.as_Generic() == g ) {
                            return ::HIR::Compare::Equal;
                        }
                        TODO(Span(), "match_val - " << g << " = " << v);
                    }

                    ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& g) const override {
                        if(g.group() == ::HIR::GENERIC_Placeholder ) {
                            auto it = m_types.find(g);
                            if( it == m_types.end() ) {
                                it = m_types.insert(std::make_pair( g, m_context.m_ivars.new_ivar_tr() )).first;
                                DEBUG("New type ivar for placeholder " << g << " = " << it->second);
                            }
                            return it->second.clone();
                        }
                        else {
                            return HIR::TypeRef(g);
                        }
                    }
                    ::HIR::ConstGeneric get_value(const Span& sp, const ::HIR::GenericRef& g) const override {
                        if(g.group() == ::HIR::GENERIC_Placeholder ) {
                            auto it = m_values.find(g);
                            if( it == m_values.end() ) {
                                auto v = ::HIR::ConstGeneric::make_Infer( ::HIR::ConstGeneric::Data_Infer { m_context.m_ivars.new_ivar_val() });
                                it = m_values.insert(std::make_pair( g, std::move(v) )).first;
                                DEBUG("New value ivar for placeholder " << g << " = " << it->second);
                            }
                            return it->second.clone();
                        }
                        else {
                            return g;
                        }
                    }
                    ::HIR::LifetimeRef get_lifetime(const Span& sp, const ::HIR::GenericRef& g) const override {
                        if(g.group() == ::HIR::GENERIC_Placeholder ) {
                            TODO(sp, "get_lifetime");
                        }
                        else {
                            return HIR::LifetimeRef(g.binding);
                        }
                    }
                } m { context };
                m.cmp_type(sp, v.impl_ty, possible_impl_ty, context.m_ivars.callback_resolve_infer());
                for(size_t i = 0; i < possible_params.m_types.size(); i ++) {
                    m.cmp_type(sp, v.params.m_types[i], possible_params.m_types[i], context.m_ivars.callback_resolve_infer());
                }
                DEBUG("> Magic params present, wait");
                return false;
            }
            const auto& impl_ty = context.m_ivars.get_type(v.impl_ty);
            if( TU_TEST1(impl_ty.data(), Path, .binding.is_Unbound()) )
            {
                DEBUG("Unbound UfcsKnown, waiting");
                return false;
            }
            if( TU_TEST1(impl_ty.data(), Infer, .is_lit() == false) )
            {
                DEBUG("Unbounded ivar, waiting - TODO: Add possibility " << impl_ty << " == " << possible_impl_ty);
                //context.possible_equate_type_bound(sp, impl_ty.data().as_Infer().index, possible_impl_ty);
                return false;
            }
            // Only one possible impl
            if( v.name != "" )
            {
                // If the output type is just < v.impl_ty as v.trait >::v.name, return false
                if( TU_TEST1(output_type.data(), Path, .path.m_data.is_UfcsKnown()) )
                {
                    auto& pe = output_type.data().as_Path().path.m_data.as_UfcsKnown();

                    if( pe.type == v.impl_ty && pe.trait.m_path == v.trait && pe.trait.m_params == v.params && pe.item == v.name )
                    {
                        if( TU_TEST1(v.left_ty.data(), Path, .path.m_data.is_UfcsKnown()) )
                        {
                            output_type.data_mut().as_Path().binding = HIR::TypePathBinding::make_Opaque({});
                        }
                        else
                        {
                            DEBUG("- Attempted recursion, stopping it");
                            return false;
                        }
                    }
                }
                context.equate_types(sp, v.left_ty, output_type);
            }
            assert( possible_impl_ty != ::HIR::TypeRef() );
            context.equate_types(sp, v.impl_ty, possible_impl_ty);
            for( unsigned int i = 0; i < possible_params.m_types.size(); i ++ ) {
                context.equate_types(sp, v.params.m_types[i], possible_params.m_types[i]);
            }
            for( unsigned int i = 0; i < possible_params.m_values.size(); i ++ ) {
                context.equate_values(sp, v.params.m_values[i], possible_params.m_values[i]);
            }
            // - Obtain the bounds required for this impl and add those as trait bounds to check/equate
            if( const auto* ep = best_impl.m_data.opt_TraitImpl() )
            {
                const auto& e = *ep;
                assert(e.impl);
                for(const auto& bound : e.impl->m_params.m_bounds )
                {
                    TU_MATCH_HDRA((bound), {)
                    default:
                        break;
                    TU_ARMA(TraitBound, be) {
                        DEBUG("New bound (pre-mono) " << bound);
                        auto ms = best_impl.get_cb_monomorph_traitimpl(sp, {});
                        static const HIR::GenericParams empty_params;
                        bool outer_present = be.hrtbs && !be.hrtbs->is_empty();
                        auto _ = ms.push_hrb(outer_present ? *be.hrtbs : empty_params);
                        auto b_ty_mono = ms.monomorph_type(sp, be.type);
                        auto b_tp_mono = ms.monomorph_traitpath(sp, be.trait, true);
                        DEBUG("- " << b_ty_mono << " : " << b_tp_mono);
                        ASSERT_BUG(sp, !outer_present || !static_cast<bool>(b_tp_mono.m_hrtbs), "Two layers of HRTBs not allowed (should have been disallowed in HIR lower)");
                        auto pp_hrl = outer_present ? be.hrtbs->make_empty_params(true)
                            : (b_tp_mono.m_hrtbs ? b_tp_mono.m_hrtbs->make_empty_params(true) : HIR::PathParams());
                        if(outer_present)
                            DEBUG("be.hrtbs = " << be.hrtbs->fmt_args());
                        if(b_tp_mono.m_hrtbs)
                            DEBUG("b_tp_mono.m_hrtbs = " << b_tp_mono.m_hrtbs->fmt_args());
                        DEBUG("pp_hrl = " << pp_hrl);
                        auto ms_hrl = MonomorphHrlsOnly(pp_hrl);
                        if( b_tp_mono.m_type_bounds.size() > 0 )
                        {
                            for(const auto& aty_bound : b_tp_mono.m_type_bounds)
                            {
                                context.equate_types_assoc(sp,
                                    aty_bound.second.type,
                                    b_tp_mono.m_path.m_path, ms_hrl.monomorph_path_params(sp, b_tp_mono.m_path.m_params, true),
                                    ms_hrl.monomorph_type(sp, b_ty_mono, true),
                                    aty_bound.first.c_str(),
                                    false
                                    );
                            }
                        }
                        else
                        {
                            context.add_trait_bound(sp, ms_hrl.monomorph_type(sp, b_ty_mono, true),  b_tp_mono.m_path.m_path, ms_hrl.monomorph_path_params(sp, b_tp_mono.m_path.m_params, true));
                        }
                        }
                    }
                }
            }
            return true;
        }
        else {
            // Multiple possible impls, don't know yet
            DEBUG("Multiple impls");
            // TODO: Make a solid list of possibilities in each of `v.params`
            std::map<unsigned, std::vector<HIR::TypeRef>>   ivar_possibilities;
            for(const auto& pi : possible_impls)
            {
                DEBUG("impl " << v.trait << pi.params << " for " << pi.impl_ty);
                for(size_t i = 0; i < pi.params.m_types.size(); i++)
                {
                    const auto& t = context.get_type(v.params.m_types[i]);
                    if( const auto* e = t.data().opt_Infer() ) {
                        const auto& pi_t = pi.params.m_types[i];
                        HIR::TypeRef    possible_ty;
                        if( !type_contains_impl_placeholder(pi_t) )
                        {
                            //ivar_possibilities[e->index].push_back( context.m_resolve.expand_associated_types(sp, pi_t.clone()) );
                            possible_ty = pi_t.clone();
                        }
                        else
                        {
                            DEBUG("Not adding placeholder-containing type as a bound - " << pi_t);
                            // Push this ivar
                            possible_ty = t.clone();
                        }

                        if( std::find(ivar_possibilities[e->index].begin(), ivar_possibilities[e->index].end(), possible_ty) == ivar_possibilities[e->index].end() ) {
                            ivar_possibilities[e->index].push_back( std::move(possible_ty) );
                        }
                    }
                }
            }
            for(auto& e : ivar_possibilities)
            {
                DEBUG("IVar _/*" << e.first << "*/ ?= [" << e.second << "]");
                context.possible_equate_ivar_bounds(sp, e.first, std::move(e.second));
            }
            return false;
        }
        } catch(const TraitResolution::RecursionDetected& ) {
            DEBUG("Recursion detected, deferring");
            return false;
        }
    }

}   // namespace "" - check_associated and check_coerce

// check_ivar_poss (and helpers)
namespace
{
    bool check_ivar_poss__fails_bounds(const Span& sp, Context& context, const ::HIR::TypeRef& ty_l, const ::HIR::TypeRef& new_ty)
    {
        TRACE_FUNCTION_F(ty_l << " <- " << new_ty);
        bool used_ty;
        struct Cb {
            bool& used_ty;
            const Span& sp;
            const Context& context;
            const HIR::TypeRef& ty_l;
            const HIR::TypeRef& new_ty;
            Cb( bool& used_ty, const Span& sp, const Context& context, const HIR::TypeRef& ty_l, const HIR::TypeRef& new_ty)
                : used_ty(used_ty)
                , sp(sp)
                , context(context)
                , ty_l(ty_l)
                , new_ty(new_ty)
            {
            }
            bool operator()(const ::HIR::TypeRef& ty, ::HIR::TypeRef& out_ty) {
                if( ty == ty_l ) {
                    out_ty = new_ty.clone();
                    used_ty = true;
                    return true;
                }
                else {
                    const auto& rty = context.get_type(ty);
                    if(rty != ty) {
                        out_ty = clone_ty_with(sp, rty, *this);
                        return true;
                    }
                    else {
                        return false;
                    }
                }
            }
        };
        Cb  cb { used_ty, sp, context, ty_l, new_ty };
        for(const auto& bound : context.link_coerce)
        {
            used_ty = false;
            auto t_l = clone_ty_with(sp, bound->left_ty, cb);
            auto t_r = clone_ty_with(sp, (*bound->right_node_ptr)->m_res_type, cb);
            if(!used_ty) {
                //DEBUG("[" << ty_l << "] Skip Corerce R" << bound->rule_idx << " - " << bound->left_ty << " := " << (*bound->right_node_ptr)->m_res_type);
                continue ;
            }
            t_l = context.m_resolve.expand_associated_types( sp, mv$(t_l) );
            t_r = context.m_resolve.expand_associated_types( sp, mv$(t_r) );
            DEBUG("Check Coerce R" << bound->rule_idx << " - " << bound->left_ty << " := " << (*bound->right_node_ptr)->m_res_type);
            DEBUG("Testing " << t_l << " := " << t_r);

            switch(check_coerce_tys(context, sp, t_l, t_r, nullptr, bound->right_node_ptr))
            {
            case CoerceResult::Fail:
                DEBUG("Fail - Invalid");
                return true;
            case CoerceResult::Unsize:
                DEBUG("Unsize - Valid");
                break;
            case CoerceResult::Unknown:
                DEBUG("Unknown?");
                break;
            case CoerceResult::Custom:
                DEBUG("Custom");
                break;
            case CoerceResult::Equality:
                // NOTE: looking for strict inequality (fuzzy is allowed)
                DEBUG("Equality requested, checking " << t_l << " == " << t_r);
                if( t_l.compare_with_placeholders(sp, t_r, context.m_ivars.callback_resolve_infer()) == HIR::Compare::Unequal )
                {
                    DEBUG("- Bound failed");
                    return true;
                }
                break;
            }
        }

        auto ivar_idx = ty_l.data().as_Infer().index;
        if( ivar_idx < context.m_ivars_sized.size() && context.m_ivars_sized[ivar_idx] )
        {
            if( context.m_resolve.type_is_sized(sp, new_ty) == ::HIR::Compare::Unequal )
            {
                DEBUG("Unsized type not valid here");
                return true;
            }
        }

        for(const auto& pty : context.possible_ivar_vals.at(ty_l.data().as_Infer().index).types_coerce_to)
        {
            HIR::ExprNodeP  stub_node;  // Empty node to 
            CoerceResult    res = CoerceResult::Unknown;
            switch(pty.op)
            {
            case Context::IVarPossible::CoerceTy::Coercion:
                res = check_coerce_tys(context, sp, pty.ty, new_ty, nullptr, &stub_node);
                break;
            case Context::IVarPossible::CoerceTy::Unsizing:
                res = check_unsize_tys(context, sp, pty.ty, new_ty, nullptr, &stub_node);
                break;
            }
            switch(res)
            {
            case CoerceResult::Unsize:
            case CoerceResult::Unknown:
            case CoerceResult::Custom:
                break;
            case CoerceResult::Fail:
                return true;
            case CoerceResult::Equality:
                // NOTE: looking for strict inequality (fuzzy is allowed)
                DEBUG("Check " << pty.ty << " == " << new_ty);
                if( pty.ty.compare_with_placeholders(sp, new_ty, context.m_ivars.callback_resolve_infer()) == HIR::Compare::Unequal )
                {
                    DEBUG("- Bound failed");
                    return true;
                }
                break;
            }
        }

        for(const auto& bound : context.link_assoc)
        {
            used_ty = false;
            auto t = clone_ty_with(sp, bound.impl_ty, cb);
            auto p = clone_path_params_with(sp, bound.params, cb);
            if(!used_ty) {
                //DEBUG("[" << ty_l << "] Skip Assoc R" << bound.rule_idx << " - " << bound.impl_ty << " : " << bound.trait << bound.params);
                continue;
            }
            // - Run EAT on t and p
            t = context.m_resolve.expand_associated_types( sp, mv$(t) );
            // TODO: Run EAT on `p`?
            DEBUG("Check Assoc R" << bound.rule_idx << " - " << bound.impl_ty << " : " << bound.trait << bound.params);
            DEBUG("-> " << t << " : " << bound.trait << p);

            // Search for any trait impl that could match this,
            bool bound_failed = true;
            context.m_resolve.find_trait_impls(sp, bound.trait, p, t, [&](const auto impl, auto cmp){
                // If this bound specifies an associated type, then check that that type could match
                if( bound.name != "" )
                {
                    auto aty = impl.get_type(bound.name.c_str(), {});
                    // The associated type is not present, what does that mean?
                    if( aty == ::HIR::TypeRef() ) {
                        DEBUG("[check_ivar_poss__fails_bounds] No ATY for " << bound.name << " in impl");
                        // A possible match was found, so don't delete just yet
                        bound_failed = false;
                        // - Return false to keep searching
                        return false;
                    }
                    else if( aty.compare_with_placeholders(sp, bound.left_ty, context.m_ivars.callback_resolve_infer()) == HIR::Compare::Unequal ) {
                        DEBUG("[check_ivar_poss__fails_bounds] ATY " << context.m_ivars.fmt_type(aty) << " != left " << context.m_ivars.fmt_type(bound.left_ty));
                        bound_failed = true;
                        // - Bail instantly
                        return true;
                    }
                    else {
                    }
                }
                bound_failed = false;
                return true;
                });
            if( bound_failed && ! t.data().is_Infer() ) {
                // If none was found, remove from the possibility list
                DEBUG("Remove possibility " << new_ty << " because it failed a bound");
                return true;
            }

            // TODO: Check for the resultant associated type
            DEBUG("Acceptable (Assoc R" << bound.rule_idx << ")");
        }

        // Handle methods
        for(const auto* node_ptr_dyn : context.to_visit)
        {
            if( const auto* node_ptr = dynamic_cast<const ::HIR::ExprNode_CallMethod*>(node_ptr_dyn) )
            {
                const auto& node = *node_ptr;
                const auto& ty_tpl = context.get_type(node.m_value->m_res_type);

                bool used_ty = false;
                auto t = clone_ty_with(sp, ty_tpl, [&](const auto& ty, auto& out_ty){ if( ty == ty_l ) { out_ty = new_ty.clone(); used_ty = true; return true; } else { return false; }});
                if(!used_ty)
                    continue;

                DEBUG("Check <" << t << ">::" << node.m_method);
                ::std::vector<::std::pair<TraitResolution::AutoderefBorrow, ::HIR::Path>> possible_methods;
                unsigned int deref_count = context.m_resolve.autoderef_find_method(node.span(), node.m_traits, node.m_trait_param_ivars, t, node.m_method,  possible_methods);
                DEBUG("> deref_count = " << deref_count << ", possible_methods={" << possible_methods << "}");
                // TODO: Detect the above hitting an ivar, and use that instead of this hacky check of if it's `_` or `&_`
                if( !(t.data().is_Infer() || TU_TEST1(t.data(), Borrow, .inner.data().is_Infer())) && possible_methods.empty() )
                {
                    // No method found, which would be an error
                    DEBUG("Remove possibility " << new_ty << " because it didn't have a method");
                    return true;
                }
            }
            else
            {
                // TODO: Other methods?
            }
        }

        DEBUG("- Bound passed");
        return false;
    }

    enum class IvarPossFallbackType {
        // No fallback, only make safe/definitive decisions
        None,
        // Can propagate backwards
        Backwards,
        // Picks an option, even if there's ivars present?
        Assume,
        // Ignores the weaker disable flags (`force_no_to` and `force_no_from`)
        IgnoreWeakDisable,
        // First bound, if nothing else works
        PickFirstBound,
        // Just picks an option (even if it might be wrong)
        FinalOption,
    };
    ::std::ostream& operator<<(::std::ostream& os, IvarPossFallbackType t) {
        switch(t)
        {
        case IvarPossFallbackType::None:    os << "";   break;
        case IvarPossFallbackType::Backwards:   os << " backwards";   break;
        case IvarPossFallbackType::Assume:  os << " weak";   break;
        case IvarPossFallbackType::IgnoreWeakDisable:  os << " unblock";   break;
        case IvarPossFallbackType::PickFirstBound:  os << " pick-bound";   break;
        case IvarPossFallbackType::FinalOption:  os << " final";   break;
        }
        return os;
    }

    struct PossibleType {
        enum {
            Equal,
            CoerceTo,
            CoerceFrom,
            UnsizeTo,
            UnsizeFrom,
        }   cls;
        const ::HIR::TypeRef* ty;

        Ordering ord(const PossibleType& o) const {
            if( static_cast<bool>(ty) != static_cast<bool>(o.ty) )
                return ::ord(static_cast<bool>(ty), static_cast<bool>(o.ty));
            if( *ty != *o.ty )
                return ::ord(*ty, *o.ty);
            if( cls != o.cls )
                return ::ord( static_cast<int>(cls), static_cast<int>(o.cls) );
            return OrdEqual;
        }
        bool operator<(const PossibleType& o) const {
            return ord(o) == OrdLess;
        }
        bool operator==(const PossibleType& o) const {
            return ord(o) == OrdEqual;
        }
        ::std::ostream& fmt(::std::ostream& os) const {
            switch(cls)
            {
            case Equal:      os << "=="; break;
            case CoerceTo:   os << "C-"; break;
            case CoerceFrom: os << "CD"; break;
            case UnsizeTo:   os << "--"; break;
            case UnsizeFrom: os << "-D"; break;
            }
            os << " ";
            if(ty) {
                os << *ty;
            }
            else {
                os << "?";
            }
            return os;
        }

        bool is_source() const {
            return cls == CoerceFrom || cls == UnsizeFrom;
        }
        bool is_dest() const {
            return cls == CoerceTo || cls == UnsizeTo;
        }
        static bool is_source_s(const PossibleType& self) { return self.is_source(); }
        static bool is_dest_s(const PossibleType& self) { return self.is_dest(); }

        bool is_coerce() const {
            return cls == CoerceTo || cls == CoerceFrom;
        }
        bool is_unsize() const {
            return cls == UnsizeTo || cls == UnsizeFrom;
        }
        static bool is_coerce_s(const PossibleType& self) { return self.is_coerce(); }
        static bool is_unsize_s(const PossibleType& self) { return self.is_unsize(); }
    };

    struct TypeRestrictiveOrdering
    {
        /// Get the inner type of a pointer (if it matches a template)
        static const ::HIR::TypeRef* match_and_extract_ptr_ty(const ::HIR::TypeRef& ptr_tpl, const ::HIR::TypeRef& ty)
        {
            if( ty.data().tag() != ptr_tpl.data().tag() )
                return nullptr;
            TU_MATCH_HDRA( (ty.data()), { )
            TU_ARMA(Borrow, te) {
                if( te.type == ptr_tpl.data().as_Borrow().type ) {
                    return &te.inner;
                }
                }
            TU_ARMA(Pointer, te) {
                if( te.type == ptr_tpl.data().as_Pointer().type ) {
                    return &te.inner;
                }
                }
            TU_ARMA(Path, te) {
                if( te.binding == ptr_tpl.data().as_Path().binding ) {
                    // TODO: Get inner
                }
                } break;
            default:
                break;
            }
            return nullptr;
        }

        /// Helper for `get_ordering_ty` - ordering of the type vs an infer type
        static Ordering get_ordering_infer(const Span& sp, const ::HIR::TypeRef& r)
        {
            // For infer, only concrete types are more restrictive
            TU_MATCH_HDRA( (r.data()), { )
            default:
                return OrdLess;
            TU_ARMA(Path, te) {
                if( te.binding.is_Opaque() )
                    return OrdLess;
                if( te.binding.is_Unbound() )
                    return OrdEqual;
                // TODO: Check if the type is concrete? (Check an unsizing param if present)
                return OrdLess;
                }
            TU_ARMA(Borrow, _)
                return OrdEqual;
            TU_ARMA(Infer, _)
                return OrdEqual;
            TU_ARMA(Pointer, _)
                return OrdEqual;
            }
            throw "";
        }
        /// Ordering of `l` relative to `r` for ?unsizing
        /// - OrdLess means that the LHS is less restrictive
        static Ordering get_ordering_ty(const Span& sp, const Context& context, const ::HIR::TypeRef& l, const ::HIR::TypeRef& r, bool& out_unordered)
        {
            if( l == r ) {
                return OrdEqual;
            }
            if( l.data().is_Infer() ) {
                return get_ordering_infer(sp, r);
            }
            if( r.data().is_Infer() ) {
                switch( get_ordering_infer(sp, l) )
                {
                case OrdLess:   return OrdGreater;
                case OrdEqual:  return OrdEqual;
                case OrdGreater:return OrdLess;
                }
            }
            if( l.data().is_Path() ) {
                const auto& te_l = l.data().as_Path();
                // Path types can be unsize targets, and can also act like infers
                // - If it's a Unbound treat as Infer
                // - If Opaque, then search for a CoerceUnsized/Unsize bound?
                // - If Struct, look for ^ tag
                // - Else, more/equal specific
                TU_MATCH_HDRA( (r.data()), { )
                default:
                    // An ivar is less restrictive?
                    if( te_l.binding.is_Unbound() )
                        return OrdLess;
                    out_unordered = true;
                    return OrdEqual;
                    //TODO(sp, l << " with " << r << " - LHS is Path, RHS is " << r.data().tag_str());
                TU_ARMA(Slice, te_r) {
                    // Paths can deref to a slice (well, to any type) - so `slice < path` in restrictiveness
                    return OrdGreater;
                    }
                TU_ARMA(Path, te_r) {
                    // If both are unbound, assume equal (effectively an ivar)
                    if( te_l.binding.is_Unbound() && te_r.binding.is_Unbound() )
                    {
                        return OrdEqual;
                    }
                    if( te_l.binding.is_Unbound() )
                        return OrdLess;
                    if( te_r.binding.is_Unbound() )
                    {
                        return OrdGreater;
                    }
                    else if( te_r.binding.is_Opaque() )
                    {
                        TODO(sp, l << " with " << r << " - LHS is Path, RHS is opaque type");
                    }
                    else if( TU_TEST1(te_r.binding, Struct, ->m_struct_markings.can_unsize) )
                    {
                        TODO(sp, l << " with " << r << " - LHS is Path, RHS is unsize-capable struct");
                    }
                    else
                    {
                        return OrdEqual;
                    }
                    }
                }
            }
            if( r.data().is_Path() ) {
                // Path types can be unsize targets, and can also act like infers
                switch( get_ordering_ty(sp, context, r, l, out_unordered) )
                {
                case OrdLess:   return OrdGreater;
                case OrdEqual:  return OrdEqual;
                case OrdGreater:return OrdLess;
                }
            }

            // Slice < Array
            if( l.data().tag() == r.data().tag() ) {
                return OrdEqual;
            }
            else {
                if( l.data().is_Slice() && r.data().is_Array() ) {
                    return OrdGreater;
                }
                if( l.data().is_Array() && r.data().is_Slice() ) {
                    return OrdLess;
                }

                if( l.data().is_Borrow() && !r.data().is_Borrow() ) {
                    return OrdGreater;
                }
                if( r.data().is_Borrow() && !l.data().is_Borrow() ) {
                    return OrdLess;
                }

                out_unordered = true;
                return OrdEqual;
                //TODO(sp, "Compare " << l << " and " << r);
            }
        }

        /// Returns the restrictiveness ordering of `l` relative to `r`
        /// - &T is more restrictive than *const T
        /// - &mut T is more restrictive than &T
        /// Restrictive means that left can't be coerced from right
        static Ordering get_ordering_ptr(
            const Span& sp, const Context& context,
            const ::HIR::TypeRef& l, const ::HIR::TypeRef& r,
            bool& out_unordered,
            bool deep=true
            )
        {
            Ordering cmp;
            TRACE_FUNCTION_FR(l << " , " << r, cmp);
            // Get ordering of this type to the current destination
            // - If lesser/greater then ignore/update
            // - If equal then what? (Instant error? Leave as-is and let the asignment happen? Disable the asignment?)
            static const ::HIR::TypeData::Tag tag_ordering[] = {
                ::HIR::TypeData::TAG_Pointer,
                ::HIR::TypeData::TAG_Borrow,
                ::HIR::TypeData::TAG_Path, // Strictly speaking, Path == Generic
                ::HIR::TypeData::TAG_Generic,
                ::HIR::TypeData::TAG_Function,
                // These two are kinda their own pair
                ::HIR::TypeData::TAG_NamedFunction,
                ::HIR::TypeData::TAG_Closure,
                };
            static const ::HIR::TypeData::Tag* tag_ordering_end = &tag_ordering[ sizeof(tag_ordering) / sizeof(tag_ordering[0] )];
            if( l.data().tag() != r.data().tag() )
            {
                auto p_l = ::std::find(tag_ordering, tag_ordering_end, l.data().tag());
                auto p_r = ::std::find(tag_ordering, tag_ordering_end, r.data().tag());
                if( p_l == tag_ordering_end ) {
                    TODO(sp, "Type " << l << " not in ordering list");
                }
                if( p_r == tag_ordering_end ) {
                    TODO(sp, "Type " << r << " not in ordering list");
                }
                cmp = ord( static_cast<int>(p_l-p_r), 0 );
            }
            else
            {
                if( l == r )
                    return OrdEqual;
                TU_MATCH_HDRA( (l.data()), { )
                default:
                    BUG(sp, "Unexpected type class " << l << " in get_ordering_ty (" << r << ")");
                    break;
                TU_ARMA(Generic, _te_l) {
                    cmp = OrdEqual;
                    }
                TU_ARMA(NamedFunction, te_l) {
                    cmp = OrdEqual;
                    }
                TU_ARMA(Path, te_l) {
                    //const auto& te = dest_type->m_data.as_Path();
                    // TODO: Prevent this rule from applying?
                    return OrdEqual;
                    }
                TU_ARMA(Closure, te_l) {
                    out_unordered = true;
                    return OrdEqual;
                    }
                TU_ARMA(Borrow, te_l) {
                    const auto& te_r = r.data().as_Borrow();
                    cmp = ord( (int)te_l.type, (int)te_r.type );   // Unique>Shared in the listing, and Unique is more restrictive than Shared
                    if( cmp == OrdEqual && deep )
                    {
                        cmp = get_ordering_ty(sp, context, context.m_ivars.get_type(te_l.inner), context.m_ivars.get_type(te_r.inner), out_unordered);
                    }
                    }
                TU_ARMA(Pointer, te_l) {
                    const auto& te_r = r.data().as_Pointer();
                    cmp = ord( (int)te_r.type, (int)te_l.type );   // Note, reversed ordering because we want Unique>Shared
                    if( cmp == OrdEqual && deep )
                    {
                        cmp = get_ordering_ty(sp, context, context.m_ivars.get_type(te_l.inner), context.m_ivars.get_type(te_r.inner), out_unordered);
                    }
                    }
                }
            }
            return cmp;
        }
    };

    /// Ordering of types based on the amount of type information they provide
    /// - E.g. `(_, i32)` will sort higher than `(_,_)`
    /// If types don't match (e.g. `i32` with `(_,_)`) then `Incompatible` is returned
    struct InfoOrdering
    {
        enum eInfoOrdering {
            Incompatible,   // The types are incompatible
            Less,   // The LHS type provides less information (e.g. has more ivars)
            Same,   // Same number of ivars
            More,   // The RHS provides more information (less ivars)
        };
        static bool is_infer(const ::HIR::TypeRef& ty) {
            if( ty.data().is_Infer() )
                return true;
            if( TU_TEST1(ty.data(), Path, .binding.is_Unbound()) )
                return true;
            return false;
        }
        static bool compare_score(int& score, const ::HIR::TypeRef& ty_l, const ::HIR::TypeRef& ty_r) {
            auto rv = compare(ty_l, ty_r);
            switch(rv)
            {
            case Incompatible:
                return Incompatible;
            case Less:  score --;   break;
            case Same:              break;
            case More:  score ++;   break;
            }
            return rv;
        }
        static eInfoOrdering compare(const ::HIR::TypeRef& ty_l, const ::HIR::TypeRef& ty_r) {
            if( is_infer(ty_l) ) {
                if( is_infer(ty_r) )
                    return Same;
                return Less;
            }
            else {
                if( is_infer(ty_r) )
                    return More;
            }
            if( ty_l.data().tag() != ty_r.data().tag() ) {
                return Incompatible;
            }
            TU_MATCH_HDRA( (ty_l.data(), ty_r.data()), {)
            default:
                return Incompatible;
            TU_ARMA(Closure, le, re) {
                if( le.node != re.node )
                    return Incompatible;
                return Same;
                }
            TU_ARMA(Tuple, le, re) {
                if( le.size() != re.size() )
                    return Incompatible;
                int score = 0;
                for(size_t i = 0; i < le.size(); i ++)
                {
                    if( compare_score(score, le[i], re[i]) == Incompatible )
                        return Incompatible;
                }
                return (score == 0 ? Same : (score < 0 ? Less : More));
                }
            }
            throw "unreachable";
        }
        static eInfoOrdering compare_top(const Context& context, const ::HIR::TypeRef& ty_l, const ::HIR::TypeRef& ty_r, bool should_deref) {
            if( context.m_ivars.types_equal(ty_l, ty_r) )
                return Same;
            if( is_infer(ty_l) )
                return Incompatible;
            if( is_infer(ty_r) )
                return Incompatible;
            if( ty_l.data().tag() != ty_r.data().tag() ) {
                return Incompatible;
            }
            if( should_deref ) {
                if( const auto* le = ty_l.data().opt_Borrow() ) {
                    const auto& re = ty_r.data().as_Borrow();
                    if( le->type != re.type )
                        return Incompatible;
                    return compare_top(context, context.m_ivars.get_type(le->inner), context.m_ivars.get_type(re.inner), false);
                }
                else if( const auto* le = ty_l.data().opt_Pointer() ) {
                    const auto& re = ty_r.data().as_Pointer();
                    if( le->type != re.type )
                        return Incompatible;
                    return compare_top(context, context.m_ivars.get_type(le->inner), context.m_ivars.get_type(re.inner), false);
                }
                else if( TU_TEST2(ty_l.data(), Path, .binding, Struct, ->m_struct_markings.coerce_unsized != ::HIR::StructMarkings::Coerce::None) )
                {
                    const auto& le = ty_l.data().as_Path();
                    const auto& re = ty_r.data().as_Path();
                    if( le.binding != re.binding )
                        return Incompatible;
                    auto param_idx = le.binding.as_Struct()->m_struct_markings.coerce_param;
                    assert(param_idx != ~0u);
                    return compare_top(context,
                        context.m_ivars.get_type(le.path.m_data.as_Generic().m_params.m_types.at(param_idx)),
                        context.m_ivars.get_type(re.path.m_data.as_Generic().m_params.m_types.at(param_idx)),
                        false
                    );
                }
                else if( const auto* le = ty_l.data().opt_Closure())
                {
                    const auto& re = ty_r.data().as_Closure();
                    if( le->node != re.node )
                        return Incompatible;
                    return Same;
                }
                else
                {
                    BUG(Span(), "Can't deref " << ty_l << " / " << ty_r);
                }
            }
            if( ty_l.data().is_Slice() )
            {
                const auto& le = ty_l.data().as_Slice();
                const auto& re = ty_r.data().as_Slice();

                switch(compare(context.m_ivars.get_type(le.inner), context.m_ivars.get_type(re.inner)))
                {
                case Less:  return Less;
                case More:  return More;
                case Same:
                case Incompatible:
                    return Same;
                }
                throw "";
            }
            return Incompatible;
        }
    };


    // TODO: Split the below into a common portion, and a "run" portion (which uses the fallback)

    /// Check IVar possibilities, from both coercion/unsizing (which have well-encoded rules) and from trait impls
    bool check_ivar_poss(Context& context, unsigned int i, Context::IVarPossible& ivar_ent, IvarPossFallbackType fallback_ty=IvarPossFallbackType::None)
    {
        static Span _span;
        const auto& sp = _span;
        const bool honour_disable = (fallback_ty != IvarPossFallbackType::IgnoreWeakDisable);

        const auto& ty_l = context.m_ivars.get_type(i);

        if( !TU_TEST1(ty_l.data(), Infer, .index == i) )
        {
            if( ivar_ent.has_rules() )
            {
                DEBUG("- IVar " << i << " had possibilities, but was known to be " << ty_l);
                // Completely clear by reinitialising
                ivar_ent = Context::IVarPossible();
            }
            else
            {
                //DEBUG(i << ": known " << ty_l);
            }
            return false;
        }

        if( ! ivar_ent.has_rules() )
        {
            // No rules, don't do anything (and don't print)
            DEBUG(i << ": No rules");
            return false;
        }

        TRACE_FUNCTION_F(i << fallback_ty);


        /// (semi) Formal rules
        /// 
        /// - Always rules:
        ///   - If the same type is in both the from/to lists, use that
        /// - Skip if:
        ///   - `bounds_include_self`: Means that the bound set is incomplete (this can be disabled)
        ///   - `bounds_populated && bounds.empty()`: Bound set is incomplete
        ///
        /// - Look for a "bottom" type in the sources
        ///   - E.g. If a trait object or slice is seen as a souce, pick that (they can't coerce to anything)
        ///   - Note: Can't look for a "top" type in the destinations, as deref coercions exist
        ///
        /// - If bounds are present:
        ///   - Look for a unique entry in the bounds also in the source/destination lists
        /// - If there are no destination disables
        ///   - Look for a destination that all other destinations can coerce from
        /// - If there are no source disables
        ///   - Look for a source that all other soures can coerce to
        ///
        /// TODO: If in fallback mode, there's no infer options, and there are bounds - Pick a random bound

        // ---
        // Always rules:
        // ---
        {
            // - Search for a type that is both a source and a destination
            for(const auto& t : ivar_ent.types_coerce_to)
            {
                for(const auto& t2 : ivar_ent.types_coerce_from)
                {
                    // TODO: Compare such that &[_; 1] == &[u8; 1]? and `&[_]` == `&[T]`
                    if(t.ty == t2.ty && t.ty != ty_l)
                    {
                        DEBUG("- Source/Destination type");
                        context.equate_types(sp, ty_l, t.ty);
                        return true;
                    }
                }
            }
        }
        // ---
        // Skip Conditions
        // ---
        if( ivar_ent.has_bounded && (!ivar_ent.bounds_include_self && ivar_ent.bounded.empty()) )
        {
            DEBUG(i << ": Bounded, but bound set empty");
            return false;
        }
        if( ivar_ent.force_disable && fallback_ty != IvarPossFallbackType::FinalOption )
        {
            DEBUG(i << ": forced unknown");
            return false;
        }

        // Don't attempt to guess literals
        // - TODO: What about if there's a bound?
        if( ty_l.data().as_Infer().is_lit() )
        {
            DEBUG(i << ": Literal " << ty_l);
            return false;
        }

        //if( ivar_ent.force_no_to || ivar_ent.force_no_from )
        //{
        //    switch(fallback_ty)
        //    {
        //    case IvarPossFallbackType::IgnoreWeakDisable:
        //    case IvarPossFallbackType::FinalOption:
        //        break;
        //    default:
        //        DEBUG(i << ": coercion blocked");
        //        return false;
        //    }
        //}


        bool has_no_coerce_posiblities;

        // Fill a single list with all possibilities, and pick the most suitable type.
        // - This list needs to include flags to say if the type can be dereferenced.
        {

            bool allow_unsized = !(i < context.m_ivars_sized.size() ? context.m_ivars_sized.at(i) : false);

            ::std::vector<PossibleType> possible_tys;
            static ::HIR::TypeRef   null_placeholder;
            bool add_placeholders = (fallback_ty < IvarPossFallbackType::IgnoreWeakDisable);
            if( add_placeholders && ivar_ent.force_no_from )
            {
                possible_tys.push_back(PossibleType { PossibleType::UnsizeFrom, &null_placeholder });
            }
            for(const auto& new_ty : ivar_ent.types_coerce_from )
            {
                possible_tys.push_back(PossibleType { new_ty.op == Context::IVarPossible::CoerceTy::Coercion ? PossibleType::CoerceFrom : PossibleType::UnsizeFrom, &new_ty.ty });
            }
            if( add_placeholders && ivar_ent.force_no_to )
            {
                possible_tys.push_back(PossibleType { PossibleType::UnsizeTo, &null_placeholder });
            }
            for(const auto& new_ty : ivar_ent.types_coerce_to )
            {
                possible_tys.push_back(PossibleType { new_ty.op == Context::IVarPossible::CoerceTy::Coercion ? PossibleType::CoerceTo : PossibleType::UnsizeTo, &new_ty.ty });
            }
            DEBUG(i << ": possible_tys = " << possible_tys);
            DEBUG(i << ": bounds = " << (ivar_ent.has_bounded ? "" : "? ") << (ivar_ent.bounds_include_self ? "+, " : "") << ivar_ent.bounded);

            // De-duplicate
            // TODO: This [ideally] shouldn't happen?
            {
                for(size_t i = 0; i < possible_tys.size(); i ++)
                {
                    if( possible_tys[i].ty )
                    {
                        auto it = std::find(possible_tys.begin()+i+1, possible_tys.end(), possible_tys[i]);
                        if( it != possible_tys.end() )
                        {
                            it->ty = nullptr;
                        }
                    }
                }
                auto new_end = std::remove_if(possible_tys.begin(), possible_tys.end(), [](const PossibleType& x){ return x.ty == nullptr; });
                DEBUG(i << ": " << (possible_tys.end() - new_end) << " duplicates");
                possible_tys.resize( new_end - possible_tys.begin() ); 
            }

            // If the bound set is populated, and is fully restrictive
            if(ivar_ent.has_bounded && !ivar_ent.bounds_include_self)
            {
                // Look for a bound that matches all other restrictions
                const HIR::TypeRef* best_ty = nullptr;
                bool found_two = false;
                for(const auto& b_ty : ivar_ent.bounded)
                {
                    // Check bound against bounds
                    if( !check_ivar_poss__fails_bounds(sp, context, ty_l, b_ty) )
                    {
                        if(best_ty) {
                            DEBUG(b_ty << " passed bounds (second)");
                            found_two = true;
                            break;
                        }
                        else {
                            DEBUG(b_ty << " passed bounds (first)");
                            best_ty = &b_ty;
                        }
                    }
                    else
                    {
                        DEBUG(b_ty << " failed bounds");
                    }
                }
                if( !best_ty )
                {
                    TODO(sp, "No none of the bounded types (" << ivar_ent.bounded << ") fit other bounds");
                }
                else if(!found_two)
                {
                    DEBUG("Only one bound fit other bounds");
                    context.equate_types(sp, ty_l, *best_ty);
                    return true;
                }
                else 
                {
                    // If there's no other rules, just pick the first fitting type
                    // TODO: Should this be restricted to a fallback mode?
                    if( fallback_ty == IvarPossFallbackType::PickFirstBound && possible_tys.empty())
                    {
                        DEBUG("Multiple fitting types in bounded and no other rules, picking first (bounded=[" << ivar_ent.bounded << "])");
                        context.equate_types(sp, ty_l, *best_ty);
                        return true;
                    }
                    // Multiple fitting types, keep going
                }
            }

            // Check if any of the bounded types match only one of the possible types
            {
                struct H {
                    static const ::HIR::TypeRef& get_borrow_inner(const ::HIR::TypeRef& ty) {
                        if( ty.data().is_Borrow() ) {
                            return get_borrow_inner(ty.data().as_Borrow().inner);
                        }
                        else {
                            return ty;
                        }
                    }
                };
                bool failed = false;
                const HIR::TypeRef* found_ty = nullptr;
                for(const auto& bounded_ty : ivar_ent.bounded)
                {
                    // Skip ivars
                    if( H::get_borrow_inner(bounded_ty).data().is_Infer() ) {
                        continue;
                    }
                    for(const auto& t : possible_tys)
                    {
                        // Skip ivars
                        if( H::get_borrow_inner(*t.ty).data().is_Infer() ) {
                            continue;
                        }

                        if( bounded_ty.compare_with_placeholders(sp, *t.ty, context.m_ivars.callback_resolve_infer()) != HIR::Compare::Unequal ) {
                            if( !found_ty ) {
                                found_ty = &bounded_ty;
                            }
                            else if( found_ty == &bounded_ty ) {
                                // Same type still, continue
                            }
                            else if( bounded_ty.compare_with_placeholders(sp, *found_ty, context.m_ivars.callback_resolve_infer()) == HIR::Compare::Unequal ) {
                                // Incompatible types
                                failed = true;
                            }
                            else {
                                // Compatible, keep the first one?
                                // - Nope, could be ivars involved.
                                failed = true;
                            }
                        }
                    }
                }
                if( found_ty && !failed ) {
                    DEBUG("- Bounded and possible type - " << *found_ty);
                    context.equate_types(sp, ty_l, *found_ty);
                    return true;
                }
            }

            // Either there are no bounds available, OR the bounds are not fully restrictive
            // - Add the bounded types to `possible_tys`
            for( const auto& new_ty : ivar_ent.bounded )
            {
                possible_tys.push_back(PossibleType { PossibleType::Equal, &new_ty });
            }

            // TODO: Rewrite ALL of the below (extract the helpers to somewhere useful)
            // Need FULLY codified rules

            // If in fallback mode, pick the only source (if it's valid)
            if( fallback_ty != IvarPossFallbackType::None
                && ::std::count_if(possible_tys.begin(), possible_tys.end(), PossibleType::is_source_s) == 1
                && !ivar_ent.force_no_from
                && !ivar_ent.has_bounded
                )
            {
                // Single source, pick it?
                const auto& ent = *::std::find_if(possible_tys.begin(), possible_tys.end(), PossibleType::is_source_s);
                // - Only if there's no ivars
                if( !context.m_ivars.type_contains_ivars(*ent.ty) && !ent.ty->data().is_Diverge() )
                {
                    if( !check_ivar_poss__fails_bounds(sp, context, ty_l, *ent.ty) )
                    {
                        DEBUG("Single concrete source, " << *ent.ty);
                        context.equate_types(sp, ty_l, *ent.ty);
                        return true;
                    }
                }
            }
            if( fallback_ty == IvarPossFallbackType::IgnoreWeakDisable
                && possible_tys.size() == 1
                )
            {
                auto ent = possible_tys[0];
                if( !check_ivar_poss__fails_bounds(sp, context, ty_l, *ent.ty) )
                {
                    DEBUG("Single option (and in final), " << *ent.ty);
                    context.equate_types(sp, ty_l, *ent.ty);
                    return true;
                }
            }

            // If there's only one source, and one destination, and no possibility of unknown options, then pick whichever has no ivars (or whichever is valid)
            if( ::std::count_if(possible_tys.begin(), possible_tys.end(), PossibleType::is_dest_s) == 1
                && ::std::count_if(possible_tys.begin(), possible_tys.end(), PossibleType::is_source_s) == 1
                && !ivar_ent.force_no_from
                && !ivar_ent.force_no_to
                && !ivar_ent.has_bounded
                )
            {
                const auto& ent_s = *::std::find_if(possible_tys.begin(), possible_tys.end(), PossibleType::is_source_s);
                const auto& ent_d = *::std::find_if(possible_tys.begin(), possible_tys.end(), PossibleType::is_dest_s);

                // Only if both options are coerce?
                // TODO: And this ivar isn't Sized bounded?
                if( ent_s.is_coerce() && ent_d.is_coerce() )
                {
                    bool src_noivars = !context.m_ivars.type_contains_ivars(*ent_s.ty);
                    bool dst_noivars = !context.m_ivars.type_contains_ivars(*ent_d.ty);
                    bool src_valid = !check_ivar_poss__fails_bounds(sp, context, ty_l, *ent_s.ty);
                    bool dst_valid = !check_ivar_poss__fails_bounds(sp, context, ty_l, *ent_d.ty);

                    if( src_valid )
                    {
                        if( src_noivars )
                        {
                            DEBUG("Single each way, concrete source, " << *ent_s.ty);
                            context.equate_types(sp, ty_l, *ent_s.ty);
                            return true;
                        }
                    }
                    if( dst_valid )
                    {
                        if( dst_noivars )
                        {
                            DEBUG("Single each way, concrete destination, " << *ent_d.ty);
                            context.equate_types(sp, ty_l, *ent_d.ty);
                            return true;
                        }
                    }

                    if( src_valid )
                    {
                        DEBUG("Single each way, ivar source, " << *ent_s.ty);
                        context.equate_types(sp, ty_l, *ent_s.ty);
                        return true;
                    }
                    if( dst_valid )
                    {
                        DEBUG("Single each way, ivar destination, " << *ent_d.ty);
                        context.equate_types(sp, ty_l, *ent_d.ty);
                        return true;
                    }
                    // All of them failed bounds, what?
                }
            }

            // If there's no disable flags set, and there's only one source, pick it.
            // - Slight hack to speed up flow-down inference
            if( possible_tys.size() == 1 && possible_tys[0].is_source() && !ivar_ent.force_no_from ) {
                const auto* ty_p = possible_tys[0].ty;
                if( possible_tys[0].is_unsize() ) {
                    HIR::TypeRef tmp_ty;

                    do {
                        if( !check_ivar_poss__fails_bounds(sp, context, ty_l, *ty_p) ) {
                            DEBUG("Single possibility failed bounds, trying deref - " << *ty_p);
                            break;
                        }
                    } while( (ty_p = context.m_resolve.autoderef(sp, *ty_p, tmp_ty)) );
                    if(!ty_p) {
                        // All would fail, just set something sensible
                        ty_p = possible_tys[0].ty;
                    }
                }
                else {
                    //if( check_ivar_poss__fails_bounds(sp, context, ty_l, *ty_p) ) {
                    //    ERROR(
                    //}
                }
                DEBUG("One possibility (before ivar removal), setting to " << *ty_p);
                context.equate_types(sp, ty_l, *ty_p);
                return true;
            }

            // TODO: This shouldn't just return, instead the above null placeholders should be tested
            if( ivar_ent.force_no_to || ivar_ent.force_no_from )
            {
                switch(fallback_ty)
                {
                case IvarPossFallbackType::IgnoreWeakDisable:
                case IvarPossFallbackType::PickFirstBound:
                case IvarPossFallbackType::FinalOption:
                    break;
                default:
                    DEBUG(i << ": coercion blocked");
                    return false;
                }
            }

            // Filter out ivars
            // - TODO: Should this also remove &_ types? (maybe not, as they give information about borrow classes)
            size_t n_ivars;
            size_t n_src_ivars;
            size_t n_dst_ivars;
            bool possibly_diverge = false;
            {
                n_src_ivars = 0;
                n_dst_ivars = 0;
                auto new_end = ::std::remove_if(possible_tys.begin(), possible_tys.end(), [&](const PossibleType& ent) {
                        // TODO: Should this remove Unbound associated types too?
                        if( ent.ty->data().is_Infer() )
                        {
                            if( ent.is_source() )
                            {
                                n_src_ivars += 1;
                            }
                            else
                            {
                                n_dst_ivars += 1;
                            }
                            return true;
                        }
                        else if( ent.ty->data().is_Diverge() )
                        {
                            possibly_diverge = true;
                            return true;
                        }
                        else
                        {
                            return false;
                        }
                        });
                n_ivars = possible_tys.end() - new_end;
                possible_tys.erase(new_end, possible_tys.end());
            }
            DEBUG(n_ivars << " ivars (" << n_src_ivars << " src, " << n_dst_ivars << " dst)");
            (void)n_ivars;

            if( ivar_ent.has_bounded && ivar_ent.bounds_include_self )
            {
                n_ivars += 1;
            }

            // Rules:
            // - If bounds_include_self


            // === If there's no source ivars, find the least permissive source ===
            // - If this source can't be unsized (e.g. in `&_, &str`, `&str` is the least permissive, and can't be
            // coerced without a `*const _` in the list), then equate to that
            // 1. Find the most accepting pointer type (if there is at least one coercion source)
            // 2. Look for an option that uses that pointer type, and contains an unsized type (that isn't a trait
            //    object with markers)
            // 3. Assign to that known most-permissive option
            // TODO: Do the oposite for the destination types (least permissive pointer, pick any Sized type)
            if( n_src_ivars == 0 || fallback_ty == IvarPossFallbackType::Assume )
            {
                const ::HIR::TypeRef* ptr_ty = nullptr;
                if( ::std::any_of(possible_tys.begin(), possible_tys.end(), [&](const auto& ent){ return ent.cls == PossibleType::CoerceFrom; }) )
                {
                    for(const auto& ent : possible_tys)
                    {
                        if( !ent.is_source() )
                            continue;

                        bool unused_unordered=false;
                        if( ptr_ty == nullptr )
                        {
                            ptr_ty = ent.ty;
                        }
                        else if( TypeRestrictiveOrdering::get_ordering_ptr(sp, context, *ent.ty, *ptr_ty, unused_unordered, /*deep=*/false) == OrdLess )
                        {
                            ptr_ty = ent.ty;
                        }
                        else
                        {
                        }
                    }
                }

                for(const auto& ent : possible_tys)
                {
                    // Sources only
                    if( ! ent.is_source() )
                        continue ;
                    // Must match `ptr_ty`'s outer pointer
                    const ::HIR::TypeRef* inner_ty = (ptr_ty ? TypeRestrictiveOrdering::match_and_extract_ptr_ty(*ptr_ty, *ent.ty) : ent.ty);
                    if( !inner_ty )
                        continue;

                    bool is_max_accepting = false;
                    if( inner_ty->data().is_Slice() ) {
                        is_max_accepting = true;
                    }
                    else if( TU_TEST1(inner_ty->data(), Primitive, == ::HIR::CoreType::Str) ) {
                        is_max_accepting = true;
                    }
                    else {
                    }
                    if( is_max_accepting )
                    {
                        DEBUG("Most accepting pointer class, and most permissive inner type - " << *ent.ty);
                        context.equate_types(sp, ty_l, *ent.ty);
                        return true;
                    }
                }
            }

            // If there's multiple destination types (which means that this ivar has to be a coercion from one of them)
            // Look for the least permissive of the available destination types and assign to that
            #if 1
            // NOTE: This only works for coercions (not usizings), so is restricted to all options being pointers
            if( ::std::all_of(possible_tys.begin(), possible_tys.end(), PossibleType::is_coerce_s)
                )
            {
                // 1. Count distinct (and non-ivar) source types
                // - This also ignores &_ types
                size_t num_distinct = 0;
                for(const auto& ent : possible_tys)
                {
                    if( !ent.is_dest() )
                        continue ;
                    // Ignore infer borrows
                    if( TU_TEST1(ent.ty->data(), Borrow, .inner.data().is_Infer()) )
                        continue;
                    bool is_duplicate = false;
                    for(const auto& ent2 : possible_tys)
                    {
                        if( &ent2 == &ent )
                            break;
                        if( !ent2.is_source() )
                            continue ;
                        if( *ent.ty == *ent2.ty ) {
                            is_duplicate = true;
                            break;
                        }
                        // TODO: Compare such that &[_; 1] == &[u8; 1]?
                    }
                    if( !is_duplicate )
                    {
                        num_distinct += 1;
                    }
                }
                DEBUG("- " << num_distinct << " distinct destinations");
                // 2. Find the most restrictive destination type
                // - Borrows are more restrictive than pointers
                // - Borrows of Sized types are more restrictive than any other
                // - Decreasing borrow type ordering: Owned, Unique, Shared
                bool is_unordered = false;
                const ::HIR::TypeRef*   dest_type = nullptr;
                for(const auto& ent : possible_tys)
                {
                    if( ent.is_dest() )
                        continue ;
                    // Ignore &_ types?
                    // - No, need to handle them below
                    if( !dest_type ) {
                        dest_type = ent.ty;
                        continue ;
                    }

                    auto cmp = TypeRestrictiveOrdering::get_ordering_ptr(sp, context, *ent.ty, *dest_type, is_unordered);
                    switch(cmp)
                    {
                    case OrdLess:
                        // This entry is less restrictive, so don't update `dest_type`
                        break;
                    case OrdEqual:
                        break;
                    case OrdGreater:
                        // This entry is more restrictive, so DO update `dest_type`
                        dest_type = ent.ty;
                        is_unordered = false;
                        break;
                    }
                }
                // TODO: Unsized types? Don't pick an unsized if coercions are present?
                // TODO: If in a fallback mode, then don't require >1 (just require dest_type)
                if( (num_distinct > 1 || fallback_ty == IvarPossFallbackType::Assume) && dest_type && !is_unordered )
                {
                    DEBUG("- Least-restrictive source " << *dest_type);
                    context.equate_types(sp, ty_l, *dest_type);
                    return true;
                }
            }
            #endif

            // If there's multiple source types (which means that this ivar has to be a coercion from one of them)
            // Look for the least permissive of the available destination types and assign to that
            #if 1
            // NOTE: This only works for coercions (not usizings), so is restricted to all options being pointers
            if( ::std::all_of(possible_tys.begin(), possible_tys.end(), PossibleType::is_coerce_s)
                //||  ::std::none_of(possible_tys.begin(), possible_tys.end(), PossibleType::is_coerce_s)
                )
            {
                // 1. Count distinct (and non-ivar) source types
                // - This also ignores &_ types
                size_t num_distinct = 0;
                for(const auto& ent : possible_tys)
                {
                    if( !ent.is_source() )
                        continue ;
                    // Ignore infer borrows
                    if( TU_TEST1(ent.ty->data(), Borrow, .inner.data().is_Infer()) )
                        continue;
                    bool is_duplicate = false;
                    for(const auto& ent2 : possible_tys)
                    {
                        if( &ent2 == &ent )
                            break;
                        if( !ent2.is_source() )
                            continue ;
                        if( *ent.ty == *ent2.ty ) {
                            is_duplicate = true;
                            break;
                        }
                        // TODO: Compare such that &[_; 1] == &[u8; 1]?
                    }
                    if( !is_duplicate )
                    {
                        num_distinct += 1;
                    }
                }
                DEBUG("- " << num_distinct << " distinct sources");
                // 2. Find the most restrictive destination type
                // - Borrows are more restrictive than pointers
                // - Borrows of Sized types are more restrictive than any other
                // - Decreasing borrow type ordering: Owned, Unique, Shared
                bool is_unordered = false;
                const ::HIR::TypeRef*   dest_type = nullptr;
                for(const auto& ent : possible_tys)
                {
                    if( ent.is_source() )
                        continue ;
                    // Ignore &_ types?
                    // - No, need to handle them below
                    if( !dest_type ) {
                        dest_type = ent.ty;
                        continue ;
                    }

                    auto cmp = TypeRestrictiveOrdering::get_ordering_ptr(sp, context, *ent.ty, *dest_type, is_unordered);
                    switch(cmp)
                    {
                    case OrdLess:
                        // This entry is less restrictive, so DO update `dest_type`
                        dest_type = ent.ty;
                        is_unordered = false;
                        break;
                    case OrdEqual:
                        break;
                    case OrdGreater:
                        // This entry is more restrictive, so don't update `dest_type`
                        break;
                    }
                }
                // TODO: Unsized types? Don't pick an unsized if coercions are present?
                // TODO: If in a fallback mode, then don't require >1 (just require dest_type)
                if( (num_distinct > 1 || fallback_ty == IvarPossFallbackType::Assume) && dest_type && !is_unordered )
                {
                    DEBUG("- Most-restrictive destination " << *dest_type);
                    context.equate_types(sp, ty_l, *dest_type);
                    return true;
                }
            }
            #endif

            // TODO: Remove any types that are covered by another type
            // - E.g. &[T] and &[U] can be considered equal, because [T] can't unsize again
            // - Comparison function: Returns one of Incomparible,Less,Same,More - Representing the amount of type information present.
            {
                // De-duplicate destinations and sources separately
                for(auto it = possible_tys.begin(); it != possible_tys.end(); ++it)
                {
                    if( !it->ty )
                        continue;
                    for(auto it2 = it + 1; it2 != possible_tys.end(); ++it2)
                    {
                        if( !it2->ty )
                            continue;
                        if(it->cls != it2->cls) {
                            continue;
                        }

                        switch(InfoOrdering::compare_top(context, *it->ty, *it2->ty, /*should_deref=*/it->is_coerce()))
                        {
                        case InfoOrdering::Incompatible:
                            break;
                        case InfoOrdering::Less:
                            DEBUG("(less) Remove " << *it << ", keep " << *it2);
                            if(0)
                        case InfoOrdering::Same:
                            DEBUG("(same) Remove " << *it << ", keep " << *it2);
                            it->ty = it2->ty;
                            // Classes are already the same
                            it2->ty = nullptr;
                            break;
                        case InfoOrdering::More:
                            DEBUG("(more) Keep " << *it << ", remove " << *it2);
                            it2->ty = nullptr;
                            break;
                        }
                    }
                }
                auto new_end = ::std::remove_if(possible_tys.begin(), possible_tys.end(), [](const auto& e){ return e.ty == nullptr; });
                DEBUG("Removing " << (possible_tys.end() - new_end) << " redundant possibilities");
                possible_tys.erase(new_end, possible_tys.end());
            }

            // TODO: If in fallback mode, pick the most permissive option
            // - E.g. If the options are &mut T and *const T, use the *const T
            if( fallback_ty == IvarPossFallbackType::Assume )
            {
                // All are coercions (not unsizings)
                if( ::std::all_of(possible_tys.begin(), possible_tys.end(), PossibleType::is_coerce_s) && n_ivars == 0 )
                {
                    // Find the least restrictive destination, and most restrictive source
                    const ::HIR::TypeRef*   dest_type = nullptr;
                    bool any_ivar_present = false;
                    bool is_unordered = false;
                    for(const auto& ent : possible_tys)
                    {
                        if( visit_ty_with(*ent.ty, [](const ::HIR::TypeRef& t){ return t.data().is_Infer(); }) ) {
                            any_ivar_present = true;
                        }
                        if( !dest_type ) {
                            dest_type = ent.ty;
                            continue ;
                        }

                        auto cmp = TypeRestrictiveOrdering::get_ordering_ptr(sp, context, *ent.ty, *dest_type, is_unordered);
                        switch(cmp)
                        {
                        case OrdLess:
                            // This entry is less restrictive, so DO update `dest_type`
                            dest_type = ent.ty;
                            is_unordered = false;
                            break;
                        case OrdEqual:
                            break;
                        case OrdGreater:
                            // This entry is more restrictive, so don't update `dest_type`
                            break;
                        }
                    }

                    if( dest_type && n_ivars == 0 && any_ivar_present == false && !dest_type->data().is_Closure() && !is_unordered )
                    {
                        DEBUG("Suitable option " << *dest_type << " from " << possible_tys);
                        context.equate_types(sp, ty_l, *dest_type);
                        return true;
                    }
                }
            }

            DEBUG("possible_tys = " << possible_tys);
            DEBUG("- Bounded [" << ivar_ent.bounded << "]");
            DEBUG("possible_tys = " << possible_tys);
            // Filter out useless options and impossiblities
            for(auto it = possible_tys.begin(); it != possible_tys.end(); )
            {
                bool remove_option = false;
                if( *it->ty == ty_l )
                {
                    remove_option = true;
                }
                else if( !allow_unsized &&  context.m_resolve.type_is_sized(sp, *it->ty) == ::HIR::Compare::Unequal )
                {
                    remove_option = true;
                }
                else
                {
                    // Keep
                }

                // TODO: Ivars have been removed, this sort of check should be moved elsewhere.
                if( !remove_option && ty_l.data().as_Infer().ty_class == ::HIR::InferClass::Integer )
                {
                    if( const auto* te = it->ty->data().opt_Primitive() ) {
                        (void)te;
                    }
                    else if( const auto* te = it->ty->data().opt_Path() ) {
                        // If not Unbound, remove option
                        (void)te;
                    }
                    else if( const auto* te = it->ty->data().opt_Infer() ) {
                        (void)te;
                    }
                    else {
                        remove_option = true;
                    }
                }

                it = (remove_option ? possible_tys.erase(it) : it + 1);
            }
            DEBUG("possible_tys = " << possible_tys);
            for(auto it = possible_tys.begin(); it != possible_tys.end(); )
            {
                bool remove_option = false;
                for(const auto& other_opt : possible_tys)
                {
                    if( &other_opt == &*it )
                        continue ;
                    if( *other_opt.ty == *it->ty )
                    {
                        // Potential duplicate
                        // - If the flag set is the same, then it is a duplicate
                        if( other_opt.cls == it->cls ) {
                            remove_option = true;
                            break;
                        }
                        // If not an ivar, AND both are either unsize/pointer AND the deref flags are different
                        // TODO: Ivars have been removed?
                        if( !it->ty->data().is_Infer() && other_opt.is_coerce() == it->is_coerce() && other_opt.is_source() != it->is_source() )
                        {
                            // TODO: Possible duplicate with a check above...
                            DEBUG("Source and destination possibility, picking " << *it->ty);
                            context.equate_types(sp, ty_l, *it->ty);
                            return true;
                        }
                        // - Otherwise, we want to keep the option which doesn't allow dereferencing (remove current
                        // if it's the deref option)
                        if( it->is_source() && other_opt.is_coerce() == it->is_coerce() ) {
                            remove_option = true;
                            break;
                        }
                    }
                }
                it = (remove_option ? possible_tys.erase(it) : it + 1);
            }
            DEBUG("possible_tys = " << possible_tys);

            if( possible_tys.size() >= 2
            // && n_ivars == 0
             && (fallback_ty == IvarPossFallbackType::FinalOption || n_src_ivars == 0)
            // && (fallback_ty == IvarPossFallbackType::FinalOption || n_ivars == 0)
             && std::all_of(possible_tys.begin(), possible_tys.end(),
                [](const auto& e){ return e.ty && (e.ty->data().is_Closure() || e.ty->data().is_NamedFunction()); })
                )
            {
                HIR::TypeRef    new_ty;
                if( const auto* te = possible_tys[0].ty->data().opt_NamedFunction() ) {
                    new_ty = te->decay(sp);
                }
                else if( const auto* t1_c = possible_tys[0].ty->data().opt_Closure() ) {
                    auto ft = HIR::TypeData_FunctionPointer { HIR::GenericParams(), false, false, RcString::new_interned(ABI_RUST), t1_c->node->m_return.clone(), {} };
                    for(const auto& t : t1_c->node->m_args)
                        ft.m_arg_types.push_back(t.second.clone());
                    new_ty = HIR::TypeRef(std::move(ft));
                }
                else {
                    BUG(sp, "");
                }
                DEBUG("HACK: All options are closures/functions, adding a function pointer - " << new_ty);
                context.equate_types(sp, ty_l, new_ty);
                return true;
            }

            // Remove any options that are filled by other options (e.g. `str` and a derferencable String)
            for(auto it = possible_tys.begin(); it != possible_tys.end(); )
            {
                bool remove_option = false;
                if( it->is_source() && !it->ty->data().is_Infer() )
                {
                    DEBUG("> " << *it);
                    // Dereference once before starting the search
                    ::HIR::TypeRef  tmp, tmp2;
                    const auto* dty = it->ty;
                    auto src_bty = ::HIR::BorrowType::Shared;
                    if(it->is_coerce())
                    {
                        if( dty->data().is_Borrow() )
                            src_bty = dty->data().as_Borrow().type;
                        dty = context.m_resolve.autoderef(sp, *dty, tmp);
                        // NOTE: Coercions can also do closure->fn, so deref isn't always possible
                        //ASSERT_BUG(sp, dty, "Pointer (coercion source) that can't dereference - " << *it->ty);
                    }
                    //while( dty && !remove_option && (dty = context.m_resolve.autoderef(sp, *dty, tmp)) )
                    if( dty )
                    {
                        for(const auto& other_opt : possible_tys)
                        {
                            if( &other_opt == &*it )
                                continue ;
                            if( other_opt.ty->data().is_Infer() )
                                continue ;
                            DEBUG(" > " << other_opt);

                            const auto* oty = other_opt.ty;
                            auto o_bty = ::HIR::BorrowType::Owned;
                            if(other_opt.is_coerce())
                            {
                                if( oty->data().is_Borrow() )
                                    o_bty = oty->data().as_Borrow().type;
                                oty = context.m_resolve.autoderef(sp, *oty, tmp2);
                                //ASSERT_BUG(sp, oty, "Pointer (coercion src/dst) that can't dereference - " << *other_opt.ty);
                            }
                            if( o_bty > src_bty )   // Smaller means less powerful. Converting & -> &mut is invalid
                            {
                                // Borrow types aren't compatible
                                DEBUG("BT " << o_bty << " > " << src_bty);
                                break;
                            }
                            // TODO: Check if unsize is possible from `dty` to `oty`
                            if( oty )
                            {
                                DEBUG(" > " << *dty << " =? " << *oty);
                                auto cmp = check_unsize_tys(context, sp, *oty, *dty, nullptr);
                                DEBUG("check_unsize_tys(..) = " << cmp);
                                if( cmp == CoerceResult::Equality )
                                {
                                    //TODO(sp, "Impossibility for " << *oty << " := " << *dty);
                                }
                                else if( cmp == CoerceResult::Unknown )
                                {
                                }
                                else
                                {
                                    remove_option = true;
                                    DEBUG("- Remove " << *it << ", can deref to " << other_opt);
                                    break;
                                }
                            }
                        }
                    }
                }
                if( !remove_option && !it->ty->data().is_Infer() && check_ivar_poss__fails_bounds(sp, context, ty_l, *it->ty) )
                {
                    remove_option = true;
                    DEBUG("- Remove " << *it << " due to bounds");
                }
                it = (remove_option ? possible_tys.erase(it) : it + 1);
            }
            DEBUG("possible_tys = {" << possible_tys << "} (" << n_src_ivars << " src ivars, " << n_dst_ivars << " dst ivars, possibly_diverge=" << possibly_diverge << ")");

            if( n_src_ivars == 0 && /*n_dst_ivars == 0 &&*/ possible_tys.empty() && possibly_diverge && fallback_ty == IvarPossFallbackType::IgnoreWeakDisable ) {
                auto t = ::HIR::TypeRef::new_diverge();
                if( !check_ivar_poss__fails_bounds(sp, context, ty_l, t) )
                {
                    DEBUG("Possibly `!` and no other options - setting");
                    context.equate_types(sp, ty_l, ::HIR::TypeRef::new_diverge());
                    return true;
                }
            }

            // Find a CD option that can deref to a `--` option
            for(const auto& e : possible_tys)
            {
                if( e.cls == PossibleType::CoerceFrom )
                {
                    ::HIR::TypeRef  tmp;
                    const auto* dty = context.m_resolve.autoderef(sp, *e.ty, tmp);
                    if( dty && !dty->data().is_Infer() )
                    {
                        for(const auto& e2 : possible_tys)
                        {
                            if( e2.cls == PossibleType::UnsizeTo )
                            {
                                if( context.m_ivars.types_equal(*dty, *e2.ty) )
                                {
                                    DEBUG("Coerce source can deref once to an unsize destination, picking source " << *e.ty);
                                    context.equate_types(sp, ty_l, *e.ty);
                                    return true;
                                }
                            }
                        }
                    }
                }
            }

            // If there's only one option (or one real option w/ ivars, if in fallback mode) - equate it
            if( possible_tys.size() == 1 )
            {
                bool active = false;
                switch(fallback_ty)
                {
                case IvarPossFallbackType::None:
                case IvarPossFallbackType::Backwards:
                case IvarPossFallbackType::IgnoreWeakDisable:
                    active = (n_ivars == 0 && ivar_ent.bounded.size() == 0);
                    break;
                case IvarPossFallbackType::Assume:
                case IvarPossFallbackType::PickFirstBound:
                    active = (ivar_ent.bounded.size() == 0);
                    break;
                case IvarPossFallbackType::FinalOption:
                    active = true;
                    break;
                }
                if(active)
                {
                    const auto& new_ty = *possible_tys[0].ty;
                    DEBUG("Only one option: " << new_ty);
                    context.equate_types(sp, ty_l, new_ty);
                    return true;
                }
            }
            // -- Single source/destination --
            // Try if in first level fallback, or the bounded list is empty
            if( (!honour_disable || !ivar_ent.has_bounded) )
            {
                // If there's only one non-deref in the list OR there's only one deref in the list
                if( n_src_ivars == 0 && ::std::count_if(possible_tys.begin(), possible_tys.end(), PossibleType::is_source_s) == 1 )
                {
                    auto it = ::std::find_if(possible_tys.begin(), possible_tys.end(), PossibleType::is_source_s);
                    const auto& new_ty = *it->ty;
                    DEBUG("Picking " << new_ty << " as the only source [" << possible_tys << "]");
                    context.equate_types(sp, ty_l, new_ty);
                    return true;
                }
                if( fallback_ty != IvarPossFallbackType::None && n_dst_ivars == 0 && ::std::count_if(possible_tys.begin(), possible_tys.end(), PossibleType::is_dest_s) == 1 )
                {
                    auto it = ::std::find_if(possible_tys.begin(), possible_tys.end(), PossibleType::is_dest_s);
                    const auto& new_ty = *it->ty;
                    if( it->is_coerce() )
                    {
                        DEBUG("Picking " << new_ty << " as the only target [" << possible_tys << "]");
                        context.equate_types(sp, ty_l, new_ty);
                        return true;
                    }
                    else
                    {
                        // HACK: Work around failure in librustc
                        DEBUG("Would pick " << new_ty << " as the only target, but it's an unsize");
                    }
                }
            }
            // If there's multiple possiblilties, we're in fallback mode, AND there's no ivars in the list
            // TODO: Exclude bounds? (not all of those are safe to include)
            if( ivar_ent.bounded.size() == 0 )
            {
                if( possible_tys.size() > 0 && !honour_disable && n_ivars == 0 )
                {
                    //::std::sort(possible_tys.begin(), possible_tys.end());  // Sorts ivars to the front
                    const auto& new_ty = *possible_tys.back().ty;
                    DEBUG("Picking " << new_ty << " as an arbitary an option from [" << possible_tys << "]");
                    context.equate_types(sp, ty_l, new_ty);
                    return true;
                }
            }

            // If only one bound meets the possible set, use it
            if( ! possible_tys.empty() && (!ivar_ent.bounds_include_self || fallback_ty == IvarPossFallbackType::FinalOption) )
            {
                DEBUG("Checking bounded [" << ivar_ent.bounded << "]");
                ::std::vector<const ::HIR::TypeRef*>    feasable_bounds;
                for(const auto& new_ty : ivar_ent.bounded)
                {
                    bool failed_a_bound = false;
                    // TODO: Check if this bounded type _cannot_ work with any of the existing bounds
                    // - Don't add to the possiblity list if so
                    for(const auto& opt : possible_tys)
                    {
                        if( opt.cls == PossibleType::Equal ) {
                            continue ;
                        }
                        // If a fuzzy compare succeeds, keep
                        switch( new_ty.compare_with_placeholders(sp, *opt.ty, context.m_ivars.callback_resolve_infer()) )
                        {
                        case HIR::Compare::Unequal:
                            // If not equal, then maybe an unsize could happen
                            break;
                        case HIR::Compare::Fuzzy:
                        case HIR::Compare::Equal:
                            continue;
                        }
                        CoerceResult    cmp;
                        if( opt.is_source() ) {
                            DEBUG("(checking bounded) > " << new_ty << " =? " << *opt.ty);
                            cmp = check_unsize_tys(context, sp, new_ty, *opt.ty, nullptr);
                        }
                        else {
                            // Destination type, this option must deref to it
                            DEBUG("(checking bounded) > " << *opt.ty << " =? " << new_ty);
                            cmp = check_unsize_tys(context, sp, *opt.ty, new_ty, nullptr);
                        }
                        DEBUG("(checking bounded) cmp = " << cmp);
                        if( cmp == CoerceResult::Equality ) {
                            failed_a_bound = true;
                            break;
                        }
                    }
                    // TODO: Should this also check check_ivar_poss__fails_bounds
                    if( !failed_a_bound )
                    {
                        feasable_bounds.push_back(&new_ty);
                    }
                }
                DEBUG("Checking bounded: " << feasable_bounds.size() << " feasible bounds");
                if( feasable_bounds.size() == 1 )
                {
                    const auto& new_ty = *feasable_bounds.front();
                    DEBUG("Picking " << new_ty << " as it's the only bound that fits coercions");
                    context.equate_types(sp, ty_l, new_ty);
                    return true;
                }
            }
            else
            {
                // Not checking bounded list, because there's nothing to check
            }

            has_no_coerce_posiblities = possible_tys.empty() && n_ivars == 0;
        }

        if( has_no_coerce_posiblities && !ivar_ent.bounded.empty() )
        {
            // TODO: Search know possibilties and check if they satisfy the bounds for this ivar
            DEBUG("Options: " << ivar_ent.bounded);
            unsigned int n_good_ints = 0;
            ::std::vector<const ::HIR::TypeRef*>    good_types;
            good_types.reserve(ivar_ent.bounded.size());
            for(const auto& new_ty : ivar_ent.bounded)
            {
                DEBUG("- Test " << new_ty << " against current rules");
                if( check_ivar_poss__fails_bounds(sp, context, ty_l, new_ty) )
                {
                }
                else
                {
                    good_types.push_back(&new_ty);

                    if( new_ty.data().is_Primitive() )
                        n_good_ints ++;

                    DEBUG("> " << new_ty << " feasible");
                }
            }
            DEBUG(good_types.size() << " valid options (" << n_good_ints << " primitives)");
            // Picks the first if in fallback mode (which is signalled by `honour_disable` being false)
            // - This handles the case where there's multiple valid options (needed for libcompiler_builtins)
            // TODO: Only pick any if all options are the same class (or just all are integers)
            if( good_types.empty() )
            {

            }
            else if( good_types.size() == 1 )
            {
                // Since it's the only possibility, choose it?
                DEBUG("Only " << *good_types.front() << " fits current bound sets");
                context.equate_types(sp, ty_l, *good_types.front());
                return true;
            }
            else if( good_types.size() > 0 && fallback_ty == IvarPossFallbackType::FinalOption )
            {
                auto typ_is_borrow = [&](const ::HIR::TypeRef* typ) { return typ->data().is_Borrow(); };
                // NOTE: We want to select from sets of primitives and generics (which can be interchangable)
                if( ::std::all_of(good_types.begin(), good_types.end(), typ_is_borrow) == ::std::any_of(good_types.begin(), good_types.end(), typ_is_borrow) )
                {
                    DEBUG("Picking " << *good_types.front() << " as first of " << good_types.size() << " options [" << FMT_CB(ss, for(auto e:good_types) ss << *e << ",";) << "]");
                    context.equate_types(sp, ty_l, *good_types.front());
                    return true;
                }
                else
                {
                    // Mix of borrows with non-borrows
                }
            }
        }

        return false;
    }
}



void Typecheck_Code_CS(const typeck::ModuleState& ms, t_args& args, const ::HIR::TypeRef& result_type, ::HIR::ExprPtr& expr)
{
    TRACE_FUNCTION;

    auto root_ptr = expr.into_unique();
    assert(!ms.m_mod_paths.empty());
    Context context { ms.m_crate, ms.m_impl_generics, ms.m_item_generics, ms.m_mod_paths.back(), ms.m_current_trait };

    // - Build up ruleset from node tree
    Typecheck_Code_CS__EnumerateRules(context, ms, args, result_type, expr, root_ptr);

    const unsigned int MAX_ITERATIONS = 1000;
    unsigned int count = 0;
    while( context.take_changed() /*&& context.has_rules()*/ && count < MAX_ITERATIONS )
    {
        TRACE_FUNCTION_F("=== PASS " << count << " ===");
        context.dump();

        // 1. Check coercions for ones that cannot coerce due to RHS type (e.g. `str` which doesn't coerce to anything)
        // 2. (???) Locate coercions that cannot coerce (due to being the only way to know a type)
        // - Keep a list in the ivar of what types that ivar could be equated to.
        if( ! context.m_ivars.peek_changed() )
        {
            DEBUG("--- Coercion checking");
            for(size_t i = 0; i < context.link_coerce.size(); )
            {
                auto ent = mv$(context.link_coerce[i]);
                const auto& span = (*ent->right_node_ptr)->span();
                auto& src_ty = (*ent->right_node_ptr)->m_res_type;
                src_ty = context.m_resolve.expand_associated_types( span, mv$(src_ty) );    // TODO: This was commented, why?
                ent->left_ty = context.m_resolve.expand_associated_types( span, mv$(ent->left_ty) );
                if( check_coerce(context, *ent) )
                {
                    DEBUG("- Consumed coercion R" << ent->rule_idx << " " << ent->left_ty << " := " << src_ty);

                    context.link_coerce.erase( context.link_coerce.begin() + i );
                }
                else
                {
                    context.link_coerce[i] = mv$(ent);
                    ++ i;
                }
            }
            // 3. Check associated type rules
            DEBUG("--- Associated types");
            unsigned int link_assoc_iter_limit = context.link_assoc.size() * 4;
            for(unsigned int i = 0; i < context.link_assoc.size(); ) {
                // - Move out (and back in later) to avoid holding a bad pointer if the list is updated
                auto rule = mv$(context.link_assoc[i]);

                DEBUG("- " << rule);
                for( auto& ty : rule.params.m_types ) {
                    ty = context.m_resolve.expand_associated_types(rule.span, mv$(ty));
                }
                if( rule.name != "" ) {
                    rule.left_ty = context.m_resolve.expand_associated_types(rule.span, mv$(rule.left_ty));
                    // HACK: If the left type is `!`, remove the type bound
                    //if( rule.left_ty.data().is_Diverge() ) {
                    //    rule.name = "";
                    //}
                }
                rule.impl_ty = context.m_resolve.expand_associated_types(rule.span, mv$(rule.impl_ty));

                if( check_associated(context, rule) ) {
                    DEBUG("- Consumed associated type rule " << i << "/" << context.link_assoc.size() << " - " << rule);
                    if( i != context.link_assoc.size()-1 )
                    {
                        //assert( context.link_assoc[i] != context.link_assoc.back() );
                        context.link_assoc[i] = mv$( context.link_assoc.back() );
                    }
                    context.link_assoc.pop_back();
                }
                else {
                    context.link_assoc[i] = mv$(rule);
                    i ++;
                }

                if( link_assoc_iter_limit -- == 0 )
                {
                    DEBUG("link_assoc iteration limit exceeded");
                    break;
                }
            }
        }
        // 4. Revisit nodes that require revisiting
        if( ! context.m_ivars.peek_changed() )
        {
            DEBUG("--- Node revisits");
            for( auto it = context.to_visit.begin(); it != context.to_visit.end(); )
            {
                ::HIR::ExprNode& node = **it;
                ExprVisitor_Revisit visitor { context };
                DEBUG("> " << &node << " " << typeid(node).name() << " -> " << context.m_ivars.fmt_type(node.m_res_type));
                node.visit( visitor );
                //  - If the node is completed, remove it
                if( visitor.node_completed() ) {
                    DEBUG("- Completed " << &node << " - " << typeid(node).name());
                    it = context.to_visit.erase(it);
                }
                else {
                    ++ it;
                }
            }
            {
                ::std::vector<bool> adv_revisit_remove_list;
                size_t  len = context.adv_revisits.size();
                for(size_t i = 0; i < len; i ++)
                {
                    auto& ent = *context.adv_revisits[i];
                    DEBUG("> " << FMT_CB(os, ent.fmt(os)));
                    adv_revisit_remove_list.push_back( ent.revisit(context, /*is_fallback=*/false) );
                }
                for(size_t i = len; i --;)
                {
                    if( adv_revisit_remove_list[i] ) {
                        context.adv_revisits.erase( context.adv_revisits.begin() + i );
                    }
                }
            }
        }

        // If nothing changed this pass, apply ivar possibilities
        // - This essentially forces coercions not to happen.
        if( ! context.m_ivars.peek_changed() )
        {
            // Check the possible equations
            DEBUG("--- IVar possibilities");
            // TODO: De-duplicate this with the block ~80 lines below
            //for(unsigned int i = context.possible_ivar_vals.size(); i --; ) // NOTE: Ordering is a hack for libgit2
            for(unsigned int i = 0; i < context.possible_ivar_vals.size(); i ++ )
            {
                if( check_ivar_poss(context, i, context.possible_ivar_vals[i]) ) {
                    static Span sp;
                    //assert( context.possible_ivar_vals[i].has_rules() );
                    // Disable all metioned ivars in the possibilities
                    for(const auto& ty : context.possible_ivar_vals[i].types_coerce_to)
                        context.possible_equate_type_unknown(sp, ty.ty, Context::IvarUnknownType::From);
                    for(const auto& ty : context.possible_ivar_vals[i].types_coerce_from)
                        context.possible_equate_type_unknown(sp, ty.ty, Context::IvarUnknownType::To);

                    // Also disable inferrence (for this pass) for all ivars in affected bounds
                    if(false)
                    for(const auto& la : context.link_assoc)
                    {
                        bool found = false;
                        auto cb = [&](const auto& t) { return TU_TEST1(t.data(), Infer, .index == i); };
                        if( la.left_ty != ::HIR::TypeRef() )
                            found |= visit_ty_with( la.left_ty, cb );
                        found |= visit_ty_with( la.impl_ty, cb );
                        for(const auto& t : la.params.m_types)
                            found |= visit_ty_with( t, cb );
                        if( found )
                        {
                            if(la.left_ty != ::HIR::TypeRef())
                                context.possible_equate_type_unknown(sp, la.left_ty, Context::IvarUnknownType::From);
                            context.possible_equate_type_unknown(sp, la.impl_ty, Context::IvarUnknownType::From);
                            for(const auto& t : la.params.m_types)
                                context.possible_equate_type_unknown(sp, t, Context::IvarUnknownType::From);
                        }
                    }
                }
                else {
                    //assert( !context.m_ivars.peek_changed() );
                }
            }
        } // `if peek_changed` (ivar possibilities)

        // If nothing has changed, 
        if( !context.m_ivars.peek_changed() )
        {
            // Check the possible equations
            DEBUG("--- IVar possibilities (fallback 0)");
            for(unsigned int i = 0; i < context.possible_ivar_vals.size(); i ++ )
            {
                if( check_ivar_poss(context, i, context.possible_ivar_vals[i], IvarPossFallbackType::Backwards) ) {
                    break;
                }
            }
        }

        // If nothing has changed, run check_ivar_poss again but allow it to assume is has all the options
        if( !context.m_ivars.peek_changed() )
        {
            // Check the possible equations
            DEBUG("--- IVar possibilities (fallback 1)");
            //for(unsigned int i = context.possible_ivar_vals.size(); i --; ) // NOTE: Ordering is a hack for libgit2
            for(unsigned int i = 0; i < context.possible_ivar_vals.size(); i ++ )
            {
                if( check_ivar_poss(context, i, context.possible_ivar_vals[i], IvarPossFallbackType::Assume) ) {
                    break;
                }
            }
        }

        // If nothing has changed, run check_ivar_poss again but ignoring the 'disable' flag
#if 1
        if( !context.m_ivars.peek_changed() )
        {
            // Check the possible equations
            DEBUG("--- IVar possibilities (fallback)");
            //for(unsigned int i = context.possible_ivar_vals.size(); i --; ) // NOTE: Ordering is a hack for libgit2
            for(unsigned int i = 0; i < context.possible_ivar_vals.size(); i ++ )
            {
                if( check_ivar_poss(context, i, context.possible_ivar_vals[i], IvarPossFallbackType::IgnoreWeakDisable) ) {
                    break;
                }
                else {
                    //assert( !context.m_ivars.peek_changed() );
                }
            }
        } // `if peek_changed` (ivar possibilities #2)
#endif

        if( !context.m_ivars.peek_changed() )
        {
            DEBUG("--- Node revisits (fallback)");
            for( auto it = context.to_visit.begin(); it != context.to_visit.end(); )
            {
                ::HIR::ExprNode& node = **it;
                ExprVisitor_Revisit visitor { context, true };
                DEBUG("> " << &node << " " << typeid(node).name() << " -> " << context.m_ivars.fmt_type(node.m_res_type));
                node.visit( visitor );
                //  - If the node is completed, remove it
                if( visitor.node_completed() ) {
                    DEBUG("- Completed " << &node << " - " << typeid(node).name());
                    it = context.to_visit.erase(it);
                }
                else {
                    ++ it;
                }
            }
            {
                ::std::vector<bool> adv_revisit_remove_list;
                size_t  len = context.adv_revisits.size();
                for(size_t i = 0; i < len; i ++)
                {
                    auto& ent = *context.adv_revisits[i];
                    DEBUG("> " << FMT_CB(os, ent.fmt(os)));
                    adv_revisit_remove_list.push_back( ent.revisit(context, /*is_fallback=*/true) );
                }
                for(size_t i = len; i --;)
                {
                    if( adv_revisit_remove_list[i] ) {
                        context.adv_revisits.erase( context.adv_revisits.begin() + i );
                    }
                }
            }
        } // `if peek_changed` (node revisits)

#if 1
        if( !context.m_ivars.peek_changed() )
        {
            // Check the possible equations
            DEBUG("--- IVar possibilities (just pick a bound)");
            for(unsigned int i = 0; i < context.possible_ivar_vals.size(); i ++ )
            {
                if( check_ivar_poss(context, i, context.possible_ivar_vals[i], IvarPossFallbackType::PickFirstBound) ) {
                    break;
                }
            }
        }
        if( !context.m_ivars.peek_changed() )
        {
            // Check the possible equations
            DEBUG("--- IVar possibilities (final fallback)");
            //for(unsigned int i = context.possible_ivar_vals.size(); i --; ) // NOTE: Ordering is a hack for libgit2
            for(unsigned int i = 0; i < context.possible_ivar_vals.size(); i ++ )
            {
                if( check_ivar_poss(context, i, context.possible_ivar_vals[i], IvarPossFallbackType::FinalOption) ) {
                    break;
                }
            }
        }
#endif

#if 0
        if( !context.m_ivars.peek_changed() )
        {
            DEBUG("--- Coercion consume");
            if( ! context.link_coerce.empty() )
            {
                auto ent = mv$(context.link_coerce.front());
                context.link_coerce.erase( context.link_coerce.begin() );

                const auto& sp = (*ent->right_node_ptr)->span();
                auto& src_ty = (*ent->right_node_ptr)->m_res_type;
                //src_ty = context.m_resolve.expand_associated_types( sp, mv$(src_ty) );
                ent->left_ty = context.m_resolve.expand_associated_types( sp, mv$(ent->left_ty) );
                DEBUG("- Equate coercion R" << ent->rule_idx << " " << ent->left_ty << " := " << src_ty);

                context.equate_types(sp, ent->left_ty, src_ty);
            }
        }
#endif

        // Finally. If nothing changed, apply ivar defaults
        if( !context.m_ivars.peek_changed() )
        {
            DEBUG("- Applying defaults");
            if( context.m_ivars.apply_defaults() ) {
                context.m_ivars.mark_change();
            }
        }

        // And after all that, apply custom defaults
        if( !context.m_ivars.peek_changed() )
        {
            DEBUG("- Applying generic defaults");
            for(unsigned int i = 0; i < context.possible_ivar_vals.size(); i ++ )
            {
                const auto& ent = context.possible_ivar_vals[i];
                if( !ent.types_default.empty() )
                {
                    const auto& ty_l = context.m_ivars.get_type(i);

                    if( TU_TEST1(ty_l.data(), Infer, .index == i) )
                    {
                        if( ent.types_default.size() != 1 )
                        {
                            // TODO: Error?
                        }
                        else
                        {
                            context.m_ivars.set_ivar_to(i, ent.types_default.begin()->clone());
                        }
                    }
                }
            }
        }

#if 1
        if( !context.m_ivars.peek_changed() )
        {
            DEBUG("--- Coercion consume");
            if( ! context.link_coerce.empty() )
            {
                auto ent = mv$(context.link_coerce.front());
                context.link_coerce.erase( context.link_coerce.begin() );

                const auto& sp = (*ent->right_node_ptr)->span();
                auto& src_ty = (*ent->right_node_ptr)->m_res_type;
                //src_ty = context.m_resolve.expand_associated_types( sp, mv$(src_ty) );
                ent->left_ty = context.m_resolve.expand_associated_types( sp, mv$(ent->left_ty) );
                DEBUG("- Equate coercion R" << ent->rule_idx << " " << ent->left_ty << " := " << src_ty);

                context.equate_types(sp, ent->left_ty, src_ty);
            }
        }
#endif

        // Clear ivar possibilities for next pass
        for(auto& ivar_ent : context.possible_ivar_vals)
        {
            ivar_ent.reset();
        }

        count ++;
        context.m_resolve.compact_ivars(context.m_ivars);
    }
    if( count == MAX_ITERATIONS ) {
        BUG(root_ptr->span(), "Typecheck ran for too many iterations, max - " << MAX_ITERATIONS);
    }

    if( context.has_rules() )
    {
        context.dump();
        for(const auto& coercion_p : context.link_coerce)
        {
            const auto& coercion = *coercion_p;
            const auto& sp = (**coercion.right_node_ptr).span();
            const auto& src_ty = (**coercion.right_node_ptr).m_res_type;
            WARNING(sp, W0000, "Spare Rule - " << context.m_ivars.fmt_type(coercion.left_ty) << " := " << context.m_ivars.fmt_type(src_ty) );
        }
        for(const auto& rule : context.link_assoc)
        {
            const auto& sp = rule.span;
            if( rule.name == "" || rule.left_ty == ::HIR::TypeRef() )
            {
                WARNING(sp, W0000, "Spare Rule - " << context.m_ivars.fmt_type(rule.impl_ty) << " : " << rule.trait << rule.params );
            }
            else
            {
                WARNING(sp, W0000, "Spare Rule - " << context.m_ivars.fmt_type(rule.left_ty)
                    << " = < " << context.m_ivars.fmt_type(rule.impl_ty) << " as " << rule.trait << rule.params << " >::" << rule.name );
            }
        }
        // TODO: Print revisit rules and advanced revisit rules.
        for(const auto& node : context.to_visit)
        {
            const auto& sp = node->span();
            WARNING(sp, W0000, "Spare rule - " << FMT_CB(os, { ExprVisitor_Print ev(context, os); node->visit(ev); }) << " -> " << context.m_ivars.fmt_type(node->m_res_type));
        }
        for(const auto& adv : context.adv_revisits)
        {
            WARNING(adv->span(), W0000, "Spare Rule - " << FMT_CB(os, adv->fmt(os)));
        }
        BUG(root_ptr->span(), "Spare rules left after typecheck stabilised");
    }
    DEBUG("root_ptr = " << root_ptr->type_name() << " " << root_ptr->m_res_type);

    // - Recreate the pointer
    expr.reset( root_ptr.release() );
    //  > Steal the binding types
    expr.m_bindings.reserve( context.m_bindings.size() );
    for(auto& binding : context.m_bindings) {
        expr.m_bindings.push_back( binding.ty.clone() );
    }

    // - Validate typeck
    {
        DEBUG("==== VALIDATE ==== (" << count << " rounds)");
        context.dump();

        ExprVisitor_Apply   visitor { context };
        visitor.visit_node_ptr( expr );
    }

    // - De-duplicate types
    {
        struct CompactTypes: HIR::ExprVisitorDef
        {
            std::map<std::string, HIR::TypeRef>  types;

            void visit_type(HIR::TypeRef& ty) override
            {
                HIR::ExprVisitorDef::visit_type(ty);

                // Use string comparison to ensure that lifetimes are checked
                auto s = FMT(ty);
                if(s[0] == '{') {
                    auto p = s.find('}');
                    s = s.substr(p+1);
                }

                auto it = types.find(s);
                if( it != types.end() ) {
                    ty = HIR::TypeRef(it->second);
                }
                else {
                    types.insert(std::make_pair(s, ty));
                }
            };
        } visitor;
        expr->visit(visitor);
        for(auto& b : expr.m_bindings)
        {
            visitor.visit_type(b);
        }
        for(auto& b : expr.m_erased_types)
        {
            visitor.visit_type(b);
        }
    }

    {
        DEBUG("==== FINAL VALIDATE ====");
        StaticTraitResolve  static_resolve(ms.m_crate);
        static_resolve.set_both_generics_raw(ms.m_impl_generics, ms.m_item_generics);
        Typecheck_Expressions_ValidateOne(static_resolve, args, result_type, expr);

        DEBUG("=== Method const params ===");
        struct VisitMethodConst: public HIR::ExprVisitorDef {
            const typeck::ModuleState&  ms;
            const StaticTraitResolve& static_resolve;

            VisitMethodConst(const typeck::ModuleState& ms, const StaticTraitResolve& static_resolve)
                : ms(ms)
                , static_resolve(static_resolve)
            {
            }

            void visit(HIR::ExprNode_CallMethod& node) override {
                HIR::ExprVisitorDef::visit(node);

                HIR::PathParams*    params_ptr = nullptr;
                TU_MATCH_HDRA( (node.m_method_path.m_data), {)
                TU_ARMA(Generic, _pe)   BUG(node.span(), "");
                TU_ARMA(UfcsUnknown, _pe)   BUG(node.span(), "");
                TU_ARMA(UfcsKnown, pe)  params_ptr = &pe.params;
                TU_ARMA(UfcsInherent, pe)  params_ptr = &pe.params;
                }
                assert(params_ptr);

                bool found = false;
                for(auto& v : params_ptr->m_values)
                {
                    if(v.is_Unevaluated())
                    {
                        found = true;
                    }
                }
                if(found)
                {
                    TRACE_FUNCTION_FR("Method const params: " << node.m_method_path, "Method const params");
                    MonomorphState  out_params;
                    auto val_ref = static_resolve.get_value(node.span(), node.m_method_path, out_params, /*signature_only=*/true, nullptr);
                    const HIR::Function& fcn = *val_ref.as_Function();
                    const HIR::GenericParams& gp_def = fcn.m_params;
                    ConvertHIR_ConstantEvaluate_MethodParams(node.span(), ms.m_crate, ms.m_mod_paths.back(), ms.m_impl_generics, ms.m_item_generics, gp_def, *params_ptr);
                }
            }
        } v(ms, static_resolve);
        expr->visit(v);
    }
}

