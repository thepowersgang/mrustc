#[test="test_trivial"]
fn test_trivial()
{
	bb0: {
		ASSIGN retval = ();
	} RETURN
}

#[test="dce_local_exp"]
fn dce_local()
{
	let useless: ();
	bb0: {
		ASSIGN useless = ();
		ASSIGN retval = ();
	} RETURN
}
fn dce_local_exp()
{
	bb0: {
		ASSIGN retval = ();
	} RETURN
}