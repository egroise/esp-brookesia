# ESP-Brookesia USB Service

* [中文版本](./README_CN.md)

## Overview

`brookesia_service_usb` provides exclusive host control over the USB
Serial/JTAG CDC channel on supported ESP32-P4 targets.

It exposes typed service status and transfer events, JSON service calls, file
uploads, and BPK runtime app installation. The host protocol and CLI are
documented in [`brookesia_usb_cli`](../../../tools/brookesia_usb_cli/README.md).

## Environment Requirements

- An ESP target with USB Serial/JTAG support.
- The USB Serial/JTAG CDC port reserved for the control session.
- `brookesia_service_helper` and `brookesia_service_manager` dependencies.

The service is ESP-only because it depends on the ESP-IDF USB Serial/JTAG
driver. Protocol checks run in the host CLI test suite.

## Add to Your Project

Add `espressif/brookesia_service_usb` to the project component dependencies and
enable USB Serial/JTAG support for the target. System Core enables the bridge
by default when automatic USB service registration is enabled.

## Configuration

The main Kconfig options are:

- `BROOKESIA_SERVICE_USB_ENABLE_AUTO_REGISTER`: register the service with
  `ServiceManager` automatically.
- `BROOKESIA_SERVICE_USB_UPLOAD_ROOT`: absolute root for host uploads and
  temporary files; defaults to `/littlefs/usb`.
- `BROOKESIA_SERVICE_USB_MAX_TRANSFER_SIZE`: maximum upload or BPK size;
  defaults to 8 MiB.
- `BROOKESIA_SERVICE_USB_COMMAND_TIMEOUT_MS`: host session timeout;
  defaults to 30 seconds.

## Security and Ownership

The USB connection is an exclusive trusted control boundary. JSON calls can
invoke registered service functions, so applications should not expose
destructive services to an untrusted physical host.

File uploads reject absolute paths, parent components, and symbolic-link
escapes. Existing files are protected by default; pass `overwrite` explicitly
to replace one. BPK installation uses the System Core package validation and
rollback flow.
