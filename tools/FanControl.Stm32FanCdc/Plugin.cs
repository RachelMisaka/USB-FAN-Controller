using System;
using System.Collections.Generic;
using System.IO.Ports;
using System.Runtime.InteropServices;
using System.Text;
using FanControl.Plugins;

namespace FanControl.Stm32FanCdc
{
    /// <summary>
    /// FanControl plugin for the STM32F103 (USBFAN) virtual COM port (USB CDC).
    ///
    /// Device: VID 0x0483 / PID 0x5740 ("STM32 Virtual ComPort"). The COM port
    /// is located by VID/PID via the registry (PortName under Device Parameters).
    ///
    /// Protocol (line based, '\n' terminated, ASCII):
    ///   device -> host (about 1 Hz): "R0:&lt;rpm&gt;\n" and "R1:&lt;rpm&gt;\n"
    ///   host   -> device:            "D&lt;ch&gt;:&lt;val&gt;\n"   ch = 0..1, val = 0..100
    ///
    /// Presence detection: Load() registers the fan/control sensors only when the
    /// device is actually plugged in, so FanControl shows no fans otherwise. When
    /// the plug/unplug state flips, RefreshRequested (IPlugin3) is raised so
    /// FanControl runs a full Close -> Initialize -> Load cycle and the sensor set
    /// is re-evaluated.
    /// </summary>
    public sealed class Stm32FanCdcPlugin : IPlugin3
    {
        private const ushort VendorId = 0x0483;
        private const ushort ProductId = 0x5740;
        private const int ChannelCount = 2;
        private const int BaudRate = 115200;

        private const int KEY_READ = 0x20019;
        private static readonly IntPtr HKEY_LOCAL_MACHINE = new IntPtr(0x80000002);

        private readonly object _sync = new object();
        private readonly float[] _duties = new float[ChannelCount];
        private readonly List<FanSensor> _fanSensors = new List<FanSensor>();
        private readonly List<ControlSensor> _controlSensors = new List<ControlSensor>();
        private readonly StringBuilder _rx = new StringBuilder();

        private SerialPort _port;
        private bool _needSend;
        private bool _wasPresent;

        public string Name => "STM32 Fan (CDC)";

        public event Action RefreshRequested;

        [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
        private static extern int RegOpenKeyEx(IntPtr hKey, string subKey, int options, int sam, out IntPtr result);

        [DllImport("advapi32.dll")]
        private static extern int RegCloseKey(IntPtr hKey);

        [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
        private static extern int RegEnumKeyEx(IntPtr hKey, uint index, StringBuilder name, ref uint nameSize,
                                               IntPtr reserved, IntPtr cls, IntPtr clsSize, out long lastWrite);

        [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
        private static extern int RegQueryValueEx(IntPtr hKey, string valueName, IntPtr reserved, out int type,
                                                  IntPtr data, ref int size);

        private static string ReadRegString(IntPtr key, string valueName)
        {
            int size = 0;
            if (RegQueryValueEx(key, valueName, IntPtr.Zero, out _, IntPtr.Zero, ref size) != 0 || size <= 0)
            {
                return null;
            }

            IntPtr buf = Marshal.AllocHGlobal(size);
            try
            {
                if (RegQueryValueEx(key, valueName, IntPtr.Zero, out _, buf, ref size) != 0)
                {
                    return null;
                }
                return Marshal.PtrToStringUni(buf);
            }
            finally
            {
                Marshal.FreeHGlobal(buf);
            }
        }

        private static string FindComPort()
        {
            string baseKey = @"SYSTEM\CurrentControlSet\Enum\USB\VID_" + VendorId.ToString("X4") +
                             "&PID_" + ProductId.ToString("X4");

            IntPtr key;
            if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, baseKey, 0, KEY_READ, out key) != 0)
            {
                return null;
            }

            try
            {
                for (uint i = 0; ; i++)
                {
                    var name = new StringBuilder(256);
                    uint size = (uint)name.Capacity;
                    if (RegEnumKeyEx(key, i, name, ref size, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, out _) != 0)
                    {
                        break;
                    }

                    IntPtr devKey;
                    if (RegOpenKeyEx(key, name.ToString(), 0, KEY_READ, out devKey) == 0)
                    {
                        try
                        {
                            IntPtr prmKey;
                            if (RegOpenKeyEx(devKey, "Device Parameters", 0, KEY_READ, out prmKey) == 0)
                            {
                                try
                                {
                                    string port = ReadRegString(prmKey, "PortName");
                                    if (!string.IsNullOrEmpty(port))
                                    {
                                        return port;
                                    }
                                }
                                finally
                                {
                                    RegCloseKey(prmKey);
                                }
                            }
                        }
                        finally
                        {
                            RegCloseKey(devKey);
                        }
                    }
                }
            }
            finally
            {
                RegCloseKey(key);
            }

            return null;
        }

        private static bool IsPresent()
        {
            return FindComPort() != null;
        }

        private void OpenPort()
        {
            lock (_sync)
            {
                if (_port != null && _port.IsOpen)
                {
                    return;
                }

                string portName = FindComPort();
                if (portName == null)
                {
                    return;
                }

                try
                {
                    var p = new SerialPort(portName, BaudRate, Parity.None, 8, StopBits.One)
                    {
                        Handshake = Handshake.None,
                        ReadTimeout = 200,
                        WriteTimeout = 200,
                        DtrEnable = true,
                        RtsEnable = true,
                    };
                    p.Open();
                    _port = p;
                    _rx.Clear();
                }
                catch
                {
                    _port = null;
                }
            }
        }

        private void ClosePort()
        {
            lock (_sync)
            {
                try
                {
                    _port?.Dispose();
                }
                catch
                {
                    /* ignore */
                }
                _port = null;
                _rx.Clear();
            }
        }

        public void Initialize()
        {
            ClosePort();
        }

        public void Load(IPluginSensorsContainer container)
        {
            _fanSensors.Clear();
            _controlSensors.Clear();

            _wasPresent = IsPresent();
            if (!_wasPresent)
            {
                return;  /* device not present: register no sensors */
            }

            for (int i = 0; i < ChannelCount; i++)
            {
                int index = i;
                string fanId = "stm32fan" + i;
                string ctrlId = "stm32ctrl" + i;
                string name = "STM32 Fan " + (i + 1);

                var fan = new FanSensor(fanId, name);
                var control = new ControlSensor(ctrlId, name + " Control", fanId, duty =>
                {
                    lock (_sync)
                    {
                        _duties[index] = duty;
                        _needSend = true;
                    }
                });

                _fanSensors.Add(fan);
                _controlSensors.Add(control);
                container.FanSensors.Add(fan);
                container.ControlSensors.Add(control);
            }

            OpenPort();
        }

        public void Update()
        {
            bool present = IsPresent();
            if (present != _wasPresent)
            {
                _wasPresent = present;
                RefreshRequested?.Invoke();
                return;
            }

            lock (_sync)
            {
                if (_port == null)
                {
                    OpenPort();
                }

                if (_port == null)
                {
                    foreach (var f in _fanSensors)
                    {
                        f.Value = null;
                    }
                    return;
                }

                try
                {
                    if (_needSend)
                    {
                        for (int i = 0; i < ChannelCount; i++)
                        {
                            int val = (int)Math.Round(_duties[i]);
                            if (val < 0)
                            {
                                val = 0;
                            }
                            if (val > 100)
                            {
                                val = 100;
                            }
                            _port.Write("D" + i + ":" + val + "\n");
                        }
                        _needSend = false;
                    }

                    int n = _port.BytesToRead;
                    if (n > 0)
                    {
                        var buf = new byte[n];
                        _port.Read(buf, 0, n);
                        _rx.Append(Encoding.ASCII.GetString(buf));

                        while (_rx.Length > 0)
                        {
                            string s = _rx.ToString();
                            int idx = s.IndexOf('\n');
                            if (idx < 0)
                            {
                                break;
                            }
                            string line = s.Substring(0, idx).Trim();
                            _rx.Remove(0, idx + 1);
                            ParseRpmLine(line);
                        }
                    }
                }
                catch
                {
                    ClosePort();
                    foreach (var f in _fanSensors)
                    {
                        f.Value = null;
                    }
                }
            }
        }

        private void ParseRpmLine(string line)
        {
            /* Format: R<ch>:<rpm>  e.g. "R0:1200" */
            if (line.Length < 4 || line[0] != 'R')
            {
                return;
            }

            int colon = line.IndexOf(':');
            if (colon <= 1 || colon == line.Length - 1)
            {
                return;
            }
            if (!int.TryParse(line.Substring(1, colon - 1), out int ch))
            {
                return;
            }
            if (ch < 0 || ch >= _fanSensors.Count)
            {
                return;
            }
            if (!int.TryParse(line.Substring(colon + 1), out int rpm))
            {
                return;
            }

            _fanSensors[ch].Value = rpm;
        }

        public void Close()
        {
            ClosePort();
        }
    }

    internal sealed class FanSensor : IPluginSensor
    {
        public FanSensor(string id, string name)
        {
            Id = id;
            Name = name;
        }

        public string Id { get; }
        public string Name { get; }
        public float? Value { get; set; }

        public void Update()
        {
        }
    }

    internal sealed class ControlSensor : IPluginControlSensor2
    {
        private readonly Action<float> _apply;

        public ControlSensor(string id, string name, string pairedFanSensorId, Action<float> apply)
        {
            Id = id;
            Name = name;
            PairedFanSensorId = pairedFanSensorId;
            _apply = apply;
        }

        public string Id { get; }
        public string Name { get; }
        public float? Value { get; set; }
        public string PairedFanSensorId { get; }

        public void Set(float val)
        {
            Value = val;
            _apply(val);
        }

        public void Reset()
        {
            Set(0);
        }

        public void Update()
        {
        }
    }
}
