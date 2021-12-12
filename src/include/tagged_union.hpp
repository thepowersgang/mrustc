/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * include/tagged_union.hpp
 * - Macro that allows construction of a tagged union (with various helper methods)
 *
 *
 * The most evil CPP abuse I have ever written
 *
 * Constructs a tagged union that correctly handles objects.
 *
 * Union is NOT copy-constructable
 */
#ifndef INCLUDED_TAGGED_UNION_H_
#define INCLUDED_TAGGED_UNION_H_

//#include "cpp_unpack.h"
#include <cassert>
#include <string>

#define TU_FIRST(a, ...)    a
#define TU_EXP1(x)  x
#define TU_EXP(...)  __VA_ARGS__

#define TU_CASE_ITEM(src, mod, var, name)   mod auto& name = src.as_##var(); (void)&name;
#define TU_CASE_BODY(class,var, ...)    case class::var: { __VA_ARGS__ } break;
#define TU_CASE(mod, class, var,  name,src, ...)    TU_CASE_BODY(mod,class,var, TU_CASE_ITEM(src,mod,var,name) __VA_ARGS__)
#define TU_CASE2(mod, class, var,  n1,s1, n2,s2, ...)   TU_CASE_BODY(mod,class,var, TU_CASE_ITEM(s1,mod,var,n1) TU_CASE_ITEM(s2,mod,var,n2) __VA_ARGS__)

// Argument iteration
#define TU_DISP0(n)
#define TU_DISP1(n, _1)   n _1
#define TU_DISP2(n, _1, _2)   n _1 n _2
#define TU_DISP3(n, v, v2, v3)   n v n v2 n v3
#define TU_DISP4(n, v, v2, v3, v4)   n v n v2 n v3 n v4
#define TU_DISP5(n, a1,a2,a3, b1,b2   )   TU_DISP3(n, a1,a2,a3) TU_DISP2(n, b1,b2)
#define TU_DISP6(n, a1,a2,a3, b1,b2,b3)   TU_DISP3(n, a1,a2,a3) TU_DISP3(n, b1,b2,b3)
#define TU_DISP7(n, a1,a2,a3,a4, b1,b2,b3   )   TU_DISP4(n, a1,a2,a3,a4) TU_DISP3(n, b1,b2,b3)
#define TU_DISP8(n, a1,a2,a3,a4, b1,b2,b3,b4)   TU_DISP4(n, a1,a2,a3,a4) TU_DISP4(n, b1,b2,b3,b4)
#define TU_DISP9(n, a1,a2,a3,a4, b1,b2,b3,b4, c1          )   TU_DISP4(n, a1,a2,a3,a4) TU_DISP3(n, b1,b2,b3   ) TU_DISP2(n, b4,c1)
#define TU_DISP10(n, a1,a2,a3,a4, b1,b2,b3,b4, c1,c2      )   TU_DISP4(n, a1,a2,a3,a4) TU_DISP4(n, b1,b2,b3,b4) TU_DISP2(n, c1,c2)
#define TU_DISP11(n, a1,a2,a3,a4, b1,b2,b3,b4, c1,c2,c3   )   TU_DISP4(n, a1,a2,a3,a4) TU_DISP4(n, b1,b2,b3,b4) TU_DISP3(n, c1,c2,c3)
#define TU_DISP12(n, a1,a2,a3,a4, b1,b2,b3,b4, c1,c2,c3,c4)   TU_DISP4(n, a1,a2,a3,a4) TU_DISP4(n, b1,b2,b3,b4) TU_DISP4(n, c1,c2,c3,c4)
#define TU_DISP13(n, a1,a2,a3,a4,a5, b1,b2,b3,b4, c1,c2,c3,c4)   TU_DISP5(n, a1,a2,a3,a4,a5) TU_DISP4(n, b1,b2,b3,b4) TU_DISP4(n, c1,c2,c3,c4)
#define TU_DISP14(n, a1,a2,a3,a4,a5, b1,b2,b3,b4,b5, c1,c2,c3,c4       )   TU_DISP5(n, a1,a2,a3,a4,a5) TU_DISP5(n, b1,b2,b3,b4,b5) TU_DISP4(n, c1,c2,c3,c4)
#define TU_DISP15(n, a1,a2,a3,a4,a5, b1,b2,b3,b4,b5, c1,c2,c3,c4,c5    )   TU_DISP5(n, a1,a2,a3,a4,a5) TU_DISP5(n, b1,b2,b3,b4,b5) TU_DISP5(n, c1,c2,c3,c4,c5)
#define TU_DISP16(n, a1,a2,a3,a4,a5, b1,b2,b3,b4,b5, c1,c2,c3,c4,c5, d1)   TU_DISP5(n, a1,a2,a3,a4,a5) TU_DISP5(n, b1,b2,b3,b4,b5) TU_DISP5(n, c1,c2,c3,c4,c5) TU_DISP1(n, d1)
#define TU_DISP17(n, a1,a2,a3,a4,a5, b1,b2,b3,b4,b5, c1,c2,c3,c4,c5, d1,d2) TU_DISP5(n, a1,a2,a3,a4,a5) TU_DISP5(n, b1,b2,b3,b4,b5) TU_DISP5(n, c1,c2,c3,c4,c5) TU_DISP2(n, d1,d2)

#define TU_DISPO0(n)
#define TU_DISPO1(n, _1)   n(_1)
#define TU_DISPO2(n, _1, _2)   n(_1) n(_2)
#define TU_DISPO3(n, v, v2, v3)   n(v) n(v2) n(v3)
#define TU_DISPO4(n, v, v2, v3, v4)   n(v) n(v2) n(v3) n(v4)
#define TU_DISPO5(n, a1,a2,a3, b1,b2   )   TU_DISPO3(n, a1,a2,a3) TU_DISPO2(n, b1,b2)
#define TU_DISPO6(n, a1,a2,a3, b1,b2,b3)   TU_DISPO3(n, a1,a2,a3) TU_DISPO3(n, b1,b2,b3)
#define TU_DISPO7(n, a1,a2,a3,a4, b1,b2,b3   )  TU_DISPO4(n, a1,a2,a3,a4) TU_DISPO3(n, b1,b2,b3)
#define TU_DISPO8(n, a1,a2,a3,a4, b1,b2,b3,b4)  TU_DISPO4(n, a1,a2,a3,a4) TU_DISPO4(n, b1,b2,b3,b4)
#define TU_DISPO9(n, a1,a2,a3,a4, b1,b2,b3,b4, c1)   TU_DISPO4(n, a1,a2,a3,a4) TU_DISPO3(n, b1,b2,b3)  TU_DISPO2(n, b4,c1)
#define TU_DISPO10(n, a1,a2,a3,a4, b1,b2,b3,b4, c1,c2)   TU_DISPO4(n, a1,a2,a3,a4) TU_DISPO4(n, b1,b2,b3,b4) TU_DISPO2(n, c1,c2)
#define TU_DISPO11(n, a1,a2,a3,a4, b1,b2,b3,b4, c1,c2,c3)   TU_DISPO4(n, a1,a2,a3,a4) TU_DISPO4(n, b1,b2,b3,b4) TU_DISPO3(n, c1,c2,c3)
#define TU_DISPO12(n, a1,a2,a3,a4, b1,b2,b3,b4, c1,c2,c3,c4)   TU_DISPO4(n, a1,a2,a3,a4) TU_DISPO4(n, b1,b2,b3,b4) TU_DISPO4(n, c1,c2,c3,c4)
#define TU_DISPO13(n, a1,a2,a3,a4,a5, b1,b2,b3,b4, c1,c2,c3,c4)   TU_DISPO5(n, a1,a2,a3,a4,a5) TU_DISPO4(n, b1,b2,b3,b4) TU_DISPO4(n, c1,c2,c3,c4)
#define TU_DISPO14(n, a1,a2,a3,a4,a5, b1,b2,b3,b4,b5, c1,c2,c3,c4)   TU_DISPO5(n, a1,a2,a3,a4,a5) TU_DISPO5(n, b1,b2,b3,b4,b5) TU_DISPO4(n, c1,c2,c3,c4)
#define TU_DISPO15(n, a1,a2,a3,a4,a5, b1,b2,b3,b4,b5, c1,c2,c3,c4,c5)   TU_DISPO5(n, a1,a2,a3,a4,a5) TU_DISPO5(n, b1,b2,b3,b4,b5) TU_DISPO5(n, c1,c2,c3,c4,c5)
#define TU_DISPO16(n, a1,a2,a3,a4,a5, b1,b2,b3,b4,b5, c1,c2,c3,c4,c5, d1)   TU_DISPO5(n, a1,a2,a3,a4,a5) TU_DISPO5(n, b1,b2,b3,b4,b5) TU_DISPO5(n, c1,c2,c3,c4,c5) TU_DISPO1(n, d1)
#define TU_DISPO17(n, a1,a2,a3,a4,a5, b1,b2,b3,b4,b5, c1,c2,c3,c4,c5, d1,d2) TU_DISPO5(n, a1,a2,a3,a4,a5) TU_DISPO5(n, b1,b2,b3,b4,b5) TU_DISPO5(n, c1,c2,c3,c4,c5) TU_DISPO2(n, d1,d2)

#define TU_DISPA(n, a)   n a
#define TU_DISPA1(n, a, _1)   TU_DISPA(n, (TU_EXP a, TU_EXP _1))
#define TU_DISPA2(n, a, _1, _2)     TU_DISPA(n, (TU_EXP a, TU_EXP _1)) TU_DISPA(n, (TU_EXP a, TU_EXP _2))
#define TU_DISPA3(n, a, _1, _2, _3) TU_DISPA(n, (TU_EXP a, TU_EXP _1)) TU_DISPA(n, (TU_EXP a, TU_EXP _2)) TU_DISPA(n, (TU_EXP a, TU_EXP _3))
#define TU_DISPA4(n, a, a1,a2, b1,b2)     TU_DISPA2(n,a, a1,a2)    TU_DISPA2(n,a, b1,b2)
#define TU_DISPA5(n, a, a1,a2,a3, b1,b2)    TU_DISPA3(n,a, a1,a2,a3) TU_DISPA2(n,a, b1,b2)
#define TU_DISPA6(n, a, a1,a2,a3, b1,b2,b3) TU_DISPA3(n,a, a1,a2,a3) TU_DISPA3(n,a, b1,b2,b3)
#define TU_DISPA7(n, a, a1,a2,a3, b1,b2, c1,c2) TU_DISPA3(n,a, a1,a2,a3) TU_DISPA2(n,a, b1,b2) TU_DISPA2(n,a, c1,c2)
#define TU_DISPA8(n, a, a1,a2,a3, b1,b2,b3, c1,c2) TU_DISPA3(n,a, a1,a2,a3) TU_DISPA3(n,a, b1,b2,b3) TU_DISPA2(n,a, c1,c2)
#define TU_DISPA9(n, a, a1,a2,a3, b1,b2,b3, c1,c2,c3) TU_DISPA3(n,a, a1,a2,a3) TU_DISPA3(n,a, b1,b2,b3) TU_DISPA3(n,a, c1,c2,c3)
#define TU_DISPA10(n, a, a1,a2,a3, b1,b2,b3, c1,c2,c3, d1) TU_DISPA3(n,a, a1,a2,a3) TU_DISPA3(n,a, b1,b2,b3) TU_DISPA3(n,a, c1,c2,c3) TU_DISPA(n, (TU_EXP a, TU_EXP d1))
#define TU_DISPA11(n, a, a1,a2,a3, b1,b2,b3, c1,c2,c3, d1,d2) TU_DISPA3(n,a, a1,a2,a3) TU_DISPA3(n,a, b1,b2,b3) TU_DISPA3(n,a, c1,c2,c3) TU_DISPA2(n,a, d1,d2)
#define TU_DISPA12(n, a, a1,a2,a3, b1,b2,b3, c1,c2,c3, d1,d2,d3) TU_DISPA3(n,a, a1,a2,a3) TU_DISPA3(n,a, b1,b2,b3) TU_DISPA3(n,a, c1,c2,c3) TU_DISPA3(n,a, d1,d2,d3)
#define TU_DISPA13(n, a, a1,a2,a3,a4, b1,b2,b3, c1,c2,c3, d1,d2,d3) TU_DISPA4(n,a, a1,a2,a3,a4) TU_DISPA3(n,a, b1,b2,b3) TU_DISPA3(n,a, c1,c2,c3) TU_DISPA3(n,a, d1,d2,d3)
#define TU_DISPA14(n, a, a1,a2,a3,a4, b1,b2,b3,b4, c1,c2,c3, d1,d2,d3) TU_DISPA4(n,a, a1,a2,a3,a4) TU_DISPA4(n,a, b1,b2,b3,b4) TU_DISPA3(n,a, c1,c2,c3) TU_DISPA3(n,a, d1,d2,d3)
#define TU_DISPA15(n, a, a1,a2,a3,a4, b1,b2,b3,b4, c1,c2,c3,c4, d1,d2,d3) TU_DISPA4(n,a, a1,a2,a3,a4) TU_DISPA4(n,a, b1,b2,b3,b4) TU_DISPA4(n,a, c1,c2,c3,c4) TU_DISPA3(n,a, d1,d2,d3)
#define TU_DISPA16(n, a, a1,a2,a3,a4, b1,b2,b3,b4, c1,c2,c3,c4, d1,d2,d3,d4) TU_DISPA4(n,a, a1,a2,a3,a4) TU_DISPA4(n,a, b1,b2,b3,b4) TU_DISPA4(n,a, c1,c2,c3,c4) TU_DISPA4(n,a, d1,d2,d3,d4)
#define TU_DISPA17(n, a, a1,a2,a3,a4, b1,b2,b3,b4, c1,c2,c3,c4, d1,d2,d3,d4, e1) TU_DISPA4(n,a, a1,a2,a3,a4) TU_DISPA4(n,a, b1,b2,b3,b4) TU_DISPA4(n,a, c1,c2,c3,c4) TU_DISPA4(n,a, d1,d2,d3,d4) TU_DISPA1(n,a, e1)

// Macro to obtain a numbered macro for argument counts
// - Raw variant
#define TU_GM_I(SUF,_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,_17,COUNT,...) SUF##COUNT
#define TU_GM(SUF,...) TU_EXP1( TU_GM_I(SUF, __VA_ARGS__,17,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0) )
// - _DISP based variant (for iteration)
#define TU_GMX(...) TU_EXP1( TU_GM(TU_DISP, __VA_ARGS__) )
#define TU_GMO(...) TU_EXP1( TU_GM(TU_DISPO, __VA_ARGS__) )
#define TU_GMA(...) TU_EXP1( TU_GM(TU_DISPA, __VA_ARGS__) )

// TODO: use `decltype` in place of the `class` argument to TU_MATCH/TU_IFLET
// "match"-like statement
// TU_MATCH(Class, m_data, ent, (Variant, CODE), (Variant2, CODE))
#define TU_MATCHA(VARS, NAMES, ...) TU_MATCH( ::std::remove_reference<decltype(TU_FIRST VARS)>::type, VARS, NAMES, __VA_ARGS__ )
#define TU_MATCH(CLASS, VAR, NAME, ...)   switch( (TU_FIRST VAR).tag()) {/*
*/    case CLASS::TAGDEAD: assert(!"ERROR: destructed tagged union used");/*
*/    TU_MATCH_ARMS(CLASS, VAR, NAME, __VA_ARGS__)/*
*/}
#define TU_MATCH_DEF(CLASS, VAR, NAME, DEF, ...)   switch( (TU_FIRST VAR).tag()) {/*
*/    case CLASS::TAGDEAD: assert(!"ERROR: destructed tagged union used");/*
*/    TU_MATCH_ARMS(CLASS, VAR, NAME, __VA_ARGS__)/*
*/    default: {TU_EXP DEF;} break;/*
*/}
#define TU_MATCH_BIND1(TAG, VAR, NAME)  /*MATCH_BIND*/ decltype((VAR).as_##TAG()) NAME = (VAR).as_##TAG(); (void)&NAME;
#define TU_MATCH_BIND2_(TAG, v1,v2, n1,n2)   TU_MATCH_BIND1(TAG, v1, n1) TU_MATCH_BIND1(TAG, v2, n2)
#define TU_MATCH_BIND2(...) TU_EXP1( TU_MATCH_BIND2_(__VA_ARGS__) )    // << Exists to cause expansion of the vars
#define TU_MATCH_ARM(CLASS, VAR, NAME, TAG, ...)  case CLASS::TAG_##TAG: {/*
*/    TU_GM(TU_MATCH_BIND, TU_EXP VAR)(TAG, TU_EXP VAR , TU_EXP NAME)/*
*/    __VA_ARGS__/*
*/} break;
#define TU_MATCH_ARMS(CLASS, VAR, NAME, ...)    TU_EXP1( TU_GMA(__VA_ARGS__)(TU_MATCH_ARM, (CLASS, VAR, NAME), __VA_ARGS__) )

#define TU_IFLET(CLASS, VAR, TAG, NAME, ...) if((VAR).tag() == CLASS::TAG_##TAG) { auto& NAME = (VAR).as_##TAG(); (void)&NAME; __VA_ARGS__ }

#define TU_MATCH_HDR(VARS, brace)  TU_MATCH_HDR_(::std::remove_reference<decltype(TU_FIRST VARS)>::type, VARS, brace)
#define TU_MATCH_HDR_(CLASS, VARS, brace)  switch( (TU_FIRST VARS).tag() ) brace case CLASS::TAGDEAD: assert(!"ERROR: destructed tagged union used");
// Evil hack: two for loops, the inner stops the outer after it's done.
#define TU_ARM(VAR, TAG, NAME)  break; case ::std::remove_reference<decltype(VAR)>::type::TAG_##TAG: for(bool tu_lc = true; tu_lc; tu_lc=false) for(decltype((VAR).as_##TAG()) NAME = (VAR).as_##TAG(); (void)NAME, tu_lc; tu_lc=false)

#define TU_MATCH_HDRA(VARS, brace)  TU_MATCH_HDRA_(::std::remove_reference<decltype(TU_FIRST VARS)>::type, VARS, brace)
#define TU_MATCH_HDRA_(CLASS, VARS, brace)  /*
    */for(bool tu_lc = true; tu_lc; tu_lc=false) for(TU_EXP1(TU_MATCH_HDRA_Decl VARS); tu_lc; tu_lc=false) /*
        */switch( tu_match_hdr2_v.tag() ) brace /*
        */case CLASS::TAGDEAD: assert(!"ERROR: destructed tagged union used");
#define TU_MATCH_HDRA_DeclRest1(v1) &tu_match_hdr2_v = v1
#define TU_MATCH_HDRA_DeclRest2(v1, v2) TU_MATCH_HDRA_DeclRest1(v1), &tu_match_hdr2_v2 = v2
#define TU_MATCH_HDRA_DeclRest3(v1, v2, v3) TU_MATCH_HDRA_DeclRest2(_, v2), &tu_match_hdr2_v3 = v3
#define TU_MATCH_HDRA_Decl(...)   auto TU_EXP1( TU_GM(TU_MATCH_HDRA_DeclRest, __VA_ARGS__)(__VA_ARGS__) )
#define TU_ARMA_DeclInner1(TAG, v1)   v1 = tu_match_hdr2_v.as_##TAG()
#define TU_ARMA_DeclInner2(TAG, v1, v2)   TU_ARMA_DeclInner1(TAG, v1), v2 = tu_match_hdr2_v2.as_##TAG()
#define TU_ARMA_DeclInner3(TAG, v1, v2, v3)   TU_ARMA_DeclInner1(TAG, v1, v2), v3 = tu_match_hdr2_v3.as_##TAG()
#define TU_ARMA_Decl(TAG, ...)   decltype(tu_match_hdr2_v.as_##TAG()) TU_EXP1( TU_GM(TU_ARMA_DeclInner, __VA_ARGS__)(TAG, __VA_ARGS__) )
#define TU_ARMA_IgnVal(v)   (void)v,
// Evil hack: two for loops, the inner stops the outer after it's done.
#define TU_ARMA(TAG, ...)  break; case ::std::remove_reference<decltype(tu_match_hdr2_v)>::type::TAG_##TAG: /*
    */for(bool tu_lc = true; tu_lc; tu_lc=false) for(TU_ARMA_Decl(TAG, __VA_ARGS__); TU_EXP1( TU_GMO(__VA_ARGS__)(TU_ARMA_IgnVal, __VA_ARGS__) ) tu_lc; tu_lc=false)

//#define TU_TEST(VAL, ...)    (VAL.is_##TAG() && VAL.as_##TAG() TEST)
#define TU_TEST1(VAL, TAG1, TEST)    ((VAL).is_##TAG1() && (VAL).as_##TAG1() TEST)
#define TU_TEST2(VAL, TAG1, FLD1,TAG2, TEST)    ((VAL).is_##TAG1() && (VAL).as_##TAG1() FLD1.is_##TAG2() && (VAL).as_##TAG1() FLD1.as_##TAG2() TEST)


#define TU_DATANAME(name)   Data_##name
// Internals of TU_CONS
#define TU_CONS_I(__name, __tag, __type) \
    __name(__type v): m_tag(TAG_##__tag) { new (&m_data.__tag) __type( ::std::move(v) ); } \
    static self_t make_##__tag(__type v) { return __name( ::std::move(v) ); }\
    bool is_##__tag() const { return m_tag == TAG_##__tag; } \
    const __type* opt_##__tag() const { if(m_tag == TAG_##__tag) return &m_data.__tag; return nullptr; } \
          __type* opt_##__tag()       { if(m_tag == TAG_##__tag) return &m_data.__tag; return nullptr; } \
    const __type& as_##__tag() const { assert(m_tag == TAG_##__tag); return m_data.__tag; } \
          __type& as_##__tag()       { assert(m_tag == TAG_##__tag); return m_data.__tag; } \
    __type unwrap_##__tag() { return ::std::move(this->as_##__tag()); } \
// Define a tagged union constructor
#define TU_CONS(__name, name, ...) TU_CONS_I(__name, name, TU_DATANAME(name))

// Type definitions_
#define TU_TYPEDEF(name, ...)    typedef __VA_ARGS__ TU_DATANAME(name);/*
*/

#define TU_TAG(name, ...)  TAG_##name,

// Destructor internals
#define TU_DEST_CASE(tag, ...)  case TAG_##tag: TU_destruct_inplace(m_data.tag); break;/*
*/

// move constructor internals
#define TU_MOVE_CASE(tag, ...)  case TAG_##tag: new(&m_data.tag) TU_DATANAME(tag)( ::std::move(x.m_data.tag) ); break;/*
*/

// "tag_to_str" internals
#define TU_TOSTR_CASE(tag,...)    case TAG_##tag: return #tag;/*
*/
// "tag_from_str" internals
#define TU_FROMSTR_CASE(tag,...)    else if(str == #tag) return TAG_##tag;/*
*/
#define TU_FROMSTR_CASES(...) TU_EXP1( TU_GMX(__VA_ARGS__)(TU_FROMSTR_CASE,__VA_ARGS__) )

#define TU_UNION_FIELD(tag, ...)    TU_DATANAME(tag) tag;/*
*/
#define TU_UNION_FIELDS(...)    TU_EXP1( TU_GMX(__VA_ARGS__)(TU_UNION_FIELD,__VA_ARGS__) )

#define TU_CONSS(_name, ...) TU_EXP1( TU_GMA(__VA_ARGS__)(TU_CONS, (_name), __VA_ARGS__) )
#define TU_TYPEDEFS(...)     TU_EXP1( TU_GMX(__VA_ARGS__)(TU_TYPEDEF   ,__VA_ARGS__) )
#define TU_TAGS(...)         TU_EXP1( TU_GMX(__VA_ARGS__)(TU_TAG       ,__VA_ARGS__) )
#define TU_DEST_CASES(...)   TU_EXP1( TU_GMX(__VA_ARGS__)(TU_DEST_CASE ,__VA_ARGS__) )
#define TU_MOVE_CASES(...)   TU_EXP1( TU_GMX(__VA_ARGS__)(TU_MOVE_CASE ,__VA_ARGS__) )
#define TU_TOSTR_CASES(...)  TU_EXP1( TU_GMX(__VA_ARGS__)(TU_TOSTR_CASE,__VA_ARGS__) )

/**
 * Define a new tagged union
 *
 * ```
 * TAGGED_UNION(Inner, Any,
 *     (Any, (struct { bool match_multiple; })),
 *     (Tuple, (::std::vector<Pattern> )),
 *     (TupleStruct, (struct { Path path; ::std::vector<Pattern> sub_patterns; })),
 *     (Value, (::std::unique_ptr<ExprNode> )),
 *     (Range, (struct { ::std::unique_ptr<ExprNode> left; ::std::unique_ptr<ExprNode> right; }))
 *     );
 * ```
 */
#define TAGGED_UNION(_name, _def, ...)  TU_EXP1( TAGGED_UNION_EX(_name, (), _def, (TU_EXP(__VA_ARGS__)), (), (), ()) )
#define TAGGED_UNION_EX(_name, _inherit, _def, _variants, _extra_move, _extra_assign, _extra) \
class _name TU_EXP _inherit { \
    typedef _name self_t;/*
*/public:\
    TU_TYPEDEFS _variants/*
*/  enum Tag { \
        TAGDEAD, \
        TU_TAGS _variants\
    };/*
*/ private:\
    Tag m_tag; \
    union DataUnion { TU_UNION_FIELDS _variants DataUnion() {} ~DataUnion() {} } m_data;/*
*/ public:\
    _name(): m_tag(TAG_##_def) { new (&m_data._def) TU_DATANAME(_def)(); }/*
*/  _name(const _name&) = delete;/*
*/  _name(_name&& x) noexcept: m_tag(x.m_tag) TU_EXP _extra_move { switch(m_tag) { case TAGDEAD: break; TU_MOVE_CASES _variants } x.m_tag = TAGDEAD; }/*
*/  _name& operator =(_name&& x) { switch(m_tag) { case TAGDEAD: break; TU_DEST_CASES _variants } m_tag = x.m_tag; TU_EXP _extra_assign switch(m_tag) { case TAGDEAD: break; TU_MOVE_CASES _variants }; return *this; }/*
*/  ~_name() { switch(m_tag) { case TAGDEAD: break; TU_DEST_CASES _variants } m_tag = TAGDEAD; } \
    \
    Tag tag() const { return m_tag; }\
    const char* tag_str() const { return tag_to_str(m_tag); }\
    TU_CONSS(_name, TU_EXP _variants) \
/*
*/    static const char *tag_to_str(Tag tag) { \
        switch(tag) {/*
*/          case TAGDEAD: return "ERR:DEAD";/*
*/          TU_TOSTR_CASES _variants/*
*/      } return ""; \
    }/*
*/    static Tag tag_from_str(const ::std::string& str) { \
        if(0); /*
*/      TU_FROMSTR_CASES _variants/*
*/      else throw ::std::runtime_error("enum "#_name" No conversion"); \
    }\
    TU_EXP _extra\
}

/*
*/

namespace {
    template<typename T> static void TU_destruct_inplace(T& v) { v.~T(); }
}


#endif

