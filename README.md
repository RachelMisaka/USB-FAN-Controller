# USBFAN — STM32F103 USB CDC 2-Channel Fan Controller

A tiny, open USB fan controller on a **STM32F103C8T6 Blue Pill**. It enumerates as a
**virtual COM port** and drives **2 PWM fans** with **2 tachometer (RPM) inputs**,
controlled by [FanControl](https://github.com/Rem0o/FanControl.Releases) through a
bundled plugin — or by anything that can speak a few ASCII lines over a serial port.

- MCU: STM32F103C8T6 (Blue Pill, LQFP48, 72 MHz, 64 KB flash / 20 KB RAM)
- USB: CDC (Virtual COM Port), VID `0x0483` / PID `0x5740` — no vendor driver needed
- 2× PWM fan channels @ 25 kHz, 2× tachometer inputs, 1 status LED
- ~60% flash / 35% RAM usage, plenty of headroom

```
中文说明见文末 / 中文説明見文末  (Chinese README at the bottom)
```

## Features

- **Dual-channel PWM + tach**: ch0/ch1 each get an independent 0–100 % duty PWM output
  and a real-time RPM reading from the fan's tach line.
- **Safe boot default**: fans start at **50 % duty** until a host sends a command.
- **USB re-enumeration on reset**: the Blue Pill's D+ pull-up is hard-wired, so the
  firmware drives a short SE0 pulse at boot to make Windows re-enumerate after a
  flash/reset without unplugging the cable.
- **Crosstalk-immune tach**: a 2 ms debounce plus whole-burst suppression rejects
  25 kHz PWM crosstalk that would otherwise read as a phantom ~15000 RPM.
- **FanControl plugin with presence detection**: when the device is unplugged, the
  plugin shows no fans; plugging it back in makes them reappear automatically.

## Hardware / wiring

| Function | ch0 | ch1 |
|---|---|---|
| PWM output (25 kHz, push-pull, 0–3.3 V) | **PB7** (TIM4_CH2) | **PB8** (TIM4_CH3) |
| Tach input (falling edge, internal pull-up) | **PA1** (EXTI1) | **PB14** (EXTI14) |
| Status LED (active-low) | **PC13** (onboard Blue Pill LED) | |

> Tach wiring gotcha: keep the tach signal ground short and local to the board GND.
> Routing it through the mains PE (e.g. a PSU earth) creates a ground loop that
> couples the 25 kHz PWM into the tach line.

## Serial protocol

Line-based ASCII, `\n` terminated (CR ignored):

| Direction | Format | Example |
|---|---|---|
| Host → device | `D<ch>:<val>\n` (ch = 0..1, val = 0..100) | `D0:75\n` |
| Device → host (≈1 Hz) | `R<ch>:<rpm>\n` | `R0:1250\n` |

## Build

Toolchain: CMake + Ninja + `arm-none-eabi-gcc` (from STM32CubeCLT, all on PATH).

```bash
cmake --preset Debug
cmake --build --preset Debug      # -> build/Debug/USBFAN.elf
```

## Flash

ST-Link via SWD:

```bash
STM32_Programmer_CLI.exe -c port=SWD -w build\Debug\USBFAN.elf -v -rst
```

The board re-enumerates on its own after flashing — no USB replug needed.

## FanControl plugin

`tools/FanControl.Stm32FanCdc/` — a `net10.0` plugin (matches FanControl's runtime)
implementing `IPlugin3` (presence detection via `RefreshRequested`). It finds the
COM port by VID/PID (`0x0483/0x5740`) through the registry and requires no NuGet
packages (references FanControl's own `FanControl.Plugins.dll` and
`System.IO.Ports.dll`).

```bash
dotnet build tools\FanControl.Stm32FanCdc\FanControl.Stm32FanCdc.csproj -c Release
# copy to the FanControl plugins folder (needs admin):
#   bin\Release\net10.0\FanControl.Stm32FanCdc.dll  ->  C:\Program Files (x86)\FanControl\Plugins\
```

## Project structure

```
Core/            firmware (main.c, fan_hw.c, HAL init)
USB_DEVICE/      CubeMX USB CDC glue (usbd_cdc_if.c holds the D/R parser)
Drivers/         STM32F1 HAL + CMSIS
Middlewares/     ST USB Device Library (CDC)
tools/FanControl.Stm32FanCdc/   FanControl plugin source
USBFAN.ioc       STM32CubeMX project (regenerate-safe; SYS = Serial Wire keeps SWD)
cmake/           CMake presets + CubeMX CMake
```

---

# 中文说明 (Chinese)

## USBFAN — STM32F103 USB CDC 双通道风扇控制器

基于 **STM32F103C8T6 Blue Pill** 的开源 USB 风扇控制器：以**虚拟串口**枚举，
驱动 **2 路 PWM 风扇** + **2 路测速（RPM）输入**，通过附带的插件由
[FanControl](https://github.com/Rem0o/FanControl.Releases) 控制，或任何能用串口发几行
ASCII 的程序直接控制。

- USB CDC 虚拟串口，VID `0x0483` / PID `0x5740`，免驱动
- 2 路 25 kHz PWM + 2 路 tach + 1 个状态 LED
- Flash 占用 ~60% / RAM ~35%

### 特性

- **双通道 PWM + tach**：每路独立 0–100% 占空比输出 + 实时转速读数
- **安全默认 50%**：开机即 50% 占空，直到主机发指令
- **复位自动重枚举**：Blue Pill 的 D+ 上拉硬接死，固件开机拉低 DP/DM 50ms
  制造断开→重连，烧录/复位后**不用拔 USB**
- **tach 抗串扰**：2ms 消抖 + 突发整体抑制，滤掉 25 kHz PWM 串扰（否则会误读成 ~15000 RPM）
- **插件在位检测**：拔掉设备风扇即消失，插回自动出现

### 引脚

| 功能 | ch0 | ch1 |
|---|---|---|
| PWM 输出（25 kHz，推挽，0–3.3V） | **PB7**（TIM4_CH2） | **PB8**（TIM4_CH3） |
| Tach 输入（下降沿，内部上拉） | **PA1**（EXTI1） | **PB14**（EXTI14） |
| 状态 LED（低电平点亮） | **PC13**（板载） | |

> 地线注意：tach 信号地要就近接板子 GND。走电网 PE 会形成地环路，把 25 kHz PWM 耦合进 tach。

### 串口协议

ASCII 行，`\n` 结尾（忽略 CR）：

| 方向 | 格式 | 示例 |
|---|---|---|
| 主机 → 设备 | `D<ch>:<val>\n`（ch=0..1，val=0..100） | `D0:75\n` |
| 设备 → 主机（约 1Hz） | `R<ch>:<rpm>\n` | `R0:1250\n` |

### 构建 / 烧录

```bash
cmake --preset Debug
cmake --build --preset Debug        # -> build/Debug/USBFAN.elf
STM32_Programmer_CLI.exe -c port=SWD -w build\Debug\USBFAN.elf -v -rst
```

### FanControl 插件

`tools/FanControl.Stm32FanCdc/`，net10.0，实现 `IPlugin3`（在位检测），按 VID/PID
从注册表找 COM 口，无 NuGet 依赖。

```bash
dotnet build tools\FanControl.Stm32FanCdc\FanControl.Stm32FanCdc.csproj -c Release
# 把 bin\Release\net10.0\FanControl.Stm32FanCdc.dll 拷到
#   C:\Program Files (x86)\FanControl\Plugins\   （需管理员）
```
