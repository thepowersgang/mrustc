
struct Struct;

trait Trait {
    type Assoc;
}

impl Trait for Struct {
    type Assoc = Struct;
}

fn f<S>(_: &S) where S: Trait<Assoc = S> {}

fn main() {
    f(&Struct);
}

