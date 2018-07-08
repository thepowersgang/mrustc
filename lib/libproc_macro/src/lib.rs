// MRustC custom version of libproc_macro
//
// Unlike the rustc version, this one is designed to live complely detached from its compiler.

//#[macro_use]
//extern crate log;
//extern crate env_logger;

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

use std::fmt;
use std::str::FromStr;

pub struct TokenStream {
    inner: Vec<Token>,
}

#[derive(Debug)]
enum Token {
    Symbol(String),
    Ident(String),
    Lifetime(String),
    String(String),
    ByteString(Vec<u8>),
    CharLit(char),
    UnsignedInt(u128, u8),    // Value, size (0=?,8,16,32,64,128)
    SignedInt(i128, u8),    // Value, size (0=?,8,16,32,64,128)
    Float(f64, u8), // Value, size (0, 32, 64)
    Fragment(FragmentType, u64),    // Type and a key
}
#[repr(u8)]
#[derive(Copy,Clone,Debug)] // TODO: Is this just a mrustc thing?
enum FragmentType {
    Ident = 0,
    Tt = 1,

    Path = 2,
    Type = 3,

    Expr = 4,
    Statement = 5,
    Block = 6,
    Pattern = 7,
}

impl From<char> for Token {
    fn from(v: char) -> Token {
        Token::String(v.to_string())
    }
}

macro_rules! some_else {
    ($e:expr => $alt:expr) => {match $e { Some(v) => v, None => $alt }};
}

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
        self.cur.unwrap()
    }
    fn next(&self) -> Option<char> {
        self.inner.peek().cloned()
    }
}

pub struct LexError {
    inner: &'static str,
}
impl ::std::fmt::Debug for LexError {
    fn fmt(&self, f: &mut ::std::fmt::Formatter) -> ::std::fmt::Result {
        write!(f, "LexError({})", self.inner)
    }
}
impl FromStr for TokenStream {
    type Err = LexError;
    fn from_str(src: &str) -> Result<TokenStream, LexError> {
        debug!("TokenStream::from_str({:?})\r", src);
        let rv = Vec::new();
        let mut it = CharStream::new(src.chars());

        fn err(s: &'static str) -> Result<TokenStream,LexError> {
            Err(LexError { inner: s })
        }

        fn get_ident<T: Iterator<Item=char>>(it: &mut CharStream<T>, mut s: String) -> String
        {
            let mut c = it.cur();
            while c.is_xid_continue() || c.is_digit(10)
            {
                s.push(c);
                c = some_else!(it.consume() => break);
            }
            s
        }

        'outer: while ! it.is_complete()
        {
            let c = it.cur();

            if c.is_whitespace() {
                it.consume();
                continue ;
            }

            let syms = [
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
            if c == '\''
            {
                it.consume();
                c = it.cur();
                if (c.is_xid_start() || c == '_') && it.next().map(|x| x != '\'').unwrap_or(true) {
                    // Lifetime
                    let ident = get_ident(&mut it, String::new());
                    rv.push(Token::Lifetime(ident))
                }
                else {
                    // Char lit
                    if c == '\\' {
                        panic!("TODO: char literal with escape");
                    }
                    else {
                        rv.push(Token::CharLit(c));
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
                            c = some_else!(it.consume() => { rv.push(Token::Ident("b".into())); break });
                            true
                        } else {
                            false
                        };

                    if c == 'r'
                    {
                        // TODO: If this isn't a string, start parsing an ident instead.
                        let ident_str = if is_byte { "br" } else { "r" };
                        c = some_else!(it.consume() => { rv.push(Token::Ident(ident_str.into())); break });
                        let mut hashes = 0;
                        while c == '#' {
                            hashes += 1;
                            c = some_else!(it.consume() => return err("rawstr eof"));
                        }

                        if c != '"' {
                            if hashes == 0 {
                                let s = get_ident(&mut it, ident_str.to_string());
                                rv.push(Token::Ident(s));
                            }
                            else {
                                rv.push(Token::Ident(ident_str.into()));
                            }
                            while hashes > 0 {
                                rv.push(Token::Symbol("#".into()));
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

                        rv.push(if is_byte { Token::ByteString(rawstr.into_bytes()) } else { Token::String(rawstr) });
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
                        rv.push(if is_byte { Token::ByteString(s.into_bytes()) } else { Token::String(s) });
                        continue 'outer;
                    }
                    else
                    {
                        // Could be an ident starting with 'b', or it's just 'b'
                        // - Fall through
                        let ident = get_ident(&mut it, "b".into());
                        rv.push(Token::Ident(ident));
                        continue 'outer;
                    }
                }

                // Identifier.
                if c.is_xid_start() || c == '_'
                {
                    let ident = get_ident(&mut it, String::new());
                    if ident == "_" {
                        rv.push(Token::Symbol(ident));
                    }
                    else {
                        rv.push(Token::Ident(ident));
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
                            match &*s
                            {
                            "u8"    => rv.push(Token::UnsignedInt(v,   8)), "i8"    => rv.push(Token::SignedInt(v as i128,   8)),
                            "u16"   => rv.push(Token::UnsignedInt(v,  16)), "i16"   => rv.push(Token::SignedInt(v as i128,  16)),
                            "u32"   => rv.push(Token::UnsignedInt(v,  32)), "i32"   => rv.push(Token::SignedInt(v as i128,  32)),
                            "u64"   => rv.push(Token::UnsignedInt(v,  64)), "i64"   => rv.push(Token::SignedInt(v as i128,  64)),
                            "u128"  => rv.push(Token::UnsignedInt(v, 128)), "i128"  => rv.push(Token::SignedInt(v as i128, 128)),
                            "usize" => rv.push(Token::UnsignedInt(v,   1)), "isize" => rv.push(Token::SignedInt(v as i128,   1)),
                            _ => return err("Unexpected integer suffix"),
                            }
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
                    rv.push(Token::UnsignedInt(v, 0));
                    continue 'outer;
                }
                // Punctuation?
                else if c as u32 <= 0xFF
                {
                    let mut start = match syms.iter().position(|v| v[0] == (c as u8))
                        {
                        Some(start) => start,
                        None => {
                            eprint!("Unknown operator character '{}'\r\n", c);
                            return err("Unknown operator")
                            },
                        };
                    let mut end = start+1;
                    while end < syms.len() && syms[end][0] == c as u8 {
                        end += 1;
                    }

                    let mut ofs = 1;
                    loop
                    {
                        let syms = &syms[start..end];
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
                    assert_eq!(syms[start].len(), ofs);
                    rv.push(Token::Symbol(::std::str::from_utf8(syms[start]).unwrap().into()));
                }
                else
                {
                    return err("Unexpectec character");
                }
            }
        }

        Ok(TokenStream {
            inner: rv,
            })
    }
}


#[stable(feature = "proc_macro_lib", since = "1.15.0")]
impl fmt::Display for TokenStream {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        for v in &self.inner
        {
            f.write_str(" ")?;
            match v
            {
            &Token::Symbol(ref s) => s.fmt(f)?,
            //&Token::Keyword(ref s) => s.fmt(f)?,
            &Token::Ident(ref s) => s.fmt(f)?,
            &Token::Lifetime(ref s) => write!(f, "'{}", s)?,
            &Token::String(ref s) => write!(f, "{:?}", s)?,
            &Token::ByteString(ref v) => {
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
            &Token::CharLit(v) => match v
                {
                '\'' => f.write_str("'\\''")?,
                '\0' => f.write_str("'\\0'")?,
                '\n' => f.write_str("'\\n'")?,
                ' ' ... 'z' => write!(f, "'{}'", v)?,
                _ => write!(f, "'\\u{:x}'", v as u32)?,
                },
            &Token::UnsignedInt(v,sz) => {
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
            &Token::SignedInt(v,sz) => {
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
            &Token::Float(v, sz) => {
                v.fmt(f)?;
                if sz == 0 {
                }
                else {
                    f.write_str("f")?;
                    sz.fmt(f)?;
                }
                },
            &Token::Fragment(ty, key) => {
                write!(f, "_{}_{:x}", ty as u8, key)?;
                },
            }
        }
        Ok( () )
    }
}

/// Receive a token stream from the compiler
pub fn recv_token_stream() -> TokenStream
{
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

    let mut s = Reader { inner: ::std::io::stdin().lock() };
    let mut toks = Vec::new();
    loop
    {
        let hdr_b = some_else!( s.getb() => break );
        toks.push(match hdr_b
            {
            0 => {
                let sym = s.get_string();
                if sym == "" { break ; }
                Token::Symbol( sym )
                },
            1 => Token::Ident( s.get_string() ),
            2 => Token::Lifetime( s.get_string() ),
            3 => Token::String( s.get_string() ),
            4 => Token::ByteString( s.get_byte_vec() ),
            5 => Token::CharLit(::std::char::from_u32(s.get_i128v() as u32).expect("char lit")),
            6 => {
                let ty = s.getb().expect("getb int ty");
                Token::UnsignedInt(s.get_u128v(), ty)
                },
            7 => {
                let ty = s.getb().expect("getb int ty");
                Token::SignedInt(s.get_i128v(), ty)
                },
            8 => {
                let ty = s.getb().expect("getb float ty");
                Token::Float(s.get_f64(), ty)
                }
            });
        //eprintln!("> {:?}\r", toks.last().unwrap());
    }
    TokenStream {
        inner: toks,
        }
}
/// Send a token stream back to the compiler
pub fn send_token_stream(ts: TokenStream)
{
    struct Writer<T> {
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

    let mut s = Writer { inner: ::std::io::stdout().lock() };

    for t in &ts.inner
    {
        //eprintln!("{:?}\r", t);
        match t
        {
        &Token::Symbol(ref v)   => { s.putb(0); s.put_bytes(v.as_bytes()); },
        &Token::Ident(ref v)    => { s.putb(1); s.put_bytes(v.as_bytes()); },
        &Token::Lifetime(ref v) => { s.putb(2); s.put_bytes(v.as_bytes()); },
        &Token::String(ref v)      => { s.putb(3); s.put_bytes(v.as_bytes()); },
        &Token::ByteString(ref v)  => { s.putb(4); s.put_bytes(&v[..]); },
        &Token::CharLit(v)         => { s.putb(5); s.put_u128v(v as u32 as u128); },
        &Token::UnsignedInt(v, sz) => { s.putb(6); s.putb(sz); s.put_u128v(v); },
        &Token::SignedInt(v, sz)   => { s.putb(7); s.putb(sz); s.put_i128v(v); },
        &Token::Float(v, sz)       => { s.putb(8); s.putb(sz); s.put_f64(v); },
        &Token::Fragment(ty, key)  => { s.putb(9); s.putb(ty as u8); s.put_u128v(key as u128); },
        }
    }

    // Empty symbol indicates EOF
    s.putb(0); s.putb(0);
    drop(s);
    ::std::io::Write::flush(&mut ::std::io::stdout());
}

pub struct MacroDesc
{
    name: &'static str,
    handler: fn(TokenStream)->TokenStream,
}

pub fn main(macros: &[MacroDesc])
{
    //::env_logger::init();

    let mac_name = ::std::env::args().nth(1).expect("Was not passed a macro name");
    //eprintln!("Searching for macro {}\r", mac_name);
    for m in macros
    {
        if m.name == mac_name {
            use std::io::Write;
            ::std::io::stdout().write(&[0]);
            ::std::io::stdout().flush();
            debug!("Waiting for input\r");
            let input = recv_token_stream();
            debug!("INPUT = `{}`\r", input);
            let output = (m.handler)( input );
            debug!("OUTPUT = `{}`\r", output);
            send_token_stream(output);
            note!("Done");
            return ;
        }
    }
    panic!("Unknown macro name '{}'", mac_name);
}

