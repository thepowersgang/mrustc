#[test="multi_use_copy_temporary_EXP"]
fn multi_use_copy_temporary(arg0: i8) -> (i8,i8,i8,i8,)
{
    let var0: i8;
    let var1: i8;
    let var2: i8;
    let var3: i8;
    bb0: {
        ASSIGN var0 = arg0;
        ASSIGN var1 = arg0;
        ASSIGN var2 = arg0;
        ASSIGN var3 = arg0;
        ASSIGN retval = (var0, var1, var2, var3,);
    } RETURN;
}
fn multi_use_copy_temporary_EXP(arg0: i8) -> (i8,i8,i8,i8,)
{
    bb0: {
        ASSIGN retval = (arg0, arg0, arg0, arg0,);
    } RETURN;
}
