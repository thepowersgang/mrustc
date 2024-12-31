// skip-codegen	(aka no-run)
// mrustc #320 - Leading digit in crate name
#![crate_type = "lib"]
pub fn get_u16_from_unaligned_manual(data: [u8; 1]) -> u8 {
    data[0] << 16
}
