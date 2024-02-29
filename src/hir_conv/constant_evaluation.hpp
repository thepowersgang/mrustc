/*
 */
#include <hir/hir.hpp>

namespace MIR {
    namespace eval {
        class AllocationPtr;
        class Allocation;
        class CallStackEntry;
    }
    class Statement;
    class Terminator;
}

namespace HIR {

struct Evaluator
{
    class Newval
    {
    public:
        virtual ::HIR::Path new_static(::HIR::TypeRef type, EncodedLiteral value) = 0;
    };
    class CsePtr {
        ::MIR::eval::CallStackEntry*    m_inner;
    public:
        ~CsePtr();
        CsePtr(::MIR::eval::CallStackEntry* ptr): m_inner(ptr) {}
        CsePtr(const CsePtr& ) = delete;
        CsePtr& operator=(const CsePtr& ) = delete;
        CsePtr(CsePtr&& x): m_inner(x.m_inner) { x.m_inner = nullptr; }
        CsePtr& operator=(CsePtr&& x) { this->~CsePtr(); this->m_inner = x.m_inner; x.m_inner = nullptr; return *this; }

        ::MIR::eval::CallStackEntry* operator->(){ return m_inner; }
        ::MIR::eval::CallStackEntry& operator*(){ return *m_inner; }
    };

    Span    root_span;
    StaticTraitResolve  resolve;
    Newval& nvs;
    unsigned int eval_index;
    unsigned int num_frames;
    // Note: Pointer is needed to maintain internal reference stability
    ::std::vector<CsePtr>   call_stack;

    static unsigned s_next_eval_index;

public:
    Evaluator(const Span& sp, const ::HIR::Crate& crate, Newval& nvs):
        root_span(sp),
        resolve(crate),
        nvs( nvs )
        , eval_index(s_next_eval_index++)
        , num_frames(0)
    {
    }
    Evaluator(Evaluator&& ) = default;
    Evaluator(const Evaluator& ) = delete;

    EncodedLiteral evaluate_constant(const ::HIR::ItemPath& ip, const ::HIR::ExprPtr& expr, ::HIR::TypeRef exp, MonomorphState ms={});

    StaticTraitResolve& get_resolve() { return this->resolve; }

private:
    void push_stack_entry(
        ::FmtLambda print_path, const ::MIR::Function& fcn, MonomorphState ms,
        ::HIR::TypeRef exp, ::HIR::Function::args_t arg_defs,
        ::std::vector<::MIR::eval::AllocationPtr> args,
        const ::HIR::GenericParams* item_params_def,
        const ::HIR::GenericParams* impl_params_def
        );

    ::MIR::eval::AllocationPtr run_until_stack_empty();
    void run_statement(::MIR::eval::CallStackEntry& local_state, const ::MIR::Statement& stmt);
    // Returns UINT_MAX on return
    unsigned run_terminator(::MIR::eval::CallStackEntry& local_state, const ::MIR::Terminator& stmt);

    EncodedLiteral allocation_to_encoded(const ::HIR::TypeRef& ty, const ::MIR::eval::Allocation& a);
};

} // namespace HIR

