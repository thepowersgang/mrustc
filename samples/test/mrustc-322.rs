// ignore-test - Rustc doesn't allow this, plus it doesn't specify the type for the `Vec<_>`
#![crate_type = "lib"]
struct Assert<const COND: bool>;
trait IsTrue {}
impl IsTrue for Assert<true> {}
trait IsNotZst {}
impl<T> IsNotZst for T
where
    Assert<{ std::mem::size_of::<T>() > 0 }>: IsTrue,
{}
fn assert_not_zero_sized<T: IsNotZst>(_: T) {}
fn main() {
    assert_not_zero_sized(vec![]);
}
