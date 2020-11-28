//
//
//
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
        Token::ByteString(s) => println!("BYTESTRING {:x?}", s),
        Token::Char(c) => println!("CHAR {:?}", c),
        Token::Unsigned(val, ty) => println!("UNSIGNED {val:?} ty={ty}", val=val, ty=ty),
        Token::Signed(val, ty) => println!("SIGNED {val:?} ty={ty}", val=val, ty=ty),
        Token::Float(val, ty) => println!("FLOAT {val:?} ty={ty}", val=val, ty=ty),
        }
    }
}

fn main()
{
    // Hacky argument parsing
    let mut is_output = false;
    for a in ::std::env::args().skip(1)
    {
        if !a.starts_with("-")
        {
            // Free
            // TODO: Error?
        }
        else if !a.starts_with("--")
        {
            // Short
            for c in a.chars().skip(1)
            {
                match c
                {
                _ => {},
                }
            }
        }
        else if a != "--"
        {
            // Long
            match &a[..]
            {
            "--is-output" => is_output = true,
            _ => {},
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
    dump_token_stream(::std::io::stdin().lock());
}
