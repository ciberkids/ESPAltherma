#ifndef ESPALTHERMA_RESTART_H
#define ESPALTHERMA_RESTART_H

#include <Arduino.h>

void restart_board()
{
  #if defined(ARDUINO_ARCH_ESP8266)
  system_restart();
  #else
  esp_restart();
  #endif
}

//Why the board last rebooted, to tell a power problem from a crash
const char *resetReasonName()
{
  #if defined(ARDUINO_ARCH_ESP8266)
  static String reason = ESP.getResetReason();
  return reason.c_str();
  #else
  switch (esp_reset_reason())
  {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "external reset pin";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    case ESP_RST_BROWNOUT: return "brownout (supply dip)";
    case ESP_RST_SDIO: return "SDIO";
    default: return "unknown";
  }
  #endif
}

#endif