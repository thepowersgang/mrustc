use crate::*;

struct Reader<R> {
    inner: R,
}
impl<R: ::std::io::Read> Reader<R> {
    fn getb(&mut self) -> Option<u8> {
        let mut b = [0];
        match self.inner.read(&mut b)
        {
        Ok(1) => Some(b[0]),
        Ok(0) => panic!("Unexpected EOF reading from stdin"),
        Ok(_) => panic!("Bad byte count"),
        Err(e) => panic!("Error reading from stdin - {}", e),
        }
    }
    fn get_u128v(&mut self) -> u128 {
        let mut ofs = 0;
        let mut raw_rv = 0u128;
        loop
        {
            let b = self.getb().unwrap();
            raw_rv |= ((b & 0x7F) as u128) << ofs;
            if b < 128 {
                break;
            }
            assert!(ofs < 18*7);  // at most 18 bytes needed for a i128
            ofs += 7;
        }
        raw_rv
    }
    fn get_i128v(&mut self) -> i128 {
        let raw_rv = self.get_u128v();
        // Zig-zag encoding (0 = 0, 1 = -1, 2 = 1, ...)
        if raw_rv & 1 != 0 {
            -( (raw_rv >> 1) as i128 + 1 )
        }
        else {
            (raw_rv >> 1) as i128
        }
    }
    fn get_byte_vec(&mut self) -> Vec<u8> {
        let size = self.get_u128v();
        assert!(size < (1<<30));
        let size = size as usize;
        let mut buf = vec![0u8; size];
        match self.inner.read_exact(&mut buf)
        {
        Ok(_) => {},
        Err(e) => panic!("Error reading from stdin get_byte_vec({}) - {}", size, e),
        }

        buf
    }
    fn get_string(&mut self) -> String {
        let raw = self.get_byte_vec();
        String::from_utf8(raw).expect("Invalid UTF-8 passed from compiler")
    }
    fn get_f64(&mut self) -> f64 {
        let mut buf = [0u8; 8];
        match self.inner.read_exact(&mut buf)
        {
        Ok(_) => {},
        Err(e) => panic!("Error reading from stdin - {}", e),
        }
        unsafe {
            ::std::mem::transmute(buf)
        }
    }
}

/// Receive a token stream from the compiler
pub fn recv_token_stream<R: ::std::io::Read>(reader: R) -> TokenStream
{
    fn get_subtree<R: ::std::io::Read>(s: &mut Reader<R>, end: &'static str) -> TokenStream {
        let mut toks: Vec<TokenTree> = Vec::new();
        loop
        {
            let hdr_b = some_else!( s.getb() => break );
            // TODO: leading span
            let t = match hdr_b
                {
                0 => {
                    let sym = s.get_string();
                    if sym == end {
                        break;
                    }
                    match &sym[..]
                    {
                    "" => panic!(""),
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
                1 => Ident { span: Span::call_site(), is_raw: false, val: s.get_string() }.into(),
                2 => {
                    toks.push(Punct::new('\'', Spacing::Joint).into());
                    Ident { span: Span::call_site(), is_raw: false, val: s.get_string() }.into()
                    },
                3 => Literal {
                    span: crate::Span::call_site(),
                    val: crate::token_tree::LiteralValue::String(s.get_string())
                    }.into(),
                4 => Literal {
                    span: crate::Span::call_site(),
                    val: crate::token_tree::LiteralValue::ByteString( s.get_byte_vec())
                    }.into(),
                5 => Literal {
                    span: crate::Span::call_site(),
                    val: crate::token_tree::LiteralValue::CharLit(::std::char::from_u32(s.get_i128v() as u32).expect("char lit")),
                    }.into(),
                6 => {
                    let ty = s.getb().expect("getb int ty");
                    Literal {
                        span: crate::Span::call_site(),
                        val: crate::token_tree::LiteralValue::UnsignedInt(s.get_u128v(), ty),
                        }.into()
                    },
                7 => {
                    let ty = s.getb().expect("getb int ty");
                    Literal {
                        span: crate::Span::call_site(),
                        val: crate::token_tree::LiteralValue::SignedInt(s.get_i128v(), ty),
                        }.into()
                    },
                8 => {
                    let ty = s.getb().expect("getb float ty");
                    Literal {
                        span: crate::Span::call_site(),
                        val: crate::token_tree::LiteralValue::Float(s.get_f64(), ty),
                        }.into()
                    }
                _ => panic!("Unknown tag byte"),
                };
            toks.push(t);
            //eprintln!("> {:?}\r", toks.last().unwrap());
        }
        TokenStream {
            inner: toks,
            }
    }

    let mut s = Reader { inner: reader };
    get_subtree(&mut s, "")
}

// --------------------------------------------------------------------
// 
// --------------------------------------------------------------------

struct Writer<T>
{
    inner: T,
}
impl<T: ::std::io::Write> Writer<T> {
    fn putb(&mut self, v: u8) {
        let buf = [v];
        self.inner.write(&buf).expect("");
    }
    fn put_u128v(&mut self, mut v: u128) {
        while v > 128 {
            self.putb( (v & 0x7F) as u8 | 0x80 );
            v >>= 7;
        }
        self.putb( (v & 0x7F) as u8 );
    }
    fn put_i128v(&mut self, v: i128) {
        if v < 0 {
            self.put_u128v( (((v + 1) as u128) << 1) | 1 );
        }
        else {
            self.put_u128v( (v as u128) << 1 );
        }
    }
    fn put_bytes(&mut self, v: &[u8]) {
        self.put_u128v(v.len() as u128);
        self.inner.write(v).expect("");
    }
    fn put_f64(&mut self, v: f64) {
        let buf: [u8; 8] = unsafe { ::std::mem::transmute(v) };
        self.inner.write(&buf).expect("");
    }
}


/// Send a token stream back to the compiler
pub fn send_token_stream<T: ::std::io::Write>(out_stream: T, ts: TokenStream)
{
    impl<T: ::std::io::Write> Writer<T> {
        fn write_sym(&mut self, v: &[u8]) {
            self.putb(0);
            self.put_bytes(v);
        }
        fn write_sym_1(&mut self, ch: char)
        {
            self.putb(0);
            self.putb(ch as u8);
        }
    }

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
            TokenTree::Ident(i) => {
                s.putb(1); s.put_bytes(i.val.as_bytes());
                },
            TokenTree::Punct(p) => {
                if p.ch == '\'' {
                    // Get next, must be ident, push lifetime
                    let v = match it.next()
                        {
                        Some(TokenTree::Ident(Ident { val: v, .. })) => v,
                        _ => panic!("Punct('\\'') not floowed by an ident"),
                        };
                    s.putb(2); s.put_bytes(v.as_bytes());
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
            TokenTree::Literal(Literal { val: v, .. }) => match v
                {
                LiteralValue::String(v) => { s.putb(3); s.put_bytes(v.as_bytes()); },
                LiteralValue::ByteString(v) => { s.putb(4); s.put_bytes(&v[..]); },
                LiteralValue::CharLit(v) => { s.putb(5); s.put_u128v(v as u32 as u128); },
                LiteralValue::UnsignedInt(v, sz) => { s.putb(6); s.putb(sz); s.put_u128v(v); },
                LiteralValue::SignedInt(v, sz)   => { s.putb(7); s.putb(sz); s.put_i128v(v); },
                LiteralValue::Float(v, sz)       => { s.putb(8); s.putb(sz); s.put_f64(v); },
                },
            }
        }
    }

    let mut s = Writer { inner: out_stream };
    // Send the token stream
    inner(&mut s, ts);
    // Empty symbol indicates EOF
    s.putb(0); s.putb(0);
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