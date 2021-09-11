/*
 */
#include <hir/hir.hpp>

namespace MIR {
    namespace eval {
        class AllocationPtr;
        class Allocation;
    }
}

namespace HIR {

struct Evaluator
{
    class Newval
    {
    public:
        virtual ::HIR::Path new_static(::HIR::TypeRef type, EncodedLiteral value) = 0;
    };

    Span    root_span;
    StaticTraitResolve  resolve;
    Newval& nvs;

    Evaluator(const Span& sp, const ::HIR::Crate& crate, Newval& nvs):
        root_span(sp),
        resolve(crate),
        nvs( nvs )
    {
    }

    EncodedLiteral evaluate_constant(const ::HIR::ItemPath& ip, const ::HIR::ExprPtr& expr, ::HIR::TypeRef exp, MonomorphState ms={});

private:
    ::MIR::eval::AllocationPtr evaluate_constant_mir(
        const ::HIR::ItemPath& ip, const ::MIR::Function& fcn, MonomorphState ms,
        ::HIR::TypeRef exp, const ::HIR::Function::args_t& arg_defs,
        ::std::vector<::MIR::eval::AllocationPtr> args);

    EncodedLiteral allocation_to_encoded(const ::HIR::TypeRef& ty, const ::MIR::eval::Allocation& a);
};

} // namespace HIR

