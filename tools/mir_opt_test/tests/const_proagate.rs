
static lifted_zero_usize: usize = "\0\0\0\0\0\0\0\0";

// Copied and simplified from `ZRIG2cF10alloc0_0_07raw_vec6RawVec2gT0G2c_A5alloc6Global0g19capacity_from_bytes0g`
// Expect the static and literal to be propagated through and the `IF` eliminated, optimising down to just a call
#[test="statics_and_borrows_exp"]
fn statics_and_borrows(arg0: usize) -> usize
{
    let var0: &usize;
    let var1: &usize;
    let var2: bool;
    let var3: usize;
    let var5: &usize;
    let var6: &&usize;
    let var8: &&usize;
    let var11: &usize;
    let var12: &usize;

    bb0: {
        ASSIGN var3 = 0x0 usize;
        ASSIGN var5 = & var3;
        ASSIGN var1 = &::""::lifted_zero_usize;
        ASSIGN var0 = var5;
        ASSIGN var2 = EQ(var5.*, var1.*);
    } IF var2 => bb1 else bb2;
    bb1: {
        ASSIGN var11 = &var0.*;
        ASSIGN var12 = &var1.*;
        ASSIGN var6 = & var11;
        ASSIGN var8 = & var12;
    } CALL retval = ""( /*var4,*/ var6, var8/*, var13*/ ) => bb3 else bb3;
    bb2: {
        ASSIGN retval = DIV(arg0, 0x0_usize);
    } RETURN;
    bb3: {
    } DIVERGE;
}
fn statics_and_borrows_exp(arg0: usize) -> usize
{
    let var3: usize;
    let var6: &&usize;
    let var8: &&usize;
    let var5: &usize;
    let var1: &usize;

    bb0: {
        ASSIGN var3 = 0x0 usize;    // Constant has to exist, to be passed to the call (which is a `panic` wrapper)
        ASSIGN var5 = & var3;   // Borrow 1
        ASSIGN var1 = const &::""::lifted_zero_usize;
        ASSIGN var6 = & var5;   // Double-borrow
        ASSIGN var8 = & var1;   // Double-borrow
    } CALL retval = ""( /*var4,*/ var6, var8/*, var13*/ ) => bb3 else bb3;
    bb3: {
    } DIVERGE;
}

#[test="static_read_exp"]
fn static_read() -> usize {
    bb0: {
        ASSIGN retval = ::""::lifted_zero_usize;
    } RETURN;
}
fn static_read_exp() -> usize {
    bb0: {
        ASSIGN retval = 0 usize;
    } RETURN;
}


// thepowersgang/mrustc#270 - propgating through casts didn't truncate properly
#[test="mrustc_270_exp"]
fn mrustc_270() -> i32 {
    let v: isize;
    let a: i32;
    bb0: {
        ASSIGN v = +545201568421088164 isize;
        ASSIGN a = CAST v as i32;
        ASSIGN retval = DIV(a, +30313_i32);
    } RETURN;
}
fn mrustc_270_exp() -> i32 {
    bb0: {
        ASSIGN retval = +13684 i32;
    } RETURN;
}
#[test="mrustc_270_2_exp"]
fn mrustc_270_2() -> i8 {
    let v: isize;
    let a: i8;
    bb0: {
        ASSIGN v = +0x10FE isize;
        ASSIGN a = CAST v as i8;
        ASSIGN retval = DIV(a, +2_i8);
    } RETURN;
}
fn mrustc_270_2_exp() -> i8 {
    bb0: {
        ASSIGN retval = -1 i8;
    } RETURN;
}

