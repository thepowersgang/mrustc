// compile-flags: --test

#[test]
fn u128_ops()
{
    assert_eq!( ((std::i128::MAX as u128) >> (128-8)) as u8, 0x7F );
}

#[test]
fn print()
{
    println!("{:#x}", 0xC0FFEEC0FFEEC0_FFEEC0FFEEC0FFEEu128);
    println!("{}", 0xC0FFEEC0FFEEC0_FFEEC0FFEEC0FFEEu128);
}

