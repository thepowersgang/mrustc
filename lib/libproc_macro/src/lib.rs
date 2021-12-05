// MRustC custom version of libproc_macro
//
// Unlike the original rustc version, this one is designed to live complely detached from its compiler.
#![allow(ellipsis_inclusive_range_patterns)]

macro_rules! some_else {
    ($e:expr => $alt:expr) => {match $e { Some(v) => v, None => $alt }};
}

macro_rules! debug {
    ( $($t:tt)* ) => {
        if ::std::env::var_os("MRUSTC_PM_DEBUG").is_some() {
            eprintln!($($t)*)
        }
    }
}
macro_rules! note {
    ( $($t:tt)* ) => {
        if ::std::env::var_os("MRUSTC_PM_DEBUG").is_some() {
            eprintln!($($t)*)
        }
    }
}

mod span;
mod token_tree;
/// Parse a TokenStream from a string
mod lex;
/// Raw IPC protocol
mod protocol;
/// Converts to/from TokenStream and IPC
mod serialisation;
mod diagnostic;

mod token_stream {

    #[derive(Debug,Clone,Default)]
    pub struct TokenStream {
        pub(crate) inner: Vec<crate::TokenTree>,
    }

    #[derive(Clone)]
    pub struct IntoIter {
        it: ::std::vec::IntoIter<crate::TokenTree>,
    }
    impl Iterator for IntoIter {
        type Item = crate::TokenTree;
        fn next(&mut self) -> Option<crate::TokenTree> {
            self.it.next()
        }
    }

    impl TokenStream {
        // 1.29
        pub fn new() -> TokenStream {
            TokenStream {
                inner: Vec::new(),
            }
        }
        // 1.29
        pub fn is_empty(&self) -> bool {
            self.inner.is_empty()
        }
    }

    // 1.29
    impl IntoIterator for TokenStream
    {
        type Item = super::TokenTree;
        type IntoIter = IntoIter;
        fn into_iter(self) -> IntoIter {
            IntoIter {
                it: self.inner.into_iter(),
            }
        }
    }

    //impl ::std::string::ToString for TokenStream
    //{
    //    fn to_string(&self) -> String {
    //        use std::fmt::Write;
    //        let mut s = String::new();
    //        for v in &self.inner
    //        {
    //            write!(&mut s, "{}", v).unwrap();
    //        }
    //        s
    //    }
    //}
    impl ::std::fmt::Display for TokenStream
    {
        fn fmt(&self, f: &mut ::std::fmt::Formatter) -> ::std::fmt::Result {
            for v in &self.inner
            {
                write!(f, "{}", v)?;
                // Put a space after every token that isn't punctuation
                if let &crate::TokenTree::Punct(_) = v {
                }
                else {
                    write!(f, " ")?;
                }
            }
            Ok(())
        }
    }

    // 1.29.0
    impl From<crate::TokenTree> for TokenStream
    {
        fn from(t: crate::TokenTree) -> TokenStream {
            TokenStream {
                inner: vec![t],
            }
        }
    }

    impl ::std::iter::FromIterator<TokenStream> for TokenStream
    {
        fn from_iter<I: IntoIterator<Item = TokenStream>>(streams: I) -> Self
        {
            let mut rv = TokenStream::new();
            rv.extend(streams);
            rv
        }
    }
    impl ::std::iter::FromIterator<crate::TokenTree> for TokenStream
    {
        fn from_iter<I: IntoIterator<Item = crate::TokenTree>>(tokens: I) -> Self
        {
            let mut rv = TokenStream::new();
            rv.extend(tokens);
            rv
        }
    }

    // 1.30
    impl ::std::iter::Extend<TokenStream> for TokenStream
    {
        fn extend<I: IntoIterator<Item = TokenStream>>(&mut self, streams: I) {
            for i in streams {
                self.inner.extend(i.inner.into_iter())
            }
        }
    }
    // 1.30
    impl ::std::iter::Extend<crate::TokenTree> for TokenStream
    {
        fn extend<I: IntoIterator<Item = crate::TokenTree>>(&mut self, trees: I)
        {
            self.inner.extend(trees)
        }
    }
}

pub use crate::span::Span;

pub use crate::token_stream::TokenStream;

pub use crate::token_tree::{TokenTree,Group,Ident,Punct,Literal};
pub use crate::token_tree::{Delimiter,Spacing};

pub use crate::lex::LexError;
pub use crate::diagnostic::{Diagnostic,Level,MultiSpan};


#[doc(hidden)]
pub struct MacroDesc
{
    name: &'static str,
    handler: fn(TokenStream)->TokenStream,
}

#[doc(hidden)]
pub fn main(macros: &[MacroDesc])
{
    //::env_logger::init();

    let mac_name = ::std::env::args().nth(1).expect("Was not passed a macro name");
    //eprintln!("Searching for macro {}\r", mac_name);
    for m in macros
    {
        if m.name == mac_name {
            use std::io::Write;
            ::std::io::stdout().write(&[0]).expect("Stdout write error?");
            ::std::io::stdout().flush().expect("Stdout write error?");
            debug!("Waiting for input\r");
            let input = crate::serialisation::recv_token_stream(::std::io::stdin().lock());
            debug!("INPUT = `{}`\r", input);
            let output = (m.handler)( input );
            debug!("OUTPUT = `{}`\r", output);
            crate::serialisation::send_token_stream(::std::io::stdout().lock(), output);
            ::std::io::Write::flush(&mut ::std::io::stdout()).expect("Stdout write error?");
            note!("Done");
            return ;
        }
    }
    panic!("Unknown macro name '{}'", mac_name);
}

