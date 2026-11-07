//! carplay-airplayd —— K230 无线 CarPlay 的 AirPlay 服务端 (TCP 7000)。
//!
//! 进度: OPTIONS / /info / **完整 pair-setup (M1~M6)** 已实现。
//! SRP 口径 = PlayPort Srp6a.kt (rk3528 真机验证), M5/M6 用 ChaCha20-Poly1305 + Ed25519
//! (HAP 风格, nonce = 4 个 0 + "PS-Msg05"/"PS-Msg06" 8 B label)。
//! 已实现: pair-setup / pair-verify / auth-setup(MFi-SAP) / **SETUP 应答 + event/timing UDP**。
//! 未实现: RECORD / 流解密 / 镜像解码。

mod info;
mod rtsp;
mod srp;
mod tlv8;

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::Arc;

use plist::{Dictionary, Value};
use chacha20poly1305::aead::{Aead, KeyInit};
use chacha20poly1305::{ChaCha20Poly1305, Key, Nonce};
use ed25519_dalek::{Signer, SigningKey, Verifier, VerifyingKey};
use sha2::Sha512;

const PORT: u16 = 7000;
const IDENTITY_SEED: &str = "/etc/carplay/ed25519.seed";
const PAIRING_ID_FILE: &str = "/etc/carplay/pairing_id";
const PAIRINGS_DIR: &str = "/etc/carplay/pairings";

fn log(m: &str) {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    println!("[{now}] [airplay] {m}");
}

fn hex(b: &[u8], max: usize) -> String {
    b.iter().take(max).map(|x| format!("{x:02x}")).collect::<Vec<_>>().join(" ")
}

// ===== 配件长期身份 (Ed25519), 首次启动生成并落盘 =====

struct Identity {
    id: String,
    signing: SigningKey,
}

const MFI_DIR: &str = "/etc/carplay/mfi";

/// MFi-SAP `/auth-setup` 应答 (口径照 PlayPort `MfiSapAuthSetup.kt`):
/// 请求 = [0x01][32B 手机 X25519 公钥]; 应答 = 我公钥(32) + u32BE 证书长 + 证书(PKCS#7 DER) + u32BE 签名长 + AES-128-CTR 加密的 64B ECDSA 签名。
/// 密钥 = SHA1("AES-KEY"‖shared)[..16] / IV = SHA1("AES-IV"‖shared)[..16]; 签名摘要 = SHA256(我公钥‖手机公钥)。
fn auth_setup(body: &[u8]) -> Option<Vec<u8>> {
    if body.len() != 33 || body[0] != 1 {
        log(&format!("/auth-setup: 请求格式不对 ({} B, ver={})", body.len(), body.first().copied().unwrap_or(0)));
        return None;
    }
    let peer_arr: [u8; 32] = body[1..].try_into().ok()?;
    let mut seed = [0u8; 32];
    rand::RngCore::fill_bytes(&mut rand::thread_rng(), &mut seed);
    let secret = x25519_dalek::StaticSecret::from(seed);
    let our_pub = x25519_dalek::PublicKey::from(&secret).to_bytes();
    let shared = secret.diffie_hellman(&x25519_dalek::PublicKey::from(peer_arr)).to_bytes();
    if shared.iter().all(|b| *b == 0) {
        log("/auth-setup: 共享密钥全 0, 拒绝");
        return None;
    }
    // 屏幕流 (type 110) 的解密密钥以它为根 (DataStream-Salt<scid> 派生), 必须留住
    if let Ok(mut g) = SCREEN_SHARED.lock() {
        *g = Some(shared);
    }

    let dir = std::env::var("CARPLAY_MFI_DIR").unwrap_or_else(|_| MFI_DIR.to_string());
    let cert = std::fs::read(format!("{dir}/certificate.p7b"))
        .map_err(|e| log(&format!("/auth-setup: 读证书失败 {dir}/certificate.p7b: {e}")))
        .ok()?;
    let pk8 = std::fs::read(format!("{dir}/identity.pk8"))
        .map_err(|e| log(&format!("/auth-setup: 读私钥失败 {dir}/identity.pk8: {e}")))
        .ok()?;
    use p256::pkcs8::DecodePrivateKey as _;
    let sk = p256::ecdsa::SigningKey::from_pkcs8_der(&pk8)
        .map_err(|e| log(&format!("/auth-setup: 解析 pk8 失败: {e}")))
        .ok()?;

    let mut hk = sha1::Sha1::new();
    hk.update(b"AES-KEY");
    hk.update(shared);
    let key = hk.finalize();
    let mut hi = sha1::Sha1::new();
    hi.update(b"AES-IV");
    hi.update(shared);
    let iv = hi.finalize();

    // protocolMajor=3 ⇒ 摘要用 SHA-256 (PlayPort LocalMfiAuthenticationClient)
    use sha2::Digest as _;
    let mut d = sha2::Sha256::new();
    d.update(our_pub);
    d.update(peer_arr);
    let digest = d.finalize();
    use p256::ecdsa::signature::hazmat::PrehashSigner as _;
    let sig: p256::ecdsa::Signature = sk.sign_prehash(&digest).ok()?;
    let sig_bytes = sig.to_bytes();

    use aes::cipher::{KeyIvInit, StreamCipher};
    type Aes128Ctr = ctr::Ctr128BE<aes::Aes128>;
    let mut enc = sig_bytes.to_vec();
    let mut c = Aes128Ctr::new(key[..16].into(), iv[..16].into());
    c.apply_keystream(&mut enc);

    let mut out = Vec::with_capacity(32 + 4 + cert.len() + 4 + enc.len());
    out.extend_from_slice(&our_pub);
    out.extend_from_slice(&(cert.len() as u32).to_be_bytes());
    out.extend_from_slice(&cert);
    out.extend_from_slice(&(enc.len() as u32).to_be_bytes());
    out.extend_from_slice(&enc);
    log(&format!("/auth-setup → {} B (pub32 + cert{} + encsig{})", out.len(), cert.len(), enc.len()));
    Some(out)
}

fn load_identity() -> Identity {
    let seed: [u8; 32] = match std::fs::read(IDENTITY_SEED) {
        Ok(v) if v.len() == 32 => v.try_into().unwrap(),
        _ => {
            let mut s = [0u8; 32];
            rand::RngCore::fill_bytes(&mut rand::thread_rng(), &mut s);
            if let Err(e) = std::fs::write(IDENTITY_SEED, &s) {
                log(&format!("身份种子写入失败 {IDENTITY_SEED}: {e}"));
            }
            s
        }
    };
    let id = std::env::var("CARPLAY_PAIRING_ID").unwrap_or_else(|_| {
        match std::fs::read_to_string(PAIRING_ID_FILE) {
            Ok(s) if !s.trim().is_empty() => s.trim().to_string(),
            _ => {
                // 伪 UUID (无 rand uuid crate, 用随机字节拼)
                let mut b = [0u8; 16];
                rand::RngCore::fill_bytes(&mut rand::thread_rng(), &mut b);
                b[6] = (b[6] & 0x0f) | 0x40;
                b[8] = (b[8] & 0x3f) | 0x80;
                let s = format!(
                    "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                    b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                    b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]
                );
                let _ = std::fs::write(PAIRING_ID_FILE, &s);
                s
            }
        }
    });
    Identity { id, signing: SigningKey::from_bytes(&seed) }
}

fn hkdf_sha512(ikm: &[u8], salt: &[u8], info: &[u8]) -> [u8; 32] {
    let hk = hkdf::Hkdf::<Sha512>::new(Some(salt), ikm);
    let mut out = [0u8; 32];
    hk.expand(info, &mut out).expect("hkdf 32B");
    out
}

fn chacha(key: &[u8; 32], label8: &[u8; 8], data: &[u8], seal: bool) -> Option<Vec<u8>> {
    let mut n12 = [0u8; 12];
    n12[4..].copy_from_slice(label8);
    let cipher = ChaCha20Poly1305::new(Key::from_slice(key));
    let nonce = Nonce::from_slice(&n12);
    if seal {
        cipher.encrypt(nonce, data).ok()
    } else {
        cipher.decrypt(nonce, data).ok()
    }
}

/// 控制面分帧 ChaCha20-Poly1305: nonce = 4×0 + u64 LE 计数器, 2 B LE 长度头做 AAD (PlayPort ControlCipher 口径)
struct ControlCipher {
    read_key: [u8; 32],
    write_key: [u8; 32],
    read_ctr: u64,
    write_ctr: u64,
    enc_buf: Vec<u8>, // 还没凑满一帧的密文
}

impl ControlCipher {
    fn new(read_key: [u8; 32], write_key: [u8; 32]) -> ControlCipher {
        ControlCipher { read_key, write_key, read_ctr: 0, write_ctr: 0, enc_buf: Vec::new() }
    }

    fn nonce(ctr: u64) -> Nonce {
        let mut n = [0u8; 12];
        n[4..].copy_from_slice(&ctr.to_le_bytes());
        *Nonce::from_slice(&n)
    }

    /// 喂密文, 返回解出的明文 (可能为空, 帧没凑齐)
    fn feed(&mut self, data: &[u8]) -> Option<Vec<u8>> {
        use chacha20poly1305::aead::Payload;
        self.enc_buf.extend_from_slice(data);
        let mut out = Vec::new();
        loop {
            if self.enc_buf.len() < 2 {
                return Some(out);
            }
            let len = (self.enc_buf[0] as usize) | ((self.enc_buf[1] as usize) << 8);
            let frame = 2 + len + 16;
            if self.enc_buf.len() < frame {
                return Some(out);
            }
            let aad = [self.enc_buf[0], self.enc_buf[1]];
            let ct = self.enc_buf[2..frame].to_vec();
            let cipher = ChaCha20Poly1305::new(Key::from_slice(&self.read_key));
            let pt = cipher
                .decrypt(&Self::nonce(self.read_ctr), Payload { msg: &ct, aad: &aad })
                .ok()?;
            self.read_ctr += 1;
            out.extend_from_slice(&pt);
            self.enc_buf.drain(..frame);
        }
    }

    /// 加密一整个 RTSP 应答 (≤0x4000 B 一块)
    fn seal(&mut self, plaintext: &[u8]) -> Vec<u8> {
        use chacha20poly1305::aead::Payload;
        let mut out = Vec::new();
        for chunk in plaintext.chunks(0x4000) {
            let header = [(chunk.len() & 0xff) as u8, (chunk.len() >> 8) as u8];
            let cipher = ChaCha20Poly1305::new(Key::from_slice(&self.write_key));
            let ct = cipher
                .encrypt(&Self::nonce(self.write_ctr), Payload { msg: chunk, aad: &header })
                .expect("seal");
            self.write_ctr += 1;
            out.extend_from_slice(&header);
            out.extend_from_slice(&ct);
        }
        out
    }
}

// ===== 每条 TCP 连接的配对状态 =====

#[derive(Default)]
struct Session {
    srp: Option<srp::Server>,
    srp_key: Option<Vec<u8>>, // pair-setup 成功后的 64B 会话密钥
    pv: Option<PvSession>,
    control_keys: Option<([u8; 32], [u8; 32])>, // (read, write), pair-verify 之后
    peer_ip: Option<std::net::IpAddr>,           // 对端 IP (NTP client 要连它)
    shared: Option<[u8; 32]>,                    // 本连接的 pair-verify ECDH 共享 (流密钥根, 必须按连接存)
    volume: f64,                                 // 音量 (SET_PARAMETER 记, GET_PARAMETER 回)
}

/// pair-verify 的 X25519 临时会话 (M1→M3 跨请求保持)
struct PvSession {
    secret: x25519_dalek::StaticSecret,
    eph_pub: [u8; 32],
    client_pub: [u8; 32],
    shared: [u8; 32],
    enc_key: [u8; 32],
}

fn load_pairing(id: &[u8]) -> Option<VerifyingKey> {
    let path = format!("{PAIRINGS_DIR}/{}", hex(id, 64).replace(' ', ""));
    let v = std::fs::read(path).ok()?;
    let arr: [u8; 32] = v.try_into().ok()?;
    VerifyingKey::from_bytes(&arr).ok()
}

/// 配对端点 (pair-setup/pair-verify) 的应答必须带这个 Content-Type, 否则 iOS 不认 (PlayPort 口径)
fn pair_resp(cseq: Option<&str>, body: &[u8]) -> Vec<u8> {
    rtsp::response(cseq, &[("Content-Type", "application/pairing+tlv8".into())], body)
}

fn err_tlv(state: u8) -> Vec<u8> {
    tlv8::Tlv8::default()
        .with_u8(tlv8::STATE, state)
        .with_u8(tlv8::ERROR, 2)
        .to_bytes()
}

// ===== SETUP + 辅助 UDP 端口 =====
//
// 口径 (UxPlay / AirPlay2): iOS 先发一个**不带 streams** 的会话级 SETUP, 我们回 eventPort + timingPort,
// 它才会再发带 streams 的第二个 SETUP。少了这两个 UDP 端口, iOS 直接 teardown (2026-11-02 实测
// 收到 SETUP 后我们回空 200, 对端立刻关闭)。

const EVENT_PORT: u16 = 7010;
const TIMING_PORT: u16 = 7011;

static AUX_UDP: std::sync::Once = std::sync::Once::new();

fn start_aux_udp() {
    AUX_UDP.call_once(|| {
        std::thread::spawn(|| match std::net::UdpSocket::bind(("0.0.0.0", EVENT_PORT)) {
            Ok(s) => {
                log(&format!("event UDP {EVENT_PORT} 已监听"));
                let mut buf = [0u8; 8192];
                loop {
                    if let Ok((n, from)) = s.recv_from(&mut buf) {
                        let body = &buf[..n];
                        let text = plist::Value::from_reader(std::io::Cursor::new(body))
                            .map(|v| format!("{v:?}"))
                            .unwrap_or_else(|_| hex(body, 48));
                        log(&format!("event {from} {n} B: {text}"));
                    }
                }
            }
            Err(e) => log(&format!("event UDP {EVENT_PORT} bind 失败: {e}")),
        });
        // 注意: 7011 不在这里被动监听 —— 它要让给 NTP client 当**源端口** (见 start_ntp_client),
        // 口径同 UxPlay: 告诉 iOS 的 timingPort 必须就是我们发 NTP 的源端口。
    });
}

fn ntp_now() -> [u8; 8] {
    let d = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default();
    let secs = (d.as_secs() + 2_208_988_800) as u32;
    let frac = ((d.subsec_nanos() as u64) << 32) / 1_000_000_000;
    let mut o = [0u8; 8];
    o[..4].copy_from_slice(&secs.to_be_bytes());
    o[4..].copy_from_slice(&(frac as u32).to_be_bytes());
    o
}

/// NTP v4 应答 (48 B): originate = 请求最后一个时间戳, receive/transmit = 现在
fn timing_reply(req: &[u8]) -> Vec<u8> {
    let mut out = vec![0u8; 48];
    out[0] = 0x24; // LI=0 VN=4 Mode=4(server)
    out[1] = 2; // stratum
    out[2] = 6; // poll
    out[3] = 0xEC; // precision -20
    if req.len() >= 8 {
        let src = if req.len() >= 32 { req.len() - 8 } else { 0 };
        out[24..32].copy_from_slice(&req[src..src + 8]);
    }
    let now = ntp_now();
    out[16..24].copy_from_slice(&now); // reference
    out[32..40].copy_from_slice(&now); // receive
    out[40..48].copy_from_slice(&now); // transmit
    out
}

static NTP_GEN: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

/// 配件是 NTP **客户端**: 必须主动向 iOS 的 timingPort 发 32 B 请求 (UxPlay raop_ntp.c 口径,
/// 头 4 B = 80 d2 00 07, [24..32] = 发送时刻的 NTP 时间戳)。整天只监听等 iOS 来问是错的 ——
/// iOS 不会来, 它认为配件发起了同步 (2026-11-02 实测: 只回 SETUP 不动, iOS 静默)。
fn start_ntp_client(ip: std::net::IpAddr, port: u16) {
    use std::sync::atomic::Ordering;
    let gen = NTP_GEN.fetch_add(1, Ordering::SeqCst) + 1;
    log(&format!("NTP client 启动: {ip}:{port} (每 3 s 同步一次)"));
    std::thread::spawn(move || {
        // 源端口必须是 SETUP 响应里给 iOS 的 timingPort (UxPlay 的 timing_lport 口径)
        // 上一轮 NTP 线程的 socket 还占着 7011 是常态 (120 次重试里会连开多个 gen),
        // 先重试 2 s 再说, 别急着退到随机端口 —— 随机端口会让 SETUP 应答里的 timingPort 变谎话。
        let mut bound = None;
        for _ in 0..20 {
            if let Ok(s) = std::net::UdpSocket::bind(("0.0.0.0", TIMING_PORT)) {
                bound = Some(s);
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(100));
        }
        let sock = match bound {
            Some(s) => s,
            None => match std::net::UdpSocket::bind(("0.0.0.0", 0)) {
                Ok(s) => {
                    log(&format!("timing UDP {TIMING_PORT} 2 s 内没抢到, 改用随机源端口"));
                    s
                }
                Err(e2) => {
                    log(&format!("NTP client socket 失败: {e2}"));
                    return;
                }
            },
        };
        if let Ok(l) = sock.local_addr() {
            log(&format!("NTP client 源端口 = {}", l.port()));
        }
        let _ = sock.set_read_timeout(Some(std::time::Duration::from_millis(1500)));
        let mut req = [0u8; 32];
        req[0] = 0x80;
        req[1] = 0xD2;
        req[2] = 0x00;
        req[3] = 0x07;
        let mut ok = 0u32;
        let mut sent = 0u32;
        while NTP_GEN.load(Ordering::SeqCst) == gen {
            let t_send = ntp_now();
            let t_send_ns = u64::from_be_bytes(t_send) as i64;
            req[24..32].copy_from_slice(&t_send);
            sent += 1;
            if sock.send_to(&req, (ip, port)).is_err() {
                log("NTP 发送失败");
                break;
            }
            let mut buf = [0u8; 128];
            match sock.recv_from(&mut buf) {
                Ok((n, _)) if n >= 32 => {
                    ok += 1;
                    // UxPlay raop_ntp.c 口径: t0=客户端收到请求, t1=客户端发出响应, t2=回填的我们发送时刻, t3=我们收到
                    let t0 = u64::from_be_bytes(buf[8..16].try_into().unwrap()) as i64;
                    let t1 = u64::from_be_bytes(buf[16..24].try_into().unwrap()) as i64;
                    let t2 = u64::from_be_bytes(buf[24..32].try_into().unwrap()) as i64;
                    let t3 = t_send_u64() as i64;
                    let offset = ((t1 - t0) + (t2 - t3)) / 2;
                    let rtt = (t3 - t_send_ns) - (t1 - t0);
                    log(&format!(
                        "NTP #{ok}/{sent} 已同步 offset={offset} (1/2^32 s) rtt={rtt} 服务器类型={:#04x}",
                        buf[1] & !0x80
                    ));
                }
                _ => log(&format!("NTP #{sent} 无响应 (1.5 s 超时)")),
            }
            std::thread::sleep(std::time::Duration::from_secs(3));
        }
        log("NTP client 退出 (新会话或断开)");
    });
}

fn t_send_u64() -> u64 {
    u64::from_be_bytes(ntp_now())
}

// ===== 事件通道 (触摸回控) =====
// iOS 在 RECORD 之后主动连配件的 eventPort, 之后配件用 **Events 密钥** 加密的 POST /command
// 把 hidSendReport 推给手机。eventPort=0 时 iOS 不建这条通道 ⇒ 点屏毫无反应 (2026-11-02 实测)。

struct EventSender {
    stream: std::net::TcpStream,
    /// 延迟到"第一次发包时"再按**当时**的 shared 派生密钥:
    /// iOS 常常在 RECORD **之前**就把 event 连接建好, 那时全局 shared 还可能是上一条连接的,
    /// 早绑定必错 ⇒ 发的帧 iOS 解不开 ⇒ 拆会话 (2026-11-02 15:58 实测: 首个触摸包发出同秒断链)。
    chan: Option<ControlCipher>,
    cseq: u32,
}

static EVENT_TCP_PORT: std::sync::Mutex<u16> = std::sync::Mutex::new(0);
static EVENT_TX: std::sync::Mutex<Option<EventSender>> = std::sync::Mutex::new(None);

fn derive_event_keys(shared: &[u8]) -> ([u8; 32], [u8; 32]) {
    use hkdf::Hkdf;
    let hk = Hkdf::<Sha512>::new(Some(b"Events-Salt"), shared);
    let mut r = [0u8; 32];
    let mut w = [0u8; 32];
    hk.expand(b"Events-Read-Encryption-Key", &mut r).ok();
    hk.expand(b"Events-Write-Encryption-Key", &mut w).ok();
    (r, w)
}

fn start_event_listener() {
    static ONCE: std::sync::Once = std::sync::Once::new();
    ONCE.call_once(|| {
        let listener = match std::net::TcpListener::bind(("0.0.0.0", 0)) {
            Ok(l) => l,
            Err(e) => {
                log(&format!("event 监听失败: {e}"));
                return;
            }
        };
        let port = listener.local_addr().map(|a| a.port()).unwrap_or(0);
        if let Ok(mut g) = EVENT_TCP_PORT.lock() {
            *g = port;
        }
        log(&format!("event TCP {port} 已监听 (等 iPhone 在 RECORD 后连入)"));
        std::thread::spawn(move || {
            for conn in listener.incoming() {
                let Ok(stream) = conn else { continue };
                let peer = stream.peer_addr().ok();
                let shared = SCREEN_SHARED.lock().ok().and_then(|g| *g).unwrap_or([0u8; 32]);
                let (kr, kw) = derive_event_keys(&shared);
                log(&format!("[event] iPhone 连上事件通道 {peer:?}, Events 密钥就绪"));
                // 入站只读丢弃 (iOS 会推 modesChanged 等), 放后台
                if let Ok(mut rd) = stream.try_clone() {
                    std::thread::spawn(move || {
                        let mut buf = [0u8; 4096];
                        let mut got = 0u64;
                        loop {
                            match rd.read(&mut buf) {
                                Ok(0) => {
                                    log(&format!("[event] 对端关闭事件通道 (期间收到 {got} B)"));
                                    break;
                                }
                                Ok(n) => {
                                    got += n as u64;
                                    log(&format!("[event] ← 收到 {n} B (累计 {got} B)"));
                                }
                                Err(_) => break,
                            }
                        }
                    });
                }
                log(&format!("[event] 连接时 shared={} (若与 RECORD 那条不同, 会在首发时改用 RECORD 的)", hex(&shared, 4)));
                let _ = (kr, kw);
                if let Ok(mut g) = EVENT_TX.lock() {
                    *g = Some(EventSender { stream, chan: None, cseq: 0 });
                }
                start_touch_reader();
            }
        });
    });
}

/// 发一条 hidSendReport (加密的 POST /command)
fn send_hid_report(uid: &str, report: &[u8]) -> bool {
    let mut d = Dictionary::new();
    d.insert("type".into(), Value::String("hidSendReport".into()));
    d.insert("uuid".into(), Value::String(uid.into()));
    d.insert("hidReport".into(), Value::Data(report.to_vec()));
    let mut body = Vec::new();
    if plist::to_writer_binary(&mut body, &Value::Dictionary(d)).is_err() {
        return false;
    }
    let Ok(mut g) = EVENT_TX.lock() else { return false };
    let Some(ev) = g.as_mut() else { return false };
    if ev.chan.is_none() {
        let shared = SCREEN_SHARED.lock().ok().and_then(|g| *g).unwrap_or([0u8; 32]);
        let (kr2, kw2) = derive_event_keys(&shared);
        ev.chan = Some(ControlCipher::new(kr2, kw2));
        log(&format!("[event] 首发: 用 shared {} 派生 Events 密钥", hex(&shared, 4)));
    }
    ev.cseq += 1;
    let head = format!(
        "POST /command RTSP/1.0
CSeq: {}
Content-Type: application/x-apple-binary-plist
Content-Length: {}

",
        ev.cseq,
        body.len()
    );
    let mut msg = head.into_bytes();
    msg.extend_from_slice(&body);
    let Some(chan) = ev.chan.as_mut() else { return false };
    let frame = chan.seal(&msg);
    ev.stream.write_all(&frame).is_ok()
}

/// 读 /dev/input/event0 (FT5x06) → 转 CarPlay 坐标 → hidSendReport
fn start_touch_reader() {
    static ONCE: std::sync::Once = std::sync::Once::new();
    ONCE.call_once(|| {
        std::thread::spawn(|| {
            use std::io::Read as _;
            let path = std::env::var("CARPLAY_TOUCH_DEV").unwrap_or_else(|_| "/dev/input/event0".into());
            let mut f = match std::fs::File::open(&path) {
                Ok(f) => f,
                Err(e) => {
                    log(&format!("[touch] 打不开 {path}: {e}"));
                    return;
                }
            };
            log(&format!("[touch] 已接管 {path} (屏 480x800 → CarPlay 800x480)"));
            let mut buf = [0u8; 24 * 64];
            let (mut x, mut y) = (0i32, 0i32);
            let mut down = false;
            let mut touch_logged: u64 = 0;
            let (mut last_x, mut last_y, mut last_down) = (0u16, 0u16, false);
            let mut last_tx = std::time::Instant::now();
            loop {
                let n = match f.read(&mut buf) {
                    Ok(0) => break,
                    Ok(n) => n,
                    Err(e) => {
                        log(&format!("[touch] 读错误 {e}"));
                        break;
                    }
                };
                let mut off = 0;
                while off + 24 <= n {
                    let ev = &buf[off..off + 24];
                    let ty = u16::from_le_bytes([ev[16], ev[17]]);
                    let code = u16::from_le_bytes([ev[18], ev[19]]);
                    let val = i32::from_le_bytes([ev[20], ev[21], ev[22], ev[23]]);
                    off += 24;
                    match (ty, code) {
                        (3, 0) => x = val,
                        (3, 1) => y = val,
                        (1, 0x14a) => down = val != 0,
                        (0, 0) => {
                            // 屏(480x800 竖) → CarPlay(800x480 横); 与 drmplay rot90_cw 相反变换。
                            // ⚠ 目标范围必须是 **800x480**: cx∈[0,799] cy∈[0,479]。
                            //   之前写成 799-x 会产出 320..799 的 Y 值, **超出描述符逻辑上限**
                            //   ⇒ iOS 判报文非法 → 整个会话被拆 (2026-11-02 主人实测"一碰就黑屏+断连")。
                            let cx = y.clamp(0, 799) as u16;
                            let cy = (479 - x.clamp(0, 479)) as u16;
                            // 限流: 只有"按下/抬起/位置变了"才发, 且最多 ~60 次/秒。
                            // 之前每个 EV_SYN 都发 (含未按下时的无意义上报) ⇒ 单核被 plist+ChaCha 挤占,
                            // 解码器写帧中途停顿 >500 ms 被 drmplay 判"截断帧" → 刷黑 (2026-11-02 实测)。
                            let changed = (cx, cy, down) != (last_x, last_y, last_down);
                            if !changed {
                                continue;
                            }
                            let contact = if down { 0x03u8 } else { 0x00u8 };
                            let mut rep = [0u8; 10];
                            rep[0] = contact;
                            rep[1..3].copy_from_slice(&cx.to_le_bytes());
                            rep[3..5].copy_from_slice(&cy.to_le_bytes());
                            // 第二指空着
                            let ok = send_hid_report("1", &rep);
                            last_x = cx;
                            last_y = cy;
                            last_down = down;
                            last_tx = std::time::Instant::now();
                            touch_logged += 1;
                            if touch_logged <= 3 || touch_logged % 50 == 0 {
                                log(&format!(
                                    "[touch] #{} 屏({x},{y}) → CarPlay({cx},{cy}) down={down} 发送{}",
                                    touch_logged, if ok { "成功" } else { "失败" }
                                ));
                            }
                        }
                        _ => {}
                    }
                }
            }
        });
    });
}

/// auth-setup 协商出的 ECDH 共享密钥 (流密钥的根)
static SCREEN_SHARED: std::sync::Mutex<Option<[u8; 32]>> = std::sync::Mutex::new(None);

/// 流密钥 (golden `stream.rs:27` 口径): HKDF-SHA512, salt="DataStream-Salt<scid>",
/// output=...Output-Encryption-Key (解手机发来的数据), input=...Input-Encryption-Key (回传用)
fn derive_stream_keys(shared: &[u8], scid: u64) -> ([u8; 32], [u8; 32]) {
    use hkdf::Hkdf;
    let salt = format!("DataStream-Salt{scid}");
    let out = Hkdf::<Sha512>::new(Some(salt.as_bytes()), shared);
    let mut o = [0u8; 32];
    let mut i = [0u8; 32];
    out.expand(b"DataStream-Output-Encryption-Key", &mut o).ok();
    out.expand(b"DataStream-Input-Encryption-Key", &mut i).ok();
    (o, i)
}

/// AVCC (长度前缀) → Annex-B (00 00 00 01 起始码)
fn avcc_to_annexb(data: &[u8], nal_len: usize) -> Vec<u8> {
    let mut out = Vec::with_capacity(data.len() + 32);
    let mut i = 0;
    while i + nal_len <= data.len() {
        let mut len = 0usize;
        for k in 0..nal_len {
            len = (len << 8) | data[i + k] as usize;
        }
        i += nal_len;
        if len == 0 || i + len > data.len() {
            break;
        }
        out.extend_from_slice(&[0, 0, 0, 1]);
        out.extend_from_slice(&data[i..i + len]);
        i += len;
    }
    out
}

/// avcC 配置记录 (opcode 1 明文) → Annex-B SPS/PPS, 返回 (annexb, nal_len)
fn avcc_config_to_annexb(cfg: &[u8]) -> (Vec<u8>, usize) {
    let mut out = Vec::new();
    if cfg.len() < 7 {
        return (out, 4);
    }
    let nal_len = ((cfg[4] & 0x03) + 1) as usize;
    let mut i = 5;
    let nsps = (cfg[i] & 0x1F) as usize;
    i += 1;
    for _ in 0..nsps {
        if i + 2 > cfg.len() {
            return (out, nal_len);
        }
        let l = ((cfg[i] as usize) << 8) | cfg[i + 1] as usize;
        i += 2;
        if i + l > cfg.len() {
            return (out, nal_len);
        }
        out.extend_from_slice(&[0, 0, 0, 1]);
        out.extend_from_slice(&cfg[i..i + l]);
        i += l;
    }
    if i < cfg.len() {
        let npps = cfg[i] as usize;
        i += 1;
        for _ in 0..npps {
            if i + 2 > cfg.len() {
                break;
            }
            let l = ((cfg[i] as usize) << 8) | cfg[i + 1] as usize;
            i += 2;
            if i + l > cfg.len() {
                break;
            }
            out.extend_from_slice(&[0, 0, 0, 1]);
            out.extend_from_slice(&cfg[i..i + l]);
            i += l;
        }
    }
    (out, nal_len)
}

/// 拉起上屏管线 (只拉一次): 建 FIFO /run/carplay.h264, v4l2_dec 读它 → 加 16 B 帧头 → /run/wfd-player.fifo
fn ensure_screen_pipeline() {
    static STARTED: std::sync::Once = std::sync::Once::new();
    STARTED.call_once(|| {
        let _ = std::fs::remove_file("/run/carplay.h264");
        let _ = std::process::Command::new("mkfifo").arg("/run/carplay.h264").status();
        // ⚠ v4l2_dec **不往 stdout 写** (实测: 重定向到管道 0 字节); 必须把目标管道当**输出路径参数**传,
        //   即 `v4l2_dec <in> <out> -H`, 否则永远黑屏 (2026-11-02 实测)。
        // ⚠ 而且它在流结束时 (FIFO 写端关闭) 会**正常退出**, 下一次会话就没人读 FIFO ⇒ 写端 open 永久阻塞
        //   ⇒ 屏黑死 (2026-11-02 15:46 实测)。所以必须**常驻看护**: 死了就重启, 保证 FIFO 始终有读者。
        std::thread::spawn(|| loop {
            match std::process::Command::new("v4l2_dec")
                .arg("/run/carplay.h264")
                .arg("/run/wfd-player.fifo")
                .arg("-H")
                .spawn()
            {
                Ok(mut c) => {
                    log(&format!("上屏管线: v4l2_dec(pid {}) → /run/wfd-player.fifo", c.id()));
                    let _ = c.wait();
                    log("v4l2_dec 退出 (本轮流结束), 300 ms 后重启等下一轮");
                }
                Err(e) => log(&format!("拉起 v4l2_dec 失败: {e}")),
            }
            std::thread::sleep(std::time::Duration::from_millis(300));
        });
    });
}

/// 当前活跃的屏幕流连接数 (0→1 置 /run/airplay-active, 1→0 删掉)
static SCREEN_CONNS: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
/// 活跃的 RTSP 连接数 (iOS 会同时开 ctrl + rctl 两条; 只有全关才算真断开)
static RTSP_CONNS: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);

/// RTSP 连接的 RAII 计数: 任一 return 路径都会正确减一, 全归零且没有屏幕流时才放行刷黑。
struct RtspGuard;
impl Drop for RtspGuard {
    fn drop(&mut self) {
        let left = RTSP_CONNS
            .fetch_sub(1, std::sync::atomic::Ordering::SeqCst)
            .saturating_sub(1);
        let screening = SCREEN_CONNS.load(std::sync::atomic::Ordering::SeqCst);
        log(&format!("RTSP 连接结束, 剩 {left} 条 (屏幕流 {screening} 条)"));
        if left == 0 {
            // 控制连接 (7000) 全结束 = 会话结束; 即使屏幕流 socket 还僵着, 也必须放行刷黑/回二级页
            mark_airplay_active(false);
            log("RTSP 控制连接全结束 → 删 /run/airplay-active (允许刷黑)");
        }
    }
}

/// 屏幕流 (type 110) 是 TCP: 手机连上来后一直灌 H.264, 先只统计字节数证明链路通。
fn spawn_screen_listener(listener: std::net::TcpListener, port: u16, out_key: [u8; 32]) {
    std::thread::spawn(move || {
        let cipher = ChaCha20Poly1305::new(Key::from_slice(&out_key));
        let mut counter: u64 = 0;
        let mut nal_len: usize = 4;
        for conn in listener.incoming() {
            match conn {
                Ok(mut s) => {
                    let peer = s.peer_addr().ok();
                    log(&format!("[screen:{port}] 手机连上了: {peer:?}"));
                    if SCREEN_CONNS.fetch_add(1, std::sync::atomic::Ordering::SeqCst) == 0 {
                        mark_airplay_active(true);
                        log("[screen] 置 /run/airplay-active (静默保持最后一帧)");
                    }
                    // FIFO 写端 (v4l2_dec 在读), 打不开就退化成只统计
                    let mut fifo = match std::fs::OpenOptions::new().write(true).open("/run/carplay.h264") {
                        Ok(f) => {
                            log(&format!("[screen:{port}] 已接入上屏管道 /run/carplay.h264"));
                            Some(f)
                        }
                        Err(e) => {
                            log(&format!("[screen:{port}] 打不开 /run/carplay.h264: {e} (只统计不解码)"));
                            None
                        }
                    };
                    let mut hdr = [0u8; 128];
                    let mut frames: u64 = 0;
                    let mut total: u64 = 0;
                    let mut wrote_dump: usize = 0;
                    loop {
                        if s.read_exact(&mut hdr).is_err() {
                            log(&format!("[screen:{port}] 连接结束: {frames} 帧 / {total} B"));
                            if SCREEN_CONNS.fetch_sub(1, std::sync::atomic::Ordering::SeqCst) == 1 {
                                mark_airplay_active(false);
                                log("[screen] 最后一条屏幕流断开 → 删 /run/airplay-active (允许刷黑)");
                            }
                            break;
                        }
                        let body_size = u32::from_le_bytes([hdr[0], hdr[1], hdr[2], hdr[3]]) as usize;
                        let opcode = hdr[4];
                        if body_size == 0 || body_size > 8 * 1024 * 1024 {
                            log(&format!("[screen:{port}] 异常 bodySize={body_size} op={opcode}, 收尾"));
                            break;
                        }
                        let mut body = vec![0u8; body_size];
                        if s.read_exact(&mut body).is_err() {
                            log(&format!("[screen:{port}] body 读取失败, {frames} 帧"));
                            break;
                        }
                        total += (128 + body_size) as u64;
                        let annexb = match opcode {
                            0 => {
                                let mut nonce = [0u8; 12];
                                nonce[4..].copy_from_slice(&counter.to_le_bytes());
                                counter += 1;
                                match cipher.decrypt(
                                    Nonce::from_slice(&nonce),
                                    chacha20poly1305::aead::Payload { msg: &body, aad: &hdr },
                                ) {
                                    Ok(p) => avcc_to_annexb(&p, nal_len),
                                    Err(_) => {
                                        if frames < 3 {
                                            log(&format!("[screen:{port}] 解密失败 (frame {counter})"));
                                        }
                                        Vec::new()
                                    }
                                }
                            }
                            1 => {
                                // VideoConfig: 明文 avcC
                                let (ab, nl) = avcc_config_to_annexb(&body);
                                nal_len = nl;
                                if frames == 0 {
                                    log(&format!(
                                        "[screen:{port}] avcC 配置 {} B → Annex-B {} B, nal_len={nl}, cfg={}",
                                        body.len(),
                                        ab.len(),
                                        hex(&body, 32)
                                    ));
                                }
                                ab
                            }
                            other => {
                                if frames < 3 {
                                    log(&format!("[screen:{port}] 忽略 opcode={other} ({body_size} B)"));
                                }
                                continue;
                            }
                        };
                        frames += 1;
                        if frames == 1 {
                            log(&format!(
                                "[screen:{port}] 首帧 Annex-B {} B, 起始 {}",
                                annexb.len(),
                                hex(&annexb, 24)
                            ));
                        } else if frames % 120 == 0 {
                            log(&format!("[screen:{port}] 已解 {frames} 帧 / {total} B"));
                        }
                        // 调试用: 前 4 MB 同时落盘, 便于离线验证裸流是否可解
                        if total <= 4 * 1024 * 1024 {
                            if let Ok(mut df) = std::fs::OpenOptions::new()
                                .create(true)
                                .append(true)
                                .open("/tmp/carplay_dump.h264")
                            {
                                let _ = df.write_all(&annexb);
                                wrote_dump += annexb.len();
                            }
                        }
                        if let Some(f) = fifo.as_mut() {
                            if f.write_all(&annexb).is_err() {
                                log(&format!("[screen:{port}] 写管道失败, 停止送解码"));
                                fifo = None;
                            }
                        }
                    }
                }
                Err(e) => {
                    log(&format!("[screen:{port}] accept 失败: {e}"));
                    break;
                }
            }
        }
    });
}

/// 收流计数线程: 证明 RTP 真的打进来了 (音频走 UDP, 先只统计不解析)。
fn spawn_rtp_counter(sock: std::net::UdpSocket, tag: &'static str, port: u16) {
    std::thread::spawn(move || {
        let _ = sock.set_read_timeout(Some(std::time::Duration::from_secs(30)));
        let mut buf = [0u8; 4096];
        let mut n: u64 = 0;
        let mut bytes: u64 = 0;
        loop {
            match sock.recv_from(&mut buf) {
                Ok((len, _from)) => {
                    n += 1;
                    bytes += len as u64;
                    if n == 1 {
                        log(&format!(
                            "[{tag}:{port}] 首个 RTP 包 {len} B, 头={}",
                            hex(&buf[..len.min(16)], 16)
                        ));
                    } else if n % 300 == 0 {
                        log(&format!("[{tag}:{port}] 累计 {n} 包 / {bytes} B"));
                    }
                }
                Err(_) => {
                    log(&format!("[{tag}:{port}] 30 s 无包, 收流结束 (共 {n} 包 / {bytes} B)"));
                    return;
                }
            }
        }
    });
}

/// SETUP: 会话级 (无 streams) 回 eventPort/timingPort; 带 streams 时给每个 stream 分配 data/control 端口。
fn setup(req: &rtsp::Request, session: &Session) -> Option<Vec<u8>> {
    let ask = plist::Value::from_reader(std::io::Cursor::new(&req.body)).ok();
    let mut out = Dictionary::new();

    let streams = ask
        .as_ref()
        .and_then(|v| v.as_dictionary())
        .and_then(|d| d.get("streams"))
        .and_then(|v| v.as_array());

    match streams {
        Some(list) => {
            log(&format!("SETUP: 带 {} 个 stream", list.len()));
            let mut resp = Vec::new();
            for (i, st) in list.iter().enumerate() {
                let d = st.as_dictionary().cloned().unwrap_or_default();
                let ty = d.get("type").and_then(|v| v.as_unsigned_integer()).unwrap_or(0);
                let mut rs = Dictionary::new();
                rs.insert("type".into(), Value::Integer((ty as i64).into()));
                let scid = d
                    .get("streamConnectionID")
                    .and_then(|v| {
                        v.as_unsigned_integer()
                            .or_else(|| v.as_signed_integer().map(|x| x as u64))
                    })
                    .unwrap_or(0);
                if ty == 110 {
                    // ⚠ CarPlay 的屏幕流 (type 110) 是 **TCP** 不是 UDP!
                    // 手机主动连这个 dataPort 往里灌 H.264 (见 golden 实现 receiver_core/session.rs:219
                    // "Screen (type 110, H.264) is a TCP listener the iPhone connects to")。
                    // 之前绑 UDP ⇒ 手机连不上 ⇒ 收到 SETUP 应答 1 s 就 TEARDOWN、RTP 0 包 (2026-11-02 实测)。
                    match std::net::TcpListener::bind(("0.0.0.0", 0)) {
                        Ok(l) => {
                            let port = l.local_addr().map(|a| a.port()).unwrap_or(0);
                            let shared = session.shared.unwrap_or([0u8; 32]);
                            let (ko, ki) = derive_stream_keys(&shared, scid);
                            log(&format!(
                                "  stream[{i}] type=110 → dataPort={port} (TCP 已监听) scid={scid} outkey={} inkey={}",
                                hex(&ko, 8),
                                hex(&ki, 8)
                            ));
                            ensure_screen_pipeline();
                            spawn_screen_listener(l, port, ko);
                            rs.insert("dataPort".into(), Value::Integer((port as i64).into()));
                        }
                        Err(e) => log(&format!("  stream[{i}] type=110 TCP bind 失败: {e}")),
                    }
                } else {
                    let dp = std::net::UdpSocket::bind(("0.0.0.0", 0)).ok();
                    let cp = std::net::UdpSocket::bind(("0.0.0.0", 0)).ok();
                    let dpn = dp.as_ref().and_then(|s| s.local_addr().ok()).map(|a| a.port()).unwrap_or(0);
                    let cpn = cp.as_ref().and_then(|s| s.local_addr().ok()).map(|a| a.port()).unwrap_or(0);
                    if let Some(s) = dp {
                        spawn_rtp_counter(s, "audio", dpn);
                    }
                    if let Some(s) = cp {
                        spawn_rtp_counter(s, "audio-ctrl", cpn);
                    }
                    rs.insert("dataPort".into(), Value::Integer((dpn as i64).into()));
                    rs.insert("controlPort".into(), Value::Integer((cpn as i64).into()));
                    log(&format!("  stream[{i}] type={ty} → dataPort={dpn} controlPort={cpn}"));
                }
                resp.push(Value::Dictionary(rs));
            }
            out.insert("streams".into(), Value::Array(resp));
        }
        None => log("SETUP: 会话级 (无 streams)"),
    }
    // eventPort 给 0: UxPlay (raop_handlers.h) 与 airplay2-receiver (ap2-receiver.py:208) 在镜像/音频
    // 模式都给 0。之前给 7010 但那个端口不讲事件协议, iOS 建不起来会卡到 30 s 超时重连。
    // eventPort 必须给**真监听端口**: 触摸 (hidSendReport) 走这条通道, 给 0 = iOS 不建通道 = 点屏没反应
    let eport = EVENT_TCP_PORT.lock().map(|g| *g).unwrap_or(0);
    out.insert("eventPort".into(), Value::Integer((eport as i64).into()));
    out.insert("timingPort".into(), Value::Integer((TIMING_PORT as i64).into()));

    // iOS 在 SETUP 里给出它自己的 timingPort —— 我们得主动去连它做 NTP 同步
    let remote_timing = ask
        .as_ref()
        .and_then(|v| v.as_dictionary())
        .and_then(|d| d.get("timingPort"))
        .and_then(|v| v.as_unsigned_integer());
    match (remote_timing, session.peer_ip) {
        (Some(p), Some(ip)) => {
            log(&format!("SETUP: iOS timingPort={p} → 起 NTP 同步 (对端 {ip})"));
            start_ntp_client(ip, p as u16);
        }
        (Some(p), None) => log(&format!("SETUP: iOS timingPort={p} 但不知道对端 IP, 跳过 NTP")),
        _ => log("SETUP: iOS 未给 timingPort"),
    }

    let mut body = Vec::new();
    plist::to_writer_binary(&mut body, &Value::Dictionary(out)).expect("bplist");
    let keys = ask
        .as_ref()
        .and_then(|v| v.as_dictionary())
        .map(|d| d.keys().cloned().collect::<Vec<_>>())
        .unwrap_or_default();
    log(&format!(
        "SETUP → {} B bplist (eventPort=0 timingPort={TIMING_PORT}) 请求键={keys:?} 内容={}",
        body.len(),
        hex(&body, 96)
    ));
    Some(rtsp::response(
        req.cseq.as_deref(),
        &[("Content-Type", "application/x-apple-binary-plist".into())],
        &body,
    ))
}

fn handle(mut s: TcpStream, ident: Arc<Identity>) {
    let peer = s.peer_addr().ok();
    log(&format!("连接: {peer:?}"));
    RTSP_CONNS.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    let _conn_guard = RtspGuard;
    let _ = s.set_read_timeout(Some(std::time::Duration::from_secs(60)));
    let mut session = Session::default();
    // peer 是 IPv4-mapped IPv6 (::ffff:a.b.c.d); NTP socket 绑 IPv4, 必须归一化
    session.peer_ip = peer.map(|p| p.ip().to_canonical());
    let mut cipher: Option<ControlCipher> = None;
    let mut buf = Vec::new();
    let mut chunk = [0u8; 4096];
    loop {
        match s.read(&mut chunk) {
            Ok(0) => {
                log("对端关闭");
                // 不要在这里直接删标记: iOS 同时开 ctrl + rctl 两条 RTSP 连接,
                // 关掉其中一条不代表会话结束 (之前那样做会把还在投屏的画面刷黑)。
                // 交给 RtspGuard: 两条都关了且没有屏幕流时才删。
                return;
            }
            Ok(n) => {
                if let Some(c) = cipher.as_mut() {
                    match c.feed(&chunk[..n]) {
                        Some(pt) => buf.extend_from_slice(&pt),
                        None => {
                            log("控制面解密失败, 断开");
                            return;
                        }
                    }
                } else {
                    buf.extend_from_slice(&chunk[..n]);
                }
            }
            Err(e) => {
                log(&format!("读错误: {e}"));
                return;
            }
        }
        loop {
            let Some((req, used)) = rtsp::Request::try_parse(&buf) else {
                break;
            };
            buf.drain(..used);
            LAST_RTSP_SECS.store(now_secs(), std::sync::atomic::Ordering::Relaxed);
            log(&format!(
                "→ {} {} ({} B body) CSeq={:?} UA={:?}",
                req.method,
                req.path,
                req.body.len(),
                req.cseq,
                req.header("User-Agent")
            ));
            if !req.body.is_empty() {
                log(&format!("  body({} B): {}", req.body.len(), hex(&req.body, 800)));
            }
            if let Some(resp) = dispatch(&req, &mut session, &ident) {
                let wire = match cipher.as_mut() {
                    Some(c) => c.seal(&resp),
                    None => resp,
                };
                if s.write_all(&wire).is_err() {
                    return;
                }
            } else {
                log("  (无应答, 继续听)");
            }
            // pair-verify 成功后: 这条连接从此走控制面加密 (M4 应答是最后一条明文)
            if cipher.is_none() {
                if let Some((r, w)) = session.control_keys.take() {
                    cipher = Some(ControlCipher::new(r, w));
                    log("控制面加密已启用 (M4 之后)");
                }
            }
        }
    }
}

/// FairPlay /fp-setup 应答 (UxPlay fairplay_playfair.c reply_message[4], 由 req[14] 选表)
const FP_REPLY: [[u8; 142]; 4] = [
    [0x46, 0x50, 0x4c, 0x59, 0x03, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x82, 0x02, 0x00, 0x0f, 0x9f, 0x3f, 0x9e, 0x0a, 0x25, 0x21, 0xdb, 0xdf, 0x31, 0x2a, 0xb2, 0xbf, 0xb2, 0x9e, 0x8d, 0x23, 0x2b, 0x63, 0x76, 0xa8, 0xc8, 0x18, 0x70, 0x1d, 0x22, 0xae, 0x93, 0xd8, 0x27, 0x37, 0xfe, 0xaf, 0x9d, 0xb4, 0xfd, 0xf4, 0x1c, 0x2d, 0xba, 0x9d, 0x1f, 0x49, 0xca, 0xaa, 0xbf, 0x65, 0x91, 0xac, 0x1f, 0x7b, 0xc6, 0xf7, 0xe0, 0x66, 0x3d, 0x21, 0xaf, 0xe0, 0x15, 0x65, 0x95, 0x3e, 0xab, 0x81, 0xf4, 0x18, 0xce, 0xed, 0x09, 0x5a, 0xdb, 0x7c, 0x3d, 0x0e, 0x25, 0x49, 0x09, 0xa7, 0x98, 0x31, 0xd4, 0x9c, 0x39, 0x82, 0x97, 0x34, 0x34, 0xfa, 0xcb, 0x42, 0xc6, 0x3a, 0x1c, 0xd9, 0x11, 0xa6, 0xfe, 0x94, 0x1a, 0x8a, 0x6d, 0x4a, 0x74, 0x3b, 0x46, 0xc3, 0xa7, 0x64, 0x9e, 0x44, 0xc7, 0x89, 0x55, 0xe4, 0x9d, 0x81, 0x55, 0x00, 0x95, 0x49, 0xc4, 0xe2, 0xf7, 0xa3, 0xf6, 0xd5, 0xba],
    [0x46, 0x50, 0x4c, 0x59, 0x03, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x82, 0x02, 0x01, 0xcf, 0x32, 0xa2, 0x57, 0x14, 0xb2, 0x52, 0x4f, 0x8a, 0xa0, 0xad, 0x7a, 0xf1, 0x64, 0xe3, 0x7b, 0xcf, 0x44, 0x24, 0xe2, 0x00, 0x04, 0x7e, 0xfc, 0x0a, 0xd6, 0x7a, 0xfc, 0xd9, 0x5d, 0xed, 0x1c, 0x27, 0x30, 0xbb, 0x59, 0x1b, 0x96, 0x2e, 0xd6, 0x3a, 0x9c, 0x4d, 0xed, 0x88, 0xba, 0x8f, 0xc7, 0x8d, 0xe6, 0x4d, 0x91, 0xcc, 0xfd, 0x5c, 0x7b, 0x56, 0xda, 0x88, 0xe3, 0x1f, 0x5c, 0xce, 0xaf, 0xc7, 0x43, 0x19, 0x95, 0xa0, 0x16, 0x65, 0xa5, 0x4e, 0x19, 0x39, 0xd2, 0x5b, 0x94, 0xdb, 0x64, 0xb9, 0xe4, 0x5d, 0x8d, 0x06, 0x3e, 0x1e, 0x6a, 0xf0, 0x7e, 0x96, 0x56, 0x16, 0x2b, 0x0e, 0xfa, 0x40, 0x42, 0x75, 0xea, 0x5a, 0x44, 0xd9, 0x59, 0x1c, 0x72, 0x56, 0xb9, 0xfb, 0xe6, 0x51, 0x38, 0x98, 0xb8, 0x02, 0x27, 0x72, 0x19, 0x88, 0x57, 0x16, 0x50, 0x94, 0x2a, 0xd9, 0x46, 0x68, 0x8a],
    [0x46, 0x50, 0x4c, 0x59, 0x03, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x82, 0x02, 0x02, 0xc1, 0x69, 0xa3, 0x52, 0xee, 0xed, 0x35, 0xb1, 0x8c, 0xdd, 0x9c, 0x58, 0xd6, 0x4f, 0x16, 0xc1, 0x51, 0x9a, 0x89, 0xeb, 0x53, 0x17, 0xbd, 0x0d, 0x43, 0x36, 0xcd, 0x68, 0xf6, 0x38, 0xff, 0x9d, 0x01, 0x6a, 0x5b, 0x52, 0xb7, 0xfa, 0x92, 0x16, 0xb2, 0xb6, 0x54, 0x82, 0xc7, 0x84, 0x44, 0x11, 0x81, 0x21, 0xa2, 0xc7, 0xfe, 0xd8, 0x3d, 0xb7, 0x11, 0x9e, 0x91, 0x82, 0xaa, 0xd7, 0xd1, 0x8c, 0x70, 0x63, 0xe2, 0xa4, 0x57, 0x55, 0x59, 0x10, 0xaf, 0x9e, 0x0e, 0xfc, 0x76, 0x34, 0x7d, 0x16, 0x40, 0x43, 0x80, 0x7f, 0x58, 0x1e, 0xe4, 0xfb, 0xe4, 0x2c, 0xa9, 0xde, 0xdc, 0x1b, 0x5e, 0xb2, 0xa3, 0xaa, 0x3d, 0x2e, 0xcd, 0x59, 0xe7, 0xee, 0xe7, 0x0b, 0x36, 0x29, 0xf2, 0x2a, 0xfd, 0x16, 0x1d, 0x87, 0x73, 0x53, 0xdd, 0xb9, 0x9a, 0xdc, 0x8e, 0x07, 0x00, 0x6e, 0x56, 0xf8, 0x50, 0xce],
    [0x46, 0x50, 0x4c, 0x59, 0x03, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x82, 0x02, 0x03, 0x90, 0x01, 0xe1, 0x72, 0x7e, 0x0f, 0x57, 0xf9, 0xf5, 0x88, 0x0d, 0xb1, 0x04, 0xa6, 0x25, 0x7a, 0x23, 0xf5, 0xcf, 0xff, 0x1a, 0xbb, 0xe1, 0xe9, 0x30, 0x45, 0x25, 0x1a, 0xfb, 0x97, 0xeb, 0x9f, 0xc0, 0x01, 0x1e, 0xbe, 0x0f, 0x3a, 0x81, 0xdf, 0x5b, 0x69, 0x1d, 0x76, 0xac, 0xb2, 0xf7, 0xa5, 0xc7, 0x08, 0xe3, 0xd3, 0x28, 0xf5, 0x6b, 0xb3, 0x9d, 0xbd, 0xe5, 0xf2, 0x9c, 0x8a, 0x17, 0xf4, 0x81, 0x48, 0x7e, 0x3a, 0xe8, 0x63, 0xc6, 0x78, 0x32, 0x54, 0x22, 0xe6, 0xf7, 0x8e, 0x16, 0x6d, 0x18, 0xaa, 0x7f, 0xd6, 0x36, 0x25, 0x8b, 0xce, 0x28, 0x72, 0x6f, 0x66, 0x1f, 0x73, 0x88, 0x93, 0xce, 0x44, 0x31, 0x1e, 0x4b, 0xe6, 0xc0, 0x53, 0x51, 0x93, 0xe5, 0xef, 0x72, 0xe8, 0x68, 0x62, 0x33, 0x72, 0x9c, 0x22, 0x7d, 0x82, 0x0c, 0x99, 0x94, 0x45, 0xd8, 0x92, 0x46, 0xc8, 0xc3, 0x59],
];
fn dispatch(req: &rtsp::Request, session: &mut Session, ident: &Identity) -> Option<Vec<u8>> {
    match (req.method.as_str(), req.path.as_str()) {
        ("OPTIONS", _) => Some(rtsp::response(
            req.cseq.as_deref(),
            &[(
                "Public",
                "ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER, POST, GET".into(),
            )],
            &[],
        )),
        ("GET", "/info") => {
            // ⚠ CarPlay 是**横屏**协议: 报 800x480 让 iOS 直接给横屏帧, 再由 drmplay 的
            // rot90_cw_nv12 转到 480x800 竖屏上屏。之前报 480x800 竖屏, iOS 把横屏 UI 转着塞进来,
            // 屏上就是"歪的" (2026-11-02 主人现场确认)。
            let body = info::build(800, 480, "K230 CarPlay");
            log(&format!("/info → {} B bplist (800x480 横屏)", body.len()));
            Some(rtsp::response(
                req.cseq.as_deref(),
                &[("Content-Type", "application/x-apple-binary-plist".into())],
                &body,
            ))
        }
        ("POST", "/pair-setup") => {
            let Some(t) = tlv8::Tlv8::parse(&req.body) else {
                // 老式 (legacy) pair-setup: Content-Type octet-stream, body 32 B = 手机公钥,
                // 应答必须是**我方 32 B Ed25519 公钥**, 否则 iOS 每 3 s 重连一次死循环 (UxPlay
                // raop_handler_pairsetup 口径)。
                if req.body.len() == 32 {
                    let pubkey = ident.signing.verifying_key().to_bytes();
                    log(&format!("/pair-setup: legacy 32 B → 回 Ed25519 公钥 {}", hex(&pubkey, 8)));
                    return Some(rtsp::response(
                        req.cseq.as_deref(),
                        &[("Content-Type", "application/octet-stream".into())],
                        &pubkey,
                    ));
                }
                log(&format!("/pair-setup: body 不是 TLV8 且长度 {} != 32", req.body.len()));
                return Some(rtsp::response(req.cseq.as_deref(), &[], &[]));
            };
            let state = t.u8_of(tlv8::STATE).unwrap_or(0);
            log(&format!(
                "/pair-setup: state={state} 字段={:?}",
                t.0.iter().map(|(k, v)| format!("{k:#04x}({})", v.len())).collect::<Vec<_>>()
            ));
            match state {
                1 => {
                    let pass = std::env::var("CARPLAY_SRP_PASS").unwrap_or_else(|_| "3939".into());
                    let srv = srp::Server::new(&pass, "Pair-Setup");
                    session.srp = Some(srv);
                    session.srp_key = None;
                    let srv = session.srp.as_ref().unwrap();
                    // 顺序照 PlayPort: STATE, PUBLIC_KEY, SALT
                    let mut m2 = tlv8::Tlv8::default();
                    m2.push(tlv8::STATE, &[2]);
                    m2.push(tlv8::PUBLIC_KEY, &srv.b_bytes());
                    m2.push(tlv8::SALT, &srv.salt);
                    let body = m2.to_bytes();
                    log(&format!("/pair-setup → M2 ({} B: state2+B384+salt16)", body.len()));
                    Some(pair_resp(req.cseq.as_deref(), &body))
                }
                3 => {
                    let a = t.concat(tlv8::PUBLIC_KEY).unwrap_or_default();
                    let proof = t.concat(tlv8::PROOF).unwrap_or_default();
                    let Some(srv) = session.srp.as_ref() else {
                        log("/pair-setup M3: 没有对应 M1 的会话, 拒绝");
                        return Some(rtsp::response(req.cseq.as_deref(), &[], &err_tlv(4)));
                    };
                    match srv.verify(&a, &proof) {
                        Some((m4, key)) => {
                            session.srp = None;
                            session.srp_key = Some(key);
                            log(&format!("/pair-setup M3: A={} B proof={} **校验通过 OK** → M4", a.len(), proof.len()));
                            let mut out = tlv8::Tlv8::default();
                            out.push(tlv8::STATE, &[4]);
                            out.push(tlv8::PROOF, &m4);
                            let body = out.to_bytes();
                            Some(pair_resp(req.cseq.as_deref(), &body))
                        }
                        None => {
                            log(&format!("/pair-setup M3: A={} B proof={} 校验不通过 FAIL", a.len(), proof.len()));
                            Some(pair_resp(req.cseq.as_deref(), &err_tlv(4)))
                        }
                    }
                }
                5 => pair_setup_m6(&t, session, ident)
                    .map(|body| pair_resp(req.cseq.as_deref(), &body)),
                other => {
                    log(&format!("/pair-setup: 未处理 state={other}"));
                    Some(rtsp::response(req.cseq.as_deref(), &[], &[]))
                }
            }
        }
        ("POST", "/pair-verify") => {
            // ⚠ legacy 报文 (03 字节头 00/01 + 64 B) 会被 TLV8 解析"假成功" (0x00 0x00 恰好是
            // type=0 len=0), 所以**先按长度+首字节判 legacy**, 再谈 TLV8。
            if req.body.len() == 68 && (req.body[0] == 0 || req.body[0] == 1) {
                return Some(legacy_pair_verify(req, session, ident));
            }
            let Some(t) = tlv8::Tlv8::parse(&req.body) else {
                // legacy (octet-stream) pair-verify: [0x01][本机 eph32][本机 Ed25519 公钥 32] → 回 eph32+sig64;
                // [0x00][sig 64] 是收尾。CarPlay 的 **控制通道** 走的是这条 (UxPlay raop_handler_pairverify)。
                if req.body.len() >= 4 && (req.body[0] == 1 || req.body[0] == 0) {
                    return Some(legacy_pair_verify(req, session, ident));
                }
                log(&format!("/pair-verify: body 不是 TLV8 ({} B, 首字节 {:#04x})", req.body.len(), req.body.first().copied().unwrap_or(0)));
                return Some(rtsp::response(req.cseq.as_deref(), &[], &[]));
            };
            let state = t.u8_of(tlv8::STATE).unwrap_or(0);
            log(&format!(
                "/pair-verify: state={state} 字段={:?}",
                t.0.iter().map(|(k, v)| format!("{k:#04x}({})", v.len())).collect::<Vec<_>>()
            ));
            match state {
                1 => pair_verify_m2(&t, session, ident)
                    .map(|body| pair_resp(req.cseq.as_deref(), &body)),
                3 => pair_verify_m4(&t, session, ident)
                    .map(|body| pair_resp(req.cseq.as_deref(), &body)),
                other => {
                    log(&format!("/pair-verify: 未处理 state={other}"));
                    Some(rtsp::response(req.cseq.as_deref(), &[], &err_tlv(other)))
                }
            }
        }
        ("POST", "/auth-setup") => {
            let body = auth_setup(&req.body).unwrap_or_default();
            Some(rtsp::response(
                req.cseq.as_deref(),
                &[("Content-Type", "application/octet-stream".into())],
                &body,
            ))
        }
        ("POST", "/fp-setup") => {
            let b = &req.body;
            if b.len() == 16 && b[4] == 0x03 {
                let mode = (b[14] as usize).min(3);
                let res = FP_REPLY[mode];
                log(&format!("/fp-setup: 16 B mode={mode} → 回 142 B (UxPlay reply_message)"));
                Some(rtsp::response(
                    req.cseq.as_deref(),
                    &[("Content-Type", "application/octet-stream".into())],
                    &res,
                ))
            } else if b.len() == 164 && b[4] == 0x03 {
                let mut res = [0u8; 32];
                res[..12].copy_from_slice(&[0x46, 0x50, 0x4c, 0x59, 0x03, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x14]);
                res[12..].copy_from_slice(&b[144..164]);
                log("/fp-setup: 164 B → 回 32 B (fp_header + req[144..164])");
                Some(rtsp::response(
                    req.cseq.as_deref(),
                    &[("Content-Type", "application/octet-stream".into())],
                    &res,
                ))
            } else {
                log(&format!("/fp-setup: 未知长度 {} (b[4]={:#04x})", b.len(), b.get(4).copied().unwrap_or(0)));
                Some(rtsp::response(req.cseq.as_deref(), &[], &[]))
            }
        }
        ("SETUP", _) => setup(req, session),
        // AirPlay 会话级收尾: iOS 在 SETUP 之后必须能查到音量, 我们之前一律"未处理"
        // ⇒ 手机判 Activation Failed (err=-6743) 并断开 (2026-11-02 14:37 手机日志实证)。
        ("GET_PARAMETER", _) => {
            let q = String::from_utf8_lossy(&req.body).trim().to_string();
            log(&format!("GET_PARAMETER: {q:?}"));
            if q.contains("volume") {
                let body = format!("volume: {:9.6}\r\n", session.volume);
                Some(rtsp::response(
                    req.cseq.as_deref(),
                    &[("Content-Type", "text/parameters".into())],
                    body.as_bytes(),
                ))
            } else {
                Some(rtsp::response(req.cseq.as_deref(), &[], &[]))
            }
        }
        ("SET_PARAMETER", _) => {
            let body = String::from_utf8_lossy(&req.body).to_string();
            for line in body.lines() {
                if let Some(v) = line.strip_prefix("volume:") {
                    if let Ok(f) = v.trim().parse::<f64>() {
                        session.volume = f;
                    }
                }
            }
            log(&format!("SET_PARAMETER: {}", body.trim().replace('\n', " | ")));
            Some(rtsp::response(req.cseq.as_deref(), &[], &[]))
        }
        ("RECORD", _) | ("FLUSH", _) | ("TEARDOWN", _) | ("PAUSE", _) => {
            // ★ RECORD 就是 iOS 紧接着要开 event 通道的那一刻: 此刻把**本条连接**的 shared 存成全局,
            //   事件通道的 Events 密钥就用它。之前全局存的是"最后一条 pair-verify 连接"(rctl) 的 shared,
            //   和 ctrl 连接不是同一份 ⇒ Events 解密失败 ⇒ iOS 拆会话 (触摸一碰就断的根因之一)。
            if req.method == "TEARDOWN" {
                log("收到 TEARDOWN (会话结束; 标记由 RtspGuard 统一收尾)");
            }
            if req.method == "RECORD" {
                if let Some(sh) = session.shared {
                    if let Ok(mut g) = SCREEN_SHARED.lock() {
                        *g = Some(sh);
                    }
                    log(&format!("RECORD: 事件通道将用本条连接的 shared {}", hex(&sh, 4)));
                }
            }
            // UxPlay raop_handler_record 会带这两个头; iOS 缺了会立刻拆链 (实测 0.3 s 内 finalizing)
            log(&format!("{} {} → 200 (+Audio-Latency/Audio-Jack-Status)", req.method, req.path));
            Some(rtsp::response(
                req.cseq.as_deref(),
                &[
                    ("Audio-Latency", "0".into()),
                    ("Audio-Jack-Status", "connected; type=analog".into()),
                ],
                &[],
            ))
        }
        _ => {
            log(&format!("未处理: {} {}", req.method, req.path));
            Some(rtsp::response(req.cseq.as_deref(), &[], &[]))
        }
    }
}

/// legacy pair-verify (octet-stream, CarPlay 控制通道用): 请求 = [1][client eph32][client Ed25519 公钥32],
/// 应答 = 我方 eph32 + Ed25519 签名 64; 收尾 [0][sig64] 只回 200。
fn legacy_pair_verify(req: &rtsp::Request, session: &mut Session, ident: &Identity) -> Vec<u8> {
    let b = &req.body;
    if b[0] == 1 {
        if b.len() < 4 + 64 {
            log(&format!("/pair-verify legacy: body {} B 太短", b.len()));
            return rtsp::response(req.cseq.as_deref(), &[], &[]);
        }
        let client_eph: [u8; 32] = match b[4..36].try_into() {
            Ok(v) => v,
            Err(_) => return rtsp::response(req.cseq.as_deref(), &[], &[]),
        };
        let mut seed = [0u8; 32];
        rand::RngCore::fill_bytes(&mut rand::thread_rng(), &mut seed);
        let secret = x25519_dalek::StaticSecret::from(seed);
        let eph_pub = x25519_dalek::PublicKey::from(&secret).to_bytes();
        let shared = secret
            .diffie_hellman(&x25519_dalek::PublicKey::from(client_eph))
            .to_bytes();
        // ★ 流/事件密钥的根 = **本连接 pair-verify** 的 ECDH 共享 (golden session.rs:51 on_paired 口径)。
        //   必须**按连接存**: iOS 会开 ctrl + rctl 两条 RTSP 连接各做一次 pair-verify,
        //   用全局变量会被后一条覆盖 ⇒ 解密必然失败 (2026-11-02 实测)。
        session.shared = Some(shared);
        if let Ok(mut g) = SCREEN_SHARED.lock() {
            *g = Some(shared); // 事件通道也用它派生 Events 密钥
        }
        // 口径 = UxPlay pairing.c pairing_session_get_signature:
        //   签名原文 = 我方 eph32 ‖ 对方 eph32 (**不含 identifier**), AES-128-CTR 加密后再发
        //   key = SHA512("Pair-Verify-AES-Key" ‖ shared)[..16], iv = SHA512("Pair-Verify-AES-IV" ‖ shared)[..16]
        let mut signed = Vec::with_capacity(64);
        signed.extend_from_slice(&eph_pub);
        signed.extend_from_slice(&client_eph);
        let sig = ident.signing.sign(&signed).to_bytes();
        use sha2::Digest as _;
        let mut hk = sha2::Sha512::new();
        hk.update(b"Pair-Verify-AES-Key");
        hk.update(shared);
        let key = hk.finalize();
        let mut hi = sha2::Sha512::new();
        hi.update(b"Pair-Verify-AES-IV");
        hi.update(shared);
        let iv = hi.finalize();
        use aes::cipher::{KeyIvInit as _, StreamCipher as _};
        type Aes128Ctr = ctr::Ctr128BE<aes::Aes128>;
        let mut sig_enc = sig.to_vec();
        let mut c = Aes128Ctr::new(key[..16].into(), iv[..16].into());
        c.apply_keystream(&mut sig_enc);
        // 注意: legacy 路径**不开控制面 ChaCha** (UxPlay 的 AirPlay1 RTSP 全程明文);
        // 开了会让 iOS 的后续明文请求解密失败。
        let mut out = Vec::with_capacity(96);
        out.extend_from_slice(&eph_pub);
        out.extend_from_slice(&sig_enc);
        log(&format!("/pair-verify legacy M1 → 回 eph32+加密sig64 ({} B)", out.len()));
        rtsp::response(
            req.cseq.as_deref(),
            &[("Content-Type", "application/octet-stream".into())],
            &out,
        )
    } else {
        log(&format!("/pair-verify legacy M3 ({} B) → 200", b.len()));
        rtsp::response(
            req.cseq.as_deref(),
            &[("Content-Type", "application/octet-stream".into())],
            &[],
        )
    }
}

/// pair-verify M1 → M2: X25519 临时密钥 + Ed25519 签名 (口径照 PlayPort PairVerify.kt)
fn pair_verify_m2(t: &tlv8::Tlv8, session: &mut Session, ident: &Identity) -> Option<Vec<u8>> {
    let client_pub = t.concat(tlv8::PUBLIC_KEY).unwrap_or_default();
    if client_pub.len() != 32 {
        log(&format!("/pair-verify M1: client 公钥 {} B != 32, 拒绝", client_pub.len()));
        return Some(err_tlv(2));
    }
    let mut seed = [0u8; 32];
    rand::RngCore::fill_bytes(&mut rand::thread_rng(), &mut seed);
    let secret = x25519_dalek::StaticSecret::from(seed);
    let eph_pub = x25519_dalek::PublicKey::from(&secret).to_bytes();
    let client_arr: [u8; 32] = client_pub.try_into().ok()?;
    let client_pk = x25519_dalek::PublicKey::from(client_arr);
    let shared = secret.diffie_hellman(&client_pk).to_bytes();
    let enc_key = hkdf_sha512(&shared, b"Pair-Verify-Encrypt-Salt", b"Pair-Verify-Encrypt-Info");
    session.pv = Some(PvSession { secret, eph_pub, client_pub: client_arr, shared, enc_key });

    // 签名 accessoryEphPub ‖ accessoryId ‖ clientEphPub, 加密回给 iOS
    let mut signed = Vec::with_capacity(32 + ident.id.len() + 32);
    signed.extend_from_slice(&eph_pub);
    signed.extend_from_slice(ident.id.as_bytes());
    signed.extend_from_slice(&client_arr);
    let sig = ident.signing.sign(&signed).to_bytes();
    let mut sub = tlv8::Tlv8::default();
    sub.push(tlv8::IDENTIFIER, ident.id.as_bytes());
    sub.push(tlv8::SIGNATURE, &sig);
    let sealed = chacha(&enc_key, b"PV-Msg02", &sub.to_bytes(), true)?;
    let mut out = tlv8::Tlv8::default();
    out.push(tlv8::STATE, &[2]);
    out.push(tlv8::PUBLIC_KEY, &eph_pub);
    out.push(tlv8::ENCRYPTED_DATA, &sealed);
    log(&format!("/pair-verify → M2 ({} B: state2+eph32+enc{})", out.to_bytes().len(), sealed.len()));
    Some(out.to_bytes())
}

/// pair-verify M3 → M4: 验 iOS 签名, 推控制面读写密钥
fn pair_verify_m4(t: &tlv8::Tlv8, session: &mut Session, _ident: &Identity) -> Option<Vec<u8>> {
    let enc = t.concat(tlv8::ENCRYPTED_DATA).unwrap_or_default();
    let pv = match session.pv.as_ref() {
        Some(p) => p,
        None => {
            log("/pair-verify M3: 没有对应 M1 的会话, 拒绝");
            return Some(err_tlv(4));
        }
    };
    let pt = match chacha(&pv.enc_key, b"PV-Msg03", &enc, false) {
        Some(p) => p,
        None => {
            log("/pair-verify M3: 解密失败");
            return Some(err_tlv(4));
        }
    };
    let sub = match tlv8::Tlv8::parse(&pt) {
        Some(s) => s,
        None => return Some(err_tlv(4)),
    };
    let ctrl_id = sub.concat(tlv8::IDENTIFIER).unwrap_or_default();
    let ctrl_sig = sub.concat(tlv8::SIGNATURE).unwrap_or_default();
    log(&format!(
        "/pair-verify M3: 解出 controller id={:?} sig={} B",
        String::from_utf8_lossy(&ctrl_id),
        ctrl_sig.len()
    ));
    let ltpk = match load_pairing(&ctrl_id) {
        Some(k) => k,
        None => {
            log(&format!("/pair-verify M3: 本地没有 {:?} 的 pairing, 拒绝", String::from_utf8_lossy(&ctrl_id)));
            return Some(err_tlv(4));
        }
    };
    // 验 iOS 签名: clientEphPub ‖ id ‖ accessoryEphPub
    let pv = session.pv.as_ref().unwrap();
    let mut signed = Vec::with_capacity(32 + ctrl_id.len() + 32);
    signed.extend_from_slice(&pv.client_pub);
    signed.extend_from_slice(&ctrl_id);
    signed.extend_from_slice(&pv.eph_pub);
    let ok = ed25519_dalek::Signature::from_slice(&ctrl_sig)
        .map(|sig| ltpk.verify(&signed, &sig).is_ok())
        .unwrap_or(false);
    if !ok {
        log("/pair-verify M3: controller 签名验证 FAIL");
        return Some(err_tlv(4));
    }
    // 控制面密钥 (PlayPort 口径: read=Write-Info, write=Read-Info, 别改)
    let read = hkdf_sha512(&pv.shared, b"Control-Salt", b"Control-Write-Encryption-Key");
    let write = hkdf_sha512(&pv.shared, b"Control-Salt", b"Control-Read-Encryption-Key");
    session.control_keys = Some((read, write));
    session.shared = Some(pv.shared);
    if let Ok(mut g) = SCREEN_SHARED.lock() {
        *g = Some(pv.shared);
    }
    session.pv = None;
    log("/pair-verify → M4 (state4, 控制面密钥就绪)");
    let out = tlv8::Tlv8::default().with_u8(tlv8::STATE, 4);
    Some(out.to_bytes())
}

/// M5 → M6: 解 iOS 的 Ed25519 长期公钥并验签, 回我们自己的 (加密)。
fn pair_setup_m6(t: &tlv8::Tlv8, session: &mut Session, ident: &Identity) -> Option<Vec<u8>> {
    let key = session.srp_key.as_ref()?;
    let enc = t.concat(tlv8::ENCRYPTED_DATA)?;
    let ekey = hkdf_sha512(key, b"Pair-Setup-Encrypt-Salt", b"Pair-Setup-Encrypt-Info");
    let pt = chacha(&ekey, b"PS-Msg05", &enc, false)?;
    let sub = tlv8::Tlv8::parse(&pt)?;
    let ctrl_id = sub.concat(tlv8::IDENTIFIER)?;
    let ctrl_ltpk = sub.concat(tlv8::PUBLIC_KEY)?;
    let ctrl_sig = sub.concat(tlv8::SIGNATURE)?;
    log(&format!(
        "/pair-setup M5: 解出 controller id={:?} ltpk={} B sig={} B",
        String::from_utf8_lossy(&ctrl_id),
        ctrl_ltpk.len(),
        ctrl_sig.len()
    ));
    if ctrl_ltpk.len() != 32 || ctrl_sig.len() != 64 {
        log("/pair-setup M5: 字段长度不对");
        return Some(err_tlv(6));
    }

    // 验 iOS 签名: HKDF(Controller-Sign) ‖ id ‖ ltpk
    let dk = hkdf_sha512(key, b"Pair-Setup-Controller-Sign-Salt", b"Pair-Setup-Controller-Sign-Info");
    let mut signed = Vec::with_capacity(32 + ctrl_id.len() + 32);
    signed.extend_from_slice(&dk);
    signed.extend_from_slice(&ctrl_id);
    signed.extend_from_slice(&ctrl_ltpk);
    let ltpk_arr: [u8; 32] = ctrl_ltpk.clone().try_into().ok()?;
    let ok = VerifyingKey::from_bytes(&ltpk_arr).ok().is_some_and(|vk| {
        ed25519_dalek::Signature::from_slice(&ctrl_sig)
            .map(|sig| vk.verify(&signed, &sig).is_ok())
            .unwrap_or(false)
    });
    if !ok {
        log("/pair-setup M5: controller 签名验证 FAIL");
        return Some(err_tlv(6));
    }

    // 验签过了才存 pairing (pair-verify 用)
    let _ = std::fs::create_dir_all(PAIRINGS_DIR);
    let path = format!("{PAIRINGS_DIR}/{}", hex(&ctrl_id, 64).replace(' ', ""));
    if let Err(e) = std::fs::write(&path, &ctrl_ltpk) {
        log(&format!("存 pairing 失败 {path}: {e}"));
    } else {
        log(&format!("已存 pairing → {path} (32 B)"));
    }


    let ak = hkdf_sha512(key, b"Pair-Setup-Accessory-Sign-Salt", b"Pair-Setup-Accessory-Sign-Info");
    let acc_ltpk = ident.signing.verifying_key().to_bytes();
    let mut signed2 = Vec::with_capacity(32 + ident.id.len() + 32);
    signed2.extend_from_slice(&ak);
    signed2.extend_from_slice(ident.id.as_bytes());
    signed2.extend_from_slice(&acc_ltpk);
    let sig = ident.signing.sign(&signed2).to_bytes();

    let mut sub2 = tlv8::Tlv8::default();
    sub2.push(tlv8::IDENTIFIER, ident.id.as_bytes());
    sub2.push(tlv8::PUBLIC_KEY, &acc_ltpk);
    sub2.push(tlv8::SIGNATURE, &sig);
    let sealed = chacha(&ekey, b"PS-Msg06", &sub2.to_bytes(), true)?;
    // 顺序照 PlayPort: STATE, ENCRYPTED_DATA
    let mut out = tlv8::Tlv8::default();
    out.push(tlv8::STATE, &[6]);
    out.push(tlv8::ENCRYPTED_DATA, &sealed);
    let body = out.to_bytes();
    log(&format!("/pair-setup → M6 ({} B, 配对完成)", body.len()));
    session.srp_key = None;
    Some(body)
}

/// drmplay 的"AirPlay 客户端在线"标记: 有这个文件时, 源端静默 (iOS 静止画面不发帧)
/// 它会**无限保持最后一帧**, 不会刷黑。没有它 ⇒ 静默 2.4 s 就刷黑 (2026-11-02 主人实测
/// "不触碰就全黑, 碰一下又恢复")。
const AIRPLAY_ACTIVE: &str = "/run/airplay-active";

/// 最近一次收到 RTSP 请求的时间 (秒)。iOS 在会话期间会每 ~2 s 发 POST /feedback,
/// 所以"长时间没有任何 RTSP 请求" = 手机已经退出 CarPlay (即使 socket 没关)。
static LAST_RTSP_SECS: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

fn now_secs() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

/// 看门狗: 标记还在但 RTSP 已经静默 >15 s ⇒ 判定手机端已断开, 删标记让屏刷黑。
/// (实测: 手机退出 CarPlay 时不一定关 socket, 也不一定发 TEARDOWN, 只停止发请求)
fn start_session_watchdog() {
    std::thread::spawn(|| loop {
        std::thread::sleep(std::time::Duration::from_secs(1));
        let last = LAST_RTSP_SECS.load(std::sync::atomic::Ordering::Relaxed);
        if last == 0 {
            continue;
        }
        let idle = now_secs().saturating_sub(last);
        // iOS 实测每 **2 s** 一条 POST /feedback (两条连接各一条), 所以 6 s = 连丢 3 拍,
        // 只有在"真的断开"时才会发生 ⇒ 断开后 ~7 s 内刷黑, 又不会把 WiFi 抖动当断开。
        // 2026-11-07: 只有「RTSP 静默」还不够 —— CarPlay 控制连接 (7000) 或屏幕流
        // 还活着时, 6 s 没请求是正常的 (手机静态画面不发 RTSP)。此前这里无条件删标记,
        // drmplay 120 s 后刷黑, 表现为「屏幕像息屏, 摸一下才亮」。
        // 改成: RTSP 静默 且 RTSP/屏幕流连接都为 0, 才删标记。
        let rtsp_open = RTSP_CONNS.load(std::sync::atomic::Ordering::SeqCst);
        let screen_open = SCREEN_CONNS.load(std::sync::atomic::Ordering::SeqCst);
        if idle >= 6 && rtsp_open == 0 && std::path::Path::new(AIRPLAY_ACTIVE).exists() {
            log(&format!("RTSP 已静默 {idle} s 且无连接 ⇒ 删 /run/airplay-active (允许刷黑)"));
            mark_airplay_active(false);
        }
    });
}

fn mark_airplay_active(on: bool) {
    if on {
        let _ = std::fs::File::create(AIRPLAY_ACTIVE);
    } else {
        let _ = std::fs::remove_file(AIRPLAY_ACTIVE);
    }
}

fn main() {
    let listener = TcpListener::bind(("::", PORT)).or_else(|_| TcpListener::bind(("0.0.0.0", PORT)));
    let listener = match listener {
        Ok(l) => l,
        Err(e) => {
            log(&format!("监听 {PORT} 失败: {e}"));
            std::process::exit(1);
        }
    };
    let ident = Arc::new(load_identity());
    start_aux_udp();
    start_event_listener(); // 触摸回控: event 通道必须在 SETUP 应答前就监听好
    start_session_watchdog(); // 手机"静默断开"(不关 socket/不发 TEARDOWN) 的兜底
    // 注意: 标记**不能开机就建** —— 断开后要能回到黑屏。改成按"屏幕流连接数"维护:
    // 有连接 → 建 (静默保持最后一帧); 最后一条连接断开 → 删 (允许刷黑)。见 spawn_screen_listener。
    log(&format!("监听 [::]:{PORT} (双栈), pairing_id={} 等 iOS", ident.id));
    for conn in listener.incoming() {
        match conn {
            Ok(s) => {
                let ident = Arc::clone(&ident);
                std::thread::spawn(move || handle(s, ident));
            }
            Err(e) => log(&format!("accept 错误: {e}")),
        }
    }
}
