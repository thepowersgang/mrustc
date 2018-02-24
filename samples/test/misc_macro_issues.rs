// compile-flags: --test

// #55 - Doesn't allow macros for :item
macro_rules! outer {
    ($it:item) => {}
}
outer! {
    inner! {}
}

// #59 - 
macro_rules! outer2 {
    ({ $($x:item)* }) => {}
}
outer2! {
    {inner!{}}
}


// #56 - Unexpanded macro in type
macro_rules! m {
    ($tt:tt) => { $tt }
}

struct A;
struct B;

impl From<m!(A)> for B {
    fn from(_: A) -> B {
        unimplemented!()
    }
}

// #61 - Not expanding macros in paths in types
macro_rules! Ty {
    () => { u8 }
}

fn f() -> Option<Ty![]> {
    None
}

