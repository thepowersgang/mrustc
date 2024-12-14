use crate::Span;
use crate::TokenStream;

// 1.29
#[derive(Debug,Clone)]
pub enum TokenTree
{
    Group(Group),
    Ident(Ident),
    Punct(Punct),
    Literal(Literal),
}
impl ::std::fmt::Display for TokenTree
{
    fn fmt(&self, f: &mut ::std::fmt::Formatter) -> ::std::fmt::Result
    {
        match self
        {
        &TokenTree::Group(ref v) => v.fmt(f),
        &TokenTree::Ident(ref v) => v.fmt(f),
        &TokenTree::Punct(ref v) => v.fmt(f),
        &TokenTree::Literal(ref v) => v.fmt(f),
        }
    }
}
impl TokenTree
{
    pub fn span(&self) -> Span {
        match self
        {
        &TokenTree::Group(ref v) => v.span,
        &TokenTree::Ident(ref v) => v.span,
        &TokenTree::Punct(ref v) => v.span,
        &TokenTree::Literal(ref v) => v.span,
        }
    }
    pub fn set_span(&mut self, span: Span) {
        let _ = span;
    }
}
impl From<Group> for TokenTree {
    fn from(g: Group) -> Self {
        TokenTree::Group(g)
    }
}
impl From<Ident> for TokenTree {
    fn from(v: Ident) -> Self {
        TokenTree::Ident(v)
    }
}
impl From<Punct> for TokenTree {
    fn from(v: Punct) -> Self {
        TokenTree::Punct(v)
    }
}
impl From<Literal> for TokenTree {
    fn from(v: Literal) -> Self {
        TokenTree::Literal(v)
    }
}

#[derive(Clone,Debug)]
pub struct Group {
    pub(crate) span: Span,
    pub(crate) delimiter: Delimiter,
    pub(crate) stream: TokenStream,
}
impl ::std::fmt::Display for Group
{
    fn fmt(&self, f: &mut ::std::fmt::Formatter) -> ::std::fmt::Result
    {
        match self.delimiter
        {
        Delimiter::Parenthesis => f.write_str("(")?,
        Delimiter::Brace => f.write_str("{")?,
        Delimiter::Bracket => f.write_str("[")?,
        Delimiter::None => {},
        }
        self.stream.fmt(f)?;
        match self.delimiter
        {
        Delimiter::Parenthesis => f.write_str(")")?,
        Delimiter::Brace => f.write_str("}")?,
        Delimiter::Bracket => f.write_str("]")?,
        Delimiter::None => f.write_str(" ")?,
        }
        Ok(())
    }
}

impl Group {
    pub fn new(delimiter: Delimiter, stream: TokenStream) -> Group {
        Group {
            span: Span::call_site(),
            delimiter,
            stream,
        }
    }
    pub fn delimiter(&self) -> Delimiter {
        self.delimiter
    }
    pub fn stream(&self) -> TokenStream {
        self.stream.clone()
    }

    pub fn span(&self) -> Span {
        self.span
    }
    pub fn set_span(&mut self, span: Span) {
        let _ = span;
    }
    pub fn span_open(&self) -> Span {
        self.span
    }
    pub fn span_close(&self) -> Span {
        self.span
    }
}

#[derive(Copy,Clone,Debug,Eq,PartialEq)]
pub enum Delimiter {
    Parenthesis,
    Brace,
    Bracket,
    None,
}

#[derive(Clone,Debug)]
pub struct Ident {
    pub(crate) span: Span,
    pub(crate) is_raw: bool,
    pub(crate) val: String,
}
impl Ident {
    pub fn new(string: &str, span: Span) -> Ident {
        Ident { span, is_raw: false, val: string.to_owned() }
    }
    // 1.47
    pub fn new_raw(string: &str, span: Span) -> Ident {
        Ident { span, is_raw: true, val: string.to_owned() }
    }
    pub fn span(&self) -> Span {
        self.span
    }
    pub fn set_span(&mut self, span: Span) {
        self.span = span;
    }
}
impl ::std::fmt::Display for Ident
{
    fn fmt(&self, f: &mut ::std::fmt::Formatter) -> ::std::fmt::Result
    {
        if self.is_raw {
            f.write_str("r#")?;
        }
        f.write_str(&self.val)?;
        Ok( () )
    }
}

#[derive(Clone,Debug)]
pub struct Punct {
    pub(crate) span: Span,
    pub(crate) spacing: Spacing,
    pub(crate) ch: char,
}
impl ::std::fmt::Display for Punct
{
    fn fmt(&self, f: &mut ::std::fmt::Formatter) -> ::std::fmt::Result
    {
        self.ch.fmt(f)?;
        match self.spacing
        {
        Spacing::Alone => f.write_str(" ")?,
        Spacing::Joint => {},
        }
        Ok(())
    }
}
impl Punct {
    pub fn new(ch: char, spacing: Spacing) -> Punct {
        Punct {
            span: Span::call_site(),
            spacing,
            ch,
        }
    }
    pub fn as_char(&self) -> char {
        self.ch
    }
    pub fn spacing(&self) -> Spacing {
        self.spacing
    }
    pub fn span(&self) -> Span {
        self.span
    }
    pub fn set_span(&mut self, span: Span) {
        self.span = span;
    }
}
impl ::std::cmp::PartialEq<char> for Punct {
    fn eq(&self, rhs: &char) -> bool {
        self.ch == *rhs
    }
}
impl ::std::cmp::PartialEq<Punct> for char {
    fn eq(&self, rhs: &Punct) -> bool {
        *self == rhs.ch
    }
}
#[derive(Copy,Clone,Debug,PartialEq,Eq)]
pub enum Spacing {
    Alone,
    Joint,
}

#[derive(Clone,Debug)]
pub struct Literal {
    pub(crate) span: Span,
    pub(crate) val: LiteralValue,
}
impl ::std::str::FromStr for Literal
{
    type Err = crate::lex::LexError;
    fn from_str(v: &str) -> Result<Self,Self::Err> {
        let mut ts = crate::TokenStream::from_str(v)?.inner;
        match &mut ts[..] {
        &mut [TokenTree::Literal(ref mut rv)] => Ok(::std::mem::replace(rv, Literal { span: Span::from_raw(0), val: LiteralValue::CharLit('\0') })),
        _ => Err(crate::lex::LexError { inner: "Wasn't a literal" }),
        }
    }
}
impl ::std::fmt::Display for Literal
{
    fn fmt(&self, f: &mut ::std::fmt::Formatter) -> ::std::fmt::Result
    {
        match self.val
        {
        LiteralValue::String(ref s) => write!(f, "{:?}", s)?,
        LiteralValue::ByteString(ref v) => {
            f.write_str("b\"")?;
            for &b in v {
                if b != b'"' && b >= b' ' && b <= b'z' {
                    (b as char).fmt(f)?;
                }
                else {
                    write!(f, "\\x{:02x}", b)?;
                }
            }
            f.write_str("\"")?;
            },
        LiteralValue::CharLit(v) => match v
            {
            '\'' => f.write_str("'\\''")?,
            '\0' => f.write_str("'\\0'")?,
            '\n' => f.write_str("'\\n'")?,
            ' ' ... 'z' => write!(f, "'{}'", v)?,
            _ => write!(f, "'\\u{:x}'", v as u32)?,
            },
        LiteralValue::UnsignedInt(v,sz) => {
            v.fmt(f)?;
            if sz == 0 {
            }
            else if sz == 1 {
                f.write_str("usize")?;
            }
            else {
                f.write_str("u")?;
                sz.fmt(f)?;
            }
            },
        LiteralValue::SignedInt(v,sz) => {
            v.fmt(f)?;
            if sz == 0 {
            }
            else if sz == 1 {
                f.write_str("isize")?;
            }
            else {
                f.write_str("i")?;
                sz.fmt(f)?;
            }
            },
        LiteralValue::Float(v, sz) => {
            v.fmt(f)?;
            if sz == 0 {
            }
            else {
                f.write_str("f")?;
                sz.fmt(f)?;
            }
            },
        }
        Ok( () )
    }
}
#[derive(Clone,Debug)]
#[derive(PartialEq)]    // Used for unit tests
pub(crate) enum LiteralValue {
    String(String),
    ByteString(Vec<u8>),
    CharLit(char),
    UnsignedInt(u128, u8),    // Value, size (0=?,8,16,32,64,128)
    SignedInt(i128, u8),    // Value, size (0=?,8,16,32,64,128)
    Float(f64, u8), // Value, size (0, 32, 64)
}
impl Literal {
    pub(crate) fn new_u(value: u128, bits: u8) -> Literal {
        Literal {
            span: Span::call_site(),
            val: LiteralValue::UnsignedInt(value, bits),
        }
    }
    pub(crate) fn new_s(value: i128, bits: u8) -> Literal {
        Literal {
            span: Span::call_site(),
            val: LiteralValue::SignedInt(value, bits),
        }
    }
    pub fn u8_suffixed(n: u8) -> Literal {
        Literal::new_u(n as u128, 8)
    }
    pub fn u16_suffixed(n: u16) -> Literal {
        Literal::new_u(n as u128, 16)
    }
    pub fn u32_suffixed(n: u32) -> Literal {
        Literal::new_u(n as u128, 32)
    }
    pub fn u64_suffixed(n: u64) -> Literal {
        Literal::new_u(n as u128, 64)
    }
    pub fn u128_suffixed(n: u128) -> Literal {
        Literal::new_u(n as u128, 128)
    }
    pub fn usize_suffixed(n: usize) -> Literal {
        Literal::new_u(n as u128, 1)
    }

    pub fn i8_suffixed(n: i8) -> Literal {
        Literal::new_s(n as i128, 8)
    }
    pub fn i16_suffixed(n: i16) -> Literal {
        Literal::new_s(n as i128, 16)
    }
    pub fn i32_suffixed(n: i32) -> Literal {
        Literal::new_s(n as i128, 32)
    }
    pub fn i64_suffixed(n: i64) -> Literal {
        Literal::new_s(n as i128, 64)
    }
    pub fn i128_suffixed(n: i128) -> Literal {
        Literal::new_s(n as i128, 128)
    }
    pub fn isize_suffixed(n: isize) -> Literal {
        Literal::new_s(n as i128, 1)
    }
    
    pub fn u8_unsuffixed(n: u8) -> Literal {
        Literal::new_u(n as u128, 0)
    }
    pub fn u16_unsuffixed(n: u16) -> Literal {
        Literal::new_u(n as u128, 0)
    }
    pub fn u32_unsuffixed(n: u32) -> Literal {
        Literal::new_u(n as u128, 0)
    }
    pub fn u64_unsuffixed(n: u64) -> Literal {
        Literal::new_u(n as u128, 0)
    }
    pub fn u128_unsuffixed(n: u128) -> Literal {
        Literal::new_u(n as u128, 0)
    }
    pub fn usize_unsuffixed(n: usize) -> Literal {
        Literal::new_u(n as u128, 0)
    }

    pub fn i8_unsuffixed(n: i8) -> Literal {
        Literal::new_s(n as i128, 0)
    }
    pub fn i16_unsuffixed(n: i16) -> Literal {
        Literal::new_s(n as i128, 0)
    }
    pub fn i32_unsuffixed(n: i32) -> Literal {
        Literal::new_s(n as i128, 0)
    }
    pub fn i64_unsuffixed(n: i64) -> Literal {
        Literal::new_s(n as i128, 0)
    }
    pub fn i128_unsuffixed(n: i128) -> Literal {
        Literal::new_s(n as i128, 0)
    }
    pub fn isize_unsuffixed(n: isize) -> Literal {
        Literal::new_s(n as i128, 0)
    }

    pub fn f32_suffixed(n: f32) -> Literal {
        Literal {
            span: Span::call_site(),
            val: LiteralValue::Float(n as f64, 32),
        }
    }
    pub fn f64_suffixed(n: f64) -> Literal {
        Literal {
            span: Span::call_site(),
            val: LiteralValue::Float(n, 64),
        }
    }
    pub fn f32_unsuffixed(n: f32) -> Literal {
        Literal {
            span: Span::call_site(),
            val: LiteralValue::Float(n as f64, 0),
        }
    }
    pub fn f64_unsuffixed(n: f64) -> Literal {
        Literal {
            span: Span::call_site(),
            val: LiteralValue::Float(n, 0),
        }
    }

    pub fn string(string: &str) -> Literal {
        Literal {
            span: Span::call_site(),
            val: LiteralValue::String(string.to_owned()),
        }
    }
    pub fn character(ch: char) -> Literal {
        Literal {
            span: Span::call_site(),
            val: LiteralValue::CharLit(ch),
        }
    }
    pub fn byte_string(bytes: &[u8]) -> Literal {
        Literal {
            span: Span::call_site(),
            val: LiteralValue::ByteString(bytes.to_owned()),
        }
    }

    pub fn span(&self) -> Span {
        self.span
    }
    pub fn set_span(&mut self, span: Span) {
        self.span = span;
    }
}

// Use for unit tests
#[cfg(test)]
pub(crate) fn tt_eq(a: &TokenTree, b: &TokenTree) -> bool
{
    match (a,b)
    {
    (&TokenTree::Group(ref a), &TokenTree::Group(ref b)) => {
        if a.delimiter != b.delimiter {
            return false;
        }
        if a.stream.inner.len() != b.stream.inner.len() {
            return false;
        }
        for (a,b) in Iterator::zip( a.stream.inner.iter(), b.stream.inner.iter() ) {
            if !tt_eq(a,b) {
                return false;
            }
        }
        true
        },
    (&TokenTree::Ident(ref a), &TokenTree::Ident(ref b)) => a.is_raw == b.is_raw && a.val == b.val,
    (&TokenTree::Punct(ref a), &TokenTree::Punct(ref b)) => a.ch == b.ch && a.spacing == b.spacing,
    (&TokenTree::Literal(ref a), &TokenTree::Literal(ref b)) => a.val == b.val,
    _ => false,
    }
}
