// compile-flags: --test --edition 2021
// CRC32 instruction templates are taken from zlib-rs.
// Link: https://github.com/trifectatechfoundation/zlib-rs/blob/v0.5.1/zlib-rs/src/crc32/acle.rs
#[cfg(target_arch = "aarch64")]
mod aarch64 {
    use std::arch::asm;

    #[target_feature(enable = "crc")]
    unsafe fn crc32b(mut crc: u32, data: u8) -> u32 {
        asm!("crc32b {crc:w}, {crc:w}, {data:w}", crc = inout(reg) crc, data = in(reg) data);
        crc
    }
    #[target_feature(enable = "crc")]
    unsafe fn crc32h(mut crc: u32, data: u16) -> u32 {
        asm!("crc32h {crc:w}, {crc:w}, {data:w}", crc = inout(reg) crc, data = in(reg) data);
        crc
    }
    #[target_feature(enable = "crc")]
    unsafe fn crc32w(mut crc: u32, data: u32) -> u32 {
        asm!("crc32w {crc:w}, {crc:w}, {data:w}", crc = inout(reg) crc, data = in(reg) data);
        crc
    }
    #[target_feature(enable = "crc")]
    unsafe fn crc32d(mut crc: u32, data: u64) -> u32 {
        asm!("crc32x {crc:w}, {crc:w}, {data:x}", crc = inout(reg) crc, data = in(reg) data);
        crc
    }

    #[test]
    fn crc32_matches_zlib() {
        if !std::arch::is_aarch64_feature_detected!("crc") {
            return;
        }
        unsafe {
            assert_eq!(!crc32b(!0, b'a'), 0xe8b7be43);
            assert_eq!(!crc32h(!0, u16::from_le_bytes(*b"ab")), 0x9e83486d);
            assert_eq!(!crc32w(!0, u32::from_le_bytes(*b"abcd")), 0xed82cd11);
            assert_eq!(!crc32d(!0, u64::from_le_bytes(*b"abcdefgh")), 0xaeef2a50);
        }
    }
}
