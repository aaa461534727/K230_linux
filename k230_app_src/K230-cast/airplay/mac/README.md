# Mac 端 AirPlay 连接自动化 (VNC 方案)

> 2026-10-29 归档。目的: **从 K230/Linux 侧全自动让苹果电脑把 AirPlay 连到 K230-CAST**,
> 全程无需人手点 Mac, 也不需要 macOS「辅助访问」权限。
> 主人原话: 「你之前使用 VNC 去点击的……苹果电脑上还有你的脚本, 你记住这个方案」。

## 为什么用 VNC, 而不是 osascript / CoreAudio

| 方案 | 结果 |
|---|---|
| AppleScript + System Events 点控制中心 | 不可用: 报 -1719 辅助访问。macOS 把 Accessibility 存在 SIP 保护的系统 TCC 库, 即便 sudo 也写不进 (readonly database) |
| CoreAudio 把 K230-CAST 设为默认输出 | 不可用: 未连接前 AirPlay 设备根本不进 CoreAudio 设备列表, 无对象可设 |
| 私有 AirPlaySender.framework | 不可用: 无 shell CLI, 私有 API 复杂 |
| VNC 远程桌面截图 + OCR + 注入点击 | 唯一可靠路径, 实测打通 |

关键: VNC 是在真实登录会话里发鼠标事件, 不经过 TCC 辅助访问授权 (ARD type-30 认证即代表用户本人操作), 因此不受辅助访问限制。

## 文件

| 文件 | 作用 |
|---|---|
| ardvnc.py | 纯 Python 的 Apple Remote Desktop VNC 客户端。RFB 3.8 + ARD 认证 (security type 30) 接管 Mac 真实会话, 支持抓屏 (Raw/CopyRect)、指针、键盘。连 127.0.0.1:5900 (Mac 本机 Screen Sharing / 远程管理)。账号密码见文件内默认值 |
| apconnect.py | 主入口: 连 VNC -> 扫菜单栏右侧逐点开控制中心 -> OCR 找 K230 字样 -> 点它 -> system_profiler SPDisplaysDataType 确认已连。分辨率自适应 (菜单栏图标 x 坐标不写死) |
| ocr.m | OCR 工具源码 (macOS Vision 框架, 中英识别)。不存二进制, 现场用 clang 重新编 |
| vncshot.py | 单独截图/OCR 调试小工具 |
| k230mac_cast.py / k230mac_recast.py | 早期固定坐标版本 (1280x720 时代), 保留作参考; 已由 apconnect.py 取代 |

## 在 Mac 上的准备 (一次性)

    # 1. 打开 系统设置 -> 通用 -> 共享 -> 屏幕共享/远程管理 (VNC 监听 5900)
    # 2. 编 OCR (需要 Xcode Command Line Tools 的 clang)
    clang -framework Foundation -framework AppKit -framework Vision -o /tmp/ocr /tmp/ocr.m
    # 3. 把 ardvnc.py / apconnect.py 放到 /tmp

## 用法 (在 Mac 上本地跑; 也可从 K230 经 ssh 远程拉起)

    python3 /tmp/apconnect.py
    # 输出: screen 1920x1080 ... click K230 at (x,y) ... OK connected after Ns

## 从 K230 侧一键连 (本次实际用的链路)

    # K230 与 Mac 同网段 (10.0.102.x); K230 装 sshpass 后可直连 Mac 拉起脚本
    sshpass -p <macpw> ssh justinfo@<mac_ip> "python3 /tmp/apconnect.py"

注意: apconnect.py 输出是行缓冲, 后台跑重定向到文件时看不到进度; 以板端 ss -tnp | grep 7000 和 /var/log/airplay.log 的 raop_rtp starting audio 为准。

## 验证是否连上 (板端)

    ss -tnp | grep -E ':700[0-2] '                 # 应看到 Mac 的 ESTAB
    tail -20 /var/log/airplay.log                  # raop_rtp_mirror starting mirroring / raop_rtp starting audio
