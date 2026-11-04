//! 极简 RTSP/HTTP 报文解析 (AirPlay 用的是 "HTTP 样子的 RTSP": 请求行 + 头 + 可选二进制体)。

#[derive(Debug, Clone)]
pub struct Request {
    pub method: String,
    pub path: String,
    pub headers: Vec<(String, String)>,
    pub body: Vec<u8>,
    pub cseq: Option<String>,
}

impl Request {
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers
            .iter()
            .find(|(k, _)| k.eq_ignore_ascii_case(name))
            .map(|(_, v)| v.as_str())
    }

    /// 从缓冲区里切出一条完整请求; 不完整返回 None (调用方继续读)。
    pub fn try_parse(buf: &[u8]) -> Option<(Request, usize)> {
        let hdr_end = find_subslice(buf, b"\r\n\r\n")?;
        let head = std::str::from_utf8(&buf[..hdr_end]).ok()?;
        let mut lines = head.split("\r\n");
        let start = lines.next()?;
        let mut parts = start.split_whitespace();
        let method = parts.next()?.to_string();
        let path = parts.next()?.to_string();
        let mut headers = Vec::new();
        for line in lines {
            if let Some((k, v)) = line.split_once(':') {
                headers.push((k.trim().to_string(), v.trim().to_string()));
            }
        }
        let cseq = headers
            .iter()
            .find(|(k, _)| k.eq_ignore_ascii_case("CSeq"))
            .map(|(_, v)| v.clone());
        let len: usize = headers
            .iter()
            .find(|(k, _)| k.eq_ignore_ascii_case("Content-Length"))
            .and_then(|(_, v)| v.parse().ok())
            .unwrap_or(0);
        let total = hdr_end + 4 + len;
        if buf.len() < total {
            return None;
        }
        let body = buf[hdr_end + 4..total].to_vec();
        Some((
            Request { method, path, headers, body, cseq },
            total,
        ))
    }
}

fn find_subslice(hay: &[u8], needle: &[u8]) -> Option<usize> {
    hay.windows(needle.len()).position(|w| w == needle)
}

pub fn response(cseq: Option<&str>, extra: &[(&str, String)], body: &[u8]) -> Vec<u8> {
    let mut out = String::from("RTSP/1.0 200 OK\r\n");
    if let Some(c) = cseq {
        out.push_str(&format!("CSeq: {c}\r\n"));
    }
    out.push_str("Server: AirTunes/220.68\r\n");
    for (k, v) in extra {
        out.push_str(&format!("{k}: {v}\r\n"));
    }
    out.push_str(&format!("Content-Length: {}\r\n\r\n", body.len()));
    let mut b = out.into_bytes();
    b.extend_from_slice(body);
    b
}
