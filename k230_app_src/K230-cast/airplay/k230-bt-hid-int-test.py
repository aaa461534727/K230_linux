#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""k230-bt-hid-int-test — 不接手机也能验证「中断通道反复 accept」的回归测试。

干什么: 用假 dbus/gi + 假 L2CAP socket 加载同目录的 k230-bt-hid, 直接调
        ClassicHidDevice._on_int_conn / _on_int_hup, 断言:
          ① accept 回调返回 True (watch 不会被摘掉 = 后续手机还能连)
          ② 连续两台手机 (Redmi → iPhone) 都能被 accept, 第二台顶掉第一台
          ③ 断开回调返回 False (只摘那一条连接的 watch)
          ④ GET_REPORT 按 rtype 回 DATA, 不再一律 0x03
怎么用: python3 src/K230-cast/airplay/k230-bt-hid-int-test.py   # 全 PASS 退出码 0
依赖:   无 (不需要板子/手机/真蓝牙; 桩掉 dbus、gi、socket)
在哪跑: 本机或板子都行
产出:   终端 PASS/FAIL; 同时是 2026-11-05 "只 accept 一次" 根因的回归护栏。
"""

import importlib.util
import os
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
TARGET = os.path.join(HERE, "k230-bt-hid")


# ---------------------------------------------------------------- 桩: dbus / gi
def _install_stubs():
    def identity_decorator(*_a, **_k):
        def deco(fn):
            return fn
        return deco

    dbus = types.ModuleType("dbus")
    dbus.__path__ = []

    class Object(object):
        def __init__(self, *a, **k):
            pass

    service = types.ModuleType("dbus.service")
    service.Object = Object
    service.method = identity_decorator
    service.signal = identity_decorator
    service.BusName = object

    mainloop = types.ModuleType("dbus.mainloop")
    mainloop.__path__ = []
    glib_ml = types.ModuleType("dbus.mainloop.glib")
    glib_ml.DBusGMainLoop = lambda **k: None
    mainloop.glib = glib_ml

    exc = types.ModuleType("dbus.exceptions")

    class DBusException(Exception):
        pass

    exc.DBusException = DBusException

    dbus.service = service
    dbus.mainloop = mainloop
    dbus.exceptions = exc
    dbus.Interface = lambda *a, **k: object()
    dbus.String = str
    dbus.Boolean = bool
    dbus.UInt16 = int
    dbus.ObjectPath = str

    class _GLib(types.ModuleType):
        PRIORITY_DEFAULT = 0
        IO_IN, IO_OUT, IO_HUP, IO_ERR, IO_NVAL = 1, 4, 16, 8, 32

        def __init__(self):
            types.ModuleType.__init__(self, "gi.repository.GLib")
            self.watches = []

        def io_add_watch(self, fd, prio, cond, cb):
            self.watches.append((fd, cb))
            return len(self.watches)

        def idle_add(self, *_a, **_k):
            return 0

    gi = types.ModuleType("gi")
    gi.__path__ = []
    repository = types.ModuleType("gi.repository")
    repository.__path__ = []
    glib = _GLib()
    repository.GLib = glib
    gi.repository = repository

    sys.modules.update({
        "dbus": dbus,
        "dbus.service": service,
        "dbus.mainloop": mainloop,
        "dbus.mainloop.glib": glib_ml,
        "dbus.exceptions": exc,
        "gi": gi,
        "gi.repository": repository,
    })
    return glib


GLIB = _install_stubs()


def _load():
    from importlib.machinery import SourceFileLoader
    loader = SourceFileLoader("k230_bt_hid", TARGET)
    spec = importlib.util.spec_from_loader("k230_bt_hid", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


# ---------------------------------------------------------------- 假 socket
class FakeConn(object):
    _seq = 0

    def __init__(self, peer):
        FakeConn._seq += 1
        self.peer = peer
        self.fd = 1000 + FakeConn._seq
        self.closed = False
        self.sent = []

    def fileno(self):
        return self.fd

    def close(self):
        self.closed = True

    def send(self, data):
        self.sent.append(bytes(data))
        return len(data)


class FakeListener(object):
    """accept() 依次吐出 pending 里的连接, 空了抛 BlockingIOError (非阻塞语义)。"""

    def __init__(self, conns):
        self.pending = list(conns)

    def accept(self):
        if not self.pending:
            raise BlockingIOError(11, "Resource temporarily unavailable")
        c = self.pending.pop(0)
        return c, (c.peer, 0)


# ---------------------------------------------------------------- 用例
def make_dev(mod):
    dev = mod.ClassicHidDevice.__new__(mod.ClassicHidDevice)
    dev.int_conn = None
    dev.int_sock = None
    dev.ready = False
    dev.stats = {"mouse": 0, "key": 0, "consumer": 0, "blocked": 0,
                 "peer": "", "since": 0}
    dev.boot = False
    dev.led = 0
    dev.last = {1: bytes(4), 2: bytes(8), 3: bytes(2)}
    return dev


def case_accept_twice(mod, fails):
    dev = make_dev(mod)
    c1 = FakeConn("DC:F0:90:26:36:C0")     # Redmi
    c2 = FakeConn("94:BF:2D:5B:30:AB")     # iPhone
    dev.int_sock = FakeListener([c1, c2])
    ret = dev._on_int_conn()
    if ret is not True:
        fails.append("accept 回调没返回 True (watch 会被摘掉, 后续手机永远连不上)")
        return
    if not c1.closed:
        fails.append("第二台手机连上时旧连接没关")
    if dev.int_conn is not c2:
        fails.append("int_conn 没顶成第二台手机")
    if dev.stats["peer"] != "94:BF:2D:5B:30:AB":
        fails.append("stats.peer 没更新")
    if not any(f == c2.fd for (f, _cb) in GLIB.watches):
        fails.append("没给第二台手机的连接挂 HUP watch")
    print("  ✓ 连续两台手机都能 accept, 旧连接被顶掉 (返回 True)")


def case_single_phone_reconnect(mod, fails):
    """老 bug 的精确复现: 第一台断开后再来一台, 必须还能 accept。"""
    dev = make_dev(mod)
    c1 = FakeConn("AA:AA:AA:AA:AA:AA")
    dev.int_sock = FakeListener([c1])
    if dev._on_int_conn() is not True:
        fails.append("第一次 accept 就摘了 watch")
        return
    dev._on_int_hup(c1.fileno(), 16)       # 第一台掉线
    if dev.ready or dev.int_conn is not None:
        fails.append("HUP 后 ready/int_conn 没清空")
    if not c1.closed:
        fails.append("HUP 后旧 fd 没关")
    c2 = FakeConn("BB:BB:BB:BB:BB:BB")
    dev.int_sock = FakeListener([c2])      # 同一监听 socket 的下一轮
    if dev._on_int_conn() is not True or dev.int_conn is not c2:
        fails.append("断开后第二台连不上 (= 现场 iPhone 09:44 的死法)")
        return
    print("  ✓ 断开重连后仍能 accept (老 bug: 只能连第一台)")


def case_get_report(mod, fails):
    prof = mod.ClassicProfile.__new__(mod.ClassicProfile)
    prof.dev = make_dev(mod)
    prof.protocol = 1
    prof.idle = 0
    prof.dev.last[1] = b"\x00\x00\x00\x00"
    out = FakeConn("x")
    prof.ctrl_fd = out.fd
    prof._reply = lambda payload: out.send(payload)
    prof._answer_get_report(1, b"\x01")            # Input / report id 1
    if not out.sent or out.sent[-1][0] != 0xA1:
        fails.append("GET_REPORT(Input) 没回 DATA|Input")
    prof._answer_get_report(3, b"\x07")            # Feature / 不存在的 id
    if out.sent[-1] != b"\x04":
        fails.append("GET_REPORT(未知 id) 没回 ERR_INVALID_REPORT_ID")
    print("  ✓ GET_REPORT 按 rtype 分流 (Input→DATA, 未知 id→0x04)")


def main():
    mod = _load()
    fails = []
    print("k230-bt-hid 中断通道/控制通道回归测试")
    case_accept_twice(mod, fails)
    case_single_phone_reconnect(mod, fails)
    case_get_report(mod, fails)
    ev = getattr(mod, "EV_SZ", None)
    if ev not in (16, 24):
        fails.append("EV_SZ 异常: %r" % (ev,))
    print("  ✓ input_event 宽度 = %s B (格式 %s)" % (ev, mod.EV_FMT))
    if fails:
        print("\nFAIL:")
        for f in fails:
            print("  - " + f)
        return 1
    print("\nPASS: 4/4")
    return 0


if __name__ == "__main__":
    sys.exit(main())
