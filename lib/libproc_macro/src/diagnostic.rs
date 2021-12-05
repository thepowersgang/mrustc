#![cfg_attr(mrustc,unstable(proc_macro_diagnostic, issue="54150"))]

// NOTE: As of 1.54, this is unstable
#[derive(Debug,Clone)]
pub struct Diagnostic
{
    parent: Option<Box<Diagnostic>>,
    spans: Vec<crate::Span>,
    level: Level,
    message: String,
}

//#[non_exhaustive]
#[derive(Debug,Copy,Clone)]
pub enum Level
{
    Error,
    Warning,
    Note,
    Help,
}

/// Constructors
impl Diagnostic
{
    pub fn new<T: Into<String>>(level: Level, message: T) -> Diagnostic
    {
        Diagnostic {
            parent: None,
            spans: Vec::new(),
            level,
            message: message.into(),
        }
    }
    pub fn spanned<S, T>(spans: S, level: Level, message: T) -> Diagnostic
    where
        S: MultiSpan,
        T: Into<String>, 
    {
        Diagnostic {
            parent: None,
            spans: spans.into_spans(),
            level,
            message: message.into(),
        }
    }
}

/// Wrappers
impl Diagnostic
{
    fn wrapped<S, T>(self, level: Level, spans: S, message: T) -> Diagnostic
    where
        S: MultiSpan,
        T: Into<String>
    {
        Diagnostic {
            parent: Some(Box::new(self)),
            spans: spans.into_spans(),
            level,
            message: message.into(),
        }
    }
    // Wrappers
    pub fn span_error<S, T>(self, spans: S, message: T) -> Diagnostic
    where
        S: MultiSpan,
        T: Into<String>
    {
        self.wrapped(Level::Error, spans, message)
    }
    pub fn error<T: Into<String>>(self, message: T) -> Diagnostic
    {
        self.span_error(Vec::new(), message)
    }

    pub fn span_warning<S, T>(self, spans: S, message: T) -> Diagnostic
    where
        S: MultiSpan,
        T: Into<String>
    {
        self.wrapped(Level::Warning, spans, message)
    }
    pub fn warning<T: Into<String>>(self, message: T) -> Diagnostic
    {
        self.span_warning(Vec::new(), message)
    }

    pub fn span_note<S, T>(self, spans: S, message: T) -> Diagnostic
    where
        S: MultiSpan,
        T: Into<String>
    {
        self.wrapped(Level::Note, spans, message)
    }
    pub fn note<T: Into<String>>(self, message: T) -> Diagnostic
    {
        self.span_note(Vec::new(), message)
    }

    pub fn span_help<S, T>(self, spans: S, message: T) -> Diagnostic
    where
        S: MultiSpan,
        T: Into<String>
    {
        self.wrapped(Level::Help, spans, message)
    }
    pub fn help<T: Into<String>>(self, message: T) -> Diagnostic
    {
        self.span_help(Vec::new(), message)
    }
}

/// Output
impl Diagnostic
{
    pub fn emit(self)
    {
        if let Some(p) = self.parent {
            p.emit();
        }
        eprintln!("{:?} {}", self.level, self.message);
        for s in self.spans {
            eprintln!("From {:?}", s);
        }
    }
}

pub trait MultiSpan {
    fn into_spans(self) -> Vec<crate::Span>;
}
impl MultiSpan for  Vec<crate::Span> {
    fn into_spans(self) -> Vec<crate::Span> {
        self
    }
}
impl<'a> MultiSpan for &'a [crate::Span] {
    fn into_spans(self) -> Vec<crate::Span> {
        self.to_owned()
    }
}
impl MultiSpan for crate::Span {
    fn into_spans(self) -> Vec<crate::Span> {
        vec![self]
    }
}