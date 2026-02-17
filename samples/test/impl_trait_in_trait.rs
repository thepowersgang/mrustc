// Test case for `impl Trait` in trait methods

trait TestTrait
{
    /// No provided body, a simple case
    fn no_default(&self) -> impl ::std::fmt::Debug ;
    /// Provided body: Hard case
    fn with_default(&self) -> impl ::std::fmt::Debug { [123;0] }
}
struct Type1;
impl TestTrait for Type1 {
    fn no_default(&self) -> impl ::std::fmt::Debug {
        [123;1]
    }
}
struct Type2;
impl TestTrait for Type2 {
    fn no_default(&self) -> impl ::std::fmt::Debug {
        [123;2]
    }
    fn with_default(&self) -> impl ::std::fmt::Debug { [123;3] }
}

fn main() {
    assert_eq!(format!("{:?}", Type1.with_default()), "[]");
    assert_eq!(format!("{:?}", Type1.no_default()), "[123]");
    assert_eq!(format!("{:?}", Type2.no_default()), "[123, 123]");
    assert_eq!(format!("{:?}", Type2.with_default()), "[123, 123, 123]");
}