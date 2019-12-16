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
		//ASSIGN ba* = +0;	// This could be here, which is why the optimisation is unsound
		ASSIGN retval = (v,);
		DROP ba;
	} RETURN;
}
