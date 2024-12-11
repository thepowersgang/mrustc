// Type Alias Impl Trait

mod define {
    use std::mem::{size_of, transmute_copy, MaybeUninit};

    // A clone of 1.74 librustc_middle/src/query/erase.rs
    pub trait EraseType: Copy {
        type Result: Copy;
    }
    pub struct Erased<T: Copy> {
        data: MaybeUninit<T>,
    }
    pub type Erase<T: EraseType> = Erased<impl Copy>;

    #[inline(always)]
    pub fn erase<T: EraseType>(src: T) -> Erase<T> {
        // Ensure the sizes match
        const {
            if std::mem::size_of::<T>() != std::mem::size_of::<T::Result>() {
                panic!("size of T must match erased type T::Result")
            }
        };

        Erased::<<T as EraseType>::Result> {
            // SAFETY: It is safe to transmute to MaybeUninit for types with the same sizes.
            data: unsafe { transmute_copy(&src) },
        }
    }

    pub fn restore<T: EraseType>(value: Erase<T>) -> T {
        let value: Erased<<T as EraseType>::Result> = value;
        // SAFETY: Due to the use of impl Trait in `Erase` the only way to safely create an instance
        // of `Erase` is to call `erase`, so we know that `value.data` is a valid instance of `T` of
        // the right size.
        unsafe { transmute_copy(&value.data) }
    }

    impl EraseType for i32 {
        type Result = [u8; 4];
    }
    impl EraseType for f64 {
        type Result = [u8; 8];
    }
}

fn main() {
    let a = define::erase(1234);
    let b = define::erase(1234.0);
    assert_eq!(define::restore(a), 1234);
    assert_eq!(define::restore(b), 1234.0);
}
