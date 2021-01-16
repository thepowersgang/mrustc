fn main() {
    let mut dropped = false;
    struct DropSentinel<'a>(&'a mut bool);
    impl<'a> std::ops::Drop for DropSentinel<'a> {
        fn drop(&mut self) {
            *self.0 = true;
        }
    }
    let v = Box::new(DropSentinel(&mut dropped));
    drop(v);
    assert!(dropped);
}
