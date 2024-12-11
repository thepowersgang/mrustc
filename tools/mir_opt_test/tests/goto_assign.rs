//
// Tests for single-use-variable elimination
//

// Check forward movement of values
#[test="simple_exp"]
fn simple(bv: bool) -> u32
{
    let v: u32;
    bb0: {
    } IF bv => bb1 else bb2;
    bb1: {
        ASSIGN v = 0 u32;
    } GOTO bb3;
    bb2: {
    } CALL v = "black_box"() => bb3 else bb_panic;
    bb3: {
        ASSIGN retval = v;
    } RETURN;
    bb_panic: {
    } DIVERGE;
}
fn simple_exp(bv: bool) -> u32
{
    bb0: {
    } IF bv => bb1 else bb2;
    bb1: {
        ASSIGN retval = 0 u32;
    } RETURN;   // Another optimisation converts the goto into a return
    bb2: {
    } CALL retval = "black_box"() => bb3 else bb_panic;
    bb3: {
    } RETURN;
    bb_panic: {
    } DIVERGE;
}

// Ensure that the optimisation doesn't apply when the target is used later on (borrow)
#[test="neg_borrowed"]
fn neg_borrowed(a: u32) -> u32
{
    let v1: u32;
    let bv: &u32;
    let v2: &u32;
    bb0: {
    } CALL v1 = "black_box"<u32>(a) => bb1 else bb_panic;
    bb1: {
        ASSIGN retval = v1;
        ASSIGN bv = &v1;
    } CALL v2 = "black_box"<&u32>(bv) => bb2 else bb_panic;
    bb2: {
    } RETURN;
    bb_panic: {
    } DIVERGE;
}
#[test="neg_reused"]
fn neg_reused(a: u32) -> u32
{
    let v1: u32;
    let v2: u32;
    bb0: {
    } CALL v1 = "black_box"<u32>(a) => bb1 else bb_panic;
    bb1: {
        ASSIGN retval = v1;
    } CALL v2 = "black_box"<u32>(v1) => bb2 else bb_panic;
    bb2: {
    } RETURN;
    bb_panic: {
    } DIVERGE;
}
#[test="neg_reused_tuple"]
fn neg_reused_tuple(a: (u32,)) -> (u32,)
{
    let v1: (u32,);
    let v2: u32;
    bb0: {
    } CALL v1 = "black_box"<(u32,)>(a) => bb1 else bb_panic;
    bb1: {
        ASSIGN retval = v1;
    } CALL v2 = "black_box"<u32>(v1.0) => bb2 else bb_panic;
    bb2: {
    } RETURN;
    bb_panic: {
    } DIVERGE;
}
