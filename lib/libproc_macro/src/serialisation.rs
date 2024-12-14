use crate::*;

use crate::protocol::Token;
use crate::protocol::{Reader,Writer};

/// Receive a token stream from the compiler
pub fn recv_token_stream<R: ::std::io::Read>(reader: R) -> TokenStream
{
    let mut s = Reader::new(reader);
    return get_subtree(&mut s, "");

    fn get_subtree<R: ::std::io::Read>(s: &mut Reader<R>, end: &'static str) -> TokenStream {
        let mut toks: Vec<TokenTree> = Vec::new();
        while let Some(t) = s.read_ent()
        {
            let tt = match t
                {
                Token::SpanRef(_idx) => {
                    // Ignore - for now
                    continue
                    },
                Token::SpanDef(sd) => {
                    crate::Span::define(sd.idx,
                        if sd.parent_idx == 0 { None } else { Some(crate::Span::from_raw(sd.parent_idx - 1)) },
                        crate::span::SourceFile( sd.path.into(), sd.is_path_real ),
                        sd.start_line .. sd.end_line,
                        sd.start_ofs .. sd.end_ofs,
                        );
                    continue
                    },
                Token::EndOfStream if end == "" => return TokenStream { inner: toks, },
                Token::Symbol(ref s) if s == end => return TokenStream { inner: toks, },
                Token::Symbol(ref s) if s == "" => panic!("Unexpected end-of-stream marker"),
                Token::EndOfStream => panic!("Unexpected end-of-stream marker"),
                Token::Symbol(sym) => {
                    match &sym[..]
                    {
                    "{" => { Group::new(Delimiter::Brace, get_subtree(s, "}")).into() },
                    "[" => { Group::new(Delimiter::Bracket, get_subtree(s, "]")).into() },
                    "(" => { Group::new(Delimiter::Parenthesis, get_subtree(s, ")")).into() },
                    _ => {
                        let mut it = sym.chars();
                        let mut c = it.next().unwrap();
                        while let Some(nc) = it.next()
                        {
                            toks.push(Punct::new(c, Spacing::Joint).into());
                            c = nc;
                        }
                        Punct::new(c, Spacing::Alone).into()
                        },
                    }
                    },
                Token::Ident(val) => Ident { span: Span::call_site(), is_raw: false, val }.into(),
                Token::Lifetime(val) => {
                    toks.push(Punct::new('\'', Spacing::Joint).into());
                    Ident { span: Span::call_site(), is_raw: false, val }.into()
                    },
                Token::String(val) => Literal {
                    span: crate::Span::call_site(),
                    val: crate::token_tree::LiteralValue::String(val)
                    }.into(),
                Token::ByteString(val) => Literal {
                    span: crate::Span::call_site(),
                    val: crate::token_tree::LiteralValue::ByteString(val)
                    }.into(),
                Token::Char(ch) => Literal {
                    span: crate::Span::call_site(),
                    val: crate::token_tree::LiteralValue::CharLit(ch),
                    }.into(),
                Token::Unsigned(val, ty) => Literal {
                    span: crate::Span::call_site(),
                    val: crate::token_tree::LiteralValue::UnsignedInt(val, ty),
                    }.into(),
                Token::Signed(val, ty) => Literal {
                    span: crate::Span::call_site(),
                    val: crate::token_tree::LiteralValue::SignedInt(val, ty),
                    }.into(),
                Token::Float(val, ty) => Literal {
                    span: crate::Span::call_site(),
                    val: crate::token_tree::LiteralValue::Float(val, ty),
                    }.into(),
                };
            toks.push(tt);
        }
        panic!("Unexpected EOF")
    }
}

// --------------------------------------------------------------------
// 
// --------------------------------------------------------------------


/// Send a token stream back to the compiler
pub fn send_token_stream<T: ::std::io::Write>(out_stream: T, ts: TokenStream)
{
    fn inner<T: ::std::io::Write>(s: &mut Writer<T>, ts: TokenStream)
    {
        use crate::token_tree::LiteralValue;
        let mut it = ts.inner.into_iter();
        while let Some(t) = it.next()
        {
            match t
            {
            TokenTree::Group(sg) => {
                match sg.delimiter
                {
                Delimiter::None => {},
                Delimiter::Brace => s.write_sym_1('{'),
                Delimiter::Parenthesis => s.write_sym_1('('),
                Delimiter::Bracket => s.write_sym_1('['),
                }
                inner(s, sg.stream);
                match sg.delimiter
                {
                Delimiter::None => {},
                Delimiter::Brace => s.write_sym_1('}'),
                Delimiter::Parenthesis => s.write_sym_1(')'),
                Delimiter::Bracket => s.write_sym_1(']'),
                }
                },
            TokenTree::Ident(i) => s.write_ent(Token::Ident(i.val)),
            TokenTree::Punct(p) => {
                if p.ch == '\'' {
                    // Get next, must be ident, push lifetime
                    let v = match it.next()
                        {
                        Some(TokenTree::Ident(Ident { val: v, .. })) => v,
                        _ => panic!("Punct('\\'') not floowed by an ident"),
                        };
                    s.write_ent(Token::Lifetime(v));
                }
                else if p.spacing == Spacing::Alone {
                    s.write_sym_1(p.ch);
                }
                else {
                    use std::io::Write;
                    // Consume punct until Spacing::Alone (error for any other)
                    let mut buf = [0u8; 4];
                    let mut c = ::std::io::Cursor::new(&mut buf[..]);
                    write!(&mut c, "{}", p.ch).expect("Overly-long punctuation sequence");
                    while match it.next()
                        {
                        Some(TokenTree::Punct(p)) => {
                            write!(&mut c, "{}", p.ch).expect("Overly-long punctuation sequence");
                            p.spacing == Spacing::Joint
                            },
                        _ => panic!("Punct(Joint) not floowed by another Punct"),
                        }
                    {
                    }
                    s.write_sym(&c.get_ref()[..c.position() as usize]);
                }
                },
            TokenTree::Literal(Literal { val: v, .. }) => s.write_ent(match v
                {
                LiteralValue::String(v) => Token::String(v),
                LiteralValue::ByteString(v) => Token::ByteString(v),
                LiteralValue::CharLit(v) => Token::Char(v),
                LiteralValue::UnsignedInt(v, sz) => Token::Unsigned(v, sz),
                LiteralValue::SignedInt(v, sz)   => Token::Signed(v, sz),
                LiteralValue::Float(v, sz)       => Token::Float(v, sz),
                }),
            }
        }
    }

    let mut s = Writer::new(out_stream);
    // Send the token stream
    inner(&mut s, ts);
    // Empty symbol indicates EOF
    s.write_sym(b"");
}


#[cfg(test)]
mod write_tests {
    use crate::*;

    #[test]
    fn empty()
    {
        let mut out = Vec::new();
        super::send_token_stream(&mut out, TokenStream::default());

        assert_eq!(out, &[
            0,0,
            ]);
    }

    #[test]
    fn symbols()
    {
        let mut out = Vec::new();
        super::send_token_stream(&mut out, TokenStream {
            inner: vec![
                Punct::new('<', Spacing::Joint).into(),
                Punct::new('<', Spacing::Alone).into(),

                Punct::new('<', Spacing::Alone).into(),
            ]
            });

        assert_eq!(out, &[
            0,2,b'<',b'<',
            0,1,b'<',
            0,0,
            ]);
    }

    #[test]
    fn lifetime()
    {
        let mut out = Vec::new();
        super::send_token_stream(&mut out, TokenStream {
            inner: vec![
                Punct::new('\'', Spacing::Joint).into(),
                Ident::new("a", Span::call_site()).into(),
            ]
            });

        assert_eq!(out, &[
            2, 1, b'a', // Lifetime
            0,0,    // Terminator
            ]);
    }
}
#[cfg(test)]
mod read_tests {
    use crate::*;

    macro_rules! assert_tt_matches {
        ($have:expr, $exp:expr) => {{
            let e = $exp;
            let h = match $have
                {
                Some(h) => h,
                None => panic!("Unexpected end of stream (expecting {:?})", e),
                };
            assert!( crate::token_tree::tt_eq(&h, &e) );
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
        let rv = super::recv_token_stream(&[
            2, 1, b'a', // Lifetime
            0,0,    // Terminator
            ][..]);
        let mut it = rv.inner.into_iter();
        assert_tt_matches!(it.next(), Punct::new('\'', Spacing::Joint).into());
        assert_tt_matches!(it.next(), Ident::new("a", Span::call_site()).into());
        assert_tt_matches!(it.next());
    }
}
