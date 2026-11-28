#[derive(Default)]
struct S<T = u16> { t: T }

#[derive(Default)]
struct Outer<T>(T);

fn main() {
    let s: S = Default::default();
    assert_eq!(s.t.leading_zeros(), 16);

    let o = Outer::<S>::default();
    assert_eq!(o.0.t.leading_zeros(), 16);
}
