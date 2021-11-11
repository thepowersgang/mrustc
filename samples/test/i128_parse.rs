// compile-flags: --test

#[test]
fn i128_from_str()
{
    assert_eq!( i128::from_str_radix("2", 10).unwrap(), 2 );
}

