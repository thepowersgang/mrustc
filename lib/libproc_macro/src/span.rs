//! 

#[derive(Clone,Copy,Debug)]
pub struct Span {
}
//impl !Send for Span {}
//impl !Sync for Span {}

impl Span
{
	pub fn call_site() -> Span {
		Span {
		}
	}
	// 1.45
	pub fn mixed_site() -> Span {
		Span {
		}
	}
	// 1.45
	pub fn resolved_at(&self, _other: Span) -> Span {
		Span {
		}
	}
	// 1.45
	pub fn located_at(&self, _other: Span) -> Span {
		Span {
		}
	}

    // 1.66
    pub fn source_text(&self) -> Option<String> {
        None
    }
}
