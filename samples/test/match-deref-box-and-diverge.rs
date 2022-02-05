// compile-flags: --test

// This test case is a simplified version of
// impl From<serde_json::Error> for io::Error

#[derive(Clone, Copy)]
struct IoError {
    inner: i32,
}

impl IoError {
fn new(err: Error) -> Self {
    unimplemented!()
}
}

pub struct Error {
    err: Box<ErrorCode>,
}

pub enum Category {
    Io,
    Eof,
}

fn from(j: Error) -> IoError {
    if let ErrorCode::Io(errx) = *j.err {
        errx
    } else {
        match Category::Io {
            Category::Io => unreachable!(),
            Category::Eof => IoError::new(j),
        }
    }
}

pub enum ErrorCode {
    Io(IoError),
    Other,
}

#[test]
fn test_match() {
    let io_error = IoError { inner: 123456789 };
    let error = Error { err: Box::new(ErrorCode::Io(io_error.clone())) };
    let result = from(error);
    assert_eq!(io_error.inner, result.inner);
}
