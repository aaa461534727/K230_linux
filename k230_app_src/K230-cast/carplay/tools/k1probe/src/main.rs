//! K1 工具链探针: 验证 riscv64gc 上 CarPlay 需要的核心 crate 能否编+跑。
use chacha20poly1305::aead::{Aead, KeyInit, Payload};
use chacha20poly1305::{ChaCha20Poly1305, Nonce};

fn main() {
    // 1) plist (bplist 收发)
    let mut d = plist::Dictionary::new();
    d.insert("name".into(), plist::Value::String("K230".into()));
    d.insert("widthPixels".into(), plist::Value::Integer(480.into()));
    let mut bytes: Vec<u8> = Vec::new();
    plist::to_writer_binary(&mut bytes, &plist::Value::Dictionary(d.clone())).expect("plist encode");
    let back: plist::Value = plist::from_bytes(&bytes).expect("plist decode");
    assert_eq!(back, plist::Value::Dictionary(d));

    // 2) HKDF-SHA512 + ChaCha20-Poly1305 (CarPlay 流加密)
    use hkdf::Hkdf;
    use sha2::Sha512;
    let hk = Hkdf::<Sha512>::new(Some(b"DataStream-Salt110"), b"shared-secret");
    let mut key = [0u8; 32];
    hk.expand(b"DataStream-Input-Encryption-Key", &mut key).expect("hkdf");
    let cipher = ChaCha20Poly1305::new((&key).into());
    let nonce = Nonce::from_slice(&[0u8; 12]);
    let ct = cipher
        .encrypt(nonce, Payload { msg: b"carplay-frame", aad: b"ts-ssrc" })
        .expect("encrypt");
    let pt = cipher
        .decrypt(nonce, Payload { msg: &ct, aad: b"ts-ssrc" })
        .expect("decrypt");
    assert_eq!(pt, b"carplay-frame");

    // 3) Curve25519 / Ed25519 (pair-verify)
    use curve25519_dalek::scalar::Scalar;
    let s = Scalar::from(42u64);
    let p = &s * curve25519_dalek::constants::ED25519_BASEPOINT_TABLE;
    assert_ne!(p.compress().to_bytes(), [0u8; 32]);

    println!(
        "K1 OK: plist {}B / hkdf+chacha ct {}B / ristretto-ish pk {}",
        bytes.len(),
        ct.len(),
        hex4(&p.compress().to_bytes())
    );
}

fn hex4(b: &[u8]) -> String {
    b.iter().take(4).map(|x| format!("{:02x}", x)).collect()
}
