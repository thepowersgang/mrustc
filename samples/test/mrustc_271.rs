fn main() {
    let b = -5_i128 >> 2;
    println!("{}", b);
    let c = b * 3;
    println!("{}", c);
    assert!(c == -6);
}
