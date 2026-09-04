// Regression test: a `while let` binding that one match arm conditionally
// moves (via a method call) and another arm conditionally moves (via an
// assignment to an outer local) must still be dropped on the arms that
// don't move it. mrustc used to register the value's drop flag with the
// `match` pseudo-loop instead of the enclosing real loop, so the flag was
// only re-initialised on the path through one arm and the value leaked on
// the others (seen as rustc_expand's MatcherPos leaking Rc<Vec<NamedMatch>>).
use std::rc::Rc;

struct MP { idx: usize, _m: Rc<u32> }

fn run(p: &mut Vec<MP>, out: &mut Vec<MP>, kinds: &[u8], c: bool, d: bool) -> Option<usize> {
    let mut e: Option<MP> = None;
    while let Some(mp) = p.pop() {
        match kinds[mp.idx] {
            0 => { if c { out.push(mp); } }
            1 => { if d { e = Some(mp); } }
            _ => {}
        }
    }
    e.map(|m| m.idx)
}

fn main() {
    let kinds = [0u8, 1, 2, 0, 1];
    for &(c, d) in &[(true, true), (false, false), (true, false), (false, true)] {
        let shared = Rc::new(1u32);
        let mut p = Vec::new();
        let mut out = Vec::new();
        for i in 0..kinds.len() {
            p.push(MP { idx: i, _m: Rc::clone(&shared) });
        }
        let _ = run(&mut p, &mut out, &kinds, c, d);
        // Only the values still held in `out` may keep the Rc alive.
        assert_eq!(Rc::strong_count(&shared), 1 + out.len(), "leak with c={} d={}", c, d);
    }
}
