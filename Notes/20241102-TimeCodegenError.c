// gcc "-ffunction-sections" "-pthread" "-O2" "-fPIC" -c "Notes/20241102-TimeCodegenError.c"
// *(uint32_t*)0x7ffe4077d044
// 0x7ffe4077d040	+00+1	-> Tag, set to 2
// 0x7ffe4077d044	+04	-> ? Unset
// 0x7ffe4077d048	+08+8	-> literal 0xffffffffffe28909
// 0x7ffe4077d050	+10+8	-> literal 0x51fe2c
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef uint8_t RUST_BOOL;
typedef struct { void* PTR; size_t META; } SLICE_PTR;


struct s_ZRG3cE9core0_0_03num7nonzero10NonZeroI320g  {
        /*@0*/int32_t _0; // i32
} ;

struct s_ZRG2cE18time0_3_29_H40000d4date4Date0g  {
        /*@0*/struct s_ZRG3cE9core0_0_03num7nonzero10NonZeroI320g _0; // ::"core-0_0_0"::num::nonzero::NonZeroI32/*S*/
} ;


struct s_ZRG2cE9core0_0_06resultG8ResultOk2gG2cE18time0_3_29_H40000d4date4Date0gG3c_D5error15component_range14ComponentRange0g  {
        /*@0*/uint8_t _1; // u8
        /*@4*/struct s_ZRG2cE18time0_3_29_H40000d4date4Date0g _0; // ::"time-0_3_29_H40000d"::date::Date/*S*/
} ;

struct s_ZRG3cE18time0_3_29_H40000d5error15component_range14ComponentRange0g  {
        /*@0*/RUST_BOOL _4; // bool
        /*@8*/int64_t _1; // i64
        /*@16*/int64_t _2; // i64
        /*@24*/int64_t _3; // i64
        /*@32*/SLICE_PTR _0; // &'static str
} ;

struct s_ZRG2cE9core0_0_06resultG9ResultErr2gG2cE18time0_3_29_H40000d4date4Date0gG3c_D5error15component_range14ComponentRange0g  {
        /*@0*/struct s_ZRG3cE18time0_3_29_H40000d5error15component_range14ComponentRange0g _0; // ::"time-0_3_29_H40000d"::error::component_range::ComponentRange/*S*/
} ;

struct e_ZRG2cE9core0_0_06result6Result2gG2cE18time0_3_29_H40000d4date4Date0gG3c_D5error15component_range14ComponentRange0g {
        union {
                struct s_ZRG2cE9core0_0_06resultG8ResultOk2gG2cE18time0_3_29_H40000d4date4Date0gG3c_D5error15component_range14ComponentRange0g var_0;
                struct s_ZRG2cE9core0_0_06resultG9ResultErr2gG2cE18time0_3_29_H40000d4date4Date0gG3c_D5error15component_range14ComponentRange0g var_1;
        } DATA;
};

#if 0

struct s_ZRG2cI17deranged0_3_8_H8110ranged_i329RangedI320v2gV4_0989e2ffV4_2cfe5100  {
        /*@0*/int32_t _0; // i32
} ;

struct s_ZRG2cE9core0_0_06optionG10OptionSome1gG2cI17deranged0_3_8_H8110ranged_i329RangedI320v2gV4_0989e2ffV4_2cfe5100  {
        /*@0*/uint8_t _1; // u8
        /*@4*/struct s_ZRG2cI17deranged0_3_8_H8110ranged_i329RangedI320v2gV4_0989e2ffV4_2cfe5100 _0; // ::"deranged-0_3_8_H81"::ranged_i32::RangedI32<{Evaluated(0989E2FF{})},{Evaluated(2CFE5100{})},>/*S*/
} ;

struct e_ZRG2cE9core0_0_06option6Option1gG2cI17deranged0_3_8_H8110ranged_i329RangedI320v2gV4_0989e2ffV4_2cfe5100 {
        union {
                // ZST: ()
                struct s_ZRG2cE9core0_0_06optionG10OptionSome1gG2cI17deranged0_3_8_H8110ranged_i329RangedI320v2gV4_0989e2ffV4_2cfe5100 var_1;
                uint8_t TAG;
        } DATA;
};

// <::"deranged-0_3_8_H81"::ranged_i32::RangedI32<{Evaluated(0989E2FF{})},{Evaluated(2CFE5100{})},>/*S*/ /*- <{Evaluated(0989E2FF{})},{Evaluated(2CFE5100{})},>*/>::new
struct e_ZRG2cE9core0_0_06option6Option1gG2cI17deranged0_3_8_H8110ranged_i329RangedI320v2gV4_0989e2ffV4_2cfe5100  ZRIG2cI17deranged0_3_8_H8110ranged_i329RangedI320v2gV4_0989e2ffV4_2cfe51003new0g(
                int32_t arg0 // i32
                );
#else
bool ZRIG2cI17deranged0_3_8_H8110ranged_i329RangedI320v2gV4_0989e2ffV4_2cfe51003new0g_OP(int32_t arg0);
#endif

struct s_ZRG2cE18time0_3_29_H40000d4date4Date0g  ZRIG2cE18time0_3_29_H40000d4date4Date0g25from_julian_day_unchecked0g(
                int32_t arg0 // i32
                ); // -> ::"time-0_3_29_H40000d"::date::Date/*S*/

// <::"time-0_3_29_H40000d"::date::Date/*S*/ /*- */>::from_julian_day
struct e_ZRG2cE9core0_0_06result6Result2gG2cE18time0_3_29_H40000d4date4Date0gG3c_D5error15component_range14ComponentRange0g  ZRIG2cE18time0_3_29_H40000d4date4Date0g15from_julian_day0g(
                int32_t arg0 // i32
                ) // -> ::"core-0_0_0"::result::Result<::"time-0_3_29_H40000d"::date::Date/*S*/,::"time-0_3_29_H40000d"::error::component_range::ComponentRange/*S*/,>/*E*/

{
        struct e_ZRG2cE9core0_0_06result6Result2gG2cE18time0_3_29_H40000d4date4Date0gG3c_D5error15component_range14ComponentRange0g rv;
        struct s_ZRG3cE18time0_3_29_H40000d5error15component_range14ComponentRange0g var6 = {0};
        struct s_ZRG2cE18time0_3_29_H40000d4date4Date0g var7;

        //struct e_ZRG2cE9core0_0_06option6Option1gG2cI17deranged0_3_8_H8110ranged_i329RangedI320v2gV4_0989e2ffV4_2cfe5100 var0; 
        //var0 = ZRIG2cI17deranged0_3_8_H8110ranged_i329RangedI320v2gV4_0989e2ffV4_2cfe51003new0g( arg0 );
        //if( var0.DATA.TAG == 0 ) {
        if( ZRIG2cI17deranged0_3_8_H8110ranged_i329RangedI320v2gV4_0989e2ffV4_2cfe51003new0g_OP(arg0) ) {
		rv.DATA.var_1._0 = var6;
		return rv;
	}
	else {
		var7 = ZRIG2cE18time0_3_29_H40000d4date4Date0g25from_julian_day_unchecked0g( arg0 );
		rv.DATA.var_0._1 = 2;
		rv.DATA.var_0._0 = var7;
		return rv;
	}
}

