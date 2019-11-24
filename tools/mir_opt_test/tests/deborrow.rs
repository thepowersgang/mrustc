//
// Tests for the "de-borrow" class of optimisations (that try to remove useless borrows)
//

#[test="simple_exp"]
fn simple(a: (i32,)) -> i32
{
	let b: &(i32,);
	bb0: {
		ASSIGN b = &a;
		ASSIGN retval = b*.0;
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
		ASSIGN retval = b*.0;
	} RETURN;
}
fn simple_mut_exp(a: (i32,)) -> i32
{
	bb0: {
		ASSIGN retval = a.0;
	} RETURN;
}