// No-op test
#[test="test_trivial"]
fn test_trivial()
{
	bb0: {
		ASSIGN retval = ();
	} RETURN;
}

// Dead code elimination, should remove any useless code
// - Unused assignments 
// - Unused locals
// - Unused drop flags
// - Unused blocks
#[test="dce_exp"]
fn dce()
{
	let df0 = false;
	let useless: ();
	bb0: {
		ASSIGN useless = ();
		ASSIGN useless = useless;	// Never read, so will be removed
		ASSIGN retval = ();
		//ASSIGN retval = ();	// Note: won't be de-duplicated (TODO)
	} RETURN;
	// Never referenced, will be removed
	bb_unused: {
	} GOTO bb_unused_indir;
	// Indirectly unused, will still be removed
	bb_unused_indir: {
	} DIVERGE;
	// References itself without other reference, will be removed
	bb_unused_self: {
	} GOTO bb_unused_self;
}
fn dce_exp()
{
	bb0: {
		ASSIGN retval = ();
	} RETURN;
}

// Inlining
#[test="inlining_exp"]
fn inlining()
{
	bb0: {
	} CALL retval = ::""::inlining_target() => bb1 else bb2;
	bb1: {
	} RETURN;
	bb2: {
	} DIVERGE;
}
fn inlining_target()
{
	bb0: {
		ASSIGN retval = ();
	} RETURN;
}
fn inlining_exp()
{
	bb0: {
		ASSIGN retval = ();
	} RETURN;
}

// Constant propagation leading to DCE
#[test="constprop_exp"]
fn constprop()
{
	let v1: bool;
	bb0: {
		ASSIGN v1 = true;
	} IF v1 => bb1 else bb2;
	bb1: {
		ASSIGN retval = ();
	} RETURN;
	bb2: {
	} DIVERGE;
}
fn constprop_exp()
{
	bb0: {
		ASSIGN retval = ();
	} RETURN;
}
