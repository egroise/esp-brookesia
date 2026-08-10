# ESP-Brookesia USB Service

* [English Version](./README.md)

## 概述

`brookesia_service_usb` 为支持的 ESP32-P4 目标提供 USB Serial/JTAG CDC 通道的独占主机控制能力。

该服务提供类型化状态与传输事件、JSON 服务调用、文件上传和 BPK 运行时应用安装。主机协议与 CLI 请参考 [`brookesia_usb_cli`](../../../tools/brookesia_usb_cli/README_CN.md)。

## 环境要求

- 具备 USB Serial/JTAG 能力的 ESP 目标。
- 将 USB Serial/JTAG CDC 端口保留给控制会话。
- `brookesia_service_helper` 和 `brookesia_service_manager` 依赖。

该服务依赖 ESP-IDF USB Serial/JTAG 驱动，仅支持 ESP 平台；协议检查在主机 CLI 测试套件中执行。

## 添加到工程

将 `espressif/brookesia_service_usb` 添加到工程组件依赖，并为目标启用 USB Serial/JTAG 支持。启用自动注册时，System Core 默认打开 USB bridge。

## 配置

主要 Kconfig 选项包括：

- `BROOKESIA_SERVICE_USB_ENABLE_AUTO_REGISTER`：自动向 `ServiceManager` 注册服务。
- `BROOKESIA_SERVICE_USB_UPLOAD_ROOT`：主机上传和临时文件使用的绝对根目录，默认为 `/littlefs/usb`。
- `BROOKESIA_SERVICE_USB_MAX_TRANSFER_SIZE`：上传或 BPK 的最大大小，默认为 8 MiB。
- `BROOKESIA_SERVICE_USB_COMMAND_TIMEOUT_MS`：主机会话超时时间，默认为 30 秒。

## 安全与所有权

USB 连接是独占的受信任控制边界。JSON 调用可以执行已注册的服务函数，因此应用不应向不受信任的物理主机暴露破坏性服务。

文件上传会拒绝绝对路径、父目录分量和符号链接逃逸。默认保护已有文件；显式传入 `overwrite` 才会替换文件。BPK 安装使用 System Core 的软件包校验与回滚流程。
