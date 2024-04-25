//! 

#[derive(Clone,Copy,Debug)]
pub struct Span(usize);
impl !Send for Span {}
impl !Sync for Span {}

static mut SPANS: Vec<Option<RealSpan>> = Vec::new();
static mut SPANS_COMPLETE: bool = false;
impl Span
{
    pub(crate) fn define(idx: usize, _parent: Option<Span>, source_file: SourceFile, lines: ::std::ops::Range<usize>, ofs: ::std::ops::Range<usize>) {
        let lh = unsafe {
            assert!(!SPANS_COMPLETE);
            &mut SPANS
            };
        while lh.len() <= idx {
            lh.push(None);
        }
        lh[idx] = Some(RealSpan {
            file: source_file,
            lines,
            bytes: ofs,
            });
    }
    pub(crate) fn freeze_definitions() {
        unsafe { SPANS_COMPLETE = true; }
    }
    pub(crate) fn from_raw(idx: usize) -> Self {
        Span(idx)
    }
}

impl Span
{
    pub fn call_site() -> Span {
        Span(1)
    }
    //pub fn def_site() -> Span {
    //    Span(1)
    //}
    // 1.45
    pub fn mixed_site() -> Span {
        Span(0)
    }
    // 1.45
    pub fn resolved_at(&self, _other: Span) -> Span {
        Span(0)
    }
    // 1.45
    pub fn located_at(&self, _other: Span) -> Span {
        Span(0)
    }

    // 1.66
    pub fn source_text(&self) -> Option<String> {
        None
    }

    // Unstable at 1.54
    pub fn source_file(&self) -> SourceFile {
        match unsafe { assert!(SPANS_COMPLETE); SPANS.get(self.0) } {
        Some(&Some(ref v)) => v.file.clone(),
        _ => panic!("Undefined span #{}", self.0),
        }
    }
}

/// The inner definition of a span
struct RealSpan {
    file: SourceFile,
    lines: ::std::ops::Range<usize>,
    bytes: ::std::ops::Range<usize>,
}


// Unstable at 1.54
#[derive(Clone)]
pub struct SourceFile( pub(crate) ::std::path::PathBuf, pub(crate) bool );
impl SourceFile
{
    pub fn path(&self) -> ::std::path::PathBuf {
        self.0.clone()
    }
    pub fn is_real(&self) -> bool {
        self.1
    }
}
