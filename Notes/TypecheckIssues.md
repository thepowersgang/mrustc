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

