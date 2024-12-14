//
// Tests for the "de-borrow" class of optimisations (that try to remove useless borrows)
//

#[test="simple_exp"]
fn simple(a: (i32,)) -> i32
{
    let b: &(i32,);
    bb0: {
        ASSIGN b = &a;
        ASSIGN retval = b.*.0;
    } RETURN;
}
fn simple_exp(a: (i32,)) -> i32
{
    bb0: {
        ASSIGN retval = a.0;
    } RETURN;
}

// Should also work for mutable borrows
#[test="simple_mut_exp"]
fn simple_mut(a: (i32,)) -> i32
{
    let b: &mut (i32,);
    bb0: {
        ASSIGN b = &mut a;
        ASSIGN retval = b.*.0;
    } RETURN;
}
fn simple_mut_exp(a: (i32,)) -> i32
{
    bb0: {
        ASSIGN retval = a.0;
    } RETURN;
}

// Multiple usage of a `&mut` shouldn't be expanded
#[test="double_mut"]
fn double_mut(a: (i32,)) -> i32
{
    let b: &mut (i32,);
    bb0: {
        ASSIGN b = &mut a;
        ASSIGN retval = ADD(b.*.0, b.*.0);
    } RETURN;
}

// Multiple usage of a shared borrow should all be replaced
#[test="double_shared_exp"]
fn double_shared(a: (i32,)) -> i32
{
    let b: &(i32,);
    bb0: {
        ASSIGN b = &a;
        ASSIGN retval = ADD(b.*.0, b.*.0);
    } RETURN;
}
fn double_shared_exp(a: (i32,)) -> i32
{
    bb0: {
        ASSIGN retval = ADD(a.0, a.0);
    } RETURN;
}

// Structure borrows
#[test="structures_exp"]
fn structures(i: &((i32, ), )) -> (&i32,)
{
    let a: &(i32,);
    let b: &i32;
    bb0: {
        ASSIGN a = &i.*.0;
        ASSIGN b = &a.*.0;
        ASSIGN retval = (b,);
    } RETURN;
}
fn structures_exp(i: &((i32, ), )) -> (&i32,)
{
    let b: &i32;
    bb0: {
        ASSIGN b = &i.*.0 .0;
        ASSIGN retval = (b,);
    } RETURN;
}


#[test="regression_273"]
fn regression_273(i: (i32,&i32)) -> ( (i32,&i32),i32 ) {
    let b: &i32;
    bb0: {
        ASSIGN b = &i.1.*;
        ASSIGN retval = ( i, b.* );
    } RETURN;
}


// ----
// Regression
// ----
/*
fn regression_20220610(_11: &(i32,i32,)) -> ()
{
    bb0: {
        ASSIGN _3 = &_11*.0;
        ASSIGN _30 = _3;
        ASSIGN _40 = _3*;
        ASSIGN _4 = &_11*.1;
    } CALL retval = ""(_4,_30,_40) => goto bb1 else bb1;
    bb1: {
    } RETURN;
}
*/
