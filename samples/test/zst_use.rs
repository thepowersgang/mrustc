// compile-flags: --test

#[test]
fn zst_enum_variant() {
    #[inline(never)]
    fn takes_fn<F: Fn( () ) -> Option<()>>(f: F) {
        f( () );
    }

    takes_fn( Option::Some );
}

