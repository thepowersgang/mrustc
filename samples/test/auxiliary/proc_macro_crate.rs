// ignore-test
extern crate proc_macro;

#[proc_macro]
pub fn proc_macro_fn(ts: proc_macro::TokenStream) -> proc_macro::TokenStream {
    ts
}
