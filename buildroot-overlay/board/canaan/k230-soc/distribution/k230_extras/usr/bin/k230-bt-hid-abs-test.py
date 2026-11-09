#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""k230-bt-hid-abs-test — 绝对定位 (AbsPointer) 的离线回归测试。

干什么: 用假 dbus/gi (复用 k230-bt-hid-int-test 的桩) 加载 k230-bt-hid, 用假 HID
        抓「实际发出去的鼠标报告」, 断言归一化绝对定位的换算与拆步:
          ① /run/wfd-uibc-geom 解析 + 触屏坐标 → 镜像像素换算
          ② 位置未知时先 clamp (X、Y 分开按到左上角), 再按实测步长表拆步
          ③ 拆步步长的和 ≈ 目标位移 (误差 ≤ 6 px, 即实测叠加误差)
          ④ 位置已知时不再 clamp, 只发增量
          ⑤ mode=rel 时不吃 abs 路径 (老行为保留)
怎么用: python3 src/K230-cast/airplay/k230-bt-hid-abs-test.py   # 全 PASS 退出码 0
依赖:   无 (不接手机/蓝牙; 桩掉 dbus、gi)
在哪跑: 本机或板子都行 (板子跑需要同目录有 k230-bt-hid)
产出:   终端 PASS/FAIL; 是 2026-11-05「触点位置对不上」修复的回归护栏。
"""

import os
import sys
import tempfile
import time as _t
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

_geom = os.path.join(tempfile.mkdtemp(prefix="abs-test-"), "geom")
with open(_geom, "w", encoding="utf-8") as _f:
    _f.write("0 0 480 800 0 0 416 720\n")
os.environ["K230_WFD_GEOM"] = _geom
os.environ["K230_BT_HID_CAL"] = os.path.join(os.path.dirname(_geom), "no-such-cal.json")

import importlib.util                                             # noqa: E402
from importlib.machinery import SourceFileLoader                  # noqa: E402

_spec = importlib.util.spec_from_loader(
    "k230_bt_hid_int_test",
    SourceFileLoader("k230_bt_hid_int_test", os.path.join(HERE, "k230-bt-hid-int-test.py")))
_int = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_int)          # 装上 dbus/gi 桩 (它只在 __main__ 里跑测试)
MOD = _int._load()


class FakeHid(object):
    def __init__(self, peer="94:BF:2D:5B:30:AB"):
        self.sent = []
        self.btns = []          # 每条报告带的按键位 (拖动判定靠它)
        self.stats = types.SimpleNamespace() if False else {"peer": peer}

    def mouse(self, buttons=0, dx=0, dy=0, wheel=0):
        self.sent.append((int(dx), int(dy)))
        self.btns.append(int(buttons))
        return True

    def key(self, *_a, **_k):
        return True

    def key_tap(self, *_a, **_k):
        return True

    def consumer(self, *_a, **_k):
        return True


def planned(steps, reports):
    """按标定表把发出的报告折回「计划位移 px」"""
    m = {}
    for px, cnt in steps:
        m.setdefault(cnt, px)
    sx = sum(m.get(abs(d), -1) for d, _ in reports if d)
    sy = sum(m.get(abs(d), -1) for _, d in reports if d)
    return sx, sy


ok = fail = 0


def check(name, cond, extra=""):
    global ok, fail
    if cond:
        ok += 1
        print("  ✓ %s%s" % (name, (" — " + extra) if extra else ""))
    else:
        fail += 1
        print("  ✗ %s%s" % (name, (" — " + extra) if extra else ""))


print("k230-bt-hid 绝对定位 (AbsPointer) 回归测试")
h = FakeHid()
ap = MOD.AbsPointer(h)

# ① 几何 + 坐标换算
check("geom 解析", ap.geom == (0.0, 0.0, 480.0, 800.0, 0.0, 0.0, 416.0, 720.0),
      str(ap.geom))
c = ap.touch_to_frame(479, 799)
check("触屏右上角 → 内容右下角", abs(c[0] - 415) < 2 and abs(c[1] - 719) < 2,
      "(%.1f,%.1f)" % c)
c = ap.touch_to_frame(240, 400)
check("触屏中心 → 内容中心", abs(c[0] - 208) < 2 and abs(c[1] - 360) < 2,
      "(%.1f,%.1f)" % c)
c = ap.touch_to_frame(-50, 9999)
check("触屏越界被夹住", c == (0.0, 719.0), str(c))

# ② 未知位置 → 先 clamp
# 2026-11-06: 锚定条数改按保守步长 CLAMP_STEP_PX 算 (以前 cw/186+2, 换跟踪速度慢的手机时
# 条数不够 ⇒ 指针没真贴边却被记成 0, 误差跨手势累积)。
N_CLAMP_X = int(416 / MOD.CLAMP_STEP_PX) + 3
N_CLAMP_Y = int(720 / MOD.CLAMP_STEP_PX) + 3
N_ANCHOR = N_CLAMP_X + N_CLAMP_Y
h.sent = []
ap.x = ap.y = None
ap.goto(200, 300)
n_clamp_x, n_clamp_y = N_CLAMP_X, N_CLAMP_Y
clamp_reports = h.sent[:n_clamp_x + n_clamp_y]
check("位置未知先 clamp", all((d, dy) in ((-127, 0), (0, -127)) for d, dy in clamp_reports),
      "%d 条 (X %d + Y %d)" % (len(clamp_reports), n_clamp_x, n_clamp_y))
check("clamp 只发单轴", all((d == 0) != (dy == 0) for d, dy in clamp_reports))

# ③ 拆步位移 ≈ 目标
moves = h.sent[n_clamp_x + n_clamp_y:]
sx, sy = planned(ap.steps, moves)
check("拆步 X 位移 ≈ 目标 200", abs(sx - 200) <= 8, "计划 %.0f px / %d 条报告" % (sx, len(moves)))
check("拆步 Y 位移 ≈ 目标 300", abs(sy - 300) <= 8, "计划 %.0f px" % sy)
check("移动报告计数 ≤ 127", all(abs(d) <= 127 and abs(dy) <= 127 for d, dy in moves))

# ④ 已知位置 → 只发增量, 不 clamp
h.sent = []
ap.goto(ap.x + 40, ap.y)
msgs = list(h.sent)
sx, _ = planned(ap.steps, msgs)
check("位置已知不重锚", len(msgs) <= 4 and abs(sx - 40) <= 8,
      "%d 条报告, 计划 %.0f px" % (len(msgs), sx))

# ⑤ 目标不动 → 一条报告都不发
h.sent = []
ap.goto(ap.x, ap.y)
check("目标=当前位置不发报告", h.sent == [], str(h.sent))

# ⑥ 增益 (换手机现场微调) 生效
ap.cal["peers"] = {"94:BF:2D:5B:30:AB": {"gain": 2.0}}
check("peers 增益按对端生效", abs(ap._gain() - 2.0) < 1e-6, "gain=%.2f" % ap._gain())
ap.cal["peers"] = {}

# ⑦ mode=rel 保留老路径
tm_rel = MOD.TouchMouse(FakeHid(), sens=2.2, mode="rel")
tm_abs = MOD.TouchMouse(FakeHid(), sens=2.2, mode="abs")
check("mode=rel 不建 AbsPointer", tm_rel.ap is None and tm_abs.ap is not None)

# ⑧ abs 模式: 第一帧触摸就定位 (手指位置 = 目标)
h2 = FakeHid()
tm = MOD.TouchMouse(h2, mode="abs")
tm.ap.x = tm.ap.y = 0.0          # 假装已锚定, 省掉 clamp 噪音
tm.ap.last_go = _t.time()        # 同时清掉"空闲超时"标记 (否则 goto 会主动重锚)
tm.feed(MOD.EV_ABS, 0x2F, 0)     # ABS_MT_SLOT
tm.feed(MOD.EV_ABS, 0x39, 1)     # ABS_MT_TRACKING_ID → 新手指
tm.feed(MOD.EV_ABS, 0x35, 240)   # ABS_MT_POSITION_X = 240 (屏中心)
tm.feed(MOD.EV_ABS, 0x36, 400)   # ABS_MT_POSITION_Y = 400
tm.feed(MOD.EV_SYN, 0, 0)
tm.commit()      # 2026-11-05: 一批事件处理完才定位一次 (见 commit)
# 2026-11-06: 每一指落下都重锚 (消掉开环累积误差), 所以前面是 N_ANCHOR 条锚定报告
n_anchor = len(h2.sent) and sum(1 for r in h2.sent if r in ((-127, 0), (0, -127)))
check("每次新手指落下都先重锚定", n_anchor == N_ANCHOR,
      "锚定 %d 条" % n_anchor)
sx, sy = planned(tm.ap.steps, [r for r in h2.sent if r not in ((-127, 0), (0, -127))])
check("触摸即定位 (abs)", abs(sx - 208) <= 8 and abs(sy - 360) <= 8,
      "发出 %d 条, 计划 (%.0f,%.0f) px" % (len(h2.sent), sx, sy))
# 2026-11-05 起: 记录的是「真正发出去的位移」而不是目标 (小位移发不出去时不许前进)
check("位置跟随 = 实际发出 (不是目标)", abs(tm.ap.x - sx) < 2 and abs(tm.ap.y - sy) < 2,
      "(%.0f,%.0f) vs 发出 (%.0f,%.0f)" % (tm.ap.x, tm.ap.y, sx, sy))

# ⑨ 常驻服务先启动 (还没有投屏会话 → 没有 geom), 会话随后才开始: 第一次触摸就要能定位
#    (2026-11-05 抓到的真 bug: 构造时读不到 geom ⇒ 整个会话里 abs 永久失效)
os.unlink(_geom)
h3 = FakeHid()
tm3 = MOD.TouchMouse(h3, mode="abs")
tm3.feed(MOD.EV_ABS, 0x2F, 0)
tm3.feed(MOD.EV_ABS, 0x39, 7)
tm3.feed(MOD.EV_ABS, 0x35, 240)
tm3.feed(MOD.EV_ABS, 0x36, 400)
tm3.feed(MOD.EV_SYN, 0, 0)
tm3.commit()      # 2026-11-05: 一批事件处理完才定位一次 (见 commit)
check("没有 geom (=非投屏会话) 时一条报告都不发", h3.sent == [], "%d 条" % len(h3.sent))
with open(_geom, "w", encoding="utf-8") as f:
    f.write("0 0 480 800 0 0 416 720\n")
tm3.feed(MOD.EV_ABS, 0x2F, 0)
tm3.feed(MOD.EV_ABS, 0x39, 8)          # 新手指 (旧手指已抬起)
tm3.feed(MOD.EV_ABS, 0x2F, 0)
tm3.feed(MOD.EV_ABS, 0x35, 240)
tm3.feed(MOD.EV_ABS, 0x36, 400)
tm3.feed(MOD.EV_SYN, 0, 0)
tm3.commit()      # 2026-11-05: 一批事件处理完才定位一次 (见 commit)
moves3 = [r for r in h3.sent if r not in ((-127, 0), (0, -127))]   # 剔掉锚定那几条
sx, sy = planned(tm3.ap.steps, moves3)
check("会话开始后 (geom 出现) 触摸立刻能定位", abs(sx - 208) <= 8 and abs(sy - 360) <= 8,
      "%d 条报告 (锚定 %d), 计划 (%.0f,%.0f) px"
      % (len(moves3), len(h3.sent) - len(moves3), sx, sy))
os.unlink(_geom)                        # 会话结束: geom 消失
h3.sent = []
tm3.feed(MOD.EV_ABS, 0x2F, 0)
tm3.feed(MOD.EV_ABS, 0x39, 9)
tm3.feed(MOD.EV_ABS, 0x2F, 0)
tm3.feed(MOD.EV_ABS, 0x35, 100)
tm3.feed(MOD.EV_ABS, 0x36, 100)
tm3.feed(MOD.EV_SYN, 0, 0)
tm3.commit()      # 2026-11-05: 一批事件处理完才定位一次 (见 commit)
# 上一指没抬起就切会话时, 新手指落下会先补一条「松开」(防卡键), 不算定位
moves3b = [r for r in h3.sent if r not in ((-127, 0), (0, -127)) and r != (0, 0)]
check("会话结束后 (geom 消失) 一个定位报告都不发", moves3b == [],
      "%d 条 (含补发松开)" % len(moves3b))

# ⑩ 空闲久了要重锚 (开环跟踪的误差截断): 位置已知 + 空闲 > ABS_ANCHOR_IDLE ⇒ 下一按先锚定
with open(_geom, "w", encoding="utf-8") as f:
    f.write("0 0 480 800 0 0 416 720\n")
h4 = FakeHid()
tm4 = MOD.TouchMouse(h4, mode="abs")
tm4.ap.x, tm4.ap.y = 200.0, 300.0          # 假装位置已知 (上次会话留下的)
tm4.last_abs = _t.time() - (MOD.ABS_ANCHOR_IDLE + 1)
tm4.ap.geom = tm4.ap.geom or (0.0, 0.0, 480.0, 800.0, 0.0, 0.0, 416.0, 720.0)
tm4.feed(MOD.EV_ABS, 0x2F, 0)
tm4.feed(MOD.EV_ABS, 0x39, 11)
tm4.feed(MOD.EV_ABS, 0x2F, 0)
tm4.feed(MOD.EV_ABS, 0x35, 100)
tm4.feed(MOD.EV_ABS, 0x36, 100)
tm4.feed(MOD.EV_SYN, 0, 0)
tm4.commit()      # 2026-11-05: 一批事件处理完才定位一次 (见 commit)
n_anchor = N_ANCHOR
check("空闲超时后按下会重锚", h4.sent[:n_anchor] and
      all(r in ((-127, 0), (0, -127)) for r in h4.sent[:n_anchor]),
      "前 %d 条是锚定报告" % min(n_anchor, len(h4.sent)))

# 紧跟着再按 (空闲没超时) **也重锚** —— 2026-11-06 起故意这么做: 唯一可靠的绝对参考是
# 「贴左边界被夹住」, 5 s 内的连续点击如果带着上一次的误差会越点越偏。
h4.sent = []
tm4.feed(MOD.EV_ABS, 0x2F, 0)
tm4.feed(MOD.EV_ABS, 0x39, 12)
tm4.feed(MOD.EV_ABS, 0x2F, 0)
tm4.feed(MOD.EV_ABS, 0x35, 130)
tm4.feed(MOD.EV_ABS, 0x36, 100)
tm4.feed(MOD.EV_SYN, 0, 0)
tm4.commit()      # 2026-11-05: 一批事件处理完才定位一次 (见 commit)
check("紧接着的触摸照样重锚 (误差不跨手势)", h4.sent and
      all(r in ((-127, 0), (0, -127), (0, 0)) for r in h4.sent[:N_ANCHOR])
      and sum(1 for r in h4.sent[:N_ANCHOR + 2] if r in ((-127, 0), (0, -127))) == N_ANCHOR,
      "锚定 %d 条 / 共 %d 条"
      % (sum(1 for r in h4.sent[:N_ANCHOR + 2] if r in ((-127, 0), (0, -127))), len(h4.sent)))

# ⑪ 控制管道直通路径 (tap / at 用的 goto) 空闲超时也必须重锚
#    (2026-11-05 真机 bug: 空闲 ~40 s 后 tap(200,319), 球已吸附左边缘而记录仍停在旧点,
#     于是只把球从 x=25 推到 x=0 ⇒ 点击落空; 现场截图 tap-bt.png 就是这一次)
h5 = FakeHid()
ap5 = MOD.AbsPointer(h5)
ap5.x, ap5.y = 249.0, 351.0
ap5.last_go = _t.time() - (MOD.ABS_ANCHOR_IDLE + 1)
h5.sent = []
ap5.goto(200, 319)
n5 = N_ANCHOR
check("goto 空闲超时后先重锚", all(r in ((-127, 0), (0, -127)) for r in h5.sent[:n5]),
      "前 %d 条是锚定报告" % min(n5, len(h5.sent)))
moves5 = [r for r in h5.sent if r not in ((-127, 0), (0, -127))]
sx, sy = planned(ap5.steps, moves5)
check("重锚后落点 = 目标 (200,319)", abs(sx - 200) <= 8 and abs(sy - 319) <= 8,
      "计划 (%.0f,%.0f) px" % (sx, sy))

h5.sent = []
ap5.goto(210, 330)
check("紧接着的 goto 不重锚", bool(h5.sent) and
      all(r not in ((-127, 0), (0, -127)) for r in h5.sent),
      "%d 条增量报告" % len(h5.sent))

# ⑫ 慢速拖动 (位移小于最小档): 一条报告都不发, 而且**记录不能前进**
#    (2026-11-05 真机: 以前 goto 无条件把位置设成目标 ⇒ 慢滑时记录跑在真实前面,
#     越滑越偏、松手位置乱 —— 这就是"滑动不稳定"的直接原因)
h6 = FakeHid()
ap6 = MOD.AbsPointer(h6)
ap6.x = ap6.y = 200.0
ap6.last_go = _t.time()
h6.sent = []
ap6.goto(205, 203)                     # 5 px / 3 px, 都小于 iOS 能动的 ~8 px
check("小位移一条报告都不发", h6.sent == [], "%d 条" % len(h6.sent))
check("小位移不改记录位置 (残差留给下一帧)",
      abs(ap6.x - 200) < 0.01 and abs(ap6.y - 200) < 0.01,
      "(%.0f,%.0f)" % (ap6.x, ap6.y))
h6.sent = []
ap6.goto(212, 200)                     # 残差累积到 12 px ⇒ 发最小档
sx, sy = planned(ap6.steps, h6.sent)
check("残差累积后发出最小档", bool(h6.sent) and abs(ap6.x - (200 + sx)) < 0.01,
      "%d 条, 记录 x=%.0f (发出 %.0f)" % (len(h6.sent), ap6.x, sx))

# ⑬ 发送失败要退避 (手机没连时不许每帧都撞 + 不许每帧都重锚)
class FailHid(FakeHid):
    def mouse(self, buttons=0, dx=0, dy=0, wheel=0):
        self.sent.append((int(dx), int(dy)))
        return False


h7 = FailHid()
ap7 = MOD.AbsPointer(h7)
ap7.x = ap7.y = 100.0
ap7.last_go = _t.time()
ap7.goto(300, 300)
n_first = len(h7.sent)
h7.sent = []
ap7.goto(300, 300)                     # 退避窗口内: 直接失败, 一条都不发
check("失败后进入退避 (不再撞通道)", n_first > 0 and h7.sent == [],
      "首次 %d 条, 退避期内 %d 条" % (n_first, len(h7.sent)))
check("失败后位置作废 (下次重锚)", ap7.x is None and ap7.y is None,
      "位置=%s" % ("未知" if ap7.x is None else "(%.0f,%.0f)" % (ap7.x, ap7.y)))

# ⑭ 拖动 (按住) 时报告必须带左键 —— 否则 iOS 当成"松开后再移动", 界面不跟手
h8 = FakeHid()
ap8 = MOD.AbsPointer(h8)
ap8.x = ap8.y = 0.0
ap8.last_go = _t.time()
h8.sent, h8.btns = [], []
ap8.goto(100, 100)
check("未按键时报告 buttons=0", bool(h8.btns) and all(b == 0 for b in h8.btns),
      "%d 条" % len(h8.btns))
h8.buttons = 1
h8.sent, h8.btns = [], []
ap8.goto(200, 150)
check("按住时报告 buttons=1 (真拖动)", bool(h8.btns) and all(b == 1 for b in h8.btns),
      "%d 条全带左键" % len(h8.btns))

tm5 = MOD.TouchMouse(FakeHid(), mode="abs")
tm5.buttons = 1
check("TouchMouse.buttons 与 HidDevice 同一份",
      tm5.hid.buttons == 1 and tm5.buttons == 1, "hid.buttons=%d" % tm5.hid.buttons)

# ⑮ 一批事件里连来多帧: 只按**最后**位置定位一次 (2026-11-05 卡顿根因的护栏)
#    以前每条事件都 goto (每条最坏 60 ms) ⇒ 主循环追不上手指, 事件堆积后成批爆出。
tm6 = MOD.TouchMouse(FakeHid(), mode="abs")
tm6.feed(MOD.EV_ABS, 0x2F, 0)
tm6.feed(MOD.EV_ABS, 0x39, 1)
tm6.feed(MOD.EV_ABS, 0x35, 100)
tm6.feed(MOD.EV_ABS, 0x36, 100)
tm6.feed(MOD.EV_SYN, 0, 0)
tm6.commit()
n_first = len(tm6.hid.sent)
for yy in (200, 300, 400):
    tm6.feed(MOD.EV_ABS, 0x35, 100)
    tm6.feed(MOD.EV_ABS, 0x36, yy)
    tm6.feed(MOD.EV_SYN, 0, 0)
    check("批内第 %d 帧不立即发报告" % yy, len(tm6.hid.sent) == n_first,
          "已发 %d 条" % len(tm6.hid.sent))
tm6.commit()
# 残差 (步长表最小档 ~10 px) 故意留到下一帧, 所以只许「差一点」不许「超前」
check("一批结束只定位一次 (目标 = 最后一帧)",
      -12 <= (100 + 300 * 720 / 800.0) - tm6.ap.y <= 12,
      "位置 y=%.0f 目标 %.0f, 本批发出 %d 条" % (tm6.ap.y, 100 + 300 * 720 / 800.0,
                                              len(tm6.hid.sent) - n_first))

# ⑯ commit 的顺序必须是「先定位, 后按左键」
#    反过来会在旧位置按下 (空闲时指针停在内容区左上角 ⇒ 从左上角往下拉 = 拉出通知中心)
tm7 = MOD.TouchMouse(FakeHid(), mode="abs")
tm7.feed(MOD.EV_ABS, 0x2F, 0)
tm7.feed(MOD.EV_ABS, 0x39, 1)
tm7.feed(MOD.EV_ABS, 0x35, 240)
tm7.feed(MOD.EV_ABS, 0x36, 400)
tm7.feed(MOD.EV_SYN, 0, 0)
tm7.commit()
# 落下那一批: 先发出一串定位报告 (buttons=0), 最后才是「按下」(0,0, buttons=1)
seq_pairs = list(zip(tm7.hid.sent, tm7.hid.btns))
press_at = next((i for i, (_d, b) in enumerate(seq_pairs) if b), None)
check("落下即按 (真鼠标语义)", press_at is not None, "第 %s 条按下" % press_at)
check("按之前先定位 (报告都在 buttons=0 的状态下发)",
      press_at is not None and all(b == 0 for _d, b in seq_pairs[:press_at])
      and seq_pairs[press_at][0] == (0, 0),
      "前 %s 条均为定位, 第 %s 条按下" % (press_at, press_at))
# 移动时必须带按键 (否则 iOS 当成「松开后移动」)
tm7.hid.sent, tm7.hid.btns = [], []
tm7.feed(MOD.EV_ABS, 0x35, 240)
tm7.feed(MOD.EV_ABS, 0x36, 300)                  # 拖动 100 px
tm7.feed(MOD.EV_SYN, 0, 0)
tm7.commit()
check("拖动中的定位报告都带左键", bool(tm7.hid.btns) and all(b == 1 for b in tm7.hid.btns),
      "%d 条" % len(tm7.hid.btns))
# 抬起必须松开
tm7.feed(MOD.EV_ABS, 0x39, -1)
tm7.commit()
check("抬起即松开左键", tm7.hid.btns and tm7.hid.btns[-1] == 0
      and tm7.hid.sent[-1] == (0, 0), "%d 条, 最后 buttons=%s"
      % (len(tm7.hid.btns), tm7.hid.btns[-1] if tm7.hid.btns else "-"))

print("\nPASS: %d/%d" % (ok, ok + fail))
sys.exit(0 if fail == 0 else 1)
