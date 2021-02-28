// compile-flags: --test
#![feature(generators,generator_trait)]
use ::std::ops::{Generator, GeneratorState};
use ::std::pin::Pin;
use ::std::fmt::Debug;
use ::std::cmp::PartialEq;

fn test_gen<Y: Debug + PartialEq,R: Debug + PartialEq,G: Generator<Yield=Y,Return=R>>(mut gen: G, yields: &[G::Yield], rv: &G::Return) {
	let mut gen_ptr = unsafe { Pin::new_unchecked(&mut gen) };
	let mut yields_it = yields.iter();
	loop
	{
		match gen_ptr.as_mut().resume()
		{
		GeneratorState::Yielded(have) =>
			match yields_it.next()
			{
			Some(exp) => assert_eq!(have, *exp),
			None => panic!("Unexpected yield"),
			},
		GeneratorState::Complete(have) =>
			match yields_it.next()
			{
			None => { assert_eq!(have, *rv); return },
			Some(_) => panic!("Expected yield"),
			},
		}
	}
}

#[test]
fn basic() {
	let gen = || {
		yield 1;
		return 'a';
		};
	
	test_gen::<i32,char,_>(gen, &[1], &'a');
}

#[test]
fn counter() {
	let mut outer_c = 0;
	let gen = static || {
		let mut inner_c = 100;
		yield (outer_c,inner_c);
		outer_c += 1;
		yield (outer_c,inner_c);
		inner_c -= 1;
		yield (outer_c,inner_c);
		return 'a';
		};
	
	test_gen::<(i32,i32),char,_>(gen, &[(0,100),(1,100),(1,99)], &'a');
}

#[test]
fn drop_0() {
	struct DropSentinel<'a>(&'a mut bool);
	impl<'a> Drop for DropSentinel<'a> {
		fn drop(&mut self) { *self.0 = true; }
	}
	
	let mut cap_dropped = false;
	let mut held_dropped = false;
	let capture = DropSentinel(&mut cap_dropped);
	let gen = static move || {
		let _ = capture;
		let held = DropSentinel(&mut held_dropped);
		yield ();
		let _ = held;
		};
	drop(gen);
	assert!(cap_dropped);
	assert!(!held_dropped);
}

#[test]
fn drop_1() {
	struct DropSentinel<'a>(&'a mut bool);
	impl<'a> Drop for DropSentinel<'a> {
		fn drop(&mut self) { *self.0 = true; }
	}
	
	let mut cap_dropped = false;
	let mut held_dropped = false;
	{
	let capture = DropSentinel(&mut cap_dropped);
	let mut held_dropped = &mut held_dropped;
	let mut gen = static move || {
		let _ = capture;
		let held = DropSentinel(held_dropped);
		yield ();
		let _ = held;
		};
	{
		let mut gen_ptr = unsafe { Pin::new_unchecked(&mut gen) };
		gen_ptr.as_mut().resume();
	}
	drop(gen);
	}
	assert!(cap_dropped);
	assert!(held_dropped);
}

#[test]
fn drop_ret() {
	struct DropSentinel<'a>(&'a mut bool);
	impl<'a> Drop for DropSentinel<'a> {
		fn drop(&mut self) {
			*self.0 = true;
			println!("?");
		}
	}
	
	let mut cap_dropped = false;
	let mut held_dropped = false;
	{
	let capture = DropSentinel(&mut cap_dropped);
	let mut held_dropped = &mut held_dropped;
	let mut gen = static move || {
		let _ = capture;
		let held = DropSentinel(held_dropped);
		yield ();
		let _ = held;
		};
	{
		let mut gen_ptr = unsafe { Pin::new_unchecked(&mut gen) };
		gen_ptr.as_mut().resume();
		gen_ptr.as_mut().resume();
	}
	drop(gen);
	}
	assert!(cap_dropped);
	assert!(held_dropped);
}
