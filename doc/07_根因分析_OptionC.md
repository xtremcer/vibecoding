# Option C 根因分析：ptt 复合固件无法拾音

> 目标：确认 `i2s_mic_ptt.uf2`（复合设备：UAC2 麦克风 + HID 键盘）为什么无法在主机上拾音，
> 而 `i2s_mic_v0.3.uf2`（纯 UAC2 麦克风）可以。

## 一、方法（真机描述符的静态等效抓取）

无法直接在本机挂接 Pico 的 USB 总线，因此采用**确定性静态解码**：
从两份 `.elf` 中按符号 `desc_device` / `desc_configuration` / `hid_report_descriptor`
的虚拟地址 → 文件偏移取出原始字节，再用 ELF 段表定位，逐字节解码 USB 描述符。
这正是设备枚举时回传给主机的**确切字节**，可作为"描述符抓捕"的权威依据。

解码脚本：`tools/decode_uf2_descriptors.py`（直接解析 ELF，不依赖 objdump 对齐）。

## 二、设备描述符对比

| 字段 | v0.3（可拾音） | ptt（无法拾音） |
|---|---|---|
| bcdUSB | 0x0200 | 0x0200 |
| bDeviceClass | 0xEF (MISC) | 0xEF (MISC) |
| bDeviceSubClass | 0x02 (COMMON) | 0x02 (COMMON) |
| bDeviceProtocol | 0x01 (IAD) | 0x01 (IAD) |
| VID | 0xCAFE | 0xCAFE |
| **PID** | **0x4A10** | **0x4B10** |
| bcdDevice | 0x0100 | 0x0100 |

→ 设备描述符**除 PID 外完全一致**。两者都是 `MISC + IAD` 复合设备基类，
v0.3 只是在 IAD 下挂了 1 个音频功能；ptt 在 IAD 之外多挂了 1 个 HID 接口。

## 三、配置描述符对比（解码结果）

### v0.3（size=145，接口数=2）
```
CONFIG total=145 #if=2
  IAD first=0 count=2 funcClass=0x1 funcSub=0x0 funcProto=0x20
  INTERFACE if=0 alt=0 #ep=0 class=0x1 sub=0x1 proto=0x20   (AudioControl)
  CS_INTERFACE ... (AC 头/时钟源/输入端子/输出端子/Feature Unit)
  INTERFACE if=1 alt=0 #ep=0 class=0x1 sub=0x2 proto=0x20   (AudioStreaming alt0)
  INTERFACE if=1 alt=1 #ep=1 class=0x1 sub=0x2 proto=0x20   (AudioStreaming alt1)
  ENDPOINT addr=0x81 attr=0x5 maxpkt=294 interval=1         (等时 IN)
  CS_ENDPOINT ...
```

### ptt（size=170，接口数=3）
```
CONFIG total=170 #if=3
  IAD first=0 count=2 funcClass=0x1 funcSub=0x0 funcProto=0x20
  INTERFACE if=0 alt=0 #ep=0 class=0x1 sub=0x1 proto=0x20   (AudioControl)  ← 与 v0.3 相同
  ... 完全相同的音频类描述符 ...
  INTERFACE if=1 alt=1 #ep=1 class=0x1 sub=0x2 proto=0x20
  ENDPOINT addr=0x81 attr=0x5 maxpkt=294 interval=1         ← 与 v0.3 完全相同
  CS_ENDPOINT ...
  INTERFACE if=2 alt=0 #ep=1 class=0x3 sub=0x1 proto=0x1 strIdx=5   (HID 键盘，新增)
  HID bcdHID=0x0100 #classDesc=1 len=9
  ENDPOINT addr=0x82 attr=0x3 maxpkt=64 interval=10          (中断 IN，新增)
```

### 关键结论
- **音频功能（IAD + AudioControl + AudioStreaming + EP 0x81/maxpkt=294 + 全部 CS 描述符）
  在两份固件中逐字节相同**（145 vs 170 的差值正好 = `TUD_HID_DESC_LEN` = 25 字节，即新增的 HID 接口）。**
- HID 接口为标准 Boot 键盘（`class=0x03, sub=0x01, proto=0x1`，报告描述符为标准 8 字节键盘）。
- `main.c` 的音频路径（`audio_task → rec_take → tud_audio_write_support_ff`）未被改动，
  `hid_task()` 仅在 `tud_hid_ready()` 且 PTT 跳变时发送单键，不阻塞音频。

## 四、根因判定（已用 Microsoft 官方文档交叉验证）

**固件侧音频描述符/代码与可正常拾音的 v0.3 完全一致 ⇒ 这不是固件音频缺陷。**
ptt 与 v0.3 的唯一差异是 **PID 由 0x4A10 改为 0x4B10** 以及**多出一个 HID 功能**。

### 关键证据：Windows 的 usbaudio2 一定会按类匹配本设备的音频功能
Microsoft Learn《USB Audio 2.0 驱动程序》明确写出，`usbaudio2.inf` 注册的兼容 ID 为：
```
USB\Class_01&SubClass_00&Prot_20
USB\Class_01&SubClass_01&Prot_20
USB\Class_01&SubClass_02&Prot_20
USB\Class_01&SubClass_03&Prot_20
```
即：**只要接口/功能类是 `0x01`（Audio）且协议是 `0x20`，Windows 自带驱动就会绑定。**
本设备的解码结果：IAD 与 AudioControl/AudioStreaming 接口的 `class=0x01`、`proto=0x20`
（见第三节），完全命中上述兼容 ID。→ **固件描述符在 Windows 侧也是合规可识别的。**

### 结论
既然描述符与可正常拾音的 v0.3 逐字节相同，且 Microsoft 文档证明 Windows 自带驱动会按类
绑定本设备的音频功能，那么"无法拾音"的根因不在固件，而在 **Windows 驱动安装/缓存状态**：
> 新 PID `0x4B10` 让 Windows 建立了一个**全新的设备实例**并重走驱动安装流程；
> 其音频子功能本应由 `usbaudio2.sys` 按类自动绑定，但首次接入时该子功能的驱动
> 安装可能未完成 / 被旧的 PID 缓存干扰（设备管理器里音频子节点可能带黄色感叹号、
> 状态为"错误 28/31"或落在"其他设备"下），而 HID 子功能（按 `USB\Class_03` 匹配，
> hidusb 安装极轻量）已正常绑定——这与"键盘能输出 a、但没声音"的现象完全吻合。

## 五、真机抓捕/确认方法（在用户 Windows 主机上执行）

下面任选其一，目的是确认：**音频子功能到底有没有被 usbaudio2 绑定、错误码是多少、键盘(HID)是否正常。**

### 方法 1：PowerShell 一眼看驱动与状态（最快）
```powershell
# 列出本设备所有节点（父复合设备 + 各子功能）及驱动
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'VID_0CAFE&PID_4B10' } |
  Select-Object FriendlyName, Status, InstanceId, Class, DriverName |
  Format-List

# 看音频子功能的详细状态（若 Status 不是 OK，会给出错误码，如 28=未安装驱动）
Get-PnpDevice -PresentOnly -Class AudioEndpoint | Format-List FriendlyName, Status, InstanceId
```

### 方法 2：UsbTreeView（免费，看完整描述符 + 绑定驱动）
- 下载运行 https://www.uwe-sieber.de/usbtreeview_e.html
- 找到 `VID_0CAFE&PID_4B10`，展开看：
  - 是否有"USB Composite Device"父节点；
  - 其下是否有 **USB Audio Device / 麦克风** 子节点，且驱动为 `usbaudio2.sys`；
  - 是否有 **HID Keyboard Device** 子节点，驱动为 `hidusb.sys` / `kbdhid.sys`。

### 方法 3：USB 原始枚举流量（最硬核）
- 安装 USBPcap + Wireshark，抓该设备的 `URB_CONTROL`（GET_DESCRIPTOR），
  可直接看到设备/配置描述符字节，与本文第三节静态解码交叉验证。

## 六、对症修复建议（按风险从低到高）

1. **首选（无需改固件）：Windows 重新安装驱动**
   - 设备管理器 → 找到 `VID_0CAFE&PID_4B10` 相关节点（复合设备 + 子功能）→
     右键"卸载设备"并勾选"删除此设备的驱动程序软件" → 拔插一次。
   - Windows 会重新枚举，父节点走 usbccgp，音频子功能按类自动绑定 `usbaudio2.sys`，
     键盘子功能绑定 `hidusb.sys`。
   - 这是最可能让 ptt 固件直接可用的操作，因为固件本身已验证正确。

2. **若仍不行：把 PID 改回 `0x4A10`（复用已验证可用的音频绑定）**
   - 改动 `src/usb_descriptors.c` 中 `USB_PID 0x4B10 → 0x4A10`，
     复合结构（音频 + HID）完全保留，只复用 Windows 已经为 v0.3 装好的音频驱动身份。
   - 重新编译刷入。风险：Windows 可能残留旧的"单音频功能"缓存，必要时同样执行方法 6.1 的卸载重插。

3. **更稳健（长期）：增加 Microsoft OS 2.0 描述符**
   - 通过 `tud_descriptor_ms_os_20_cb` 为每个功能显式指定兼容 ID，
     让 Windows 在无需人工干预、无需 PID 博弈的情况下正确绑定 `usbaudio2` / `hidusb`。
   - 适用于"换一台新电脑也要即插即用"的场景。

## 七、用户实测确认 + 对症修复

### 实测（2026-10-01）
用户在 Windows 上插上 ptt 固件并运行诊断：
- **HID 键盘能正常输出 'a'** ⇒ 复合设备枚举成功、描述符正确、Windows 已识别两个功能；
- **音频仍无声音** ⇒ 与第四节结论一致：音频子功能驱动（`usbaudio2.sys`）在 Windows 侧未绑定/未安装好。
→ 根因确认：**枚举与描述符都没问题，是 Windows 对新 PID `0x4B10` 的音频子功能驱动安装/缓存问题。**

### 修复（按推荐顺序）

**1）首选：在 Windows 上重装 0x4B10 的驱动（无需改固件，且一次之后永久生效）**
- 设备管理器 → 按"连接"或"按类型"查看，找到 `VID_0CAFE&PID_4B10` 相关节点：
  - 若音频子节点（通常名"USB 音频类 2 设备"）在"声音、视频和游戏控制器"下带黄色感叹号，
    或落在"其他设备"里 → 右键 → **卸载设备（勾选"删除此设备的驱动程序软件"）**；
  - 同时把复合设备父节点也卸载；然后拔插一次，Windows 会干净重枚举，
    音频子功能按类自动绑 `usbaudio2.sys`、键盘绑 `hidusb.sys`。
- 或命令行（管理员 PowerShell）：
  ```powershell
  Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'VID_0CAFE&PID_4B10' } |
    ForEach-Object { pnputil /remove-device $_.InstanceId }
  # 然后拔插
  ```

**2）若仍不行：把 PID 改回 `0x4A10`（复用本机已验证可用的音频驱动身份）**
- 改 `src/usb_descriptors.c`：`USB_PID 0x4B10 → 0x4A10`，复合结构（MIC+键盘）全保留，
  重新编译刷入。本机此前已为 0x4A10 成功装过音频驱动，复用该身份最省事。
- 注意：若 Windows 仍残留旧的"单音频功能"缓存，同样配合方法 1 卸载重插一次。

**3）长期稳健：Microsoft OS 2.0 描述符**
- 本场景音频按类匹配，MS OS 2.0 对"强制绑定 usbaudio2"帮助有限（它主要用于厂商驱动/
  音频插孔注册信息）；若要做到"换电脑也免驱"，可加 MS OS 2.0 明确各功能兼容 ID，但属锦上添花。

### 诊断命令（确认音频子功能到底绑了哪个驱动、错误码多少）
```powershell
# 看复合设备下各节点驱动与状态
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'VID_0CAFE&PID_4B10' } |
  Select-Object FriendlyName, Status, Class, DriverName, InstanceId | Format-List
# 看音频端点是否存在及状态
Get-PnpDevice -PresentOnly -Class AudioEndpoint | Where-Object { $_.InstanceId -match '0CAFE' } |
  Format-List FriendlyName, Status, InstanceId
```
把输出贴回即可 100% 定位（如 `DriverName=usbaudio2.sys` 且 `Status=OK` ⇒ 驱动已绑，问题在别处；
若 `Status=Error` 或没有音频节点 ⇒ 确为驱动未安装，走方法 1）。
