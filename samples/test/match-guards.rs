// compile-flags: --test

#[test]
fn multi_guard()
{
    enum Foo {
        EnumMatching(usize, Vec<u8>),
        Struct(Vec<u8>),
        None,
    }

    fn test(v: &Foo) -> bool {
        match *v {
            Foo::EnumMatching(.., ref all_fields) |
            Foo::Struct(.., ref all_fields) if !all_fields.is_empty() => {
                true
                },
            _ => false,
        }
    }

    assert!( test(&Foo::EnumMatching(0, vec![0])) );
    assert!( test(&Foo::Struct(vec![0])) );
    assert!( !test(&Foo::EnumMatching(0, vec![])) );
    assert!( !test(&Foo::Struct(vec![])) );
    assert!( !test(&Foo::None) );
}
