struct Struct1 {
    var1: usize,
}

fn main() -> () {
    Struct1 {
        var1: { vec![1i16] }.len(),
    };
}
