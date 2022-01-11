// compile-flags: --test

struct Wrapped;

fn unwrap(_: Wrapped) -> bool { true }


#[test]
fn test_with_wrapped() {
    assert!(run_with_wrapped());
}

fn run_with_wrapped() -> bool {
    let w = Wrapped;
    match () {
        // This program is a regression test for a bug that happened when
        // a match guard moved out of the surrounding environment, which
        // is supposed to be allowed (moving out of the value being matched
        // on, however, is not allowed).
        _ if unwrap(w) => true,
        () => return false,
    }
}
