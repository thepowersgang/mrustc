
enum Values
{
    One,
    Two,
    Three,
}

fn main()
{
    assert!(match Some(&1) { Some(1) => true, _ => false });
    assert!(match Some(1) { Some(1) => true, _ => false });
    assert!(match Values::One { Values::One => true, _ => false });
}

