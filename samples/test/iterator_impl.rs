fn consume_iterator_impl<I: Iterator<Item = impl AsRef<str>>>(i: I) -> String {
    i.map(|s| s.as_ref().to_string()).collect()
}

fn main() {
    assert_eq!(consume_iterator_impl(vec![String::from("Hello, "), String::from("world.")].into_iter()), "Hello, world.");
}
