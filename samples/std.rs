#![no_std]

pub mod option
{
    pub enum Option<T>
    {
        None,
        Some(T)
    }
}
pub mod result
{
    pub enum Result<G,E>
    {
        Ok(G),
        Err(E)
    }
}

pub mod io
{
    pub type IoResult<T> = ::result::Result<T,IoError>;
    pub enum IoError
    {
        EndOfFile,
    }
    
    pub trait Reader
    {
    }
}

pub mod iter
{
    use option::Option::{self, None};
    pub trait Iterator
    {
        type Item;
        fn next(&self) -> Option<<Self as Iterator>::Item>;
        fn size_hint(&self) -> (usize, Option<usize>) {
            return (0, None);
        }
    }
}

pub mod char
{
    pub fn from_u32(v: u32) -> char
    {
        v   // TODO: This should generate a typecheck failure, but that part is incomplete
        // Will eventually need a version of mem::transmute()
    }
}

pub mod prelude
{
    pub mod v1
    {
        pub use option::Option::{self, None, Some};
        pub use result::Result::{self, Ok, Err};
        pub use io::IoResult;
        pub use io::Reader;
        pub use iter::Iterator;
    }
}

// vim: ft=rust

