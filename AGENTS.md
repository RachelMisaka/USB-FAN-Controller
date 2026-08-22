# AGENTS.md

USB CDC (virtual COM port) 2-channel fan controller ("USBFAN") on STM32F103C8T6 (Blue Pill, LQFP48, 72 MHz, 64 KB flash / 20 KB RAM). Originally ported from the F407 project at `C:\Users\Rachel\Documents\Hello World STM32\HW`, but the F407 is retired: the HID protocol was dropped and the USB stack is now **native CubeMX CDC** (nothing copied from F4). Toolchain: **CMake + Ninja + arm-none-eabi-gcc** (STM32CubeCLT). No package manager, no test suite, no lint.

## Build / flash (verified)

```bash
cmake --build --preset Debug   # -> build/Debug/USBFAN.elf  (or: cmake --preset Debug first)
STM32_Programmer_CLI.exe -c port=SWD -w build\Debug\USBFAN.elf -v -rst
```

- ST-Link (clone, VID 0483/PID 3748) is reliable **now that SWD stays enabled**; the old "firmware kills SWD at boot" issue is fixed at the source (`.ioc` SYS = Serial Wire → `__HAL_AFIO_REMAP_SWJ_NOJTAG()`, SWD kept).
- After flash/reset the board **re-enumerates on its own** — no replug needed (`UsbForceReEnumerate` drives DP/DM low ~50 ms at boot; the Blue Pill D+ pull-up is hard-wired, so a plain reset alone would never drop "connected" and the host wouldn't re-enumerate).
- Root `CMakeLists.txt` adds only user sources (`Core/Src/fan_hw.c`). HAL tim sources come from the regenerated `cmake/stm32cubemx/CMakeLists.txt` (TIM4 is in the `.ioc`) — don't add them to the root list (duplicate object/link error).
- Linker `STM32F103xx_FLASH.ld`, startup `startup_stm32f103xb.s` at repo root, newlib stubs `Core/Src/syscalls.c`/`sysmem.c`. `EWARM/` is stale — ignore.

## USB / serial protocol (CDC)

- Enumerates as **VID 0x0483 / PID 0x5740**, "STM32 Virtual ComPort", e.g. COM4. Found by the plugin via registry `HKLM\...\Enum\USB\VID_0483&PID_5740\...\Device Parameters\PortName`.
- Line-based ASCII, `\n` terminated (CR ignored):
  - Host → device: `D<ch>:<val>\n`  ch = 0..1, val = 0..100
  - Device → host (~1 Hz): `R<ch>:<rpm>\n` (2 lines/sec)
- Parser lives in `USB_DEVICE/App/usbd_cdc_if.c` (`Cdc_ParseLine` in `CDC_Receive_FS`, USB ISR context — keep it fast). The 1 Hz reporter lives in `Core/Src/main.c` USER CODE 3 (`snprintf` into a local buffer → `CDC_Transmit_FS`).

## Fan hardware (`Core/Src/fan_hw.c`, `Core/Inc/fan_hw.h`)

- PWM: ch0 = **PB7** (TIM4_CH2), ch1 = **PB8** (TIM4_CH3). Push-pull AF (`GPIO_MODE_AF_PP`), 25 kHz at 72 MHz timer clock: **PSC=1, ARR=1439**.
- Tach: ch0 = **PA1** (EXTI1), ch1 = **PB14** (EXTI14). Falling edge, internal pull-up, 2 ms debounce **plus burst suppression**.
- LED: **PC13** (Blue Pill, active-low). `FanHw_UpdateLeds` lights it when any duty > 0.
- Default duty on boot: **50%** (`FAN_HW_START_DUTY`) — safe fan start before any host command.
- Tach gotcha: a 2 ms *plain* debounce FOLDS 25 kHz PWM crosstalk into exactly 500 edges/s → reads as 15000 RPM. `FanHw_TachEdge` now measures the gap between every edge and suppresses whole fast bursts instead (`tach_burst[]`). Keep it.
- PC13/RTC gotcha: `.ioc` enables RTC with `RTC_OUTPUTSOURCE_ALARM`, which programs PC13 (backup domain, survives reset) as the RTC alarm-second output and overrides the GPIO LED. `FanHw_Init` clears `BKP->RTCCR` (CCO/ASOE/ASOS) to reclaim it.

## FanControl plugin (`tools/FanControl.Stm32FanCdc/`)

- net10.0 (matches FanControl's runtime — don't target netstandard2.0; the host's `System.IO.Ports.dll` is .NET 10). References FanControl's own `FanControl.Plugins.dll` + `System.IO.Ports.dll` (Private=false), COM discovery via registry P/Invoke — single DLL, no NuGet.
- Implements **IPlugin3**: presence detection — `Load()` registers the 2 fan + 2 control sensors only when the device is present; when plug/unplug state flips, `Update()` fires `RefreshRequested` so FanControl does Close→Initialize→Load and the fans appear/disappear.
- Build: `dotnet build tools\FanControl.Stm32FanCdc\FanControl.Stm32FanCdc.csproj -c Release`; deploy `bin\Release\net10.0\FanControl.Stm32FanCdc.dll` to `C:\Program Files (x86)\FanControl\Plugins\` (needs admin — T13-style; `Start-Process powershell -Verb RunAs`).

## Source of truth: `USBFAN.ioc` (now regenerate-safe)

Configured: USB_DEVICE = **CDC**, SYS = **Serial Wire** (SWD kept), **TIM4** on PB7/PB8, **PA1/PB14 EXTI**, NVIC for EXTI1/EXTI15_10. A CubeMX regen now produces a working base (native F1 CDC middleware + HAL_TIM enabled + tim sources), and all custom logic is in USER CODE markers that survive. Two `.ioc` caveats:
- The `.ioc` TIM4 channels generate as **Input Capture** (`HAL_TIM_IC_ConfigChannel`), not PWM — `fan_hw.c` re-inits them as PWM after. Optionally fix in the CubeMX GUI (PB7/PB8 → "PWM Generation CH2/3").
- EXTI generates as rising edge — `fan_hw.c` reconfigures to falling.

Headless regen (paths with spaces MUST be quoted):
```
config load "C:\...\USBFAN.ioc"
project generate
exit
```
run with `"C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeMX\STM32CubeMX.exe" -q script.txt`.

## Gotchas / conventions

- Verify on hardware via USB enumeration + serial (COM port, e.g. PowerShell `System.IO.Ports.SerialPort`), not register reads. After a re-flash the COM port may briefly refuse to open — wait a few seconds.
- **Ground loops**: a fan tach ground routed through the mains PE produces 25 kHz PWM crosstalk noise on the tach (the 15000 RPM phantom). Keep tach signal ground short and local to the board GND.
- Don't hand-edit generated `MX_*_Init` or fully generated files (`Drivers/`, `Middlewares/`, `USB_DEVICE/Target/usbd_conf.c`, `usbd_desc.*`, `system_stm32f1xx.c`). Keep logic in USER CODE markers + `fan_hw.c`.
- `HW\traps.txt` still has relevant USB/HID/fan lessons (T05-T07, T17, T23, T24) even though the transport is now serial.
