
Target Overrides
================

Mrustc supports a few targets out of the box (of varying levels of usablity), these are listed in
`src/trans/target.cpp`. If the built-in targets aren't suitable, a custom target can be specified with the help of a
custom target toml file.

To specify a target when running `mrustc` (or `minicargo`), pass `--target <triple>` or `--target
./path/to/target.toml` (the presence of a slash, backwards or forwards, is what determines if the target is treated as
a custom target file.


Custom target format
--------------------

Recreation of the `arm-linux-gnu` target
```toml
[target]
family = "unix"
os-name = "linux"
env-name = "gnu"
arch = "arm"

[backend.c]
variant = "gnu"
target = "arm-linux-gnu"
```

Recreation of the `i586-windows-gnu` target (with architecture definition)
```toml
[target]
family = "windows"
os-name = "windows"
env-name = "gnu"

[backend.c]
variant = "gnu"
target = "mingw32"

[arch]
name = "x86"
pointer-bits = 32
is-big-endian = false
has-atomic-u8 = true
has-atomic-u16 = true
has-atomic-u32 = true
has-atomic-u64 = false
has-atomic-ptr = true
```
