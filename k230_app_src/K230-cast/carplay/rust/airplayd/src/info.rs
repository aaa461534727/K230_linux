//! GET /info —— CarPlay 的"能力宣告"。iOS 在端点激活时逐键校验, 缺键或结构不对就 teardown。
//!
//! 本轮结构**逐键照抄**仓内设备验证过的 golden sample:
//! `third_party/carplayd/vendor/ncm_carplayd/receiver_core/crates/receiver/src/info.rs`
//! (那份是 C 版 `AirPlayCopyServerInfo` + `_Build{Display,HID,AudioFormats,AudioLatencies}InfoArray` 的忠实移植)。
//! 关键经验 (那份代码的注释原文): **缺 modes 时 iOS 把屏幕资源判成 type=N/A → StartupFailed -17483**,
//! 实测表现就是 `carEndpoint_setInitialMode -71960` → Endpoint_Failed (2026-11-02)。

use plist::{Dictionary, Value};

/// 显示与 HID 共用的 UUID (大写): iOS 靠它把触摸绑到这块屏, 两边**必须完全一致**。
const DISPLAY_UUID: &str = "E0CB6FB0-0000-0000-0000-0000C0FFEE01";

const HID_VENDOR_ID: i64 = 0x05AC; // Apple
const HID_COUNTRY_CODE: i64 = 0;

/// 触摸屏 HID 报告描述符: 固定模板, X/Y 逻辑上限在 offset 39/40 与 52/53 按屏尺寸改写 (小端)。
fn touchscreen_descriptor(width: u16, height: u16) -> Vec<u8> {
    // ⚠ 不要用 golden sample 里那份单指 0x09 0x33 模板: iPhone 16.7 实测报
    //   `AppleUserHIDEventService::Start:0xe00002bc` + `Invalid digitizer transducer` (2026-11-02)。
    // 下面这份 2 指 + Tip Switch(0x42)/In Range(0x32) 的才是真机上能让 iOS "digitizer attached" 的。
    let mut d: Vec<u8> = vec![0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01]; // Digitizer / TouchScreen / Collection(App)
    for finger in 0..2 {
        if finger > 0 {
            d.extend_from_slice(&[0x05, 0x0D]); // 第二指重新选 Digitizer 页
        }
        d.extend_from_slice(&[
            0x09, 0x22, // Usage (Finger)
            0xA1, 0x02, // Collection (Logical)
            0x09, 0x42, // Tip Switch
            0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
            0x09, 0x32, // In Range
            0x81, 0x02,
            0x95, 0x06, 0x81, 0x03, // 6 bit 填充
            0x05, 0x01, 0x09, 0x30, // Generic Desktop / X
            0x15, 0x00,
            0x26, (width & 0xFF) as u8, (width >> 8) as u8, // Logical Max = 屏宽
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,
            0x09, 0x31, // Y
            0x26, (height & 0xFF) as u8, (height >> 8) as u8,
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,
            0xC0,
        ]);
    }
    d.push(0xC0);
    d
}

/// 媒体按键 HID 描述符 (Consumer Control: Play/Pause/Next/Prev...), 固定。
fn media_buttons_descriptor() -> Vec<u8> {
    vec![
        0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, // Consumer / Consumer Control / Collection(App)
        0x15, 0x00, 0x25, 0x05, 0x05, 0x0C, // Logical Min 0 / Max 5 / Consumer
        0x0A, 0x00, 0x00, // Usage 0
        0x0A, 0xB0, 0x00, // Play
        0x0A, 0xB1, 0x00, // Pause
        0x0A, 0xCD, 0x00, // Play/Pause
        0x0A, 0xB5, 0x00, // Next
        0x0A, 0xB6, 0x00, // Prev
        0x81, 0x02, // Input (Data,Var,Abs)
        0xC0, // End
    ]
}

fn hid_device(uid: u32, name: &str, product_id: i64, descriptor: Vec<u8>) -> Value {
    let mut d = Dictionary::new();
    d.insert("uuid".into(), Value::String(format!("{uid:X}")));
    d.insert("name".into(), Value::String(name.into()));
    d.insert("displayUUID".into(), Value::String(DISPLAY_UUID.into()));
    d.insert("hidProductID".into(), Value::Integer(product_id.into()));
    d.insert("hidVendorID".into(), Value::Integer(HID_VENDOR_ID.into()));
    d.insert("hidCountryCode".into(), Value::Integer(HID_COUNTRY_CODE.into()));
    d.insert("hidDescriptor".into(), Value::Data(descriptor));
    Value::Dictionary(d)
}

/// 无线 CarPlay 的音频能力: PCM 48k/16k 兜底 (type 100/101, 不带 audioType = 通配)。
fn audio_formats() -> Vec<Value> {
    const PCM_16K_MONO: i64 = 0x10;
    const PCM_48K_STEREO: i64 = 0x8000;
    // (type, audioType, in, out); audioType=None 表示通配
    // iOS 按 audioType 逐类"重打包": 通配项会先被选中, 到 telephony 这一路就 6705 kParamErr
    // (实测: APCarPlayAudioFormatsGetFormatsForAudioType:567 false condition / type 100 / telephony)。
    // 所以**全部改成显式 audioType**, 不给通配。
    let entries: &[(i64, Option<&str>, i64, i64)] = &[
        (100, Some("media"), PCM_16K_MONO, PCM_48K_STEREO | PCM_16K_MONO),
        (101, Some("media"), PCM_16K_MONO, PCM_48K_STEREO),
        (100, Some("telephony"), PCM_16K_MONO, PCM_48K_STEREO | PCM_16K_MONO),
        (101, Some("telephony"), PCM_16K_MONO, PCM_48K_STEREO),
        (100, Some("speechRecognition"), PCM_16K_MONO, PCM_48K_STEREO | PCM_16K_MONO),
        (101, Some("speechRecognition"), PCM_16K_MONO, PCM_48K_STEREO),
        (100, Some("alert"), PCM_16K_MONO, PCM_48K_STEREO | PCM_16K_MONO),
    ];
    entries
        .iter()
        .map(|&(ty, at, inp, outp)| {
            let mut e = Dictionary::new();
            e.insert("type".into(), Value::Integer(ty.into()));
            if let Some(a) = at {
                e.insert("audioType".into(), Value::String(a.into()));
            }
            e.insert("audioInputFormats".into(), Value::Integer(inp.into()));
            e.insert("audioOutputFormats".into(), Value::Integer(outp.into()));
            Value::Dictionary(e)
        })
        .collect()
}

pub fn build(w: u32, h: u32, name: &str) -> Vec<u8> {
    let mut d = Dictionary::new();

    d.insert("audioFormats".into(), Value::Array(audio_formats()));

    // ★ modes —— 端点激活时 iOS 读它决定资源归属; 缺了就是 setInitialMode -71960 / StartupFailed -17483。
    // 声明本机 **Take** MainScreen(1) + MainAudio(2): transferType=1, priority/constraint=100。
    let mode_resource = |rid: i64| -> Value {
        let mut r = Dictionary::new();
        r.insert("resourceID".into(), Value::Integer(rid.into()));
        r.insert("transferType".into(), Value::Integer(1.into())); // Take
        r.insert("transferPriority".into(), Value::Integer(100.into())); // NiceToHave
        r.insert("takeConstraint".into(), Value::Integer(100.into())); // Anytime
        r.insert("borrowConstraint".into(), Value::Integer(100.into())); // Anytime
        Value::Dictionary(r)
    };
    let mut modes = Dictionary::new();
    modes.insert("resources".into(), Value::Array(vec![mode_resource(1), mode_resource(2)]));
    d.insert("modes".into(), Value::Dictionary(modes));

    // audioLatencies —— 一条通配 (省略 type/sr/ss/ch 即适用全部)
    let mut lat = Dictionary::new();
    lat.insert("inputLatencyMicros".into(), Value::Integer(0.into()));
    lat.insert("outputLatencyMicros".into(), Value::Integer(0.into()));
    d.insert("audioLatencies".into(), Value::Array(vec![Value::Dictionary(lat)]));

    d.insert("deviceID".into(), Value::String("04:7F:0E:0E:FB:E3".into()));

    let mut disp = Dictionary::new();
    disp.insert("uuid".into(), Value::String(DISPLAY_UUID.into()));
    disp.insert("features".into(), Value::Integer(0x0A.into())); // HighFidelityTouch | Knobs
    disp.insert("primaryInputDevice".into(), Value::Integer(0.into())); // index 0 = 触摸屏
    disp.insert("maxFPS".into(), Value::Integer(60.into()));
    disp.insert("widthPixels".into(), Value::Integer((w as i64).into()));
    disp.insert("heightPixels".into(), Value::Integer((h as i64).into()));
    disp.insert("widthPhysical".into(), Value::Integer(240.into()));
    disp.insert("heightPhysical".into(), Value::Integer(90.into()));
    disp.insert("type".into(), Value::Integer(110.into()));
    d.insert("displays".into(), Value::Array(vec![Value::Dictionary(disp)]));

    d.insert("extendedFeatures".into(), Value::Array(vec![
        Value::String("vocoderInfo".into()),
        Value::String("enhancedRequestCarUI".into()),
    ]));
    // 0x0000_0061_4444_0B80 = CCPA 车机原值: Audio(9)|RedundantAudio(11)|PCM(18)|Unencrypted(22)|
    // AES-128-MFi-SAPv1(26) + 高字 0x61 = Car(32)|CarPlayControl(37)|HKPairing(38)
    d.insert("features".into(), Value::Integer((0x0000_0061_4444_0B80u64 as i64).into()));
    d.insert("firmwareRevision".into(), Value::String("1.0".into()));

    d.insert("hidDevices".into(), Value::Array(vec![
        hid_device(1, "K230 Touchscreen", 0x0001, touchscreen_descriptor(w as u16, h as u16)),
        hid_device(2, "K230 Media Buttons", 0x0002, media_buttons_descriptor()),
    ]));

    d.insert("keepAliveLowPower".into(), Value::Boolean(true));
    d.insert("keepAliveSendStatsAsBody".into(), Value::Boolean(true));
    d.insert("manufacturer".into(), Value::String("K230".into()));
    d.insert("model".into(), Value::String("K230-CarPlay1,1".into()));
    d.insert("name".into(), Value::String(name.into()));
    d.insert("pi".into(), Value::String("04:7F:0E:0E:FB:E3".into()));
    d.insert("sourceVersion".into(), Value::String("366.0".into()));
    d.insert("statusFlags".into(), Value::Integer(4.into()));

    let mut out = Vec::new();
    plist::to_writer_binary(&mut out, &Value::Dictionary(d)).expect("bplist");
    out
}
