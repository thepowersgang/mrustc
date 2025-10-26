use ::std::convert::AsRef;
use ::std::ffi::OsStr;

pub fn var<K: AsRef<OsStr> + AsRef<str>>(key: K) -> Result<String, ::std::env::VarError> {
    // TODO: Signal to compiler that this environment variable should be added to the dependency file
    ::std::env::var(key)
}