struct A([u32; 0]);
struct B([u8; 3], A); 
pub fn test(b: B) -> B { b }

fn main() {
}