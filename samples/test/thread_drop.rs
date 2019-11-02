
use std::sync::Arc;
use std::sync::atomic::{Ordering, AtomicBool};

fn main() {
    struct Foo(Arc<AtomicBool>);
    impl Drop for Foo {
        fn drop(&mut self) {
            self.0.store(true, Ordering::SeqCst);
        }
    }
    let h = Arc::new(AtomicBool::new(false));
    let h2 = Foo(h.clone());
    let th = ::std::thread::spawn(move || { let _ = h2; });
    th.join().unwrap();
    assert!(h.load(Ordering::SeqCst), "Foo's Drop impl was never called");
}
