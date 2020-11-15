
struct Reader<R> {
    inner: R,
}
impl<R: ::std::io::Read> Reader<R> {
    fn getb(&mut self) -> Option<u8> {
        let mut b = [0];
        match self.inner.read(&mut b)
        {
        Ok(1) => Some(b[0]),
        Ok(0) => None,
        Ok(_) => panic!("Bad byte count"),
        Err(e) => panic!("Error reading from stream - {}", e),
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
pub fn dump_token_stream<R: ::std::io::Read>(reader: R)
{
    let mut s = Reader { inner: reader };
    loop
    {
        let hdr_b = match s.getb()
            {
            Some(b) => b,
            None => break,
            };
        // TODO: leading span
        match hdr_b
        {
        0 => {
            let sym = s.get_string();
            if sym == "" {
                println!("EOF")
            }
            else {
                println!("SYM {}", sym);
            }
            },
        1 => println!("IDENT {}", s.get_string()),
        2 => println!("LIFETIME {}", s.get_string()),
        3 => println!("STRING {:?}", s.get_string()),
        4 => println!("BYTESTRING {:x?}", s.get_byte_vec()),
        5 => println!("CHAR {:?}", ::std::char::from_u32(s.get_i128v() as u32).expect("char lit")),
        6 => println!("UNSIGNED {val:?} ty={ty}", ty=s.getb().expect("getb int ty"), val=s.get_u128v()),
        7 => println!("SIGNED {val:?} ty={ty}", ty=s.getb().expect("getb int ty"), val=s.get_i128v()),
        8 => println!("FLOAT {val:?} ty={ty}", ty=s.getb().expect("getb float ty"), val=s.get_f64()),
        _ => panic!("Unknown tag byte: {:#x}", hdr_b),
        }
    }
}

fn main()
{
    {
        assert_eq!( Reader { inner: ::std::io::stdin().lock() }.getb(), Some(0) );
    }
    dump_token_stream(::std::io::stdin().lock());
}
