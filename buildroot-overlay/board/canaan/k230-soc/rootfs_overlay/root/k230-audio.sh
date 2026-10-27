#!/bin/sh
# k230-audio —— 音频所有者: 常驻 pcmplay 独占总线的 ALSA 设备
#
# 两条投屏链路 (Miracast / AirPlay) 都只往 /run/wfd-audio.pcm 写 S16LE 48k 2ch,
# 由本进程出声。这样: 换源时设备不重开 (没有爆音), 谁在投谁有声音, 空闲时补静音。
set -u

FIFO="${WFD_AUDIO_FIFO:-/run/wfd-audio.pcm}"
DEV="${ALSA_DEV:-default}"
RATE="${ALSA_RATE:-48000}"
CH="${ALSA_CH:-2}"
LOG="${AUDIO_LOG:-/var/log/k230-audio.log}"

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" >>"$LOG"; }

[ -x /usr/bin/pcmplay ] || { log "✗ /usr/bin/pcmplay 不存在"; sleep 3600; exit 1; }

# 设备可能被 ALSA 的 pulse/其他进程占着: 先清掉可能挡路的旧实例
pkill -9 -x pcmplay 2>/dev/null || true
sleep 0.2
mkfifo "$FIFO" 2>/dev/null || true
chmod 666 "$FIFO" 2>/dev/null || true

# 音量: inno codec 的硬件音量先拉到 80% (播放链里没有软件音量了)
if command -v amixer >/dev/null 2>&1; then
    amixer -q sset "$(amixer scontrols 2>/dev/null | head -1 | sed "s/.*'\\(.*\\)'.*/\\1/")" 80% 2>/dev/null || true
fi

log "接管音频: $FIFO -> $DEV ${RATE}Hz ${CH}ch"
exec /usr/bin/pcmplay -f "$FIFO" -d "$DEV" -r "$RATE" -c "$CH"
