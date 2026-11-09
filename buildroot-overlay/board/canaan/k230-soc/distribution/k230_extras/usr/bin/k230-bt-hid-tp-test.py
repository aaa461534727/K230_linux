#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""k230-bt-hid-tp-test — HID 绝对触摸 (Digitizer / Report ID 4) 的离线回归测试。

干什么: 用假 dbus/gi + 假 HID 加载 k230-bt-hid, 断言「手指 → 绝对触摸报告」这条新路:
          ① Report Map 里有 Touch Screen 集合 (0x05 0x0D 0x09 0x04), Report ID 4 = 10 B
          ② 单指落下 = tip=1 + In Range + 归一化到 0..1920 的绝对坐标
          ③ 移动 = 再发一条报告 (坐标跟着走), 不需要按键/拖动判定
          ④ 抬手 = 该触点 tip=0 (手机自己算点击/拖动/手势)
          ⑤ 两指 = 一条报告里两个触点 (Contact ID 0/1)
          ⑥ 没有投屏几何 (geom 不存在) 时一条都不发
为什么重要: 这一路是照海奇的实现做的 (海奇 BT HID 描述符里就有 Touch Screen 集合)。
        鼠标指针永远做不到「单击不被判成拖动」和「底部上滑 = 控制中心」;
        绝对触摸天然做到 (手机认为是一根真手指)。
怎么用: python3 src/K230-cast/airplay/k230-bt-hid-tp-test.py   # 全 PASS 退出码 0
依赖:   无 (不接手机/蓝牙; 桩掉 dbus、gi)
在哪跑: 本机或板子都行 (板子跑需要同目录有 k230-bt-hid)
产出:   终端 PASS/FAIL。
"""

import os
import re
import struct
import sys
import tempfile
import time as _t

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

_geom = os.path.join(tempfile.mkdtemp(prefix="tp-test-"), "geom")
with open(_geom, "w", encoding="utf-8") as _f:
    _f.write("0 0 480 800 0 0 416 720\n")
os.environ["K230_WFD_GEOM"] = _geom
os.environ["K230_BT_HID_CAL"] = os.path.join(os.path.dirname(_geom), "no-such-cal.json")
os.environ.pop("K230_BT_HID_TOUCH", None)

import importlib.util                                             # noqa: E402
from importlib.machinery import SourceFileLoader                  # noqa: E402

_spec = importlib.util.spec_from_loader(
    "k230_bt_hid_int_test",
    SourceFileLoader("k230_bt_hid_int_test", os.path.join(HERE, "k230-bt-hid-int-test.py")))
_int = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_int)
MOD = _int._load()


class FakeHid(object):
    """只实现触摸发送; 没有 mouse() —— 用来证明「有 touch_report 就自动走触摸路」。"""

    def __init__(self):
        self.tp = []            # 每条 (f0, f1)
        self.stats = {"peer": "94:BF:2D:5B:30:AB"}

    def touch_report(self, f0, f1):
        self.tp.append((tuple(f0), tuple(f1)))
        return True

    # 触摸路不应该调用这些; 调了就记下来让断言能抓到
    def mouse(self, *_a, **_k):
        raise AssertionError("触摸模式下不该发鼠标报告")

    def key(self, *_a, **_k):
        return True

    def key_tap(self, *_a, **_k):
        return True

    def consumer(self, *_a, **_k):
        return True


ok = fail = 0


def check(name, cond, extra=""):
    global ok, fail
    if cond:
        ok += 1
        print("  ✓ %s%s" % (name, (" — " + extra) if extra else ""))
    else:
        fail += 1
        print("  ✗ %s%s" % (name, (" — " + extra) if extra else ""))


print("k230-bt-hid 绝对触摸 (Digitizer, Report ID 4) 回归测试")

# ---- ① 描述符 ----
desc = bytes(MOD.REPORT_MAP)
check("描述符含 Touch Screen 集合 (05 0D 09 04)", b"\x05\x0d\x09\x04" in desc,
      "描述符 %d B" % len(desc))
check("Touch Screen 的 Report ID = 4", b"\x05\x0d\x09\x04\xa1\x01\x85\x04" in desc)
# 极简解析: 算出 Report ID 4 的输入报告位宽
_d = list(desc)
_i = 0
_rid, _rsz, _rcnt, _bits = None, 0, 0, {}
while _i < len(_d):
    _b = _d[_i]
    _i += 1
    if _b == 0xFE:
        _sz = _d[_i]
        _i += 1 + _sz
        continue
    _sz = [0, 1, 2, 4][_b & 3]
    _typ, _tag = (_b >> 2) & 3, _b >> 4
    _v = int.from_bytes(bytes(_d[_i:_i + _sz]), "little") if _sz else 0
    _i += _sz
    if _typ == 1:
        if _tag == 7:
            _rsz = _v
        elif _tag == 9:
            _rcnt = _v
        elif _tag == 8:
            _rid = _v
    elif _typ == 0 and _tag == 8:
        _bits[_rid] = _bits.get(_rid, 0) + _rsz * _rcnt
check("Report ID 4 输入报告 = 10 B (X/Y 各 16 bit ×2 指 + 各 1 B 标志)",
      _bits.get(4) == 80, "%s bit" % _bits.get(4))
check("FMT_TP 打包长度 = 11 B (含 Report ID)", struct.calcsize(MOD.FMT_TP) == 11,
      "%d B" % struct.calcsize(MOD.FMT_TP))

# ---- 包的位序: tip / in range / contact id / contact count ----
p = MOD.pack_tp((1, 1, 3, 1, 0x1234, 0x5678), (0, 0, 0, 0, 0, 0))
check("标志字节 = Tip|InRange|(ID<<2)|(Count<<7)",
      p[1] == (1 | 2 | (3 << 2) | 0x80), "0x%02x" % p[1])
check("X/Y 小端 16 位", p[2:4] == b"\x34\x12" and p[4:6] == b"\x78\x56",
      p.hex())

# ---- ②③④ 单指 ----
h = FakeHid()
tm = MOD.TouchMouse(h, mode="abs")
check("有 touch_report ⇒ 自动走绝对触摸", tm.tp_on is True)

# 手指落在触屏中心 (240,400) → 内容区中心 (208,360) → 归一化 (960,960)
tm.feed(MOD.EV_ABS, 0x2F, 0)
tm.feed(MOD.EV_ABS, 0x39, 1)
tm.feed(MOD.EV_ABS, 0x35, 240)
tm.feed(MOD.EV_ABS, 0x36, 400)
tm.feed(MOD.EV_SYN, 0, 0)
tm.commit()
f0, f1 = h.tp[-1]
check("单指落下: tip=1 / In Range=1 / Contact ID 合法 (0..31)",
      f0[0] == 1 and f0[1] == 1 and 0 <= f0[2] <= 31, str(f0))
check("单指落下: 坐标归一化到 0..1920 (中心)",
      abs(f0[4] - 960) <= 6 and abs(f0[5] - 960) <= 6,
      "(%d,%d)" % (f0[4], f0[5]))
check("单指落下: 第二指 tip=0 (固定长度报告)", f1[0] == 0, str(f1))

# 移动: 手指往右下走 → 触点跟着走, 仍然是 tip=1 (不需要按键)
# 注意: 纯移动帧有 12 ms 限频 (抖掉重复帧, 免得塞爆经典蓝牙队列), 这里等过窗口。
_t.sleep(0.02)
tm.feed(MOD.EV_ABS, 0x35, 360)
tm.feed(MOD.EV_ABS, 0x36, 600)
tm.feed(MOD.EV_SYN, 0, 0)
tm.commit()
f0b, _ = h.tp[-1]
check("移动: 触点跟着走且仍然 tip=1 (拖动不需要按键)",
      f0b[0] == 1 and f0b[4] > f0[4] and f0b[5] > f0[5],
      "(%d,%d)" % (f0b[4], f0b[5]))

# 重复帧去重: 坐标不变再 commit 一次, 不许再发
_n_before = len(h.tp)
_t.sleep(0.05)
tm.feed(MOD.EV_SYN, 0, 0)
tm.commit()
check("内容没变的重复帧不再发 (否则抬手会被队列顶到后面 → 长按)",
      len(h.tp) == _n_before, "多发 %d 条" % (len(h.tp) - _n_before))

# 抬手
tm.feed(MOD.EV_ABS, 0x39, -1)
tm.commit()
f0c, _ = h.tp[-1]
check("抬手: 该触点 tip=0 (点击/拖动由手机自己判)", f0c[0] == 0, str(f0c))

# ---- ⑤ 两指 ----
h.tp = []
tm2 = MOD.TouchMouse(h, mode="abs")
for slot, tid, xx, yy in ((0, 1, 120, 200), (1, 2, 360, 600)):
    tm2.feed(MOD.EV_ABS, 0x2F, slot)
    tm2.feed(MOD.EV_ABS, 0x39, tid)
    tm2.feed(MOD.EV_ABS, 0x35, xx)
    tm2.feed(MOD.EV_ABS, 0x36, yy)
tm2.feed(MOD.EV_SYN, 0, 0)
tm2.commit()
f0d, f1d = h.tp[-1]
check("两指: 一条报告里两个触点都在且 Contact ID 不同",
      f0d[0] == 1 and f1d[0] == 1 and f0d[2] != f1d[2],
      "触点0=%s 触点1=%s" % (str(f0d), str(f1d)))
check("两指: 坐标一左一右 (左上 / 右下)",
      f0d[4] < f1d[4] and f0d[5] < f1d[5],
      "(0:%d,%d) (1:%d,%d)" % (f0d[4], f0d[5], f1d[4], f1d[5]))

# ---- ⑤b 面板只在"值变化"时才发轴 —— 同一点连点会被整段吞掉 (2026-11-06 真机根因) ----
# 板端 /dev/input/event0 原始实录 (主人反馈"点设置返回没反应"的那个窗口):
#   MT_TID 303 / MT_Y 71 / BTN_TOUCH 1 / ABS_Y 71 / SYN_REPORT     ← 一趟新触点**没有 MT_X**
#   (ft5x06 只在值变化时才发 ABS_MT_POSITION_X/Y; 连点同一列 ⇒ x 没变 ⇒ 不发)
# 老代码把新触点的 x/y 置 None ⇒ 这个槽永远凑不齐坐标 ⇒ _tp_commit 的 act 过滤把它剔除
# ⇒ **整段手势一条报告都不发**, 手机侧表现就是"点了没反应" (换行/换列点才会生效)。
h.tp = []
tm4 = MOD.TouchMouse(h, mode="abs")


def _tap(x, y, tid, mt_x=True, mt_y=True, lg_x=True, lg_y=True):
    """按真机驱动的事件顺序喂一次点击; 可以只发变化的轴 (模拟 ft5x06 的省略行为)。"""
    tm4.feed(MOD.EV_ABS, 0x2F, 0)
    tm4.feed(MOD.EV_ABS, 0x39, tid)
    if mt_x:
        tm4.feed(MOD.EV_ABS, 0x35, x)
    if mt_y:
        tm4.feed(MOD.EV_ABS, 0x36, y)
    if lg_x:
        tm4.feed(MOD.EV_ABS, 0x00, x)
    if lg_y:
        tm4.feed(MOD.EV_ABS, 0x01, y)
    tm4.feed(MOD.EV_KEY, 0x14A, 1)
    tm4.feed(MOD.EV_SYN, 0, 0)
    tm4.commit()
    _t.sleep(0.02)
    tm4.feed(MOD.EV_ABS, 0x39, -1)
    tm4.feed(MOD.EV_KEY, 0x14A, 0)
    tm4.feed(MOD.EV_SYN, 0, 0)
    tm4.commit()
    _t.sleep(0.05)


_tap(36, 71, 301)                                            # 第一下: 两个轴都发
_n1 = len(h.tp)
_p1 = h.tp[0] if h.tp else None
_per = 1 + MOD.TP_REL_REPEAT          # 1 条按下 + N 条抬起 (抬起冗余发, 见 TP_REL_REPEAT)
check("连点第 1 下: 发出按下 + 抬起 (抬起冗余 %d 条)" % (MOD.TP_REL_REPEAT - 1),
      _n1 == _per, "%d 条" % _n1)

_tap(36, 66, 302, mt_x=False, lg_x=False)                    # 第二下: 只有 Y 变 (x 一样)
_n2 = len(h.tp) - _n1
check("连点第 2 下 (面板只发 Y, 没有 MT_X): 照样发出按下 + 抬起",
      _n2 == _per, "%d 条" % _n2)
_p2 = h.tp[_n1] if _n2 >= 1 else None
check("连点第 2 下: 落点沿用上一段的 X (没有被吞成全 0 报告)",
      _p2 is not None and _p2[0][0] == 1 and _p1 is not None and _p2[0][4] == _p1[0][4],
      "第1下 x=%s 第2下 x=%s" % (_p1[0][4] if _p1 else "?", _p2[0][4] if _p2 else "?"))
check("连点第 2 下: Contact ID 与第 1 下不同 (新触点 ≠ 旧 contact 在移动)",
      _p1 is not None and _p2 is not None and _p1[0][2] != _p2[0][2],
      "id %s → %s" % (_p1[0][2] if _p1 else "?", _p2[0][2] if _p2 else "?"))

_n3 = len(h.tp)
_tap(36, 66, 303, mt_x=False, mt_y=False, lg_x=False, lg_y=False)   # 第三下: 一个轴都不发
_n3 = len(h.tp) - _n3
check("连点第 3 下 (同一个像素, 面板一个轴都不发): 仍发出按下 + 抬起",
      _n3 == _per, "%d 条" % _n3)
check("抬手帧坐标停在原地 (不是 0,0 —— 否则 iOS 会当成一次猛的上滑)",
      h.tp[_n1][0][4] != 0 and h.tp[-1][0][4] != 0,
      "抬手=(%d,%d)" % (h.tp[-1][0][4], h.tp[-1][0][5]))

# ---- ⑤c Contact ID 策略 (2026-11-06 真机根因, 板端实测 80 笔) ----
# 这块是 **2 指触摸屏**: 同时存在的 contact 最多 2 个。老行为每笔按下 +1,
# 一路涨到 31 再绕回 ⇒ iOS 侧等价于"每笔都新建一个触点", 槽位占满后新的按下
# 全被忽略, 约 13~15 s 超时清掉才恢复。板端纯左右滑对照 (同一手机/同一连接):
#   rotate(0..31) 26~27/40, 第 18~31 笔连挂; slot(0/1) 80/80、再测 30/30。
# 三档行为都钉住, 免得以后被改回 rotate 而无人发现。
_def_mode = MOD.CID_MODE


def _press_cids(mode, n=6):
    """连续按下 n 次 (每次都完整抬手), 返回每笔按下报告里的 Contact ID。"""
    MOD.CID_MODE = mode
    tm5 = MOD.TouchMouse(h, mode="abs")
    out = []
    for k in range(n):
        base = len(h.tp)
        tm5.feed(MOD.EV_ABS, 0x2F, 0)
        tm5.feed(MOD.EV_ABS, 0x39, 900 + k)
        tm5.feed(MOD.EV_ABS, 0x35, 100 + k)
        tm5.feed(MOD.EV_ABS, 0x36, 200 + k)
        tm5.feed(MOD.EV_SYN, 0, 0)
        tm5.commit()
        _t.sleep(0.02)
        down = [r for r in h.tp[base:] if r[0][0] == 1]
        out.append(down[0][0][2] if down else None)
        tm5.feed(MOD.EV_ABS, 0x39, -1)
        tm5.feed(MOD.EV_SYN, 0, 0)
        tm5.commit()
        _t.sleep(0.03)
    return out


check("默认 Contact ID 策略 = slot (2 指屏, 不许回退成 rotate)",
      _def_mode == "slot", _def_mode)
_cs = _press_cids("slot", 6)
check("slot: 连按 6 笔 Contact ID 只在 0/1 之间轮转",
      all(c in (0, 1) for c in _cs) and all(_cs[i] != _cs[i + 1] for i in range(len(_cs) - 1)),
      "%s" % (_cs,))
_cr = _press_cids("rotate", 6)
check("rotate: 相邻 cid 差 1 (老行为, 会一路涨到 31 —— 板端 26~27/40)",
      all(_cr[i + 1] == ((_cr[i] + 1) & 0x1F) for i in range(len(_cr) - 1)),
      "%s" % (_cr,))
_cf = _press_cids("fixed", 6)
check("fixed: 每笔按下都是 1 (A/B 对照用)", set(_cf) == {1}, "%s" % (_cf,))
MOD.CID_MODE = "slot"                     # 复原, 不影响后面 ⑥⑦

# ---- ⑥ 没有几何 ----
os.unlink(_geom)
h.tp = []
tm3 = MOD.TouchMouse(h, mode="abs")
tm3.feed(MOD.EV_ABS, 0x2F, 0)
tm3.feed(MOD.EV_ABS, 0x39, 7)
tm3.feed(MOD.EV_ABS, 0x35, 100)
tm3.feed(MOD.EV_ABS, 0x36, 100)
tm3.feed(MOD.EV_SYN, 0, 0)
tm3.commit()
check("没有投屏几何时一条触摸报告都不发", h.tp == [], "%d 条" % len(h.tp))

# ---- ⑦ 回归护栏: 经典通道发送路径不许因"少一个 stats 键"崩掉 ----
# 现场 (2026-11-05 14:54:15): ClassicProfile 的 stats 只在 BLE 那份加了 'touch',
# 手机连上后主人第一次碰板子屏幕 ⇒ KeyError('touch') ⇒ 回调中断 ⇒ 抬手那条报告
# 发不出去 ⇒ iOS 30 s 后重置中断通道, 反控全废。这里把它钉住。
class _Conn(object):
    def __init__(self):
        self.sent = []

    def send(self, b):
        self.sent.append(bytes(b))
        return len(b)


class _FakeClassic(object):
    pass


# 注意: 经典传输的发报告在 ClassicHidDevice 里 (ClassicProfile 只管 SDP/profile)。
fc = _FakeClassic()
fc.int_conn = _Conn()
fc.stats = {"mouse": 0, "blocked": 0}          # 故意**没有** touch
try:
    ok_send = MOD.ClassicHidDevice._send_raw(fc, b"\x04\x03\x00" + b"\x00" * 8, "touch")
    err = None
except Exception as e:                                        # noqa: BLE001
    ok_send, err = None, e
check("经典通道 stats 没有 kind 键时也不抛 (自动补计数)",
      err is None and ok_send is True and fc.stats.get("touch") == 1,
      "err=%s stats=%s 发了 %d 条" % (err, fc.stats, len(fc.int_conn.sent)))
check("经典通道确实把报告写进了中断通道 (0xA1 头 + Report ID 4)",
      bool(fc.int_conn.sent) and fc.int_conn.sent[0][:2] == b"\xa1\x04",
      "首字节 %s" % (fc.int_conn.sent[0][:3].hex() if fc.int_conn.sent else "-"))

# 两个传输类都必须自带 'touch' 计数 (源码级护栏: 少一份就复发)
_src = open(os.path.join(HERE, "k230-bt-hid"), encoding="utf-8").read()
check("BLE 与 经典 两份 stats 都含 touch",
      _src.count('"touch": 0') >= 2, "出现 %d 次" % _src.count('"touch": 0'))

# ---- ⑧ HID SDP 记录: 属性 ID ↔ 类型必须严格对上 (iOS 比 BlueZ 挑) ----
# 现场 (2026-11-05 15:1x): 0x020A 往后类型整体错位 (0x020A 写成 uint16, 0x020C 写成
# boolean, 0x020E 写成 uint16, 末尾还多了 0x020F/0x0210 两个不存在的属性)。
# iOS 读到不合规的 HID SDP 记录会直接"连接不成功", 而 BlueZ 侧注册却会成功 ⇒
# 从板子上完全看不出问题。
import xml.etree.ElementTree as ET                                 # noqa: E402

_ESC = {0x0000: "uint32", 0x0001: "sequence", 0x0004: "sequence",
        0x0005: "sequence", 0x0006: "sequence", 0x0009: "sequence",
        0x000d: "sequence", 0x0100: "text", 0x0101: "text",
        0x0200: "uint16", 0x0201: "uint16", 0x0202: "uint8", 0x0203: "uint8",
        0x0204: "boolean", 0x0205: "boolean", 0x0206: "sequence",
        0x0207: "sequence", 0x0208: "boolean", 0x0209: "boolean",
        0x020a: "boolean", 0x020b: "uint16", 0x020c: "uint16",
        0x020d: "boolean", 0x020e: "boolean"}
_root = ET.fromstring(MOD.SDP_XML % {"name": "K230-CAST",
                                     "report_map": MOD.REPORT_MAP.hex()})
_attrs = {}
for _a in _root.findall("attribute"):
    _k = int(_a.get("id"), 16)
    _attrs[_k] = list(_a)[0].tag
_missing = sorted(set(_ESC) - set(_attrs))
_extra = sorted(set(_attrs) - set(_ESC))
_wrong = sorted("%02x:%s≠%s" % (k, _attrs[k], v) for k, v in _ESC.items()
                if k in _attrs and _attrs[k] != v)
check("SDP: HID 属性 ID 一个不少", not _missing,
      "缺 %s" % (["0x%04x" % k for k in _missing] or "无"))
check("SDP: 没有规范外的多余属性", not _extra,
      "多 %s" % (["0x%04x" % k for k in _extra] or "无"))
check("SDP: 每个属性的类型都符合 HID 1.1 规范", not _wrong, "; ".join(_wrong) or "全对")
check("SDP: HIDDescriptorList 里带的描述符 = REPORT_MAP",
      (MOD.SDP_XML % {"name": "x", "report_map": MOD.REPORT_MAP.hex()}).count(
          MOD.REPORT_MAP.hex()) == 1,
      "%d B" % len(MOD.REPORT_MAP))

print()
if fail:
    print("FAIL: %d/%d" % (ok, ok + fail))
    sys.exit(1)
print("PASS: %d/%d" % (ok, ok + fail))
