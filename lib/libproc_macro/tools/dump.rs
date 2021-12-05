//
//
//
#![allow(ellipsis_inclusive_range_patterns)]    // 1.19 doesn't support `..=`

#[allow(dead_code)]
#[path="../src/protocol.rs"]
mod protocol;

use protocol::Token;

pub fn dump_token_stream<R: ::std::io::Read>(reader: R)
{
    let mut s = protocol::Reader::new(reader);
    while let Some(v) = s.read_ent()
    {
        match v
        {
        Token::Symbol(s) => println!("SYM {}", s),
        Token::Ident(s) => println!("IDENT {}", s),
        Token::Lifetime(s) => println!("LIFETIME {}", s),
        Token::String(s) => println!("STRING {:?}", s),
        Token::ByteString(s) => println!("BYTESTRING {:?}", FmtByteString(&s)),
        Token::Char(c) => println!("CHAR {:?}", c),
        Token::Unsigned(val, ty) => println!("UNSIGNED {val:?} ty={ty}", val=val, ty=ty),
        Token::Signed(val, ty) => println!("SIGNED {val:?} ty={ty}", val=val, ty=ty),
        Token::Float(val, ty) => println!("FLOAT {val:?} ty={ty}", val=val, ty=ty),
        }
    }
}

struct FmtByteString<'a>(&'a [u8]);
impl<'a> std::fmt::Debug for FmtByteString<'a> {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        use std::fmt::Write;
        f.write_char('"')?;
        for &b in self.0 {
            match b
            {
            b'\\' => f.write_str("\\\\")?,
            b'"' => f.write_str("\\\"")?,
            0x20...0x7F => f.write_char(b as char)?,
            b => write!(f, "\\x{:02X}", b)?,
            }
        }
        f.write_char('"')?;
        Ok(())
    }
}

pub fn dump_token_stream_pretty<R: ::std::io::Read>(reader: R)
{
    let mut s = protocol::Reader::new(reader);
    while let Some(v) = s.read_ent()
    {
        match v
        {
        Token::Symbol(s) => print!("{} ", s),
        Token::Ident(s) => print!("{} ", s),
        Token::Lifetime(s) => print!("'{} ", s),
        Token::String(s) => print!("{:?}", s),
        Token::ByteString(s) => print!("{:?}", FmtByteString(&s)),
        Token::Char(c) => print!("'{:?}'", c),
        Token::Unsigned(val, ty) => print!("{val:?} /*ty={ty}*/", val=val, ty=ty),
        Token::Signed(val, ty) => print!("{val:?} /*ty={ty}*/", val=val, ty=ty),
        Token::Float(val, ty) => print!("{val:?} /*ty={ty}*/", val=val, ty=ty),
        }
    }
    println!("");
}

fn main()
{
    // Hacky argument parsing
    let mut is_pretty = false;
    let mut is_output = false;
    for a in ::std::env::args().skip(1)
    {
        if !a.starts_with("-")
        {
            // Free
            // TODO: Error?
            panic!("Unknown free argument {:?}", a);
        }
        else if !a.starts_with("--")
        {
            // Short
            for c in a.chars().skip(1)
            {
                match c
                {
                _ => panic!("Unknown short argument `-{}`", c),
                }
            }
        }
        else if a != "--"
        {
            // Long
            match &a[..]
            {
            "--is-output" => is_output = true,
            "--pretty" => is_pretty = true,
            _ => panic!("Unknown argument `{}`", a),
            }
        }
        else
        {
            // All free
        }
    }

    // If --is-output was passed, consume the leading NUL byte
    // - Maybe this should just be an empty symbol instead of a single NUL?
    if is_output
    {
        use std::io::Read;
        let mut b = [0];
        ::std::io::stdin().read_exact(&mut b).unwrap();
        assert_eq!(b[0], 0);
    }
    // 
    if is_pretty {
        dump_token_stream_pretty(::std::io::stdin().lock());
    }
    else {
        dump_token_stream(::std::io::stdin().lock());
    }
}
