/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * include/cpp_unpack.hpp
 * - Macro that performs variadic unpacking and calls a function/macro with the unpacked values
 */
#ifndef _CPP_UNAPCK_H_
#define _CPP_UNAPCK_H_

#define CC_EXP(...)  __VA_ARGS__

// - Calls the passed function, expanding 'a' but leaving _1 as a single argument
#define CC_CALL_A(__fcn, __args)   __fcn __args
#define CC_CALL_A1(f, a, _1)   CC_CALL_A(f, (CC_EXP a, _1))
#define CC_CALL_A2(f, a, _1, _2)   CC_CALL_A(f, (CC_EXP a, _1)) CC_CALL_A(f, (CC_EXP a, _2))
#define CC_CALL_A3(f, a, _1, _2, _3) CC_CALL_A(f, (CC_EXP a, _1)) CC_CALL_A(f, (CC_EXP a, _2)) CC_CALL_A(f, (CC_EXP a, _3))

#define CC_CALL_A4(fn, a, a1,a2, b1,b2)       CC_CALL_A2(fn,a, a1,a2)    CC_CALL_A2(fn,a, b1,b2)
#define CC_CALL_A5(fn, a, a1,a2,a3, b1,b2)    CC_CALL_A3(fn,a, a1,a2,a3) CC_CALL_A2(fn,a, b1,b2)
#define CC_CALL_A6(fn, a, a1,a2,a3, b1,b2,b3) CC_CALL_A3(fn,a, a1,a2,a3) CC_CALL_A3(fn,a, b1,b2,b3)
#define CC_CALL_A7(fn, a, a1,a2,a3, b1,b2, c1,c2) CC_CALL_A3(fn,a, a1,a2,a3) CC_CALL_A2(fn,a, b1,b2) CC_CALL_A2(fn,a, c1,c2)
#define CC_CALL_A8(fn, a, a1,a2,a3, b1,b2,b3, c1,c2) CC_CALL_A3(fn,a, a1,a2,a3) CC_CALL_A3(fn,a, b1,b2,b3) CC_CALL_A2(fn,a, c1,c2)
#define CC_CALL_A9(fn, a, a1,a2,a3, b1,b2,b3, c1,c2,c3) CC_CALL_A3(fn,a, a1,a2,a3) CC_CALL_A3(fn,a, b1,b2,b3) CC_CALL_A3(fn,a, c1,c2,c3)
#define CC_CALL_A10(f, a, a1,a2,a3, b1,b2,b3, c1,c2,c3, d1) CC_CALL_A3(f,a, a1,a2,a3) CC_CALL_A3(f,a, b1,b2,b3) CC_CALL_A3(f,a, c1,c2,c3) CC_CALL_A(f, (CC_EXP a, CC_EXP d1))
#define CC_CALL_A11(f, a, a1,a2,a3, b1,b2,b3, c1,c2,c3, d1,d2) CC_CALL_A3(f,a, a1,a2,a3) CC_CALL_A3(f,a, b1,b2,b3) CC_CALL_A3(f,a, c1,c2,c3) CC_CALL_A2(f,a, d1,d2)
#define CC_CALL_A12(f, a, a1,a2,a3, b1,b2,b3, c1,c2,c3, d1,d2,d3) CC_CALL_A3(f,a, a1,a2,a3) CC_CALL_A3(f,a, b1,b2,b3) CC_CALL_A3(f,a, c1,c2,c3) CC_CALL_A3(f,a, d1,d2,d3)
#define CC_CALL_A13(f, a, a1,a2,a3,a4, b1,b2,b3, c1,c2,c3, d1,d2,d3) CC_CALL_A4(f,a, a1,a2,a3,a4) CC_CALL_A3(f,a, b1,b2,b3) CC_CALL_A3(f,a, c1,c2,c3) CC_CALL_A3(f,a, d1,d2,d3)
#define CC_CALL_A14(f, a, a1,a2,a3,a4, b1,b2,b3,b4, c1,c2,c3, d1,d2,d3) CC_CALL_A4(f,a, a1,a2,a3,a4) CC_CALL_A4(f,a, b1,b2,b3,b4) CC_CALL_A3(f,a, c1,c2,c3) CC_CALL_A3(f,a, d1,d2,d3)

// Macro to obtain a numbered macro for argument counts
// - Raw variant
#define CC_GM_I(SUF,_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,COUNT,...) SUF##COUNT
#define CC_GM(SUF,...) CC_EXP( CC_GM_I(SUF,__VA_ARGS__,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1) )

#define CC_ITERATE(fcn, args, ...)  CC_EXP( CC_GM(CC_CALL_A, __VA_ARGS__)(fcn, args, __VA_ARGS__) )

#endif

