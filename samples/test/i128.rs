// compile-flags: --test

#[test]
fn u128_ops()
{
    assert_eq!( ((std::i128::MAX as u128) >> (128-8)) as u8, 0x7F );
}

// `__builtin_popcountll` is 64-bit, so a native-u128 target used to count
// only the low half - which made `u128::BITS` (`Self::MAX.count_ones()`
// in libcore) equal 64.
#[inline(never)]
fn count_ones_u128(v: u128) -> u32 { v.count_ones() }
#[inline(never)]
fn count_ones_i128(v: i128) -> u32 { v.count_ones() }

#[test]
fn count_ones()
{
    assert_eq!( count_ones_u128(0), 0 );
    assert_eq!( count_ones_u128(std::u128::MAX), 128 );
    assert_eq!( count_ones_u128(std::u128::MAX >> 64), 64 );
    assert_eq!( count_ones_u128(1 << 127), 1 );
    assert_eq!( count_ones_i128(-1), 128 );
}

#[test]
fn print()
{
    println!("{:#x}", 0xC0FFEEC0FFEEC0_FFEEC0FFEEC0FFEEu128);
    println!("{}", 0xC0FFEEC0FFEEC0_FFEEC0FFEEC0FFEEu128);
}

