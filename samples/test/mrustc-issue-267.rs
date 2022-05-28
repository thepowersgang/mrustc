fn main() {
	let a: (i32, (i32,i32)) = (0, (1,2));

	// Float ranges
	0.0..1.0;
	1.0...1.5;
	1.0..=1.5;
	// Tuple indexing with ranges
	a.0..a.1.1;
	a.1.0..a.1.1;
}
