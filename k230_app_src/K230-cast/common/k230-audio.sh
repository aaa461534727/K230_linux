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

# 音量 (2026-10-29 主人要求「手机整体音量≤10%, 办公室很小声」):
#   ★静音的真正来源是手机端 —— Miracast 投屏时 STREAM_MUSIC 走 proxy 设备,
#    投屏音量(遥控器/侧键)落在 proxy 上, 实测被压到 1/15 (≈6.7%, 即 ≤10%)。
#    K230 抓到的就是已经被手机压过的 PCM, 板上再压就是「双重衰减」→ 几乎听不见。
#   ⇒ 本机只做 **透传 (增益 100%)**, 不额外衰减; 安静上限完全由手机侧 10% 卡死。
#   AUDIO_GAIN  软件增益百分比 (pcmplay -g), 默认 100 (透传; 真要更吵就 +g 即可)
#   注: 板上无 amixer (inno codec mixer 调不了), 硬件音量那段是死代码, 不生效。
HW_VOL="${AUDIO_HW_VOL:-100}"
GAIN="${AUDIO_GAIN:-100}"
if command -v amixer >/dev/null 2>&1; then
    _ctl="$(amixer scontrols 2>/dev/null | head -1 | sed "s/.*'\\(.*\\)'.*/\\1/")"
    if [ -n "$_ctl" ]; then amixer -q sset "$_ctl" "${HW_VOL}%" 2>/dev/null || true; fi
    log "音量: 硬件 ${HW_VOL}% / 软件增益 ${GAIN}%(透传; 静音由手机侧 10% 把关)"
fi

log "接管音频: $FIFO -> $DEV ${RATE}Hz ${CH}ch"
exec /usr/bin/pcmplay -f "$FIFO" -d "$DEV" -r "$RATE" -c "$CH" -g "$GAIN"
