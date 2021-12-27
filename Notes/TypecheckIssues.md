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

# `::"rustc_apfloat"::ieee::sig::olsb`
`..\rustc-1.39.0-src\src\librustc_apfloat\ieee.rs:2300: error:0:Failed to find an impl of ::"core"::cmp::PartialEq<_/*26:i*/,> for &u128`

```rust
    pub(super) fn olsb(limbs: &[Limb]) -> usize {
        limbs.iter().enumerate().find(|(_, &limb)| limb != 0).map_or(0,
            |(i, limb)| i * LIMB_BITS + limb.trailing_zeros() as usize + 1)
    }
```

Looks like the `&limb` isn't dereferencing properly

Theory: Recursion for match-ergonomics `&` patterns doesn't reset binding mode
Fix: Do that.


# `<I/*I:1*/ as ::"syn-0_15_35"::punctuated::IterTrait<T/*I:0*/,>>::clone_box`
`..\rustc-1.39.0-src\vendor\syn-0.15.35\src\punctuated.rs:658: error:0:Failed to find an impl of ::"syn-0_15_35"::punctuated::IterTrait<T/*I:0*/,> for I/*I:1*/ with Item = &T/*I:0*/`

```
Typecheck Expressions-     check_coerce: >> (R1 ::"alloc"::boxed::Box<dyn (::"syn-0_15_35"::punctuated::IterTrait<T/*I:0*/,Item=&T/*I:0*/,>+'_),>/*S*/ := 000000A226B4E470 0000028A127C0460 (_/*0*/) - ::"alloc"::boxed::Box<(::"syn-0_15_35"::punctuated::IterTrait<T/*I:0*/,>+ ''_),> := ::"alloc"::boxed::Box<I/*I:1*/,>)
Typecheck Expressions-        Context::equate_types_assoc: ++ R3 &T/*I:0*/ = < `I/*I:1*/` as `::"syn-0_15_35"::punctuated::IterTrait<T/*I:0*/,>` >::Item
Typecheck Expressions-     Typecheck_Code_CS: - R3 &T/*I:0*/ = < `I/*I:1*/` as `::"syn-0_15_35"::punctuated::IterTrait<T/*I:0*/,>` >::Item
Typecheck Expressions-     check_associated: >> (R3 &T/*I:0*/ = < `I/*I:1*/` as `::"syn-0_15_35"::punctuated::IterTrait<T/*I:0*/,>` >::Item)
Typecheck Expressions-        ftic_check_params: >> (impl<'a,T,I,> ::"syn-0_15_35"::punctuated::IterTrait<T/*I:0*/,> for I/*I:1*/)
Typecheck Expressions-         find_trait_impls: >> (trait = ::"core"::iter::traits::exact_size::ExactSizeIterator, type = I/*I:1*/)
Typecheck Expressions-        `anonymous-namespace'::check_associated::<lambda_ec0a0cf4bb4ae9fc7de4b523e49adcc6>::operator (): [check_associated] - (fail) known result can't match (&T/*I:0*/ and <I/*I:1*/ as ::"syn-0_15_35"::punctuated::IterTrait<T/*I:0*/,>>::Item)
Typecheck Expressions-      `anonymous-namespace'::check_associated: No impl of ::"syn-0_15_35"::punctuated::IterTrait<T/*I:0*/,> for I/*I:1*/ with Item = &T/*I:0*/
```

Cause: ExpandAssociatedTypes doesn't check bounds after figuring out which trait the type was from.

Fix: Move `find_type_in_trait` call to above the bounded lookup

# `<::"rand_core-0_5_0_H23"::block::BlockRng64<R/*I:0*/,>/*S*/ as ::"rand_core-0_5_0_H23"::RngCore>::next_u32`
```
..\rustc-1.39.0-src\vendor\rand_core\src\block.rs:367: warn:0:Spare Rule - <R/*I:0*/ as ::"rand_core-0_5_0_H23"::block::BlockRngCore>::Results : ::"core"::convert::AsRef<_/*83*/,>
..\rustc-1.39.0-src\vendor\rand_core\src\block.rs:367: warn:0:Spare rule - _Cast {&_/*83*/} -> *const [u64]
..\rustc-1.39.0-src\vendor\rand_core\src\block.rs:375: BUG:..\..\src\hir_typeck\expr_cs.cpp:6760: Spare rules left after typecheck stabilised
```

```
Typecheck Expressions-      Context::possible_equate_ivar_bounds: 83 bounded as [[<R/*I:0*/ as ::"rand_core-0_5_0_H23"::block::BlockRngCore>::Item/*?*/], [u64]]
```

Notes:
- Trait definition of `BlockRngCore` adds `Result: AsRef<[Self::Item]>`
- Impl block adds `R::Result: AsRef<[u64]>` and bounds `R::Item = u64`

Solution:
- Run EAT when adding to ivar bounds (to de-duplicate this pair)


# 1.39 `<::"rustc_data_structures"::bit_set::BitMatrix<R/*I:0*/,C/*I:1*/,>/*S*/>::from_row_n`
```
..\rustc-1.39.0-src\src\librustc_data_structures\bit_set.rs:718: error:0:No applicable methods for {::"core"::iter::adapters::Cloned<::"core"::iter::adapters::flatten::Flatten<::"core"::iter::adapters::Take<::"core"::iter::sources::Repeat<_/*91*/,>,>,>,>}.collect
```

```
Typecheck Expressions-       find_method: >> (ty=::"core"::iter::adapters::Cloned<::"core"::iter::adapters::flatten::Flatten<::"core"::iter::adapters::Take<::"core"::iter::sources::Repeat<_/*91*/,>/*S*/,>/*S*/,>/*S*/,>/*S*/, name=collect, access=Move)
Typecheck Expressions-        find_trait_impls_crate: >> (::"core"::iter::traits::iterator::Iterator for ::"core"::iter::adaptersCloned<::"core"::iter::adapters::flatten::Flatten<::"core"::iter::adapters::Take<::"core"::iter::sources::Repeat<_/*91*/,>/*S*/,>/*S*/,>/*S*/,>/*S*/)
Typecheck Expressions-           find_trait_impls_crate: >> (::"core"::iter::traits::iterator::Iterator for ::"core"::iter::adapters::flatten::Flatten<::"core"::iter::adapters::Take<::"core"::iter::sources::Repeat<_/*91*/,>/*S*/,>/*S*/,>/*S*/)
Typecheck Expressions-              find_trait_impls_crate: >> (::"core"::iter::traits::iterator::Iterator for ::"core"::iter::adapters::Take<::"core"::iter::sources::Repeat<_/*91*/,>/*S*/,>/*S*/)
Typecheck Expressions-                 find_trait_impls_crate: >> (::"core"::iter::traits::iterator::Iterator for ::"core"::iter::sources::Repeat<_/*91*/,>/*S*/)
Typecheck Expressions-                   find_trait_impls: >> (trait = ::"core"::clone::Clone, type = _/*91*/)
Typecheck Expressions-                      TraitResolution::type_is_clone: Fuzzy Clone impl for ivar?
Typecheck Expressions-                   TraitResolution::ftic_check_params: - Bound _/*91*/ : ::"core"::clone::Clone fuzzed
Typecheck Expressions-                TraitResolution::ftic_check_params: - Bound ::"core"::iter::sources::Repeat<_/*91*/,>/*S*/ : ::"core"::iter::traits::iterator::Iterator fuzzed
Typecheck Expressions-              ImplRef::get_type: name=Item tpl_ty=<I/*I:0*/ as ::"core"::iter::traits::iterator::Iterator>::Item/*O*/ impl(00000212B6B842F0)<I,> ::"core"::iter::traits::iterator::Iterator for ::"core"::iter::adapters::Take<I/*I:0*/,>/*S*/ where I/*I:0*/: ::"core"::iter::traits::iterator::Iterator {I = ::"core"::iter::sources::Repeat<_/*91*/,>/*S*/,Self::Item = <I/*I:0*/ as ::"core"::iter::traits::iterator::Iterator>::Item/*O*/,}
Typecheck Expressions-              expand_associated_types_inplace__UfcsKnown: >> (input=<::"core"::iter::sources::Repeat<_/*91*/,>/*S*/ as ::"core"::iter::traits::iterator::Iterator>::Item/*?*/)
Typecheck Expressions-             TraitResolution::ftic_check_params: - Bound _/*91*/ : ::"core"::iter::traits::collect::IntoIterator fuzzed
Typecheck Expressions-             TraitResolution::ftic_check_params: - Bound impl_?_00000212B6B85EF0/*P:1*/ : ::"core"::iter::traits::iterator::Iterator fuzzed
Typecheck Expressions-            TraitResolution::ftic_check_params::<lambda_6b008489904a9b32483bac7b2dde67f5>::operator (): Assoc `Item` didn't match - <impl_?_00000212B6B85EF0/*P:1*/ as ::"core"::iter::traits::iterator::Iterator>::Item/*O*/ != &impl_?_00000212B6B80DF0/*P:1*/
Typecheck Expressions-         TraitResolution::find_trait_impls_crate::<lambda_a7e5a3053faa3aa2cf85154a2a37986e>::operator (): [find_trait_impls_crate] - Params mismatch
Typecheck Expressions-       find_method: << ()
```

Look up failed at the comparison of these two types:
- `<impl_?_00000212B6B85EF0/*P:1*/ as ::"core"::iter::traits::iterator::Iterator>::Item/*O*/`
- `&impl_?_00000212B6B80DF0/*P:1*/`

But ATYs of placeholders shouldn't be opaque, they should stay as unknown (same as ivars).

Solution:
- Add check in EAT to not attempt to resolve if the type is a placeholder
  - HOWEVER: A placeholder ties to a given impl block, so could have a ATY bound


# (1.39) `::"syntax_pos"::symbol::sym::integer`
```
..\rustc-1.39.0-src\src\libsyntax_pos\symbol.rs:1079: warn:0:Spare Rule - N/*M:0*/ : ::"core"::convert::TryInto<_/*23*/,>
..\rustc-1.39.0-src\src\libsyntax_pos\symbol.rs:1080: warn:0:Spare Rule - _/*23*/ : ::"core"::slice::SliceIndex<[::"syntax_pos"::symbol::Symbol/*S*/],>
..\rustc-1.39.0-src\src\libsyntax_pos\symbol.rs:1084: warn:0:Spare Rule - ::"syntax_pos"::symbol::Symbol = < _/*23*/ as ::"core"::slice::SliceIndex<[::"syntax_pos"::symbol::Symbol/*S*/],> >::Output
..\rustc-1.39.0-src\src\libsyntax_pos\symbol.rs:1084: warn:0:Spare Rule - _/*25*/ = < _/*23*/ as ::"core"::convert::TryFrom<N/*M:0*/,> >::Error
..\rustc-1.39.0-src\src\libsyntax_pos\symbol.rs:1086: BUG:..\..\src\hir_typeck\expr_cs.cpp:6767: Spare rules left after typecheck stabilised
```

The rule `_/*25*/ = < _/*23*/ as ::"core"::convert::TryFrom<N/*M:0*/,> >::Error` shouldn't exist, it should be `_/*25*/ = < N/*M:0*/ as ::"core"::convert::TryInto<_/*23*/,> >::Error`

```
Typecheck Expressions-          expand_associated_types_inplace__UfcsKnown: >> (input=<N/*M:0*/ as ::"core"::convert::TryInto<_/*19*/,>>::Error/*?*/)
Typecheck Expressions-           trait_contains_type: >> (::"core"::convert::TryInto<_/*19*/,> has Error)
Typecheck Expressions-            TraitResolution::trait_contains_type: - Found in cur
Typecheck Expressions-           trait_contains_type: << (::"core"::convert::TryInto<_/*19*/,>)
Typecheck Expressions-           TraitResolution::expand_associated_types_inplace__UfcsKnown::<lambda_822c87aaf355502ac7b99f073152996a>::operator (): [expand_associated_types_inplace__UfcsKnown] Trait bound - N/*M:0*/ : ::"core"::convert::TryInto<usize,>
Typecheck Expressions-           find_named_trait_in_trait: >> (::"core"::convert::TryInto<_/*19*/,> in ::"core"::convert::TryInto<usize,>)
Typecheck Expressions-           find_named_trait_in_trait: << ()
Typecheck Expressions-           TraitResolution::expand_associated_types_inplace__UfcsKnown::<lambda_822c87aaf355502ac7b99f073152996a>::operator (): [expand_associated_types_inplace__UfcsKnown] Trait bound - N/*M:0*/ : ::"core"::marker::Copy
```
Looks like EAT didn't find the trait impl for `N: TryInto<usize>` to match with `N: TryInto<_23>`

Reason: The matching of traits in generic bounds isn't fuzzy (requires exact match)

Implementing a fuzzy match in EAT's bound lookup fixed the issue.

# (1.19) `::"cargo-0_20_0"::ops::cargo_run::run`
```
:0: error:0:Type mismatch between ::"cargo-0_20_0"::core::package::Package and &::"cargo-0_20_0"::core::package::Package
```

`Typecheck Expressions-        check_coerce: >> (R28 ::"cargo-0_20_0"::core::package::Package/*S*/ := 0000022180786E80 0000022184AA1670 (_/*58*/) - ::"cargo-0_20_0"::core::package::Package := &::"cargo-0_20_0"::core::package::Package)`
- `Typecheck Expressions-        Context::dump: R28 _/*66*/ := 0000022180786E80 0000022184AA1670 (_/*58*/)`
- Chase `_/*66*/`
- [31495] `Typecheck Expressions-          HMTypeInferrence::ivar_unify: IVar 66 = @65`
- [ 2113] `Typecheck Expressions-                  HMTypeInferrence::set_ivar_to: Set IVar 65 = !`
- [50745] `Typecheck Expressions-          HMTypeInferrence::set_ivar_to: Set IVar 65 = ::"cargo-0_20_0"::core::package::Package/*S*/`
- Chase `_/*58*/`
- [16971] `Typecheck Expressions-          HMTypeInferrence::set_ivar_to: Set IVar 58 = &::"cargo-0_20_0"::core::package::Package/*S*/`
- From: ``` Typecheck Expressions-        check_associated: >> (R26 _/*58*/ = < `::"core-0_0_0"::result::Result<&::"cargo-0_20_0"::core::package::Package/*S*/,::"cargo-0_20_0"::util::errors::CargoError/*S*/,>/*E*/` as `::"core-0_0_0"::ops::Try` >::Ok) ````

Note: `_/*58*/` is the type of the `pkg` variable, and should be a borrow
- So, the `_/*66*/` is being set too early

- [31495] `Typecheck Expressions-          HMTypeInferrence::ivar_unify: IVar 66 = @65`
- [50745] `Typecheck Expressions-          HMTypeInferrence::set_ivar_to: Set IVar 65 = ::"cargo-0_20_0"::core::package::Package/*S*/`
- [50738] `Typecheck Expressions-        check_coerce: >> (R23 ::"cargo-0_20_0"::core::package::Package/*S*/ := 000002218D99B190 0000022183663170 (_/*66*/) - ::"cargo-0_20_0"::core::package::Package := _/*65:!*/)`
- [ 1932] `Typecheck Expressions-              Context::equate_types_coerce: ++ R23 _/*134*/ := 000002218D99B190 0000022183663170 (_/*66*/)`
- [35058] `Typecheck Expressions-          HMTypeInferrence::ivar_unify: IVar 134 = @133`
- [ 2703] `Typecheck Expressions-                   HMTypeInferrence::set_ivar_to: Set IVar 133 = !`
- [50733] `Typecheck Expressions-          HMTypeInferrence::set_ivar_to: Set IVar 133 = ::"cargo-0_20_0"::core::package::Package/*S*/`
- [50724] `Typecheck Expressions-        check_coerce: >> (R22 ::"cargo-0_20_0"::core::package::Package/*S*/ := 000002218D99B360 00000221836636C0 (_/*134*/) - ::"cargo-0_20_0"::core::package::Package := _/*133:!*/)`
- [ 1897] `Typecheck Expressions-            Context::equate_types_coerce: ++ R22 _/*135*/ := 000002218D99B360 00000221836636C0 (_/*134*/)`
- [ 1281] `Typecheck Expressions-           HMTypeInferrence::ivar_unify: IVar 135 = @5`
- [ 2974] `Typecheck Expressions-                HMTypeInferrence::ivar_unify: IVar 5 = @138`
- [ 5143] `Typecheck Expressions-                HMTypeInferrence::ivar_unify: IVar 138 = @326`
- [31304] `Typecheck Expressions-          HMTypeInferrence::ivar_unify: IVar 326 = @29`
- [ 1368] `Typecheck Expressions-                 HMTypeInferrence::set_ivar_to: Set IVar 29 = !`
- [31484] `Typecheck Expressions-          HMTypeInferrence::ivar_unify: IVar 29 = @51`
- [ 1658] `Typecheck Expressions-                 HMTypeInferrence::set_ivar_to: Set IVar 51 = !`
- [49958] `Typecheck Expressions-          HMTypeInferrence::set_ivar_to: Set IVar 51 = ::"cargo-0_20_0"::core::package::Package/*S*/`
- [49952] ```Typecheck Expressions-         `anonymous-namespace'::check_ivar_poss: Only ::"cargo-0_20_0"::core::package::Package/*S*/ is an option```
- [49867]
  ```
Typecheck Expressions-        check_ivar_poss: >> (51 weak)
Typecheck Expressions-         `anonymous-namespace'::check_ivar_poss: 51: possible_tys = CD _/*133:!*/, -- ::"cargo-0_20_0"::core::package::Package/*S*/
Typecheck Expressions-         `anonymous-namespace'::check_ivar_poss: 51: bounds = ?
```

`&Package` should have been an option here... (but what is the path?)
- Probably wasn't able to flow through the ivars

Query: Why did weak end up being tried? What was the ruleset that lead to this stall?
```
Typecheck Expressions-        Context::dump: --- CS Context - 16 Coercions, 7 associated, 11 nodes, 0 callbacks
Typecheck Expressions-        Context::dump: R19 &&str := 0000022188E36F60 0000022184AA1C30 (&&str)
Typecheck Expressions-        Context::dump: R22 _/*51:!*/ := 000002218D99B360 00000221836636C0 (_/*133:!*/)
Typecheck Expressions-        Context::dump: R23 _/*133:!*/ := 000002218D99B190 0000022183663170 (_/*65:!*/)
Typecheck Expressions-        Context::dump: R28 _/*65:!*/ := 0000022180786E80 0000022184AA1670 (&::"cargo-0_20_0"::core::package::Package)
Typecheck Expressions-        Context::dump: R37 _/*133:!*/ := 000002218D99B1B8 0000022183663670 (_/*110:!*/)
Typecheck Expressions-        Context::dump: R48 _/*89*/ := 00000221E792A220 0000022184AA17B0 (&::"collections-0_0_0"::string::String)
Typecheck Expressions-        Context::dump: R54 &_/*400*/ := 0000022188E36FC0 0000022184AA15B0 (_/*89*/)
Typecheck Expressions-        Context::dump: R59 _/*110:!*/ := 0000022180786A60 0000022184AA20B0 (&::"cargo-0_20_0"::core::package::Package)
Typecheck Expressions-        Context::dump: R71 _/*117*/ := 00000221E792A400 0000022184AA2670 (&&str)
Typecheck Expressions-        Context::dump: R76 &_/*408*/ := 0000022188E37160 0000022184AA24B0 (_/*117*/)
Typecheck Expressions-        Context::dump: R133 _/*268*/ := 0000022188E377A0 0000022184AA3870 (&&usize)
Typecheck Expressions-        Context::dump: R134 _/*272*/ := 0000022188E377A8 0000022184AA3F30 (&&usize)
Typecheck Expressions-        Context::dump: R140 &_/*437*/ := 0000022188E37600 0000022184AA3BF0 (_/*268*/)
Typecheck Expressions-        Context::dump: R143 &_/*438*/ := 0000022188E37880 0000022184AA3CB0 (_/*272*/)
Typecheck Expressions-        Context::dump: R197 &::"cargo-0_20_0"::core::manifest::Target := 00000221E7931EA0 0000022184AA25F0 (_/*160*/)
Typecheck Expressions-        Context::dump: R211 &::"cargo-0_20_0"::core::package::Package := 0000022188E37C68 0000022184AA4F70 (&_/*51:!*/)
Typecheck Expressions-        Context::dump: R83 bool = < `_/*148*/` as `::"core-0_0_0"::ops::Not` >::Output - op
Typecheck Expressions-        Context::dump: R21 req ty _/*384*/ impl ::"core-0_0_0"::fmt::Display
Typecheck Expressions-        Context::dump: R82 bool = < `_/*145*/` as `::"core-0_0_0"::ops::Not` >::Output - op
Typecheck Expressions-        Context::dump: R142 req ty _/*437*/ impl ::"core-0_0_0"::fmt::Debug
Typecheck Expressions-        Context::dump: R145 req ty _/*438*/ impl ::"core-0_0_0"::fmt::Debug
Typecheck Expressions-        Context::dump: R56 req ty _/*400*/ impl ::"core-0_0_0"::fmt::Display
Typecheck Expressions-        Context::dump: R78 req ty _/*408*/ impl ::"core-0_0_0"::fmt::Display
Typecheck Expressions-        Context::dump: 00000221845532B0 _CallMethod {_/*51:!*/}.manifest() -> _/*139*/
Typecheck Expressions-        Context::dump: 0000022184551C30 _CallMethod {_/*139*/}.targets() -> _/*140*/
Typecheck Expressions-        Context::dump: 0000022184552FB0 _CallMethod {_/*140*/}.iter() -> _/*141*/
Typecheck Expressions-        Context::dump: 0000022184554C30 _CallMethod {_/*160*/}.is_lib() -> _/*145*/
Typecheck Expressions-        Context::dump: 0000022184550130 _CallMethod {_/*160*/}.is_custom_build() -> _/*148*/
Typecheck Expressions-        Context::dump: 000002218454F230 _CallMethod {_/*160*/}.is_bin() -> bool
Typecheck Expressions-        Context::dump: 0000022184554DB0 _CallMethod {_/*141*/}.filter({{000002218DA43410}(_/*160*/,)->bool}, ) -> _/*194*/
Typecheck Expressions-        Context::dump: 0000022184550A30 _CallMethod {_/*194*/}.next() -> _/*170*/
Typecheck Expressions-        Context::dump: 00000221845523B0 _CallMethod {_/*170*/}.is_none() -> bool
Typecheck Expressions-        Context::dump: 0000022184551DB0 _CallMethod {_/*194*/}.next() -> _/*195*/
Typecheck Expressions-        Context::dump: 000002218454F3B0 _CallMethod {_/*195*/}.is_some() -> bool
Typecheck Expressions-        Context::dump: ---
```

Idea: Detect coercion chains
- Above `_/*65*/` is in a chain (it's only ever used as a source/destionation of a coercion, not used in any other rules)
- Sounds like `check_ivar_poss` with one source, one destination, and no bounds/restricts

Implemented the above change, and added a bound check for Sized.
- All this does is speed up the failure.

Stricter version:
- If both sides of an ivar are a single ivar coercion, pick the source.

Final version:
- If both sides are a single-option coercion (and no bounds/disables) - then pick one side (preferring no ivars, and preferring the source)
  - This helps eliminate coercions that don't need to do anything.
- This seems to have worked (reduces the chain, providing more useful information for fallbacks)


# (1.39) `<::"typenum-1_10_0"::uint::UInt<U/*I:0*/,B/*I:1*/,>/*S*/ as ::"typenum-1_10_0"::type_operators::Max<Ur/*I:2*/,>>::max`
```
..\rustc-1.39.0-src\vendor\typenum\src\uint.rs:1571: error:0:Failed to find an impl of ::"typenum-1_10_0"::private::PrivateMax<Ur/*I:2*/,<::"typenum-1_10_0"::uint::UInt<U/*I:0*/,B/*I:1*/,> as ::"typenum-1_10_0"::type_operators::Cmp<Ur/*I:2*/,>>::Output,> for ::"typenum-1_10_0"::uint::UInt<U/*I:0*/,B/*I:1*/,> with Output = <::"typenum-1_10_0"::uint::UInt<U/*I:0*/,B/*I:1*/,> as ::"typenum-1_10_0"::type_operators::Cmp<Ur/*I:2*/,>>::Output
```

```
..\rustc-1.39.0-src\vendor\typenum\src\uint.rs:1571: error:0:
  Failed to find an impl of 
  ::"typenum-1_10_0"::private::PrivateMax<Ur/*I:2*/,<::"typenum-1_10_0"::uint::UInt<U/*I:0*/,B/*I:1*/,> as ::"typenum-1_10_0"::type_operators::Cmp<Ur/*I:2*/,>>::Output,>
  for ::"typenum-1_10_0"::uint::UInt<U/*I:0*/,B/*I:1*/,>
  with Output = <::"typenum-1_10_0"::uint::UInt<U/*I:0*/,B/*I:1*/,> as ::"typenum-1_10_0"::type_operators::Cmp<Ur/*I:2*/,>>::Output
```

Source:
```
impl<U, B, Ur> Max<Ur> for UInt<U, B>
where
    U: Unsigned,
    B: Bit,
    Ur: Unsigned,
    UInt<U, B>: Cmp<Ur> + PrivateMax<Ur, Compare<UInt<U, B>, Ur>>,
{
    type Output = PrivateMaxOut<UInt<U, B>, Ur, Compare<UInt<U, B>, Ur>>;
    fn max(self, rhs: Ur) -> Self::Output {
        self.private_max(rhs)
    }
}
```

Looks like it's the impl required for the `private_max` method, but the associated type for output doesn't look right
Should be `<UInt<U,B> as Max<Ur>>::Output`, instead it's `<Uint<U,B> as Cmp<Ur>>::Output`

Return type is wrong, puts the problem in `Resolve UFCS Outer`

`Resolve UFCS Outer-    visit_path: >> (UfcsUnknown - p=<::"typenum-1_10_0"::uint::UInt<U/*I:0*/,B/*I:1*/,>/*S*/ as _>::Output)`
`Self` has already been replaced with the expanded type, which changes the resolution rules
- Probably `Resolve Absolute` being a little too enthusiastic

Huge chain of rewrites later, problem was `Self` being expanded before the logic that requires it.
- Deferred that until aftter `Resolve UFCS Outer` (could be moved later, to after `Resolve UFCS paths`?)

# (1.39) `<* as ::"aho_corasick-0_7_3_H1"::automaton::Automaton>::standard_find_at_imp`
```
TTStream:0: error:0:Failed to find an impl of ::"aho_corasick-0_7_3_H1"::state_id::StateID for &mut <Self/**/ as ::"aho_corasick-0_7_3_H1"::automaton::Automaton>::ID
..\rustc-1.39.0-src\vendor\aho-corasick\src\automaton.rs:173: note: From here
```

```
Typecheck Expressions-        visit: >> ((CallMethod) {&mut <Self/**/ as ::"aho_corasick-0_7_3_H1"::automaton::Automaton>::ID}.to_usize() -> _/*7*/)
Typecheck Expressions-          TraitResolution::autoderef_find_method: FOUND *{0}, fcn_path = <&mut <Self/**/ as ::"aho_corasick-0_7_3_H1"::automaton::Automaton>::ID/*O*/ as ::"aho_corasick-0_7_3_H1"::state_id::StateID>::to_usize
Typecheck Expressions-          TraitResolution::autoderef_find_method: FOUND 1 options: (None, <&mut <Self/**/ as ::"aho_corasick-0_7_3_H1"::automaton::Automaton>::ID/*O*/ as ::"aho_corasick-0_7_3_H1"::state_id::StateID>::to_usize)
Typecheck Expressions-         visit_call_populate_cache: >> (<&mut <Self/**/ as ::"aho_corasick-0_7_3_H1"::automaton::Automaton>::ID/*O*/ as ::"aho_corasick-0_7_3_H1"::state_id::StateID>::to_usize)
Typecheck Expressions-          Context::equate_types_assoc: ++ R51 req ty &mut <Self/**/ as ::"aho_corasick-0_7_3_H1"::automaton::Automaton>::ID/*O*/ impl ::"aho_corasick-0_7_3_H1"::state_id::StateID
Typecheck Expressions-        Typecheck_Code_CS: - R51 req ty &mut <Self/**/ as ::"aho_corasick-0_7_3_H1"::automaton::Automaton>::ID/*O*/ impl ::"aho_corasick-0_7_3_H1"::state_id::StateID
Typecheck Expressions-         `anonymous-namespace'::check_associated: No impl of ::"aho_corasick-0_7_3_H1"::state_id::StateID for &mut <Self/**/ as ::"aho_corasick-0_7_3_H1"::automaton::Automaton>::ID
```

Looks like `autoderef_find_method` is finding a method on a trait that still requires dereferencing to be valid.

Forgot to check the result of `check_method_receiver` against the expected type before returning success when processing ATY bounds
Fix that, and all works

# (1.39) `::"rustc_passes-0_0_0"::ast_validation::validate_generics_order`
```
..\rustc-1.39.0-src\src\librustc_passes\ast_validation.rs:396: error:0:Unsized type not valid here - str
```

```
Typecheck Expressions-                Context::equate_types_assoc: ++ R32 req ty _/*120*/ impl ::"core-0_0_0"::ops::arith::AddAssign<_/*122*/,>
Typecheck Expressions-                 visit: >> (000001A168BD63A0 ident{25})
Typecheck Expressions-                   require_sized: >> (_/*121*/ -> _/*121*/)
Typecheck Expressions-      check_associated: >> (R32 req ty ::"alloc-0_0_0"::string::String/*S*/ impl ::"core-0_0_0"::ops::arith::AddAssign<&_/*121*/,>)
Typecheck Expressions-       equate_types: >> (&_/*121*/ == &str)
```

```
ordered_params += &ident;
```

No type coercions in `<OP>=` assignments, leading to the above hard equality in the assignment.
Attempt adding a coercion point to the RHS of op-assign.
Required a new ivar for the coercion, worked.


# (1.39) `<::"rustc_metadata-0_0_0"::cstore::CrateMetadata/*S*/>::get_implementations_for_trait`
```
:0: error:0:Type mismatch between &mut [::"rustc-0_0_0"::hir::def_id::DefId/*S*/] and &[_/*90*/; 0] - Borrow classes differ
```

`Typecheck Expressions-     check_coerce: >> (R12 &mut [::"rustc-0_0_0"::hir::def_id::DefId/*S*/] := 0000022E69C45458 0000022E63E38580 (_/*52*/) - &mut [::"rustc-0_0_0"::hir::def_id::DefId] := &[_/*90*/; 0])`
`Typecheck Expressions-           Context::equate_types_coerce: ++ R12 _/*53*/ := 0000022E69C45458 0000022E63E38580 (_/*52*/)`

Looking at the code, `_53` should be `&[DefId]`
```
Typecheck Expressions-          HMTypeInferrence::ivar_unify: IVar 53 = @54
Typecheck Expressions-       HMTypeInferrence::set_ivar_to: Set IVar 54 = &mut [::"rustc-0_0_0"::hir::def_id::DefId/*S*/]
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: - Source/Destination type
```
Check usage of `_54`
```
Typecheck Expressions-     check_coerce: >> (R9 &mut [::"rustc-0_0_0"::hir::def_id::DefId/*S*/] := 0000022E69C454F0 0000022E63E37BF0 (_/*54*/) - &mut [::"rustc-0_0_0"::hir::def_id::DefId] := _/*54*/)
Typecheck Expressions-       Context::possible_equate_ivar: 54 CoerceTo &mut [::"rustc-0_0_0"::hir::def_id::DefId/*S*/] &mut [::"rustc-0_0_0"::hir::def_id::DefId/*S*/]
Typecheck Expressions-     check_coerce: >> (R10 _/*54*/ := 0000022E69C45430 0000022E63E37DA0 (_/*49*/) - _/*54*/ := &mut [::"rustc-0_0_0"::hir::def_id::DefId])
Typecheck Expressions-       Context::possible_equate_ivar: 54 CoerceFrom &mut [::"rustc-0_0_0"::hir::def_id::DefId/*S*/] &mut [::"rustc-0_0_0"::hir::def_id::DefId/*S*/]
Typecheck Expressions-     check_coerce: >> (R12 _/*54*/ := 0000022E69C45458 0000022E63E38580 (_/*52*/) - _/*54*/ := &[_/*90*/; 0])
Typecheck Expressions-       Context::possible_equate_ivar: 54 CoerceFrom &[_/*90*/; 0] &[_/*90*/; 0]
```
R9 looks suspicious, the result should be `&[DefId]`
```
Typecheck Expressions-        Context::equate_types_coerce: ++ R9 _/*77*/ := 0000022E69C454F0 0000022E63E37BF0 (_/*54*/)
```
Chase `_77`
```
Typecheck Expressions-       HMTypeInferrence::ivar_unify: IVar 77 = @0
```
Chase `_0`
```
Typecheck Expressions-     check_ivar_poss: >> (0 weak)
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: 0: possible_tys = CD _/*54*/, CD &mut [::"rustc-0_0_0"::hir::def_id::DefId/*S*/], C- &[::"rustc-0_0_0"::hir::def_id::DefId/*S*/]
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: Most accepting pointer class, and most permissive inner type - &mut [::"rustc-0_0_0"::hir::def_id::DefId/*S*/]
Typecheck Expressions-       HMTypeInferrence::set_ivar_to: Set IVar 0 = &mut [::"rustc-0_0_0"::hir::def_id::DefId/*S*/]
```

It's hitting the fallback case of least restrictive source (so picks `&mut` from the provided options).
```
Typecheck Expressions-     Context::dump: --- CS Context - 5 Coercions, 0 associated, 0 nodes, 0 callbacks
Typecheck Expressions-     Context::dump: R9 _/*0*/ := 0000022E69C454F0 0000022E63E37BF0 (_/*54*/)
Typecheck Expressions-     Context::dump: R10 _/*54*/ := 0000022E69C45430 0000022E63E37DA0 (&mut [::"rustc-0_0_0"::hir::def_id::DefId])
Typecheck Expressions-     Context::dump: R12 _/*54*/ := 0000022E69C45458 0000022E63E38580 (&[_/*90*/; 0])
Typecheck Expressions-     Context::dump: R13 _/*0*/ := 0000022E69C45518 0000022E63E39DB0 (&mut [::"rustc-0_0_0"::hir::def_id::DefId])
Typecheck Expressions-     Context::dump: R16 &[::"rustc-0_0_0"::hir::def_id::DefId] := 000000793833EAB0 0000022E63E37410 (_/*0*/)
```
Looking at the available rules, `_54` should have triggered the non-fallback of the least-restrictive source rule (as it has both `&` and `&mut` options with no others.
```
Typecheck Expressions-     check_ivar_poss: >> (54)
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: 54: possible_tys = CD &mut [::"rustc-0_0_0"::hir::def_id::DefId/*S*/], CD &[_/*90*/; 0], C- _/*0*/
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: 54: bounds = ?
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: 54: 0 duplicates
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: 1 ivars (0 src, 1 dst)
Typecheck Expressions-      get_ordering_ptr: >> (&[_/*90*/; 0] , &mut [::"rustc-0_0_0"::hir::def_id::DefId/*S*/])
Typecheck Expressions-      get_ordering_ptr: << (0)
```
Note: Zero (in this version of the code) is OrdLess
The above can't pick a single type, as it would need `&[DefId]` not `&[DefId; 0]`

Only working chain for the available rules is for `_0` to be set to `&[DefId]` via its single destination... but isn't there a single-destination rule?
It's gated by `honour_disable` - `if( !honour_disable && n_dst_ivars == 0 && ::std::count_if(possible_tys.begin(), possible_tys.end(), PossibleType::is_dest_s) == 1 )`
This is OLD logic, try regression test with the gate removed.

Got libgit2'd, ends up picking the wrong type in `git2 v0.7.3` (1.29)
- libgit2's case has bounds present, the above does not - switch the gating to be no bounds (`ivar_ent.has_bounded`) OR fallback (`!honour_disable`)
Also broke libcargo 1.29
- The ivar bounds and usage implies anything can be picked, but it's also cloned with an expectation of `String`.
- Add the above gating to single source too (higher priority)

Success!

## Final Fix
Change gating of single source/destination to be either no bounds, OR fallback


# (1.39) `<::"rustc_metadata-0_0_0"::schema::EntryKind/*E*/ as ::"core-0_0_0"::clone::Clone>::clone`
```
:0: warn:0:Spare Rule - ::"rustc_metadata-0_0_0"::schema::Lazy<::"rustc_metadata-0_0_0"::schema::TraitAliasData,(),> : ::"core-0_0_0"::clone::Clone
:0: warn:0:Spare Rule - ::"rustc_metadata-0_0_0"::schema::Lazy<::"rustc_metadata-0_0_0"::schema::RenderedConst,(),> : ::"core-0_0_0"::clone::Clone
:0: warn:0:Spare Rule - ::"rustc_metadata-0_0_0"::schema::Lazy<::"rustc_metadata-0_0_0"::schema::RenderedConst,(),> : ::"core-0_0_0"::clone::Clone
:0: warn:0:Spare Rule - ::"rustc_metadata-0_0_0"::schema::Lazy<::"rustc_metadata-0_0_0"::schema::VariantData,(),> : ::"core-0_0_0"::clone::Clone
:0: warn:0:Spare Rule - ::"rustc_metadata-0_0_0"::schema::Lazy<::"rustc_metadata-0_0_0"::schema::VariantData,(),> : ::"core-0_0_0"::clone::Clone
:0: warn:0:Spare Rule - ::"rustc_metadata-0_0_0"::schema::Lazy<::"rustc_metadata-0_0_0"::schema::MethodData,(),> : ::"core-0_0_0"::clone::Clone
:0: warn:0:Spare Rule - ::"rustc_metadata-0_0_0"::schema::Lazy<::"rustc_metadata-0_0_0"::schema::VariantData,(),> : ::"core-0_0_0"::clone::Clone
:0: warn:0:Spare Rule - ::"rustc_metadata-0_0_0"::schema::Lazy<::"rustc_metadata-0_0_0"::schema::ImplData,(),> : ::"core-0_0_0"::clone::Clone
:0: warn:0:Spare Rule - ::"rustc_metadata-0_0_0"::schema::Lazy<::"rustc_metadata-0_0_0"::schema::FnData,(),> : ::"core-0_0_0"::clone::Clone
:0: warn:0:Spare Rule - ::"rustc_metadata-0_0_0"::schema::Lazy<::"rustc_metadata-0_0_0"::schema::FnData,(),> : ::"core-0_0_0"::clone::Clone
:0: warn:0:Spare Rule - ::"rustc_metadata-0_0_0"::schema::Lazy<::"rustc_metadata-0_0_0"::schema::ModData,(),> : ::"core-0_0_0"::clone::Clone
:0: warn:0:Spare Rule - ::"rustc_metadata-0_0_0"::schema::Lazy<::"rustc_metadata-0_0_0"::schema::MacroDef,(),> : ::"core-0_0_0"::clone::Clone
:0: warn:0:Spare Rule - ::"rustc_metadata-0_0_0"::schema::Lazy<::"rustc_metadata-0_0_0"::schema::ClosureData,(),> : ::"core-0_0_0"::clone::Clone
:0: warn:0:Spare Rule - ::"rustc_metadata-0_0_0"::schema::Lazy<::"rustc_metadata-0_0_0"::schema::GeneratorData,(),> : ::"core-0_0_0"::clone::Clone
:0: warn:0:Spare Rule - ::"rustc_metadata-0_0_0"::schema::Lazy<::"rustc_metadata-0_0_0"::schema::TraitData,(),> : ::"core-0_0_0"::clone::Clone
:0: BUG:..\..\src\hir_typeck\expr_cs.cpp:6958: Spare rules left after typecheck stabilised
```

All of these are fully known, why did they not resolve/remove?


```
Typecheck Expressions-      `anonymous-namespace'::check_associated: Multiple impls
Typecheck Expressions-      `anonymous-namespace'::check_associated:  for ::"rustc_metadata-0_0_0"::schema::Lazy<::"rustc_metadata-0_0_0"::schema::TraitData/*S*/,(),>/*S*/
Typecheck Expressions-      `anonymous-namespace'::check_associated:  for ::"rustc_metadata-0_0_0"::schema::Lazy<::"rustc_metadata-0_0_0"::schema::TraitData/*S*/,<::"rustc_metadata-0_0_0"::schema::TraitData/*S*/ as ::"rustc_metadata-0_0_0"::schema::LazyMeta>::Meta/*?*/,>/*S*/
```

Had forgotten to run EAT on the impl type (had already run it on trait params, but not on the type)




# (1.54) `<::"core-0_0_0"::result::Result<T/*I:0*/,E/*I:1*/,>/*E*/>::into_ok`
Uses `!` as a type, which kinda breaks with some of the type inferrence
- Looks like coercions are being done first, which means that the method call's return coercion site isn't being propagated fast enough
- Leading to a method lookup failure as the return type of `e.into()` is "known" to be `T` (but it's actually `!`)

```
Typecheck Expressions-     check_ivar_poss: >> (4)
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: 4: possible_tys = -D T/*I:0*/
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: 4: bounds = +, !
```

## EXPERIMENT: Move node revisits to first in the typecheck cycle
Failed in 1.39 std, `..\rustc-1.39.0-src\src\libstd\sys_common\thread.rs:14: error:0:Failed to find an impl of ::"core-0_0_0"::ops::function::Fn<(),> for ::"alloc-0_0_0"::boxed::Box<::"alloc-0_0_0"::boxed::Box<(::"core-0_0_0"::ops::function::FnOnce<(),>+ ''_),>,> with Output = ()`

## EXPERIMENT: Remove short-circuit for `T = _` in `check_coerce_tys`
Fails early in libcore 1.39, because other users of `check_coerce_tys` don't get info on the weak equality

## EXPERIMENT: Add weak equality return to `check_coerce_tys`
- Use this to defer the equality action

## EXPERIMENT: Remove short-circuit for `T = _` in `check_coerce_tys`, AND remember to add possibility
- Lots of other breakage?


## EXPERIMENT: Treat `!` as a full type (remove special case handling in ivars)
- Requires special handling in `check_ivar_poss` (ignore completely? Add as final fallback?)
- Coerce rule `! := *` becomes equality
- Some other additions required to handle inference breakage for `!` (e.g. around block returns)


# (1.54) `<i8 as ::"num_integer-0_1_44"::Integer>::gcd`
Calling a method on an integer ivar leading to ambigious lookup. mrustc returns all the inherent methods (with no auto-ref) so doesn't see the trait method (which requires one)
```
return (1 << shift).abs();
```

## EXPERIMENT: Hack avoid inherent methods on bounded ivars (in `find_method)
- libcore fails looking for `saturating_sub`
## EXPERIMENT: Hack avoid inherent methods on bounded ivars (in `visit(_Method)`)
- Failed libcore again first try
- Limited only to fallback mode (and only if it doesn't remove eveything).
- Worked.

# (1.54) 
```
fn compute_lifetime_flags<I: Interner>(lifetime: &Lifetime<I>, interner: &I) -> TypeFlags { ... }

fn compute_flags<I: Interner>(kind: &TyKind<I>, interner: &I) -> TypeFlags {
   ...
   dyn_flags |= compute_lifetime_flags(&(lifetime_outlives.a), &interner)
```
Passes `&interner` but there's no `Interner for &I` impl (so trait bound fails)

```
Typecheck Expressions-    check_ivar_poss: >> (202)
Typecheck Expressions-     `anonymous-namespace'::check_ivar_poss: One possibility (before ivar removal), setting to &I/*M:0*/
Typecheck Expressions-      HMTypeInferrence::set_ivar_to: Set IVar 202 = &I/*M:0*/
```

## Experiment: When picking a lone possibility (and it is dereferencable), check that it meets bounds and deref until it does
- Simple check and loop.
- Seems to have worked

# (1.54) `::"chalk_solve-0_55_0_H7"::clauses::push_alias_implemented_clause`
```rs
// chalk-ir-0.55.0/src/lib.rs
#[derive(..., HasInterner)]
pub struct Ty<I: Interner> {
    interned: I::InternedType,
}
// chalk-ir-0.55.0/src/lib.rs
impl<T: HasInterner> Binders<T> {
    ...
    pub fn with_fresh_type_var(
            interner: &T::Interner,
            op: impl FnOnce(Ty<T::Interner>) -> T,
        ) -> Binders<T> { ... }
    ...
}

// chalk-solve-0.55.0/src/clauses.rs
let binders = Binders::with_fresh_type_var(interner, |ty_var| ty_var);
```
Leads to a self-recursive type
- `T` = `Ty<T::Interner>`
- `T` = `Ty<?>`
- This type isn't know around this point, requires some downstream bounds to work.

## Added anti-recursion guards
```
..\rustc-1.54.0-src\vendor\chalk-solve-0.55.0\src\clauses.rs:761: warn:0:Spare Rule - &<::"chalk_ir-0_55_0"::Ty<</*RECURSE*/ as ::"chalk_ir-0_55_0"::interner::HasInterner>::Interner,> as ::"chalk_ir-0_55_0"::interner::HasInterner>::Interner := &I/*M:0*/
..\rustc-1.54.0-src\vendor\chalk-solve-0.55.0\src\clauses.rs:753: warn:0:Spare Rule - <::"chalk_ir-0_55_0"::Ty<</*RECURSE*/ as ::"chalk_ir-0_55_0"::interner::HasInterner>::Interner,> as ::"chalk_ir-0_55_0"::interner::HasInterner>::Interner : ::"chalk_ir-0_55_0"::interner::Interner
..\rustc-1.54.0-src\vendor\chalk-solve-0.55.0\src\clauses.rs:781: warn:0:Spare Rule - I/*M:0*/ = < ::"chalk_ir-0_55_0"::Ty<<::"chalk_ir-0_55_0"::Ty</*RECURSE*/,> as ::"chalk_ir-0_55_0"::interner::HasInterner>::Interner,> as ::"chalk_ir-0_55_0"::interner::HasInterner >::Interner
:0: BUG:..\..\src\hir_typeck\expr_cs.cpp:7313: Spare rules left after typecheck stabilised
```

Probably the ivar for `Binders_T` shouldn't have been expanded/assigned to be `Ty<Binders_T>::Interner` (so the rule above doing `&<Ty<Binders_T>::Interner> = &I` would set it to `I` correctly via `Ty<T>::Interner == T`)


## HACK: Updating the source worked
```
let binders = Binders::with_fresh_type_var(interner, |ty_var| ty_var);
```
to
```
let binders = Binders::<Ty<I>>::with_fresh_type_var(interner, |ty_var| ty_var);
```

So, from that `Binders_T = Ty<I>`

## Error source: Generating the loop
```
Typecheck Expressions-      check_coerce: >> (R26 _/*155*/ := 000001BE70835FD0 000001BE75D536A0 (::"chalk_ir-0_55_0"::Ty<<_/*155*/ as ::"chalk_ir-0_55_0"::interner::HasInterner>::Interner/*?*/,>/*S*/) - _/*155*/ := ::"chalk_ir-0_55_0"::Ty<<_/*155*/ as ::"chalk_ir-0_55_0"::interner::HasInterner>::Interner,>)
Typecheck Expressions-       check_coerce_tys: >> (_/*155*/ := ::"chalk_ir-0_55_0"::Ty<<_/*155*/ as ::"chalk_ir-0_55_0"::interner::HasInterner>::Interner/*?*/,>/*S*/)
Typecheck Expressions-       check_coerce_tys: << ()
Typecheck Expressions-       `anonymous-namespace'::check_coerce: Trigger equality - Completed
Typecheck Expressions-        HMTypeInferrence::set_ivar_to: Set IVar 155 = ::"chalk_ir-0_55_0"::Ty<<_/*155*/ as ::"chalk_ir-0_55_0"::interner::HasInterner>::Interner/*?*/,>/*S*/
```
Before allowing an assignment in `check_coerce` (especially of an ivar), check if it would trigger recursion (i.e. the mentioned ivar is in the target type, but the types aren't equal)
- If it does, defer the coercion once more.
- Worked, in that it didn't cause the loop, but still has a failure to infer.
- Not consuming `R26` means that `_/*155*/` isn't inferred to be a `Ty<...>`
  - Could detect the potential loop and introduce a new ivar for the param to `Ty`?
    - Check/ensure that there is a UfcsKnown in the looping variable
    - Replace the UfcsKnown with a new ivar (and add a ATY rule for it)
 - Worked!
 - Moved the anti-recursion to `Context::equate_types` (where it works all the time, and is less expensive to implement)

# (1.54) `::"rustc_middle-0_0_0"::middle::limits::update_limit`
```
..\rustc-1.54.0-src\compiler\rustc_middle\src\middle\limits.rs:54: error:0:Type mismatch between ::"core-0_0_0"::num::error::IntErrorKind and &::"core-0_0_0"::num::error::IntErrorKind
```

`.parse()` result type only inferred through a `From::from` call, leads to the match default being applied (via ivar possibilities) before the parse result type is known (and thus the error type is known).

Query: `parse()`'s result should be known, and the error type should be an aty bound.
- BUT, there's a method call on that ATY/UfcsKnown bound, which is the thing being inferred.
- So, why does the result type not infer beforehand?

Solution: If there's an ivar with only one possibility, and we're in a fallback mode - then pick that possibility (if it fits available bounds)
- This leads to the coercions being traversed earlier, thus avoiding the inference race.


# (1.54) `<::"rustc_mir_build-0_0_0"::build::Builder/*S*/>::bind_pattern`
```
..\rustc-1.54.0-src\compiler\rustc_mir_build\src\build\matches/mod.rs:387: error:0:Failed to find an impl of ::"core-0_0_0"::ops::function::FnOnce<(::"rustc_mir_build-0_0_0"::build::matches::Candidate,&mut _/*93*/,),> for {000002938DE60E10}(::"rustc_mir_build-0_0_0"::build::matches::Candidate,&[(::"alloc-0_0_0"::vec::Vec<::"rustc_mir_build-0_0_0"::build::matches::Binding,::"alloc-0_0_0"::alloc::Global,>,::"alloc-0_0_0"::vec::Vec<::"rustc_mir_build-0_0_0"::build::matches::Ascription,::"alloc-0_0_0"::alloc::Global,>,)],)->() with Output = ()
```

Early-infer of the closure's argument type, before the expected argument type information was available.
```
Typecheck Expressions-     Context::dump: R11 &mut _/*95*/ := 000002938C755870 000002938C7548E0 (&mut {000002938DE60E10}(_/*39*/,_/*40*/,)->_/*30*/)
Typecheck Expressions-           Context::equate_types_assoc: ++ R4 () = < `_/*95*/` as `::"core-0_0_0"::ops::function::FnOnce<(_/*92*/, &mut _/*93*/, ),>` >::Output
Typecheck Expressions-     check_ivar_poss: >> (40)
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: 40: possible_tys = C- &[(::"alloc-0_0_0"::vec::Vec<::"rustc_mir_build-0_0_0"::build::matches::Binding/*S*/,::"alloc-0_0_0"::alloc::Global/*S*/,>/*S*/, ::"alloc-0_0_0"::vec::Vec<::"rustc_mir_build-0_0_0"::build::matches::Ascription/*S*/,::"alloc-0_0_0"::alloc::Global/*S*/,>/*S*/, )]
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: Only &[(::"alloc-0_0_0"::vec::Vec<::"rustc_mir_build-0_0_0"::build::matches::Binding/*S*/,::"alloc-0_0_0"::alloc::Global/*S*/,>/*S*/, ::"alloc-0_0_0"::vec::Vec<::"rustc_mir_build-0_0_0"::build::matches::Ascription/*S*/,::"alloc-0_0_0"::alloc::Global/*S*/,>/*S*/, )] is an option
Typecheck Expressions-       HMTypeInferrence::set_ivar_to: Set IVar 40 = &[(::"alloc-0_0_0"::vec::Vec<::"rustc_mir_build-0_0_0"::build::matches::Binding/*S*/,::"alloc-0_0_0"::alloc::Global/*S*/,>/*S*/, ::"alloc-0_0_0"::vec::Vec<::"rustc_mir_build-0_0_0"::build::matches::Ascription/*S*/,::"alloc-0_0_0"::alloc::Global/*S*/,>/*S*/, )]
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: One possibility (before ivar removal), setting to closure[000002938DE60E10](_/*39*/, _/*40*/, ) -> ()
Typecheck Expressions-       HMTypeInferrence::set_ivar_to: Set IVar 95 = closure[000002938DE60E10](_/*39*/, _/*40*/, ) -> ()
Typecheck Expressions-     Typecheck_Code_CS: - R4 () = < `_/*95*/` as `::"core-0_0_0"::ops::function::FnOnce<(::"rustc_mir_build-0_0_0"::build::matches::Candidate/*S*/, &mut _/*93*/, ),>` >::Output
```

Need to prevent auto-inferring of closure argument types? (bound them to include self?)
- Challenge: Closures aren't revisited (currently)
- HACK: Do a disable on closures when being unsized to an ivar.


# (1.54) `<::"schannel-0_1_19"::security_context::SecurityContext/*S*/>::attribute`
```
..\rustc-1.54.0-src\vendor\schannel\src\security_context.rs:101: error:0:Type mismatch between T/*M:0*/ and ::"core-0_0_0"::ffi::c_void
```

```
Typecheck Expressions-         Context::equate_types_coerce: ++ R6 _/*35*/ := 000001D9AD738D70 000001D9BC0014E0 (_/*24*/)
Typecheck Expressions-     check_coerce: >> (R10 ::"core-0_0_0"::result::Result<T/*M:0*/,::"std-0_0_0"::io::error::Error/*S*/,>/*E*/ := 000000A6D912E5A8 000001D9BA421FC0 (::"core-0_0_0"::result::Result<_/*35*/,::"std-0_0_0"::io::error::Error/*S*/,>/*E*/) - ::"core-0_0_0"::result::Result<T/*M:0*/,::"std-0_0_0"::io::error::Error,> := ::"core-0_0_0"::result::Result<_/*35*/,::"std-0_0_0"::io::error::Error,>)
Typecheck Expressions-       HMTypeInferrence::set_ivar_to: Set IVar 35 = T/*M:0*/
Typecheck Expressions-     visit: >> (*mut _/*13*/ as *mut ::"core-0_0_0"::ffi::c_void/*E*/)
Typecheck Expressions-       HMTypeInferrence::set_ivar_to: Set IVar 13 = ::"core-0_0_0"::ffi::c_void/*E*/
Typecheck Expressions-     check_coerce: >> (R6 T/*M:0*/ := 000001D9AD738D70 000001D9BC0014E0 (::"core-0_0_0"::ffi::c_void/*E*/) - T/*M:0*/ := ::"core-0_0_0"::ffi::c_void)
Typecheck Expressions-       Context::equate_types_inner: - l_t = T/*M:0*/, r_t = ::"core-0_0_0"::ffi::c_void/*E*/
```

```
Typecheck Expressions-     check_ivar_poss: >> (13)
Typecheck Expressions-      `anonymous-namespace'::check_ivar_poss: - Source/Destination type
Typecheck Expressions-      Context::equate_types: _/*13*/ == _/*13*/
Typecheck Expressions-     check_ivar_poss: << ()
Typecheck Expressions-     Context::possible_equate_ivar_unknown: 13 = ?? (From)
Typecheck Expressions-     Context::possible_equate_ivar_unknown: 13 = ?? (To)
Typecheck Expressions-     Typecheck_Code_CS: --- IVar possibilities (fallback 1)
```

Solution: check if the equal source/destination is the current type, and skip if so.

# (1.54) `::"cargo_util-0_1_0"::paths::write_if_changed`
```
..\rustc-1.54.0-src\src\tools\cargo\crates\cargo-util\src\paths.rs:183: warn:0:Spare Rule - _/*2*/ : ::"anyhow-1_0_40_H2"::context::ext::StdError
..\rustc-1.54.0-src\src\tools\cargo\crates\cargo-util\src\paths.rs:183: warn:0:Spare Rule - _/*2*/ : ::"core-0_0_0"::marker::Send
:0: warn:0:Spare Rule - _/*2*/ : ::"core-0_0_0"::convert::From<::"std-0_0_0"::io::error::Error/*S*/,>
..\rustc-1.54.0-src\src\tools\cargo\crates\cargo-util\src\paths.rs:183: warn:0:Spare Rule - _/*2*/ : ::"core-0_0_0"::marker::Sync
:0: BUG:..\..\src\hir_typeck\expr_cs.cpp:7391: Spare rules left after typecheck stabilised
```

```
Typecheck Expressions-            equate_types: >> (_/*91*/ == closure[000001DF369EA8E0]() -> ::"core-0_0_0"::result::Result<(),_/*2*/,>/*E*/)
```

Looks like a name resolution error, used `std::result::Result` instead of `anyhow::Result`. Also, didn't error properly when `Result<()>` was seen (should require all or none)
- Resolve is correct (points at `anyhow`'s `Result`), but HIR dump shows closure result as not having the error set
- "Resolve Type Aliases"

```
Resolve Type Aliases-             ConvertHIR_ExpandAliases_GetExpansion_GP: ::"anyhow-1_0_40_H2"::Result<(),> -> ::"anyhow-1_0_40_H2"::Result<(),_,> -> ::"core-0_0_0"::result::Result<(),_,>/*?*/
```

Anyhow's definition: `pub type Result<T, E = Error> = core::result::Result<T, E>;`

Problem: RTA doesn't correctly handle missing/not-complete argument lists