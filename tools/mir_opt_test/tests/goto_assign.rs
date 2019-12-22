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
	} RETURN;	// Another optimisation converts the goto into a return
	bb2: {
	} CALL retval = "black_box"() => bb3 else bb_panic;
	bb3: {
	} RETURN;
	bb_panic: {
	} DIVERGE;
}
