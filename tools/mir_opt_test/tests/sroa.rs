//
// Tests for SROA-style optimisations
// - SROA = Scalar Reorganisation Of Aggregates - breaking composite types up into individual values
//

#[test="simple_exp"]
fn simple(a: i32, b: i32) -> i32
{
    let v: (i32, i32,);
    bb0: {
        ASSIGN v = (a, b);
        ASSIGN retval = ADD(v.0, v.1);
    } RETURN;
}
fn simple_exp(a: i32, b: i32) -> i32
{
    bb0: {
        ASSIGN retval = ADD(a, b);
    } RETURN;
}

/*
struct Foo { i32, i32 };
#[test="struct_exp"]
fn struct_(a: i32, b: i32) -> i32
{
    let v: Foo;
    bb0: {
        ASSIGN v = {a, b}: Foo;
        ASSIGN retval = ADD(v.0, v.1);
    } RETURN;
}
fn struct_exp(a: i32, b: i32) -> i32
{
    bb0: {
        ASSIGN retval = ADD(a, b);
    } RETURN;
}
*/

