//
// Test cases for various issues
//

// thepowersgang/mrustc#142
// - Want to ensure that `old` is kept (i.e. the access of `sh` isn't moved across `uniq`'s creation)
#[test="shared_read_across_mut_borrow_OPT"]
fn shared_read_across_mut_borrow<'a>(data: &'a mut (i32,)) -> i32
{
    let sh: &i32;
    let uniq: &mut i32;
    let old: i32;
    bb0: {
        ASSIGN sh = &data.*.0;
        ASSIGN old = sh.*;
        ASSIGN uniq = &mut data.*.0;
        ASSIGN uniq.* = ADD(uniq.*, uniq.*);
    } CALL retval = ""<>(old) => bb1 else bb_p;
    bb1: {
    } RETURN;
    bb_p: {} DIVERGE;
}
fn shared_read_across_mut_borrow_OPT(data: &mut (i32,)) -> i32
{
    let uniq: &mut i32;
    let old: i32;
    bb0: {
        ASSIGN old = data.*.0;
        ASSIGN uniq = &mut data.*.0;
        ASSIGN uniq.* = ADD(uniq.*, uniq.*);
    } CALL retval = ""<>(old) => bb1 else bb_p;
    bb1: {
    } RETURN;
    bb_p: {} DIVERGE;
}
