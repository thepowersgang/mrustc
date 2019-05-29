macro_rules! pat {
  [$a:expr;$b:expr] => (
    println!("{} {}", $a, $b);
  )
}
fn main() {
        pat![4;5];
}

