import json
import types
import unittest

from brookesia_usb_cli import cli


class FakeSerial:
    def __init__(self, *_args, **_kwargs):
        self.responses = []
        self.writes = []
        self.closed = False

    def reset_input_buffer(self):
        pass

    def write(self, payload):
        self.writes.append(payload)
        request = json.loads(payload.decode("utf-8"))
        if request["op"] == "hello":
            self.responses.append(
                json.dumps({
                    "version": 1,
                    "op": "hello",
                    "request_id": request["request_id"],
                    "ok": True,
                    "protocol_version": 1,
                    "transport": "serial_jtag",
                    "session": "exclusive",
                }).encode() + b"\n"
            )
        elif request["op"] == "goodbye":
            self.responses.append(json.dumps({
                "version": 1,
                "op": "done",
                "request_id": request["request_id"],
                "ok": True,
                "command": "goodbye",
            }).encode() + b"\n")

    def readline(self):
        return self.responses.pop(0) if self.responses else b""

    def close(self):
        self.closed = True

    def flush(self):
        pass


class TimeoutSerial(FakeSerial):
    def readline(self):
        return b""


class CliTests(unittest.TestCase):
    def test_serial_jtag_identity_is_accepted(self):
        port = types.SimpleNamespace(
            vid=0x303A, pid=0x1001, description="USB JTAG/serial", interface=None, hwid=""
        )
        self.assertTrue(cli.is_serial_jtag_port(port))

    def test_otg_port_is_rejected(self):
        port = types.SimpleNamespace(
            vid=0x303A, pid=0x1002, description="USB OTG", interface=None, hwid=""
        )
        self.assertFalse(cli.is_serial_jtag_port(port))

    def test_hello_and_goodbye_validate_single_transport(self):
        fake = FakeSerial()
        original_loader = cli._serial_module
        cli._serial_module = lambda: (types.SimpleNamespace(Serial=lambda *_args, **_kwargs: fake), None)
        try:
            with cli.UsbClient("/dev/ttyACM0", 115200, 1.0) as client:
                hello = client.hello()
                self.assertEqual(hello["protocol_version"], 1)
                self.assertEqual(hello["transport"], "serial_jtag")
                goodbye = client.goodbye()
                self.assertEqual(goodbye["command"], "goodbye")
            self.assertTrue(fake.closed)
        finally:
            cli._serial_module = original_loader

    def test_single_port_discovery_does_not_probe_before_the_real_command(self):
        port = types.SimpleNamespace(
            device="/dev/ttyACM0", vid=0x303A, pid=0x1001,
            description="USB JTAG/serial", interface=None, hwid=""
        )
        original_loader = cli._serial_module
        cli._serial_module = lambda: (None, types.SimpleNamespace(comports=lambda: [port]))
        try:
            self.assertEqual(cli.discover_control_port(timeout=1.0), "/dev/ttyACM0")
        finally:
            cli._serial_module = original_loader

    def test_timed_out_hello_sends_best_effort_cleanup(self):
        fake = TimeoutSerial()
        original_loader = cli._serial_module
        cli._serial_module = lambda: (types.SimpleNamespace(Serial=lambda *_args, **_kwargs: fake), None)
        try:
            with self.assertRaises(TimeoutError):
                with cli.UsbClient("/dev/ttyACM0", 115200, 0.01) as client:
                    client.hello()
            operations = [json.loads(payload.decode("utf-8"))["op"] for payload in fake.writes]
            self.assertEqual(operations, ["hello", "goodbye"])
        finally:
            cli._serial_module = original_loader

    def test_parser_exposes_all_commands(self):
        parser = cli.build_parser()
        for argv in (
            ["devices"],
            ["status"],
            ["call", "SystemCore", "GetSystemInfo"],
            ["put", "file.bin", "logs/file.bin"],
            ["install", "app.bpk"],
            ["abort", "42"],
        ):
            self.assertEqual(parser.parse_args(argv).command, argv[0])


if __name__ == "__main__":
    unittest.main()
