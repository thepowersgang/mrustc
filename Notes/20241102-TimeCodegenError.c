// gcc "-ffunction-sections" "-pthread" "-O2" "-fPIC" -c "Notes/20241102-TimeCodegenError.c"
// FILED: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=117423
// *(uint32_t*)0x7ffe4077d044
// 0x7ffe4077d040	+00+1	-> Tag, set to 2
// 0x7ffe4077d044	+04	-> ? Unset
// 0x7ffe4077d048	+08+8	-> literal 0xffffffffffe28909
// 0x7ffe4077d050	+10+8	-> literal 0x51fe2c
#include <stdint.h>
#include <assert.h>

struct s_ZRG2cE18time0_3_29_H40000d4date4Date0g  {
        // /*@0*/struct s_ZRG3cE9core0_0_03num7nonzero10NonZeroI320g _0; // ::"core-0_0_0"::num::nonzero::NonZeroI32/*S*/
        /*@0*/int32_t _0; // i32
} ;

struct s_ZRG2cE9core0_0_06resultG9ResultErr2gG2cE18time0_3_29_H40000d4date4Date0gG3c_D5error15component_range14ComponentRange0g  {
        /*@0*/uint8_t _4; // bool
        /*@8*/int64_t _1; // i64
} ;

struct e_ZRG2cE9core0_0_06result6Result2gG2cE18time0_3_29_H40000d4date4Date0gG3c_D5error15component_range14ComponentRange0g {
        union {
		struct s_ZRG2cE9core0_0_06resultG8ResultOk2gG2cE18time0_3_29_H40000d4date4Date0gG3c_D5error15component_range14ComponentRange0g  {
			/*@0*/uint8_t _1; // u8
			/*@4*/struct s_ZRG2cE18time0_3_29_H40000d4date4Date0g _0; // Required!
		} var_0;
                struct s_ZRG2cE9core0_0_06resultG9ResultErr2gG2cE18time0_3_29_H40000d4date4Date0gG3c_D5error15component_range14ComponentRange0g var_1;
        } DATA;
};

int ZRIG2cI17deranged0_3_8_H8110ranged_i329RangedI320v2gV4_0989e2ffV4_2cfe51003new0g_OP(int32_t arg0) {
	return arg0 > 12345;
}

struct s_ZRG2cE18time0_3_29_H40000d4date4Date0g  ZRIG2cE18time0_3_29_H40000d4date4Date0g25from_julian_day_unchecked0g(int32_t arg0)
{
	struct s_ZRG2cE18time0_3_29_H40000d4date4Date0g rv = { arg0 };
	return rv;
}

// <::"time-0_3_29_H40000d"::date::Date/*S*/ /*- */>::from_julian_day
struct e_ZRG2cE9core0_0_06result6Result2gG2cE18time0_3_29_H40000d4date4Date0gG3c_D5error15component_range14ComponentRange0g  ZRIG2cE18time0_3_29_H40000d4date4Date0g15from_julian_day0g(
                int32_t arg0 // i32
                ) // -> ::"core-0_0_0"::result::Result<::"time-0_3_29_H40000d"::date::Date/*S*/,::"time-0_3_29_H40000d"::error::component_range::ComponentRange/*S*/,>/*E*/

{
        struct e_ZRG2cE9core0_0_06result6Result2gG2cE18time0_3_29_H40000d4date4Date0gG3c_D5error15component_range14ComponentRange0g rv;
        struct s_ZRG2cE9core0_0_06resultG9ResultErr2gG2cE18time0_3_29_H40000d4date4Date0gG3c_D5error15component_range14ComponentRange0g var6 = {0};
        struct s_ZRG2cE18time0_3_29_H40000d4date4Date0g var7;

        if( ZRIG2cI17deranged0_3_8_H8110ranged_i329RangedI320v2gV4_0989e2ffV4_2cfe51003new0g_OP(arg0) ) {
		rv.DATA.var_1 = var6;
		return rv;
	}
	else {
		rv.DATA.var_0._1 = 2;
		// Intermetdiate variable needed.
		var7 = ZRIG2cE18time0_3_29_H40000d4date4Date0g25from_julian_day_unchecked0g( arg0 );
		rv.DATA.var_0._0 = var7;
		return rv;
	}
}

int main() {
	// Assert twice, in case of stray in-memory data
	assert(ZRIG2cE18time0_3_29_H40000d4date4Date0g15from_julian_day0g(12345).DATA.var_0._0._0 == 12345);
	assert(ZRIG2cE18time0_3_29_H40000d4date4Date0g15from_julian_day0g(12344).DATA.var_0._0._0 == 12344);
}
