//! SRP-6a 服务端 (RFC 5054 风格, 3072 位组 + SHA-512) —— 参数口径照抄 uxplay `lib/srp.c`:
//!   · 组 3072 (N 384 B, g=5)
//!   · k  = H(N | PAD(g))            (H_nn_rfc5054: 两段都左补零到 len(N))
//!   · x  = H(salt | H(user ":" pass))
//!   · u  = H(PAD(A) | PAD(B))
//!   · S  = (A * v^u)^b mod N,  K = H(S)
//!   · M1 = H( H(N) xor H(g) | H(I) | salt | A | B | K )   (客户端送来的 proof)
//!   · M4 = H( A | M1 | K )                                (我们回的 proof)
//!
//! 真机实测: iOS 的 M3 是 `0x06 state=3` + `0x03 publicKey 384 B`(分片 255+129) + `0x04 proof 64 B`,
//! 与上面这套参数完全吻合。

use num_bigint::BigUint;
use num_traits::Num;
use sha2::{Digest, Sha512};

const N_HEX: &str = "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3BE39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";

fn h(parts: &[&[u8]]) -> Vec<u8> {
    let mut d = Sha512::new();
    for p in parts {
        d.update(p);
    }
    d.finalize().to_vec()
}

fn pad(x: &BigUint, len: usize) -> Vec<u8> {
    let b = x.to_bytes_be();
    let mut v = vec![0u8; len.saturating_sub(b.len())];
    v.extend_from_slice(&b);
    v
}

pub struct Server {
    n: BigUint,
    g: BigUint,
    k: BigUint,
    b: BigUint,
    v: BigUint,
    b_pub: BigUint,
    pub salt: [u8; 16],
    username: String,
}

impl Server {
    pub fn new(password: &str, username: &str) -> Server {
        let n = BigUint::from_str_radix(N_HEX, 16).expect("N");
        let g = BigUint::from(5u32);
        let len = 384;
        let k = BigUint::from_bytes_be(&h(&[&pad(&n, len), &pad(&g, len)]));

        let mut salt = [0u8; 16];
        use rand::RngCore;
        rand::thread_rng().fill_bytes(&mut salt);

        // x = H(salt | H(user ":" pass))
        let inner = h(&[username.as_bytes(), b":", password.as_bytes()]);
        let x = BigUint::from_bytes_be(&h(&[&salt, &inner]));
        let v = g.modpow(&x, &n);

        let b = BigUint::from_bytes_be(&{
            let mut r = [0u8; 32];
            rand::thread_rng().fill_bytes(&mut r);
            r
        });
        let b_pub = (k.clone() * &v + g.modpow(&b, &n)) % &n;

        Server { n, g, k, b, v, b_pub, salt, username: username.to_string() }
    }

    pub fn b_bytes(&self) -> Vec<u8> {
        pad(&self.b_pub, 384)
    }

    /// 用客户端 M3 的 A 与 proof 校验, 成功返回 (M4 proof, 64B 会话密钥)。
    ///
    /// 口径 = **PlayPort Srp6a.kt** (rk3528 真机 iPhone CarPlay 验证通过, LIVI srp.ts 的移植):
    ///   u  = H(PAD(A) | PAD(B))        (A/B 左补零到 384, RFC 5054)
    ///   S  = (A · v^u)^b mod N
    ///   K  = H(S)                       (64 B, 不是 uxplay 的交错 128B)
    ///   M1 = H( H(N)^H(g) | H(I) | salt | PAD(A) | PAD(B) | K )
    ///   M4 = H( PAD(A) | M1 | K )
    /// 上一个版本按 uxplay AirPlay 变体 (不补零 + 交错 K) 写, CarPlay 的 iPhone 客户端不吃这套。
    pub fn verify(&self, a_bytes: &[u8], client_proof: &[u8]) -> Option<(Vec<u8>, Vec<u8>)> {
        let a = BigUint::from_bytes_be(a_bytes) % &self.n;
        let a_pad = pad(&a, 384);
        let b_pad = pad(&self.b_pub, 384);

        let u = BigUint::from_bytes_be(&h(&[&a_pad, &b_pad]));
        let av = (a * self.v.modpow(&u, &self.n)) % &self.n;
        let s = av.modpow(&self.b, &self.n);
        let key = h(&[&s.to_bytes_be()]); // 64 B

        let hn = h(&[&self.n.to_bytes_be()]);
        let hg = h(&[&self.g.to_bytes_be()]);
        let xor: Vec<u8> = hn.iter().zip(hg.iter()).map(|(x, y)| x ^ y).collect();
        let hi = h(&[self.username.as_bytes()]);
        let m1 = h(&[&xor, &hi, &self.salt, &a_pad, &b_pad, &key]);
        if m1 != client_proof {
            return None;
        }
        let m4 = h(&[&a_pad, &m1, &key]);
        Some((m4, key))
    }
}
