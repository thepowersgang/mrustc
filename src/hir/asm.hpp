/*
 */
#pragma once
#include <tagged_union.hpp>
#include <climits>

namespace AsmCommon {

    enum class Direction {
        In,
        Out,
        LateOut,
        InOut,
        InLateOut
    };
    static inline std::ostream& operator<<(std::ostream& os, const Direction& d) {
        switch(d)
        {
        case Direction::In:     return os << "in";
        case Direction::Out:    return os << "out";
        case Direction::LateOut:    return os << "lateout";
        case Direction::InOut:      return os << "inout";
        case Direction::InLateOut:  return os << "inlateout";
        }
        return os;
    }

    enum class RegisterClass {
        x86_reg,
        x86_reg_abcd,
        x86_reg_byte,
        x86_xmm,
        x86_ymm,
        x86_zmm,
        //x86_mm, // Requires
        x86_kreg,

        //aarch64_reg,
        //aarch64_vreg,
        
        //arm_reg,
        //arm_sreg,
        //arm_dreg,
        //arm_qreg,
        
        //mips_reg,
        //mips_freg,
        
        //nvptx_reg16,
        //nvptx_reg32,
        //nvptx_reg64,
        
        //riscv_reg,
        //riscv_freg,
        
        //hexagon_reg,
        
        //powerpc_reg,
        //powerpc_reg_nonzero,
        //powerpc_freg,
        
        //wasm32_local,
        
        //bpf_reg,
        //bpf_wreg,
    };

    TAGGED_UNION_EX(RegisterSpec, (), Explicit, (
        (Class, RegisterClass),
        (Explicit, std::string)
        ), (),(), (
            RegisterSpec clone() const {
                TU_MATCH_HDRA((*this),{)
                TU_ARMA(Class, e)   return e;
                TU_ARMA(Explicit, e)    return e;
                }
                throw "";
            }
        )
        );
    static inline bool operator==(const RegisterSpec& a, const RegisterSpec& b) {
        if(a.tag() != b.tag())
            return false;
        TU_MATCH_HDRA( (a,b), {)
        TU_ARMA(Class, ae,be)
            return ae == be;
        TU_ARMA(Explicit, ae,be)
            return ae == be;
        }
        return true;
    }
    static inline bool operator!=(const RegisterSpec& a, const RegisterSpec& b) {
        return !(a == b);
    }
    static inline const char* to_string(const RegisterClass& c) {
        switch(c)
        {
        case RegisterClass::x86_reg:    return "reg";
        case RegisterClass::x86_reg_abcd:   return "reg_abcd";
        case RegisterClass::x86_reg_byte:   return "reg_byte";
        case RegisterClass::x86_xmm:    return "xmm_reg";
        case RegisterClass::x86_ymm:    return "ymm_reg";
        case RegisterClass::x86_zmm:    return "zmm_reg";
        case RegisterClass::x86_kreg:   return "kreg";
        }
        throw "";
    }
    static inline std::ostream& operator<<(std::ostream& os, const RegisterSpec& s) {
        TU_MATCH_HDRA((s), {)
        TU_ARMA(Class, c) {
            os << to_string(c);
            }
        TU_ARMA(Explicit, e) {
            os << "\"" << e << "\"";
            }
        }
        return os;
    }

    struct LineFragment {
        std::string before;

        unsigned    index;
        char    modifier;
        
        LineFragment()
            : index(UINT_MAX)
            , modifier('\0')
        {
        }
        bool operator==(const LineFragment& x) const {
            return before == x.before
                && index == x.index
                && modifier == x.modifier
                ;
        }
    };
    struct Line {
        std::vector<LineFragment>   frags;
        std::string trailing;

        void fmt(std::ostream& os) const;
        bool operator==(const Line& x) const {
            return frags == x.frags && trailing == x.trailing;
        }
    };
    struct Options {
        unsigned pure : 1;
        unsigned nomem : 1;
        unsigned readonly : 1;
        unsigned preserves_flags : 1;
        unsigned noreturn : 1;
        unsigned nostack : 1;
        unsigned att_syntax : 1;
        Options()
            : pure(0)
            , nomem(0)
            , readonly(0)
            , preserves_flags(0)
            , noreturn(0)
            , nostack(0)
            , att_syntax(0)
        {
        }
        bool any() const {
            #define _(n)    if(n) return true
            _(pure);
            _(nomem);
            _(readonly);
            _(preserves_flags);
            _(noreturn);
            _(nostack);
            _(att_syntax);
            #undef _
            return false;
        }

        void fmt(std::ostream& os) const {
            os << "options(";
            #define _(n)    if(n) os << #n ","
            _(pure);
            _(nomem);
            _(readonly);
            _(preserves_flags);
            _(noreturn);
            _(nostack);
            _(att_syntax);
            #undef _
            os << ")";
        }
        bool operator==(const Options& x) const {
            #define _(n)    if(n != x.n)return false
            _(pure);
            _(nomem);
            _(readonly);
            _(preserves_flags);
            _(noreturn);
            _(nostack);
            _(att_syntax);
            #undef _
            return true;
        }
    };
}
