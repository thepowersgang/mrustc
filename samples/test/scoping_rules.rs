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
    let _foo = ({ DropFlag(&drop_count).0 }, assert_eq!(drop_count.get(), 0) );
    drop(_foo);
    assert_eq!(drop_count.get(), 1);
}


