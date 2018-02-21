// compile-flags: --test

struct DropFlag<'a>(&'a mut i32);
impl<'a> ::std::ops::Drop for DropFlag<'a>
{
    fn drop(&mut self) {
        *self.0 += 1;
    }
}

#[test]
fn values_in_for_loop_dropped()
{
    let mut foo = 0;
    for _ in Some(DropFlag(&mut foo))
    {
    }
    assert_eq!(foo, 1);
}
