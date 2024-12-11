//! mrustc's libproc_macro protocol - interface between the compiler and the macro process
//!
//! See also [crate::serialisation], which uses this to convert to public types

pub enum Token
{
    EndOfStream,
    Symbol(String),
    Ident(String),
    Lifetime(String),
    String(String),
    ByteString(Vec<u8>),
    Char(char),
    Unsigned(u128, u8),
    Signed(i128, u8),
    Float(f64, u8),

    /// A reference to a span
    SpanRef(usize),
    /// The definition of a span
    SpanDef(SpanDef),
}
pub struct SpanDef {
    pub idx: usize,
    pub parent_idx: usize,

    pub path: String,
    pub is_path_real: bool,
    pub start_line: usize,
    pub end_line: usize,
    pub start_ofs: usize,
    pub end_ofs: usize,
}

pub struct Reader<R>
{
    inner: R,
}
impl<R: ::std::io::Read> Reader<R> {
    pub fn new(r: R) -> Reader<R> {
        Reader {
            inner: r,
        }
    }
} 
impl<R: ::std::io::Read> Reader<R>
{
    pub fn read_ent(&mut self) -> Option<Token>
    {
        let hdr_b = match self.getb()
            {
            Some(b) => b,
            None => return None,
            };
        // TODO: leading span
        Some(match hdr_b
        {
        0 => Token::EndOfStream,
        1 => Token::Symbol(self.get_string()),
        2 => Token::Ident(self.get_string()),
        3 => Token::Lifetime(self.get_string()),
        4 => Token::String(self.get_string()),
        5 => Token::ByteString(self.get_byte_vec()),
        6 => Token::Char({
            let v = self.get_u128v();
            assert!(v < 0x10FFFF, "Protocol error: malformed char literal {:#x}", v);
            let v = v as u32;
            ::std::char::from_u32(v).expect("protocol: char lit invalid")
            }),
        7 => {
            let ty = self.getb().expect("getb int ty");
            let val = self.get_u128v();
            Token::Unsigned(val, ty)
            },
        8 => {
            let ty = self.getb().expect("getb int ty");
            let val = self.get_i128v();
            Token::Signed(val, ty)
            },
        9 => {
            let ty = self.getb().expect("getb int ty");
            let val = self.get_f64();
            Token::Float(val, ty)
            },
        10 => Token::SpanRef(self.get_u128v() as usize),
        11 => Token::SpanDef(SpanDef {
            idx: self.get_u128v() as usize,
            parent_idx: self.get_u128v() as usize,
            path: self.get_string(),
            is_path_real: self.getb().expect("getb span path_is_real") != 0,
            start_line: self.get_u128v() as usize,
            end_line: self.get_u128v() as usize,
            start_ofs: self.get_u128v() as usize,
            end_ofs: self.get_u128v() as usize,
            }),
        _ => panic!("Unknown tag byte: {:#x}", hdr_b),
        })
    }

    fn getb(&mut self) -> Option<u8> {
        let mut b = [0];
        match self.inner.read(&mut b)
        {
        Ok(1) => Some(b[0]),
        Ok(0) => None,
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

pub struct Writer<T>
{
    inner: T,
}
impl<T: ::std::io::Write> Writer<T>
{
    pub fn new(w: T) -> Writer<T> {
        Writer {
            inner: w,
        }
    }

    pub fn write_ent(&mut self, t: Token)
    {
        match t
        {
        Token::EndOfStream     => { self.putb(0); },
        Token::Symbol(v)       => { self.putb(1); self.put_bytes(v.as_bytes()); },
        Token::Ident(v)        => { self.putb(2); self.put_bytes(v.as_bytes()); },
        Token::Lifetime(v)     => { self.putb(3); self.put_bytes(v.as_bytes()); },
        Token::String(v)       => { self.putb(4); self.put_bytes(v.as_bytes()); },
        Token::ByteString(v)   => { self.putb(5); self.put_bytes(&v[..]); },
        Token::Char(v)         => { self.putb(6); self.put_u128v(v as u32 as u128); },
        Token::Unsigned(v, sz) => { self.putb(7); self.putb(sz); self.put_u128v(v); },
        Token::Signed(v, sz)   => { self.putb(8); self.putb(sz); self.put_i128v(v); },
        Token::Float(v, sz)    => { self.putb(9); self.putb(sz); self.put_f64(v); },

        Token::SpanRef(idx) => { self.putb(10); self.put_u128v(idx as u128); },
        Token::SpanDef(sd) => {
            self.putb(11);
            self.put_u128v(sd.idx as u128);
            self.put_u128v(sd.parent_idx as u128);
            self.put_bytes(sd.path.as_bytes());
            self.putb(sd.is_path_real as u8);
            self.put_u128v(sd.start_line as u128);
            self.put_u128v(sd.end_line   as u128);
            self.put_u128v(sd.start_ofs as u128);
            self.put_u128v(sd.end_ofs   as u128);
            },
        }
    }
    pub fn write_sym(&mut self, v: &[u8])
    {
        self.putb(1);   // "Symbol"
        self.put_bytes(v);
    }
    pub fn write_sym_1(&mut self, ch: char)
    {
        self.putb(1);   // "Symbol"
        self.putb(1);   // Length = 1 byte
        self.putb(ch as u8);
    }


    fn putb(&mut self, v: u8) {
        let buf = [v];
        self.inner.write(&buf).expect("");
    }
    fn put_u128v(&mut self, mut v: u128) {
        while v >= 128 {
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
