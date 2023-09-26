use std::env;

fn main() {
    let args: Vec<String> = env::args().collect();

    match args.len() {
        2 => {
            match args[1].parse() {
                Ok(42) => println!("branch ok(42)"),
                _ => println!("branch _"),
            }
        },
        _ => {
            // code
        }
    }
}
