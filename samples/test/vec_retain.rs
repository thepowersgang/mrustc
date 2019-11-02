fn main()
{
    let mut vec = vec![1, 2, 3];
    vec.retain(|v| { println!("{}", v); true });
    assert!(vec.len() == 3);
}
