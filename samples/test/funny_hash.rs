use std::hash::{Hash,Hasher};
extern {
    type OpaqueSliceContents;
}

#[repr(C)]
pub struct Slice<T> {
    len: usize,
    data: [T; 0],
    opaque: OpaqueSliceContents,
}
impl<T> Hash for Slice<T> {
    #[inline]
    fn hash<H: Hasher>(&self, s: &mut H) {
        (self as *const Slice<T>).hash(s)
    }
}


fn main() {
}

struct MyHasher {
    count: usize,
}
impl Hasher for MyHasher
{
    fn finish(&self) -> u64 {
        self.count as u64
    }
    fn write(&mut self, bytes: &[u8]) {
        self.count += bytes.len();
    }
}
fn do_hash(v: &Slice<u8>) {
    let mut mh = MyHasher { count: 0 };
    v.hash(&mut mh);
    assert_eq!( mh.finish() as usize, ::std::mem::size_of::<usize>() );
}
