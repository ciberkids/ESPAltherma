#ifndef ESPALTHERMA_RESTART_H
#define ESPALTHERMA_RESTART_H

#include <Arduino.h>
#if !defined(ARDUINO_ARCH_ESP8266)
#include <esp_core_dump.h>
#include <esp_ota_ops.h>
#include <esp_rom_sys.h>
#include <esp_partition.h>
#endif

//What the firmware was last doing. On ESP32 it is kept in RTC memory, which survives crashes,
//watchdog and software resets (not a power loss), so after a hang that left no crash dump it
//still tells where the program got stuck. The numbers must stay stable across builds.
enum Activity : uint8_t
{
  ACT_BOOT = 1,
  ACT_WIFI = 2,
  ACT_WIFI_SCAN = 3,
  ACT_WIFI_ROAM = 4,
  ACT_MQTT = 5,
  ACT_QUERY = 6,
  ACT_DECODE = 7,
  ACT_PUBLISH = 8,
  ACT_WAIT = 9,
  ACT_OTA = 10,
  ACT_RESTART = 11,
};

#if !defined(ARDUINO_ARCH_ESP8266)
#define ACTIVITY_MAGIC 0x41637431 //"Act1": tells a record from the garbage RTC memory holds after power-up
struct ActivityRecord
{
  uint32_t magic;
  uint8_t activity;
  uint8_t reg;
  uint32_t uptime;
};
RTC_NOINIT_ATTR ActivityRecord activityRecord;
#endif

void noteActivity(Activity activity, uint8_t reg = 0)
{
  #if !defined(ARDUINO_ARCH_ESP8266)
  activityRecord.activity = activity;
  activityRecord.reg = reg;
  activityRecord.uptime = (uint32_t)(esp_timer_get_time() / 1000000ULL);
  activityRecord.magic = ACTIVITY_MAGIC;
  #endif
}

void restart_board()
{
  noteActivity(ACT_RESTART);
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
    case ESP_RST_WDT:
      //The RTC watchdog is only armed while panicking (7 s), restarting (1 s) or booting (9 s),
      //so it means the chip got stuck in one of those. The hardware code tells which.
      #if CONFIG_IDF_TARGET_ESP32
      switch (esp_rom_get_reset_reason(0))
      {
        case RESET_REASON_CORE_RTC_WDT: return "RTC watchdog (stuck in crash/restart)";
        case RESET_REASON_SYS_RTC_WDT: return "RTC watchdog (stuck while booting)";
        default: break;
      }
      #endif
      return "RTC watchdog";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    case ESP_RST_BROWNOUT: return "brownout (supply dip)";
    case ESP_RST_SDIO: return "SDIO";
    default: return "unknown";
  }
  #endif
}

//Why the board rebooted plus what it was doing at the time, e.g.
//"RTC watchdog (stuck in crash/restart), was querying register 0x61, up 233791 s".
//Call once at boot, before the first noteActivity(), which overwrites the record.
const char *describeLastReset()
{
  static char text[128] = "";
  if (text[0])
  {
    return text;
  }
  snprintf(text, sizeof(text), "%s", resetReasonName());
  #if !defined(ARDUINO_ARCH_ESP8266)
  if (activityRecord.magic == ACTIVITY_MAGIC && esp_reset_reason() != ESP_RST_POWERON)
  {
    const char *what;
    switch (activityRecord.activity)
    {
      case ACT_BOOT: what = "starting up"; break;
      case ACT_WIFI: what = "connecting WiFi"; break;
      case ACT_WIFI_SCAN: what = "scanning WiFi"; break;
      case ACT_WIFI_ROAM: what = "roaming to another AP"; break;
      case ACT_MQTT: what = "connecting MQTT"; break;
      case ACT_QUERY: what = "querying register"; break;
      case ACT_DECODE: what = "decoding register"; break;
      case ACT_PUBLISH: what = "sending values"; break;
      case ACT_WAIT: what = "waiting between reads"; break;
      case ACT_OTA: what = "receiving an update"; break;
      case ACT_RESTART: what = "restarting itself"; break;
      default: what = "unknown"; break;
    }
    size_t used = strlen(text);
    if (activityRecord.activity == ACT_QUERY || activityRecord.activity == ACT_DECODE)
    {
      snprintf(text + used, sizeof(text) - used, ", was %s 0x%02x, up %u s", what, activityRecord.reg, (unsigned)activityRecord.uptime);
    }
    else
    {
      snprintf(text + used, sizeof(text) - used, ", was %s, up %u s", what, (unsigned)activityRecord.uptime);
    }
  }
  #endif
  return text;
}

#if !defined(ARDUINO_ARCH_ESP8266)
//Xtensa EXCCAUSE values of the usual crashes. abort() and failed asserts show up as
//StoreProhibited at address 0, so the backtrace is what tells them apart.
const char *excCauseName(uint32_t cause)
{
  switch (cause)
  {
    case 0: return "IllegalInstruction";
    case 2: return "InstructionFetchError";
    case 3: return "LoadStoreError";
    case 6: return "IntegerDivideByZero";
    case 9: return "LoadStoreAlignment";
    case 20: return "InstFetchProhibited";
    case 28: return "LoadProhibited";
    case 29: return "StoreProhibited";
    default: return "Exception";
  }
}
#endif

//Reads the crash dump that the last panic wrote to flash, if any. The dump survives resets
//and OTA updates, so "crash_fw" (ELF fingerprint of the build that crashed) can differ from
//"running_fw". Fills `json` with the report: one-line "summary", plus the PC and backtrace to
//decode with addr2line against that build's firmware.elf. Returns false when there is no dump.
bool describeLastCrash(char *json, size_t jsonLen)
{
  #if defined(ARDUINO_ARCH_ESP8266)
  return false;
  #else
  char running[17] = "";
  esp_ota_get_app_elf_sha256(running, sizeof(running));
  //An erased (or never used) dump partition starts with size 0xFFFFFFFF. The IDF reports that
  //as ESP_ERR_INVALID_SIZE, the same as a corrupted dump, so look at the size word first.
  const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
  uint32_t dumpSize = 0xFFFFFFFF;
  if (part == NULL || esp_partition_read(part, 0, &dumpSize, sizeof(dumpSize)) != ESP_OK || dumpSize == 0xFFFFFFFF)
  {
    return false;
  }
  esp_err_t err = esp_core_dump_image_check();
  if (err == ESP_ERR_NOT_FOUND)
  {
    return false;
  }
  static esp_core_dump_summary_t summary;//a few hundred bytes: keep it off the stack
  if (err == ESP_OK)
  {
    err = esp_core_dump_get_summary(&summary);
  }
  if (err != ESP_OK)
  {//e.g. the RTC watchdog cut the panic handler short while it was writing the dump
    snprintf(json, jsonLen, "{\"summary\":\"crash dump unreadable (%s)\",\"running_fw\":\"%s\"}", esp_err_to_name(err), running);
    return true;
  }
  char backtrace[16 * 11 + 1] = "";
  for (uint32_t i = 0; i < summary.exc_bt_info.depth && i < 16; i++)
  {
    size_t used = strlen(backtrace);
    snprintf(backtrace + used, sizeof(backtrace) - used, "%s0x%08x", i ? " " : "", (unsigned)summary.exc_bt_info.bt[i]);
  }
  const char *cause = excCauseName(summary.ex_info.exc_cause);
  snprintf(json, jsonLen,
           "{\"summary\":\"%s in %.16s at 0x%08x (fw %.8s)\",\"task\":\"%.16s\",\"pc\":\"0x%08x\",\"cause\":\"%s (%u)\","
           "\"address\":\"0x%08x\",\"backtrace\":\"%s\",\"backtrace_corrupted\":%s,\"crash_fw\":\"%.16s\",\"running_fw\":\"%s\"}",
           cause, summary.exc_task, (unsigned)summary.exc_pc, (const char *)summary.app_elf_sha256,
           summary.exc_task, (unsigned)summary.exc_pc, cause, (unsigned)summary.ex_info.exc_cause,
           (unsigned)summary.ex_info.exc_vaddr, backtrace, summary.exc_bt_info.corrupted ? "true" : "false",
           (const char *)summary.app_elf_sha256, running);
  return true;
  #endif
}

#endif