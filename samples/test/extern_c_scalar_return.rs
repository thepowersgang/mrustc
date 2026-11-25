// compile-flags: --test

use std::os::raw::{c_char, c_int};
use std::ptr::NonNull;

#[repr(transparent)]
struct Int(c_int);
#[repr(transparent)]
struct Float(f32);
#[repr(transparent)]
struct Double(f64);
#[repr(C)]
#[derive(PartialEq, Debug)]
enum Small { Zero = 0, One = 1, Two = 2 }

extern "C" {
    fn strchr(s: *const c_char, c: c_int) -> Option<NonNull<c_char>>;
    #[link_name = "strchr"]
    fn strchr_ref(s: *const c_char, c: c_int) -> Option<&'static c_char>;
    fn abs(x: c_int) -> Int;
    #[link_name = "abs"]
    fn abs_enum(x: c_int) -> Small;
    fn strtof(s: *const c_char, end: *mut *mut c_char) -> Float;
    fn strtod(s: *const c_char, end: *mut *mut c_char) -> Double;
}

static TEXT: &'static [u8] = b"mrustc\0";

#[test]
fn niche_option_pointer()
{
    let s = TEXT.as_ptr() as *const c_char;
    unsafe {
        assert_eq!(strchr(s, b's' as c_int).map(|p| p.as_ptr() as *const c_char), Some(s.offset(3)));
        assert!(strchr(s, b'x' as c_int).is_none());
        assert_eq!(strchr_ref(s, b'c' as c_int).map(|p| *p as u8), Some(b'c'));
    }
}

#[test]
fn transparent_integer()
{
    unsafe {
        assert_eq!(abs(-42).0, 42);
    }
}

#[test]
fn unit_only_c_enum()
{
    unsafe {
        assert_eq!(abs_enum(-2), Small::Two);
        assert_eq!(abs_enum(1), Small::One);
    }
}

#[test]
fn transparent_float()
{
    unsafe {
        assert_eq!(strtof(b"1.5\0".as_ptr() as *const c_char, 0 as *mut _).0, 1.5);
        assert_eq!(strtod(b"-2.25\0".as_ptr() as *const c_char, 0 as *mut _).0, -2.25);
    }
}
