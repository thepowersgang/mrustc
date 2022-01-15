// compile-flags: --test

#[test]
fn u128_ops()
{
    assert_eq!( ((std::i128::MAX as u128) >> (128-8)) as u8, 0x7F );
}

