use crate::TokenStream;
use crate::{Ident,Literal};
use crate::{Punct,Spacing};
use crate::Span;

struct CharStream<T: Iterator<Item=char>> {
    inner: ::std::iter::Peekable<T>,
    cur: Option<char>,
}
impl<T: Iterator<Item=char>> CharStream<T> {
    fn new(i: T) -> Self {
        CharStream {
            inner: i.peekable(),
            cur: Some(' '),
            }
    }
    fn consume(&mut self) -> Option<char> {
        self.cur = self.inner.next();
        self.cur
    }
    fn is_complete(&self) -> bool {
        self.cur.is_none()
    }
    fn cur(&self) -> char {
        self.cur.expect("CharStream::cur called with no current")
    }
    fn next(&mut self) -> Option<char> {
        self.inner.peek().cloned()
    }
}

static SYMS: [&[u8]; 53] = [
    b"!" as &[u8],
    b"!=",
    b"#",
    b"$",
    b"%", b"%=",
    b"&", b"&&", b"&=",
    b"(",
    b")",
    b"*", b"*=",
    b"+", b"+=",
    b",",
    b"-", b"-=", b"->",
    b".", b"..", b"...",
    b"/", b"/=",
    b":", b"::",
    b";",
    b"<", b"<-", b"<<", b"<<=", b"<=",
    b"=", b"==", b"=>",
    b">", b">=", b">>", b">>=",
    b"?",
    b"@",
    b"[",
    b"\\",
    b"]",
    b"^", b"^=",
    b"`",
    b"{",
    b"|", b"|=", b"||",
    b"}",
    b"~",
    ];

pub struct LexError {
    inner: &'static str,
}
impl ::std::fmt::Display for LexError {
    fn fmt(&self, f: &mut ::std::fmt::Formatter) -> ::std::fmt::Result {
        f.write_str(self.inner)
    }
}
impl ::std::fmt::Debug for LexError {
    fn fmt(&self, f: &mut ::std::fmt::Formatter) -> ::std::fmt::Result {
        write!(f, "LexError({})", self.inner)
    }
}
impl ::std::str::FromStr for TokenStream {
    type Err = LexError;
    fn from_str(src: &str) -> Result<TokenStream, LexError> {
        debug!("TokenStream::from_str({:?})\r", src);
        let mut rv: Vec<crate::TokenTree> = Vec::new();
        let mut it = CharStream::new(src.chars());

        fn err(s: &'static str) -> Result<TokenStream,LexError> {
            Err(LexError { inner: s })
        }

        fn get_ident<T: Iterator<Item=char>>(it: &mut CharStream<T>, mut s: String) -> String
        {
            let mut c = it.cur();
            while c == '_' || c.is_alphanumeric() || c.is_digit(10)
            {
                s.push(c);
                c = some_else!(it.consume() => break);
            }
            s
        }

        'outer: while ! it.is_complete()
        {
            let mut c = it.cur();

            if c.is_whitespace() {
                it.consume();
                continue ;
            }

            if c == '\''
            {
                it.consume();
                c = it.cur();
                if (c.is_alphabetic() || c == '_') && it.next().map(|x| x != '\'').unwrap_or(true) {
                    // Lifetime
                    let ident = get_ident(&mut it, String::new());
                    rv.push(Punct::new('\'', Spacing::Joint).into());
                    rv.push(Ident { is_raw: false, val: ident, span: crate::Span::call_site() }.into());
                }
                else {
                    // Char lit
                    if c == '\\' {
                        panic!("TODO: char literal with escape");
                    }
                    else {
                        rv.push(Literal::character(c).into());
                        match it.consume()
                        {
                        Some('\'') => {},
                        Some(c) => {
                            debug!("Stray charcter '{}'", c);
                            return err("Multiple characters in char literal");
                            },
                        None => {
                            return err("Unterminated char literal");
                            },
                        }
                    }
                }
            }
            else if c == '/' && it.next() == Some('/')
            {
                // Line comment
                while it.consume() != Some('\n') {
                }
            }
            else if c == '/' && it.next() == Some('*')
            {
                // Block comment
                panic!("TODO: Block comment");
            }
            else
            {
                // byte or raw string literals
                if c == 'b' || c == 'r' || c == '"' {
                    let mut c = c;
                    let is_byte = if c == 'b' {
                            c = some_else!(it.consume() => { rv.push(Ident::new("b", crate::Span::call_site()).into()); break });
                            true
                        } else {
                            false
                        };

                    if c == 'r'
                    {
                        // TODO: If this isn't a string, start parsing an ident instead.
                        let ident_str = if is_byte { "br" } else { "r" };
                        c = some_else!(it.consume() => { rv.push(Ident::new(ident_str.into(), crate::Span::call_site()).into()); break });
                        let mut hashes = 0;
                        while c == '#' {
                            hashes += 1;
                            c = some_else!(it.consume() => return err("rawstr eof"));
                        }

                        if c != '"' {
                            if hashes == 0 {
                                let s = get_ident(&mut it, ident_str.to_string());
                                rv.push(Ident { is_raw: false, val: s, span: crate::Span::call_site() }.into());
                            }
                            else {
                                rv.push(Ident::new(ident_str.into(), crate::Span::call_site()).into());
                            }
                            while hashes > 0 {
                                rv.push(Punct::new('#', if hashes == 1 { Spacing::Alone } else { Spacing::Joint }).into());
                                hashes -= 1;
                            }
                            continue 'outer;
                        }

                        let req_hashes = hashes;
                        let mut rawstr = String::new();
                        loop
                        {
                            c = some_else!(it.consume() => return err("Rawstr eof"));
                            if c == '"' {
                                let mut hashes = 0;
                                while hashes < req_hashes {
                                    c = some_else!(it.consume() => return err("rawstr eof"));
                                    if c != '#' { break ; }
                                    hashes += 1;
                                }

                                if hashes != req_hashes {
                                    rawstr.push('"');
                                    for _ in 0 .. hashes {
                                        rawstr.push('#');
                                    }
                                    rawstr.push(c);
                                }
                                else {
                                    break ;
                                }
                            }
                            else {
                                rawstr.push(c);
                            }
                        }

                        rv.push(Literal {
                            span: crate::Span::call_site(),
                            val: if is_byte {
                                crate::token_tree::LiteralValue::ByteString(rawstr.into_bytes())
                            } else {
                                crate::token_tree::LiteralValue::String(rawstr)
                            }
                        }.into());
                        continue 'outer;
                    }
                    else if c == '\''
                    {
                        // Byte character literal?
                        // NOTE: That b'foo is not `b` followed by `'foo`
                        assert!(is_byte);
                        panic!("TODO: Byte character literal");
                    }
                    else if c == '\"'
                    {
                        // String literal
                        let mut s = String::new();
                        loop
                        {
                            c = some_else!(it.consume() => return err("str eof"));
                            if c == '"' {
                                it.consume();
                                break ;
                            }
                            else if c == '\\' {
                                match some_else!(it.consume() => return err("str eof"))
                                {
                                c @ _ => panic!("Unknown escape in string {}", c),
                                }
                            }
                            else {
                                s.push(c);
                            }
                        }
                        rv.push(Literal {
                            span: crate::Span::call_site(),
                            val: if is_byte {
                                crate::token_tree::LiteralValue::ByteString(s.into_bytes())
                            } else {
                                crate::token_tree::LiteralValue::String(s)
                            }
                        }.into());
                        continue 'outer;
                    }
                    else
                    {
                        // Could be an ident starting with 'b', or it's just 'b'
                        // - Fall through
                        let ident = get_ident(&mut it, "b".into());
                        rv.push(Ident { span: Span::call_site(), is_raw: false, val: ident }.into());
                        continue 'outer;
                    }
                }

                // Identifier.
                if c.is_alphabetic() || c == '_'
                {
                    let ident = get_ident(&mut it, String::new());
                    if ident == "_" {
                        rv.push(Punct::new('_', Spacing::Alone).into());
                    }
                    else {
                        rv.push(Ident { span: Span::call_site(), is_raw: false, val: ident }.into());
                    }
                }
                else if c.is_digit(10)
                {
                    let base =
                        if c == '0' {
                            match it.consume()
                            {
                            Some('x') => { it.consume(); 16 },
                            Some('o') => { it.consume(); 8 },
                            Some('b') => { it.consume(); 2 },
                            None => {
                                rv.push(Literal::new_u(0, 0).into());
                                continue 'outer;
                                },
                            _ => 10,
                            }
                        }
                        else {
                            10
                        };
                    let mut v = 0;
                    let mut c = it.cur();
                    'int: loop
                    {
                        while c == '_' {
                            c = some_else!( it.consume() => { break 'int; } );
                        }
                        if c == 'u' || c == 'i' {
                            let s = get_ident(&mut it, String::new());
                            rv.push(match &*s
                                {
                                "u8"    => Literal::new_u(v,   8), "i8"    => Literal::new_s(v as i128,   8),
                                "u16"   => Literal::new_u(v,  16), "i16"   => Literal::new_s(v as i128,  16),
                                "u32"   => Literal::new_u(v,  32), "i32"   => Literal::new_s(v as i128,  32),
                                "u64"   => Literal::new_u(v,  64), "i64"   => Literal::new_s(v as i128,  64),
                                "u128"  => Literal::new_u(v, 128), "i128"  => Literal::new_s(v as i128, 128),
                                "usize" => Literal::new_u(v,   1), "isize" => Literal::new_s(v as i128,   1),
                                _ => return err("Unexpected integer suffix"),
                                }.into());
                            continue 'outer;
                        }
                        else if let Some(d) = c.to_digit(base) {
                            v *= base as u128;
                            v += d as u128;
                            c = some_else!( it.consume() => { break 'int; } );
                        }
                        else if c == '.' {
                            panic!("TODO: Floating point");
                        }
                        else {
                            break;
                        }
                    }
                    rv.push(Literal::new_u(v, 0).into());
                    continue 'outer;
                }
                // Punctuation?
                else if c as u32 <= 0xFF
                {
                    let mut start = match SYMS.iter().position(|v| v[0] == (c as u8))
                        {
                        Some(start) => start,
                        None => {
                            eprint!("Unknown operator character '{}'\r\n", c);
                            return err("Unknown operator")
                            },
                        };
                    let mut end = start+1;
                    while end < SYMS.len() && SYMS[end][0] == c as u8 {
                        end += 1;
                    }

                    let mut ofs = 1;
                    loop
                    {
                        let syms = &SYMS[start..end];
                        assert_eq!(ofs, syms[0].len(), "{:?}", syms[0]);
                        c = some_else!(it.consume() => break);
                        let step = match syms[1..].iter().position(|v| v[ofs] == (c as u8))
                            {
                            Some(s) => s+1,
                            None => break,
                            };
                        start += step;
                        end = start+1;
                        while end < syms.len() && syms[end][ofs] == c as u8 {
                            end += 1;
                        }
                        ofs += 1;
                    }
                    assert_eq!(SYMS[start].len(), ofs);
                    for (i,&c) in Iterator::enumerate(SYMS[start].iter())
                    {
                        let sep = if i == SYMS[start].len() - 1 { Spacing::Alone } else { Spacing::Joint };
                        rv.push(Punct::new(c as char, sep).into());
                    }
                }
                else
                {
                    return err("Unexpected character");
                }
            }
        }

        Ok(TokenStream {
            inner: rv,
            })
    }
}

#[cfg(test)]
mod tests {
    use crate::*;
    use ::std::str::FromStr;
    
    macro_rules! assert_tt_matches {
        ($have:expr, $exp:expr) => {{
            let e = $exp;
            let h = match $have
                {
                Some(h) => h,
                None => panic!("Unexpected end of stream (expecting {:?})", e),
                };
            assert!( crate::token_tree::tt_eq(&h, &e), "Expected {:?} got {:?}", e, h );
        }};
        ($have:expr) => {{
            match $have
            {
            Some(h) => panic!("Expected end of stream, found {:?}", h),
            None => {},
            }
        }};
    }

    #[test]
    fn lifetime()
    {
        let rv = TokenStream::from_str("foo::bar<'a>").expect("Failed to parse");

        let mut it = rv.inner.into_iter();
        assert_tt_matches!(it.next(), Ident::new("foo", Span::call_site()).into());
        assert_tt_matches!(it.next(), Punct::new(':', Spacing::Joint).into());
        assert_tt_matches!(it.next(), Punct::new(':', Spacing::Alone).into());
        assert_tt_matches!(it.next(), Ident::new("bar", Span::call_site()).into());
        assert_tt_matches!(it.next(), Punct::new('<', Spacing::Alone).into());
        assert_tt_matches!(it.next(), Punct::new('\'', Spacing::Joint).into());
        assert_tt_matches!(it.next(), Ident::new("a", Span::call_site()).into());
        assert_tt_matches!(it.next(), Punct::new('>', Spacing::Alone).into());
        assert_tt_matches!(it.next());
    }

    #[test]
    fn trailing_zero()
    {
        let rv = TokenStream::from_str("0").expect("Failed to parse");

        let mut it = rv.inner.into_iter();
        assert_tt_matches!(it.next(), Literal::new_u(0,0).into());
        assert_tt_matches!(it.next());
    }
    #[test]
    fn tuple_index_method()
    {
        let rv = TokenStream::from_str("key . 1 . def_id").expect("Failed to parse");
        let mut it = rv.inner.into_iter();
        assert_tt_matches!(it.next(), Ident::new("key", Span::call_site()).into());
        assert_tt_matches!(it.next(), Punct::new('.', Spacing::Alone).into());
        assert_tt_matches!(it.next(), Literal::new_u(1,0).into());
        assert_tt_matches!(it.next(), Punct::new('.', Spacing::Alone).into());
        assert_tt_matches!(it.next(), Ident::new("def_id", Span::call_site()).into());
        assert_tt_matches!(it.next());
    }
}