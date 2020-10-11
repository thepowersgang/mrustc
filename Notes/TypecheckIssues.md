% Typecheck Problem Cases

# `git2::Diff::foreach`
Problem: Calling FFI via a macro leads to under-constrained type

# `getopts::<something>` ...
Problem: Don't remember, but it caused problems.

# `compiler_builtins::float::add`
Problem: Under-constrained types leading to all integer types (plus an associated type) being valid

```
Typecheck Expressions-      check_ivar_poss: >> (513 final)
Typecheck Expressions-       `anonymous-namespace'::check_ivar_poss: 513: possible_tys =
Typecheck Expressions-       `anonymous-namespace'::check_ivar_poss: 513: bounds = <F/*M:0*/ as ::"compiler_builtins"::float::Float>::Int/*O*/, usize, isize, u32, i32, u64, i64, u128, i128
```

Solution:
- If only bounds are present in `check_ivar_poss`, select the first bounded type
  - First will pick `usize` before other types, and then work up in size.
  - Also ends up preferencing associate types/bounded types
  
# `<::"std"::sys_common::process::CommandEnv<K/*I:0*/,>/*S*/>::capture`
Problem: Wrong type picked in the bounded only fallback.

Solution:
- Gate the fallback to only apply in final fallback.

# `<::"url-1_7_1"::form_urlencoded::Serializer<T/*I:0*/,>/*S*/>::extend_pairs`
Problem: 

```
Typecheck Expressions-     check_ivar_poss: >> (48 final)
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: 48: possible_tys = -- (_/*52*/, _/*53*/, ), -- (_/*52*/, _/*53*/, )
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: 48: bounds = (K/*M:1*/, V/*M:2*/, ), <I/*M:0*/ as ::"core"::iter::traits::IntoIterator>::Item/*O*/
```
- Tuple is not being picked, despite being only valid option
- Tuple `--` comes from a match

Solution: Check `possible_tys` when checking bounds?
- OR, check match somehow
- `possible_tys`, just for destinations

# `::"syntax"::feature_gate::check_crate`
Problem:
- CallValue argument inferred too early

```
Typecheck Expressions-     check_ivar_poss: >> (150)
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: 150: possible_tys = CD &&::"syntax"::feature_gate::Features/*S*/, -- _
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: 150: bounds = ?
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: Single concrete source, &&::"syntax"::feature_gate::Features/*S*/
```

# `::"syntax"::attr::is_known_lint_tool`
Problem:
- None of the bounded types fit requirements
```
Typecheck Expressions-     check_ivar_poss: >> (7)
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: 7: possible_tys = -- &str, -- str
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: 7: bounds = str, [u8], ::"std"::ffi::os_str::OsStr/*S*/, ::"std"::path::Path/*S*/
```

```
Typecheck Expressions-      check_coerce_tys: >> (&&str := &&_/*7*/)
Typecheck Expressions-       check_unsize_tys: >> (dst=&str, src=&_/*7*/)
Typecheck Expressions-        `anonymous-namespace'::check_unsize_tys: -- Deref coercions
Typecheck Expressions-        TraitResolution::autoderef: Deref &_/*7*/ into _/*7*/
Typecheck Expressions-        Context::possible_equate_ivar: 7 UnsizeTo &str &str
Typecheck Expressions-        TraitResolution::autoderef: Deref &str into str
Typecheck Expressions-        Context::possible_equate_ivar: 7 UnsizeTo str str
```

Solution:
- In `check_unsize_tys` auto-deref, add the final deref option instead of all options
  > So here, that's `str`

Notes:
- `_#7` could be `&&&&str` (but only a &-chain)


# 
Problem:
- None of the bounded types fit requirements
- `:0: BUG:..\..\src\hir_typeck\expr_cs.cpp:7535: TODO: check_ivar_poss - No none of the bounded types (*const ::"git2-0_7_3"::diff::ForeachCallbacks/*S*/, *mut ::"git2-0_7_3"::diff::ForeachCallbacks/*S*/) fit other bounds`

```
Typecheck Expressions-     check_ivar_poss: >> (70)
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: 70: possible_tys = C- *mut ::"libc-0_2_42"::c_void/*E*/
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: 70: bounds = *const _/*66*/, *mut _/*66*/
```

```
Typecheck Expressions-     check_coerce: >> (R19 *mut ::"libc-0_2_42"::c_void/*E*/ := 0000020F4EEF1298 0000020F5046C590 (_/*70*/) - *mut ::"libc-0_2_42"::c_void := _/*70*/)
Typecheck Expressions-       Context::possible_equate_ivar: 70 CoerceTo *mut ::"libc-0_2_42"::c_void/*E*/ *mut ::"libc-0_2_42"::c_void/*E*/

Typecheck Expressions-     check_ivar_poss: >> (66)
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: 66: possible_tys =
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: 66: bounds = +, ::"git2-0_7_3"::diff::ForeachCallbacks/*S*/
...
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: Only ::"git2-0_7_3"::diff::ForeachCallbacks/*S*/ is an option
Typecheck Expressions-       HMTypeInferrence::set_ivar_to: Set IVar 66 = ::"git2-0_7_3"::diff::ForeachCallbacks/*S*/

```

Solution:
- Treat a `+` bound as increasing `n_ivars`
- Gate the `Only <Foo> is an option` to `n_ivars == 0 || fallback`


# `::"cargo-0_30_0"::ops::fix::rustfix_crate`
`:0: error:0:Failed to find an impl of ::"core"::str::FromStr for ::"std"::net::addr::SocketAddr with Err = ::"failure-0_1_2"::error::Error`

```
Typecheck Expressions-                Context::equate_types_coerce: ++ R26 _/*36*/ := 000001BE2865AFD0 000001BE5E44DC20 (_/*37*/)
Typecheck Expressions-        Context::equate_types_assoc: ++ R138 _/*37*/ = < `_/*272*/` as `::"core"::str::FromStr` >::Err
Typecheck Expressions-        HMTypeInferrence::ivar_unify: IVar 272 = @286
Typecheck Expressions-        HMTypeInferrence::ivar_unify: IVar 286 = @33
Typecheck Expressions-      check_ivar_poss: >> (36 unblock)
Typecheck Expressions-       `anonymous-namespace'::check_ivar_poss: 36: possible_tys = CD _/*37*/
Typecheck Expressions-       `anonymous-namespace'::check_ivar_poss: 36: bounds = +, ::"failure-0_1_2"::error::Error/*S*/
Typecheck Expressions-       `anonymous-namespace'::check_ivar_poss: Only ::"failure-0_1_2"::error::Error/*S*/ is an option
Typecheck Expressions-        HMTypeInferrence::set_ivar_to: Set IVar 36 = ::"failure-0_1_2"::error::Error/*S*/
Typecheck Expressions-        HMTypeInferrence::set_ivar_to: Set IVar 37 = ::"failure-0_1_2"::error::Error/*S*/
Typecheck Expressions-      Typecheck_Code_CS: - Consumed coercion R26 ::"failure-0_1_2"::error::Error/*S*/ := _/*37*/
Typecheck Expressions-        HMTypeInferrence::set_ivar_to: Set IVar 33 = ::"std"::net::addr::SocketAddr/*E*/
Typecheck Expressions-      Typecheck_Code_CS: - R138 ::"failure-0_1_2"::error::Error/*S*/ = < `_/*33*/` as `::"core"::str::FromStr` >::Err
Typecheck Expressions-       `anonymous-namespace'::check_associated: No impl of ::"core"::str::FromStr for ::"std"::net::addr::SocketAddr with Err = ::"failure-0_1_2"::error::Error
```
Problem: Picked `_36` too early.

Solution?: Remove the bounds from the possible type list?
- That causes other issues
Solution?: Gate the "Only foo is an option" check on that option not being from the bounds list?
- Basic gating doesn't work (same reason as the above)
- Could defer the rule?
Soltution?: Fine-grained gate (only allow fully when in later fallback modes)
- Still fails with a basic version
- Works if deferred until `::Final`


# `<::"datafrog-2_0_1"::treefrog::extend_anti::ExtendAnti<Key/*I:0*/,Val/*I:1*/,Tuple/*I:2*/,Func/*I:3*/,>/*S*/ as ::"datafrog-2_0_1"::treefrog::Leaper<Tuple/*I:2*/,Val/*I:1*/,>>::intersect`
`..\rustc-1.39.0-src\vendor\datafrog\src\treefrog.rs:464: error:0:Failed to find an impl of ::"core"::cmp::PartialOrd<&&Val/*I:1*/,> for &Val/*I:1*/`

```rustc
	// pub(crate) fn gallop<T>(mut slice: &[T], mut cmp: impl FnMut(&T) -> bool) -> &[T] {
	
	// relation: &'leap Relation<(Key, Val)>,
	
    impl<'leap, Key: Ord, Val: Ord + 'leap, Tuple: Ord, Func> Leaper<'leap, Tuple, Val>
        for ExtendAnti<'leap, Key, Val, Tuple, Func>
    where
        Key: Ord + 'leap,
        Val: Ord + 'leap,
        Tuple: Ord,
        Func: Fn(&Tuple) -> Key,
    {
		...
        fn intersect(&mut self, prefix: &Tuple, values: &mut Vec<&'leap Val>) {
            let key = (self.key_func)(prefix);
            let start = binary_search(&self.relation[..], |x| &x.0 < &key);
            let slice1 = &self.relation[start..];
            let slice2 = gallop(slice1, |x| &x.0 <= &key);
            let mut slice = &slice1[..(slice1.len() - slice2.len())];
            if !slice.is_empty() {
                values.retain(|v| {
                    slice = gallop(slice, |kv| &kv.1 < v);	/* Line 464 */
                    slice.get(0).map(|kv| &kv.1) != Some(v)
                });
            }
		}
	}
```

```rust
            let mut slice/*: &[(Key, Val)]*/ = &slice1[..(slice1.len() - slice2.len())];
            if !slice.is_empty() {
                values/*: &mut Vec<&'leap Val>*/.retain(|v/*: &&'leap Val*/| {
                    slice = gallop(slice, |kv/*: &(Key, Val)*/| &kv.1/*: &Val*/ < v/*: &&'leap Val*/);
				});
			}
```

Issue: Types as annotated above are likely incorrect, the reference count for the comparison should match.

Theory: Maybe the closure type should turn `&&T` into `&T` (part of pattern ergonmics)
Experiment:
- Ran `rustc -Z unpretty=hir,typed`, result was `(&((kv as &(Key, Val)).1 as Val) as &Val)  <  (v as &&Val)`
  - Confirmined that mrustc's type inferrence is correct : `&Val < &&Val`
- Emitted rustc MIR, it `for<'r, 's> fn(&'r &Val, &'s &Val) -> bool {<&Val as std::cmp::PartialOrd>::lt}`
  - This looks like it's either added an auto-deref of the RHS before expanding the `<`, OR has omitted the `&` when doing the expansion
  - `let _ = &1i32 < &DerefI32(1);` compiles with `rustc` - there's hidden deref coercion happening
  - The reverse does not, so it's RHS coercing to LHS?

Cause: There's a coercion point at the RHS of the comparison, but trait lookup isn't propagating the type.
- The trait lookup failed due to it being a fuzzy match, and two copies of `Val: Ord` in the bounds list.
Fix: Check the fuzzy placeholder list for equality when a new fuzzy is seen, and use it if the list is always the same.