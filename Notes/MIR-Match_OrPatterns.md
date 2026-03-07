Or patterns, Match guards, and if-let

Problem: or-patterns mean that there are multiple inputs and outputs of a guarded match arm
It's possible for or-patterns to overlap, and thus the guard run twice

e.g.
```rust
fn test(a: Option<u32>, b: Option<u32>) {
    match (a, b)
    {
    (Some(v), _) | (_, Some(v)) => if v == 0 {},
    _ => {},
    }
}
```
In this case, the guard has to run twice, if, `(Some(1), Some(0))` is passed


# Background
Logically, a `match` evaluates as if each arm is tried in sequence (including or patterns).
This would intuitively mean that guards and body code is duplicated for each or-pattern, but
in practice the body can be shared between or-pattern duplications (although the guard needs
to be duplicated in order for the first or to fall through to the second one).

E.g.
```rust
match foo {
    Ok(_) if bar() => 1,
    Ok(v) => v,
    Err(_) => panic(),
}
```
expands as if it's the following
```rust
loop {
    let _value = foo;
    if let Ok(_) = _value {
        if bar() {
            break { 1 };
        }
    }
    if let Ok(v) = _value {
        if true {
            break v;
        }
    }
    if let Err(_) = _value {
        if true {
            break panic();
        }
    }
    diverge()
}
```

# Scope stacks
To get the right variable state tracking, MIR match lowering needs to follow the logical pattern of the match arms:
- A loop scope wrapping the entire block, early-terminated in each arm.
- split scope with empty arm for each pattern
  - Early exited 
- same (split with empty arm) for each guard rule