"""Guard the size profile against accidentally disabling reader features."""

import configparser
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class FirmwareConfigTest(unittest.TestCase):
    def setUp(self):
        self.config = configparser.ConfigParser(interpolation=None)
        self.config.read(ROOT / "platformio.ini", encoding="utf-8")
        requested = self.config.get("firmware_tuned", "custom_sdkconfig")
        requested = requested.replace("${base.custom_sdkconfig}", self.config.get("base", "custom_sdkconfig"))
        self.settings = {}
        for line in requested.splitlines():
            key, separator, value = line.strip().partition("=")
            if separator and key.startswith("CONFIG_"):
                self.assertNotIn(key, self.settings, f"Duplicate SDK setting: {key}")
                self.settings[key] = value

    def test_all_firmware_profiles_inherit_tuned_sdk(self):
        for profile in ("default", "slim", "gh_release", "gh_release_rc", "x4_release_slim"):
            with self.subTest(profile=profile):
                parents = self.config.get("env:" + profile, "extends")
                self.assertIn("firmware_tuned", [value.strip() for value in parents.split(",")])

    def test_essential_arduino_components_remain_available(self):
        for component in ("SPI", "Wire", "FS", "Preferences", "Update", "Network", "WiFi",
                          "WebServer", "NetworkClientSecure", "HTTPClient", "DNSServer", "ESPmDNS", "Hash"):
            with self.subTest(component=component):
                self.assertNotEqual(self.settings.get("CONFIG_ARDUINO_SELECTIVE_" + component), "n")

    def test_unused_wrappers_are_disabled(self):
        for component in ("ArduinoOTA", "BLE", "BluetoothSerial", "SimpleBLE", "EEPROM", "Ethernet",
                          "FFat", "LittleFS", "Matter", "NetBIOS", "OpenThread", "PPP", "SD", "SD_MMC",
                          "SPIFFS", "WiFiProv", "RainMaker", "Insights", "ESP_SR"):
            with self.subTest(component=component):
                self.assertEqual(self.settings.get("CONFIG_ARDUINO_SELECTIVE_" + component), "n")
        self.assertEqual(self.settings.get("CONFIG_ARDUINO_SELECTIVE_COMPILATION"), "y")
        self.assertEqual(self.settings.get("CONFIG_BT_ENABLED"), "n")

    def test_rom_libc_and_existing_memory_limits_are_preserved(self):
        self.assertEqual(self.settings["CONFIG_LIBC_OPTIMIZED_MISALIGNED_ACCESS"], "n")
        self.assertEqual(self.settings["CONFIG_ESP_TIMER_TASK_STACK_SIZE"], "4096")
        self.assertEqual(self.settings["CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH"], "2560")
        self.assertEqual(self.settings["CONFIG_ESP_WIFI_IRAM_OPT"], "n")
        self.assertEqual(self.settings["CONFIG_ESP_WIFI_RX_IRAM_OPT"], "n")


if __name__ == "__main__":
    unittest.main()
