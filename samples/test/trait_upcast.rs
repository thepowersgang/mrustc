//

trait A {
    fn bar(&self) -> i32;
}
trait B: A {
    fn baz(&self) -> i32;
}

struct Foo;
impl A for Foo {
    fn bar(&self) -> i32 {
        12345
    }
}
impl B for Foo {
    fn baz(&self) -> i32 {
        54321
    }
}

fn main() {
    let v = Foo;
    let b: &dyn B = &v;
    let a: &dyn A = b;
    assert_eq!( a.bar(), 12345 );
}
