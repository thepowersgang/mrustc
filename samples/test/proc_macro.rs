// aux-build: proc_macro_crate.rs --crate-type proc-macro -g
extern crate proc_macro_crate;
fn main() {
    proc_macro_crate::proc_macro_fn!();
}
