// SPDX-License-Identifier: MIT
// Beacon: exercise Rust std on riscv64 Haiku.
use std::collections::HashMap;
use std::io::{Read, Write};
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

fn check(name: &str, ok: bool, detail: String) {
    println!("{} {:<10} {}", if ok { "PASS" } else { "FAIL" }, name, detail);
}

fn main() {
    // threads, channels, mutex
    let total = Arc::new(Mutex::new(0u64));
    let (tx, rx) = mpsc::channel();
    let mut hs = Vec::new();
    for t in 0..4u64 {
        let total = Arc::clone(&total);
        let tx = tx.clone();
        hs.push(std::thread::spawn(move || {
            let s: u64 = (0..100_000u64).map(|x| x * t).sum();
            *total.lock().unwrap() += s;
            tx.send(t).unwrap();
        }));
    }
    drop(tx);
    for h in hs { h.join().unwrap(); }
    let got: Vec<u64> = rx.iter().collect();
    check("threads", got.len() == 4 && *total.lock().unwrap() == 29999700000, format!("{:?}", *total.lock().unwrap()));

    // thread-locals
    thread_local!(static TL: std::cell::Cell<u32> = std::cell::Cell::new(7));
    let other = std::thread::spawn(|| { TL.with(|v| { v.set(9); v.get() }) }).join().unwrap();
    check("tls", TL.with(|v| v.get()) == 7 && other == 9, format!("main 7, other {}", other));

    // files
    let p = std::env::temp_dir().join("beacon_rust_test.txt");
    std::fs::write(&p, b"beacon").unwrap();
    let back = std::fs::read_to_string(&p).unwrap_or_default();
    let _ = std::fs::remove_file(&p);
    check("files", back == "beacon", format!("{}", p.display()));

    // time
    let t0 = Instant::now();
    std::thread::sleep(Duration::from_millis(200));
    let el = t0.elapsed().as_millis();
    let now = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    check("time", (190..400).contains(&el) && now > 1_700_000_000, format!("slept {} ms, unix {}", el, now));

    // env, hashmap, formatting of floats
    let mut m = HashMap::new();
    for i in 0..1000 { m.insert(i, i * i); }
    check("misc", m[&999] == 998001 && format!("{:.3}", 2.0f64.sqrt()) == "1.414",
        format!("HOME={}", std::env::var("HOME").unwrap_or_default()));

    // DNS + TCP (plain HTTP)
    let net = (|| -> std::io::Result<String> {
        let mut s = std::net::TcpStream::connect(("example.com", 80))?;
        s.set_read_timeout(Some(Duration::from_secs(10)))?;
        s.write_all(b"HEAD / HTTP/1.0\r\nHost: example.com\r\n\r\n")?;
        let mut buf = String::new();
        s.read_to_string(&mut buf)?;
        Ok(buf.lines().next().unwrap_or("").to_string())
    })();
    check("network", matches!(&net, Ok(l) if l.starts_with("HTTP/")), format!("{:?}", net));

    // panic in a thread is caught by join
    let r = std::thread::spawn(|| { panic!("deliberate test panic") }).join();
    check("panic", r.is_err(), "thread panic caught".into());
}
