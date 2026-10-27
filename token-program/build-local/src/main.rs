//! `cargo run --release -- ..` from here builds ../ into ../token.bin.
fn main() {
    let dir = std::env::args().nth(1).unwrap_or_else(|| "..".into());
    let pkg = risc0_build::get_package(&dir);
    let out = std::path::Path::new(&dir).join("target/local");
    for g in risc0_build::build_package(&pkg, &out, risc0_build::GuestOptions::default()).expect("guest build failed") {
        let p = std::path::Path::new(&dir).join(format!("{}.bin", g.name));
        std::fs::write(&p, &g.elf).expect("write the binary");
        println!("wrote {} ({} bytes), image id {}", p.display(), g.elf.len(), g.image_id);
    }
}
