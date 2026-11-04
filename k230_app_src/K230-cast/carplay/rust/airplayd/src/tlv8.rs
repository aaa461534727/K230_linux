//! TLV8 —— Apple 配对报文的基本编码 (HAP/AirPlay2 的 /pair-setup 用的就是这个)。
//!
//! 格式: [type:u8][len:u8][value:len], 值超过 255 字节时要拆成多段同 type 的 TLV, 接收端按
//! 顺序拼接。K230 实测 iOS 首发 /pair-setup 的 body 是 `00 01 00 06 01 01`:
//!   type=0x00 (method) len=1 value=0  → PairSetup
//!   type=0x06 (state)  len=1 value=1  → M1 (起步)

pub const METHOD: u8 = 0x00;
pub const IDENTIFIER: u8 = 0x01;
pub const SALT: u8 = 0x02;
pub const PUBLIC_KEY: u8 = 0x03;
pub const PROOF: u8 = 0x04;
pub const ENCRYPTED_DATA: u8 = 0x05;
pub const STATE: u8 = 0x06;
pub const ERROR: u8 = 0x07;
pub const SIGNATURE: u8 = 0x0A;

#[derive(Debug, Clone, Default)]
pub struct Tlv8(pub Vec<(u8, Vec<u8>)>);

impl Tlv8 {
    pub fn parse(b: &[u8]) -> Option<Tlv8> {
        let mut out: Vec<(u8, Vec<u8>)> = Vec::new();
        let mut i = 0;
        while i + 2 <= b.len() {
            let ty = b[i];
            let len = b[i + 1] as usize;
            if i + 2 + len > b.len() {
                return None;
            }
            out.push((ty, b[i + 2..i + 2 + len].to_vec()));
            i += 2 + len;
        }
        Some(Tlv8(out))
    }

    /// 把同一 type 的所有分片按出现顺序拼起来 (TLV8 的标准语义)。
    /// 注意: **不要在 parse 里就地合并** —— 那样会丢掉分片边界, 一旦某片长度读错,
    /// 后面所有字段都会串位 (实测把 384 B 的 A 读成 639 B、64 B 的 proof 读成 128 B)。
    pub fn concat(&self, ty: u8) -> Option<Vec<u8>> {
        let mut v = Vec::new();
        let mut found = false;
        for (t, val) in &self.0 {
            if *t == ty {
                v.extend_from_slice(val);
                found = true;
            }
        }
        if found { Some(v) } else { None }
    }

    pub fn get(&self, ty: u8) -> Option<&[u8]> {
        self.0.iter().find(|(t, _)| *t == ty).map(|(_, v)| v.as_slice())
    }

    pub fn u8_of(&self, ty: u8) -> Option<u8> {
        self.get(ty).and_then(|v| v.first().copied())
    }

    pub fn push(&mut self, ty: u8, val: &[u8]) {
        let mut rest = val;
        loop {
            let n = rest.len().min(255);
            self.0.push((ty, rest[..n].to_vec()));
            rest = &rest[n..];
            if rest.is_empty() {
                break;
            }
        }
    }

    pub fn with_u8(mut self, ty: u8, v: u8) -> Self {
        self.push(ty, &[v]);
        self
    }

    pub fn to_bytes(&self) -> Vec<u8> {
        let mut out = Vec::new();
        for (t, v) in &self.0 {
            out.push(*t);
            out.push(v.len() as u8);
            out.extend_from_slice(v);
        }
        out
    }
}
