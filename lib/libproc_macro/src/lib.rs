// MRustC custom version of libproc_macro
//
// Unlike the original rustc version, this one is designed to live complely detached from its compiler.
#![allow(ellipsis_inclusive_range_patterns)]
#![feature(crate_in_paths)]
#![feature(optin_builtin_traits)]
#![feature(vec_resize_with)]
#![feature(const_vec_new)]

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

pub mod token_stream {

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
pub enum MacroType {
    SingleStream(fn(TokenStream)->TokenStream),
    Attribute(fn(TokenStream,TokenStream)->TokenStream),
}
#[doc(hidden)]
pub struct MacroDesc
{
    name: &'static str,
    handler: MacroType,
}

static mut IS_AVAILABLE: bool = false;
#[doc(hidden)]
pub fn main(macros: &[MacroDesc])
{
    // SAFE: This is the entrypoint, so no threads running yet
    unsafe {
        IS_AVAILABLE = true;
    }
    //::env_logger::init();

    let mut args = ::std::env::args();
    let _ = args.next().expect("Should have an executable name");
    let mac_name = args.next().expect("Was not passed a macro name");
    let input_path = args.next();
    //eprintln!("Searching for macro {}\r", mac_name);
    for m in macros
    {
        if m.name == mac_name {
            use std::io::Write;
            ::std::io::stdout().write(&[0]).expect("Stdout write error?");
            ::std::io::stdout().flush().expect("Stdout write error?");
            debug!("Waiting for input\r");
            let mut stdin_raw;
            let mut fp_raw;
            let stdin = if let Some(p) = input_path {
                    fp_raw = ::std::fs::File::open(p).unwrap();
                    &mut fp_raw as &mut /*dyn */::std::io::Read
                }
                else {
                    stdin_raw = ::std::io::stdin().lock();
                    &mut stdin_raw
                };
            let input = crate::serialisation::recv_token_stream(stdin);
            debug!("INPUT = `{}`\r", input);
            let output = match m.handler
                {
                MacroType::SingleStream(h) => {
                    Span::freeze_definitions();
                    (h)(input)
                    },
                MacroType::Attribute(h) => {
                    let input_body = crate::serialisation::recv_token_stream(stdin);
                    debug!("INPUT BODY = `{}`\r", input_body);
                    Span::freeze_definitions();
                    (h)(input, input_body)
                    },
                };
            debug!("OUTPUT = `{}`\r", output);
            let stdout = ::std::io::stdout();
            crate::serialisation::send_token_stream(stdout.lock(), output);
            ::std::io::Write::flush(&mut ::std::io::stdout()).expect("Stdout write error?");
            note!("Done");
            return ;
        }
    }
    panic!("Unknown macro name '{}'", mac_name);
}

pub fn is_available() -> bool {
    // SAFE: Reading from a value only ever written in single-threaded code
    unsafe { IS_AVAILABLE }
}
