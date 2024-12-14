//
// Tests for single-use-variable elimination
//

// Check forward movement of values
#[test="simple_fwd_exp"]
fn simple_fwd(a: i32) -> (i32,)
{
    let v: i32;
    bb0: {
        ASSIGN v = a;
        ASSIGN retval = (v,);
    } RETURN;
}
fn simple_fwd_exp(a: i32) -> (i32,)
{
    bb0: {
        ASSIGN retval = (a,);
    } RETURN;
}

// Reverse (upwards) movement
#[test="simple_rev_exp"]
fn simple_rev(a: i32) -> (i32,)
{
    let v: (i32,);
    bb0: {
        ASSIGN v = (a,);
        ASSIGN retval = v;
    } RETURN;
}
fn simple_rev_exp(a: i32) -> (i32,)
{
    bb0: {
        ASSIGN retval = (a,);
    } RETURN;
}


static mut FOO: (i32, i32, i32) = "\x00\0\0\0\x01\0\0\0\x02\0\0\0";
#[test="static_multiple_exp"]
fn static_multiple() -> [i32; 3] {
    let a: i32;
    let b: i32;
    let c: i32;
    bb0: {
        ASSIGN a = ::""::FOO.0;
        ASSIGN b = ::""::FOO.1;
        ASSIGN c = ::""::FOO.2;
        ASSIGN retval = [a, b, c];
    } RETURN;
}
fn static_multiple_exp() -> [i32; 3] {
    bb0: {
        ASSIGN retval = [::""::FOO.0, ::""::FOO.1, ::""::FOO.2];
    } RETURN;
}

// Check that if there's a mutable borrow of the source, that the source isn't propagated forwards
// - NOTE: This relies on the optimiser not being smart enough to move the `retval` construction up
#[test="nomut"]
fn nomut(a: i32) -> (i32,)
{
    let v: i32;
    let ba: &mut i32;
    bb0: {
        ASSIGN v = a;
        ASSIGN ba = &mut a;
        ASSIGN ba.* = +0 i32;    // if the read was moved, it would be unsound
        ASSIGN ba.* = +0 i32;    // Second mutation, just so the optimiser doesn't get smart and move the `&mut`
        ASSIGN retval = (v,);
        DROP ba;
    } RETURN;
}

// NOTE: Test based on a snippet from `<::"alloc"::rc::Rc<[u8],>>::allocate_for_ptr`
// Reverse (upwards) movement
#[test="borrowed_rev_exp"]
fn borrowed_rev(a: &mut [u8], a2: &mut u8)
{
    let var11: *mut [u8];
    let var21: *mut u8;
    let var30: *mut [u8];
    let var22: &mut *mut [u8];
    bb0: {
        ASSIGN var11 = CAST a as *mut [u8];
        ASSIGN var21 = CAST a2 as *mut u8;
        ASSIGN var30 = var11;
        ASSIGN var22 = &mut var30;
    } CALL retval = "black_box"(var22, var21) => bb1 else bb2;
    bb1: {
    } RETURN;
    bb2: {
    } DIVERGE;
}
fn borrowed_rev_exp(a: &mut [u8], a2: &mut u8)
{
    let var21: *mut u8;
    let var30: *mut [u8];
    let var22: &mut *mut [u8];
    bb0: {
        ASSIGN var30 = CAST a as *mut [u8];
        ASSIGN var21 = CAST a2 as *mut u8;
        ASSIGN var22 = &mut var30;
    } CALL retval = "black_box"(var22, var21) => bb1 else bb2;
    bb1: {
    } RETURN;
    bb2: {
    } DIVERGE;
}
//*/


//fn <&usize as ::"core"::ops::bit::Shl<&i8,>>::shl(arg0: &usize, arg1: &i8) -> usize
//#[test="borrow_usize_shl_borrow_i8_exp"]
//fn borrow_usize_shl_borrow_i8(arg0: &usize, arg1: &i8) -> usize {
//    let var0: usize;
//    let var1: i8;
//    bb0: {
//        ASSIGN var1 = arg1.*;
//        ASSIGN var0 = arg0.*;
//        ASSIGN retval = BIT_SHL(var0, var1);
//    } RETURN;
//}
//fn borrow_usize_shl_borrow_i8_exp(arg0: &usize, arg1: &i8) -> usize {
//    bb0: {
//        ASSIGN retval = BIT_SHL(arg0.*, arg1.*);
//    } RETURN;
//}


// Check that SS&U works when a terminator (e.g. call) is the usage site.
#[test="call_exp"]
fn call(a: (fn(),)) {
    let v: fn();
    bb0: {
        ASSIGN v = a.0;
    } CALL retval = (v)() => bb1 else bb2;
    bb1: {
    } RETURN;
    bb2: {
    } DIVERGE;
}
fn call_exp(a: (fn(),)) {
    bb0: {
    } CALL retval = (a.0)() => bb1 else bb2;
    bb1: {
    } RETURN;
    bb2: {
    } DIVERGE;
}


// Shouldn't move on if the source is invalidated by in the target statement
#[test="neg_srcused"]
fn neg_srcused(v: (u32, &mut u32,)) -> ((u32, &mut u32,), u32,)
{
    let v1: u32;
    bb0: {
        ASSIGN v1 = v.0;
        ASSIGN retval = (v, v1);
    } RETURN;
}
#[test="neg_srcused_call"]
fn neg_srcused_call(a: (&mut (), i32))
{
    let b: i32;
    bb0: {
        ASSIGN b = a.1;
    } CALL retval = ""(a, b) => bb1 else bb1;
    bb1: { } DIVERGE;
}
