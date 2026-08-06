# Brookesia USB CLI

`brookesia-usb` 通过 USB Serial/JTAG 提供的单个 CDC-ACM 通道控制 ESP-Brookesia USB service。协议版本为 `1`。

## 安装

在当前 Python 环境中安装：

```bash
python -m pip install -e .
```

唯一的主机依赖是 `pyserial`。

## 端口选择

CLI 会按 USB 设备标识自动选择 USB Serial/JTAG 设备，并使用 `hello` 命令验证设备：

```bash
brookesia-usb devices
brookesia-usb status
brookesia-usb --port /dev/ttyACM0 status
```

`--baudrate` 仅为兼容 pyserial，USB Serial/JTAG 不使用物理波特率。默认值为 `115200`，`--timeout` 默认值为 10 秒。

## 常用命令

查询 USB service 状态：

```bash
brookesia-usb status
```

调用已注册的服务函数：

```bash
brookesia-usb call Manager GetServiceNames '{}'
brookesia-usb call Storage FSStat '{"Path":"/littlefs"}'
```

上传文件到设备上传根目录下的相对路径：

```bash
brookesia-usb put ./logs/session.bin logs/session.bin
brookesia-usb put ./config/device.json config/device.json --overwrite
```

默认不会覆盖已有文件。绝对路径、`..` 路径分量、符号链接逃逸和上传根目录外的路径都会被拒绝。

安装 BPK 运行时应用：

```bash
brookesia-usb install ./build/my_app.bpk
```

终止请求为 `42` 的传输：

```bash
brookesia-usb abort 42
```

## 控制会话与安全

CLI 会在需要时建立独占控制会话，校验协议版本和传输类型，并在退出时发送 `goodbye`。会话期间设备日志会被抑制，避免干扰文件帧。

USB 连接被视为受信任的物理控制边界。JSON 服务调用不能传递 `RawBuffer` 参数；需要传输文件或软件包时应使用 `put` 或 `install`。

USB Serial/JTAG 与刷写、JTAG 调试、控制台日志共享同一个 CDC 通道。运行控制命令前请关闭 `idf.py monitor`、minicom 或其他读取 `/dev/ttyACM0` 的程序。
