#!/usr/bin/env python3
"""生成 adb 用 RSA-2048 密钥 + 安卓 ADB_AUTH(RSAPUBLICKEY) 所需的 524 字节公钥 blob。

输出 adb_key.h (C 头), 内含:
  AB_N[256]     模数 n            (小端)
  AB_D[256]     私钥指数 d         (小端)
  AB_RR[256]    R^2 mod n, R=2^2048 (小端, Montgomery 用)
  AB_N0INV      -n^-1 mod 2^64
  AB_BLOB[524]  安卓公钥 blob: len(4)=64 | n0inv(4) | n(256) | rr(256) | e(4)=65537

用法: python3 tools/gen_adb_key.py [输出路径]
"""
import sys, os, struct

from cryptography.hazmat.primitives.asymmetric import rsa

E = 65537


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', 'adb_key.h')
    key = rsa.generate_private_key(public_exponent=E, key_size=2048)
    pn = key.private_numbers()
    n = pn.public_numbers.n
    d = pn.d
    assert n.bit_length() == 2048

    rr = pow(2, 2048 * 2, n)                     # Montgomery R^2 mod n
    n0inv = (-pow(n, -1, 1 << 64)) % (1 << 64)   # -n^-1 mod 2^64

    blob = bytearray(524)
    struct.pack_into('<I', blob, 0, 64)          # len = 2048/32 (32 位字数)
    struct.pack_into('<I', blob, 4, n0inv & 0xffffffff)
    blob[8:264] = n.to_bytes(256, 'little')
    blob[264:520] = rr.to_bytes(256, 'little')
    struct.pack_into('<I', blob, 520, E)

    def arr(name, data, per=12):
        lines = ['static const unsigned char %s[%d] = {' % (name, len(data))]
        for i in range(0, len(data), per):
            lines.append('    ' + ', '.join('0x%02x' % b for b in data[i:i + per]) + ',')
        lines.append('};')
        return '\n'.join(lines)

    hdr = []
    hdr.append('/* 自动生成, 勿手改 —— tools/gen_adb_key.py */')
    hdr.append('#ifndef ADB_KEY_H')
    hdr.append('#define ADB_KEY_H')
    hdr.append('')
    hdr.append('/* 指纹 = 公钥 blob SHA1 前 8 字节, 板端日志会打印, 与手机 adb_keys 对照用 */')
    hdr.append('#define AB_FP "%s"' % (''.join('%02x' % b for b in __import__('hashlib').sha1(bytes(blob)).digest()[:8])))
    hdr.append('')
    hdr.append(arr('AB_N', n.to_bytes(256, 'little')))
    hdr.append(arr('AB_D', d.to_bytes(256, 'little')))
    hdr.append(arr('AB_RR', rr.to_bytes(256, 'little')))
    hdr.append('#define AB_N0INV 0x%016xULL' % n0inv)
    hdr.append(arr('AB_BLOB', bytes(blob), 20))
    hdr.append('')
    hdr.append('#endif')
    open(out, 'w').write('\n'.join(hdr) + '\n')
    print('wrote', os.path.abspath(out), 'fp=%s' % ''.join('%02x' % b for b in __import__('hashlib').sha1(bytes(blob)).digest()[:8]))


if __name__ == '__main__':
    main()

