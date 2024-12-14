#![allow(internal_features)]
#![feature(no_core,lang_items)]
#![no_core]
#[lang="sized"]
trait Sized {}
#[no_mangle]
fn __libc_start_main()->i32{
    0
}
#[lang="start"]
fn lang_start<T>(
    main: fn() -> T,
    argc: isize,
    argv: *const *const u8,
    sigpipe: u8
)->isize {
    0
}
fn main() {
}
