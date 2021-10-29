// compile-flags: --test

#![feature(asm)]

#[test]
fn inlined_copy_args()
{
    #[inline(always)]
    fn inline_fn(mut v: u8) {
        v = 2;
        llvm_asm!("" : : "r" (v) : /*clobber*/ : "volatile");
    }
    let v = 1;
    inline_fn(v);
    assert_eq!(v, 1);
}
