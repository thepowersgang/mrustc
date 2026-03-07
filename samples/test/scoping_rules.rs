// compile-flags: --test

struct DropFlag<'a>(&'a ::std::cell::Cell<i32>);
impl<'a> ::std::ops::Drop for DropFlag<'a>
{
    fn drop(&mut self) {
        self.0.set( self.0.get() + 1 );
    }
}

// Any temporaries defined in the expression part of a statement (i.e. in the yeilded part of a
// block) are stored in the parent scope.
#[test]
fn temporaries_in_yielded_expr()
{
    let drop_count = ::std::cell::Cell::new(0);
    // NOTE: This is edition specific! in 2024 this is what happens, but in pre 24 the drop happens the `let`
    let _foo = ({ DropFlag(&drop_count).0 }, assert_eq!(drop_count.get(), 1) );
    drop(_foo);
    assert_eq!(drop_count.get(), 1);
}


#[test]
fn temp_in_if()
{
    let drop_count = ::std::cell::Cell::new(0);
    if (DropFlag(&drop_count), true).1 {
        assert_eq!(drop_count.get(), 1);
    }
    if let true = (DropFlag(&drop_count), true).1 {
        // Temporaries in a `let` get dropped after the body
        assert_eq!(drop_count.get(), 1);
    }
    assert_eq!(drop_count.get(), 2);

    if let true = true && (DropFlag(&drop_count), true).1 {
        // Expect the non-let to have its temporaries dropped before the body
        assert_eq!(drop_count.get(), 3);
    }
    assert_eq!(drop_count.get(), 3);
    
    {}
}