/*
 */
#include <hir/hir.hpp>

namespace MIR {
    namespace eval {
        class ValueRef;
    }
}

namespace HIR {

struct Evaluator
{
    class Newval
    {
    public:
        virtual ::HIR::Path new_static(::HIR::TypeRef type, ::HIR::Literal value) = 0;
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

    ::HIR::Literal evaluate_constant(const ::HIR::ItemPath& ip, const ::HIR::ExprPtr& expr, ::HIR::TypeRef exp, MonomorphState ms={});

private:
    ::MIR::eval::ValueRef evaluate_constant_mir(
        const ::HIR::ItemPath& ip, const ::MIR::Function& fcn, MonomorphState ms,
        ::HIR::TypeRef exp, const ::HIR::Function::args_t& arg_defs,
        ::std::vector<::MIR::eval::ValueRef> args);

    void replace_borrow_data(const HIR::TypeRef& ty, HIR::Literal& lit);
};

} // namespace HIR

