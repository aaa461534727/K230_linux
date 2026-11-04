
#!/usr/bin/env python3
"""ArdVNC: 用 Apple 认证(type 30)接管 Mac 真实会话, 支持抓屏/指针/键盘."""
import socket, struct, os, hashlib, subprocess, time

def recvn(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError("eof need %d got %d" % (n, len(b)))
        b += c
    return b

def aes128ecb(key, data):
    p = subprocess.run(["openssl","enc","-aes-128-ecb","-nopad","-nosalt","-K",key.hex()],
                       input=data, capture_output=True)
    if p.returncode != 0 or len(p.stdout) != len(data):
        raise RuntimeError("aes fail")
    return p.stdout

class ArdVNC:
    def __init__(self, user="justinfo", password="cs2012", host="127.0.0.1", port=5900):
        s = socket.create_connection((host, port), 8)
        s.settimeout(25)
        recvn(s, 12)
        s.sendall(b"RFB 003.008\n")
        n = recvn(s, 1)[0]
        types = list(recvn(s, n))
        if 30 not in types:
            raise RuntimeError("no ARD auth: %s" % types)
        s.sendall(b"\x1e")
        g, keylen = struct.unpack(">HH", recvn(s, 4))
        p = int.from_bytes(recvn(s, keylen), "big")
        peer = int.from_bytes(recvn(s, keylen), "big")
        priv = int.from_bytes(os.urandom(keylen), "big") % (p - 3) + 2
        pub = pow(g, priv, p)
        shared = pow(peer, priv, p)
        key = hashlib.md5(shared.to_bytes(keylen, "big")).digest()
        u, w = user.encode(), password.encode()
        creds = bytearray(os.urandom(128))
        creds[0:len(u)] = u; creds[len(u)] = 0
        creds[64:64+len(w)] = w; creds[64+len(w)] = 0
        s.sendall(aes128ecb(key, bytes(creds)) + pub.to_bytes(keylen, "big"))
        r = struct.unpack(">I", recvn(s, 4))[0]
        if r != 0:
            raise RuntimeError("ARD auth failed %d" % r)
        s.sendall(b"\x01")
        self.w, self.h = struct.unpack(">HH", recvn(s, 4))
        self.pf = recvn(s, 16)
        nlen = struct.unpack(">I", recvn(s, 4))[0]
        self.name = recvn(s, nlen)
        self.s = s
        self.fb = bytearray(self.w * self.h * 4)
        self.last_ptr = None

    def _rects(self):
        cnt = struct.unpack(">H", recvn(self.s, 2))[0]
        for _ in range(cnt):
            x, y, w, h = struct.unpack(">HHHH", recvn(self.s, 8))
            enc = struct.unpack(">i", recvn(self.s, 4))[0]
            if enc == 0:                      # Raw
                data = recvn(self.s, w * h * 4)
                for row in range(h):
                    o = ((y + row) * self.w + x) * 4
                    self.fb[o:o + w * 4] = data[row * w * 4:(row + 1) * w * 4]
            elif enc == 1:                    # CopyRect
                sx, sy = struct.unpack(">HH", recvn(self.s, 4))
                for row in range(h):
                    so = ((sy + row) * self.w + sx) * 4
                    do = ((y + row) * self.w + x) * 4
                    self.fb[do:do + w * 4] = self.fb[so:so + w * 4]
            else:
                raise RuntimeError("unsupported encoding %d" % enc)

    def update(self, x=0, y=0, w=0, h=0):
        if w == 0:
            w, h = self.w, self.h
        self.s.sendall(struct.pack(">BBHHHH", 3, 0, x, y, w, h))
        while True:
            t = recvn(self.s, 1)[0]
            if t == 0:
                recvn(self.s, 1); self._rects(); return
            if t == 1:
                recvn(self.s, 1); recvn(self.s, 2); self._rects(); return
            if t == 3:
                recvn(self.s, 1); recvn(self.s, 6); continue
            if t == 2:
                # Bell 是服务端提示, 无害 (Mac 在切换显示器/控制中心时会发)。
                # 原来这里抛异常, 导致一连上 AirPlay 就报 bell 崩掉。
                continue
            raise RuntimeError("msg %d" % t)

    def save_ppm(self, path):
        rmax, gmax, bmax = struct.unpack(">HHH", self.pf[4:10])
        rsh, gsh, bsh = self.pf[10], self.pf[11], self.pf[12]
        rf = 8 - rmax.bit_length(); gf = 8 - gmax.bit_length(); bf = 8 - bmax.bit_length()
        out = bytearray(self.w * self.h * 3)
        fb = self.fb
        for i in range(self.w * self.h):
            px = fb[i*4] | (fb[i*4+1] << 8) | (fb[i*4+2] << 16) | (fb[i*4+3] << 24)
            out[i*3]   = ((px >> rsh) & rmax) >> rf
            out[i*3+1] = ((px >> gsh) & gmax) >> gf
            out[i*3+2] = ((px >> bsh) & bmax) >> bf
        with open(path, "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (self.w, self.h))
            f.write(bytes(out))
        return path

    def ptr(self, x, y, mask=0):
        self.s.sendall(struct.pack(">BBHH", 5, mask, x, y))
        self.last_ptr = (x, y, mask)

    def click(self, x, y, delay=0.06):
        self.ptr(x, y, 0); time.sleep(delay)
        self.ptr(x, y, 1); time.sleep(delay)
        self.ptr(x, y, 0); time.sleep(0.05)

    def key(self, keysym, down):
        # keysym -> X11 keysym, RFB KeyEvent 用 X keysym
        self.s.sendall(struct.pack(">BBHI", 4, 1 if down else 0, 0, keysym))

    def type_char(self, ch):
        ks = keysym_of(ch)
        self.key(ks, True); self.key(ks, False)
        time.sleep(0.02)

KEYSYM = {'a':0x61,'b':0x62,'c':0x63,'d':0x64,'e':0x65,'f':0x66,'g':0x67,'h':0x68,'i':0x69,
          'j':0x6a,'k':0x6b,'l':0x6c,'m':0x6d,'n':0x6e,'o':0x6f,'p':0x70,'q':0x71,'r':0x72,
          's':0x73,'t':0x74,'u':0x75,'v':0x76,'w':0x77,'x':0x78,'y':0x79,'z':0x7a,
          '0':0x30,'1':0x31,'2':0x32,'3':0x33,'4':0x34,'5':0x35,'6':0x36,'7':0x37,'8':0x38,'9':0x39,
          ' ' :0x20,'\n':0xff0d,'\t':0xff09,'-':0x2d,'_':0x5f,'.':0x2e,',':0x2c,'/':0x2f,
          ':':0x3a,';':0x3b,'=':0x3d,'+':0x2b,'@':0x40,'#':0x23,'!':0x21,'?':0x3f}
def keysym_of(ch):
    return KEYSYM.get(ch, ord(ch))
