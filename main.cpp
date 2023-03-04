#include "app_error.h"
#include "softdevice_handler.h"
#include "nrf_log_ctrl.h"
#include "nrf_gpio.h"

#include "gj/base.h"
#include "gj/gjbleserver.h"
#include "gj/eventmanager.h"
#include "gj/commands.h"
#include "gj/config.h"
#include "gj/gjota.h"
#include "gj/esputils.h"
#include "gj/nrf51utils.h"
#include "gj/datetime.h"
#include "gj/file.h"
#include "gj/platform.h"
#include "gj/sensor.h"
#include "gj/sensorcb.h"
#include "datacollector.h"

#if defined(NRF51)
  #define GJ_NRF51_OR_NRF52(code51, code52) code51
#elif defined(NRF52)
  #define GJ_NRF51_OR_NRF52(code51, code52) code52
#endif



DEFINE_CONFIG_INT32(modulesuffix, modulesuffix, (uint32_t)'X');

DEFINE_CONFIG_INT32(wheelpwrpin, wheelpwrpin, GJ_NRF51_OR_NRF52(10, 17));

DEFINE_CONFIG_INT32(wheelpin, wheelpin, GJ_NRF51_OR_NRF52(4, 16));
DEFINE_CONFIG_INT32(wheelpinpull, wheelpinpull, -1);
DEFINE_CONFIG_INT32(wheeldbg, wheeldbg, 0);
DEFINE_CONFIG_INT32(wheeldataid, wheeldataid, 0);

const uint32_t period = 15*60;

static void power_manage(void)
{
    if (softdevice_handler_is_enabled())
    {
      uint32_t err_code = sd_app_evt_wait();

      APP_ERROR_CHECK(err_code);
    }
    else
    {
      __WFE();
    }
}

GJBLEServer bleServer;
GJOTA ota;
BuiltInTemperatureSensor tempSensor;
AnalogSensor battSensor(10);

void OnBattReady(AnalogSensor &sensor)
{
  SER("Batt:%d\n\r", sensor.GetValue());
}

void Command_ReadBattery()
{
  battSensor.SetPin(GJ_ADC_VDD_PIN);
  battSensor.SetOnReady(OnBattReady);
  battSensor.Sample();
}

struct ManufData
{
  uint32_t m_lastSessionUnixtime;
};

ManufData s_manufData = {};

void RefreshManufData()
{
  bleServer.SetAdvManufData(&s_manufData, sizeof(s_manufData));
}

struct TurnData
{
  TurnData();
  DataCollector *m_collector = nullptr;

  DigitalSensor m_sensor;
  DigitalSensorAutoToggleCB m_sensorCB;
} turnData;

TurnData::TurnData()
: m_sensor(10)
, m_sensorCB(&m_sensor, -1, 0)
{

}

void OnTurnDataTimer();
void SetTurnDataTimer();

#define WHEEL_DBG_SER(format, ...) { const bool wheeldbg = GJ_CONF_INT32_VALUE(wheeldbg) != 0; if (wheeldbg) { SER(format, ##__VA_ARGS__); } } 

void WriteTurnData(uint32_t time)
{
  WHEEL_DBG_SER("WriteTurnData\n\r");
  const int32_t sessionTime = GetSessionTime(*turnData.m_collector);
        
  WriteDataSession(*turnData.m_collector);
  InitDataSession(*turnData.m_collector, time);

  //update manuf unixtime after file write
  s_manufData.m_lastSessionUnixtime = sessionTime;
  RefreshManufData();

  SetTurnDataTimer();
}

void SetTurnDataTimer()
{
  const int32_t sessionTime = GetSessionTime(*turnData.m_collector);
  const int32_t endTime = sessionTime + period * 16;
  const int32_t delay = endTime - GetUnixtime();

  //make sure delay is positive and non zero
  const int32_t adjustedDelay = Max(delay, 1);
  
  WHEEL_DBG_SER("Turn timer set to %d(%ds)\n\r", GetUnixtime() + adjustedDelay, adjustedDelay);

  const int64_t secsToMicros = 1000000;
  GJEventManager->DelayAdd(OnTurnDataTimer, adjustedDelay * secsToMicros);
}

void OnTurnDataTimer()
{
  WHEEL_DBG_SER("OnTurnDataTimer\n\r");

  const uint32_t time = GetUnixtime();
  const bool isSessionExpired = IsExpired(*turnData.m_collector, time);
  if (isSessionExpired)
  {
    WriteTurnData(time);
  }
}

void OnWheelTurn(DigitalSensor &sensor, uint32_t val)
{
  const uint32_t time = GetUnixtime();
  const bool isSessionExpired = IsExpired(*turnData.m_collector, time);
  if (isSessionExpired)
  {
    WriteTurnData(time);
  }
  
  if (val != 0)
  {
    const int16_t turnCount = 1;
    const bool dataAdded = AddData(*turnData.m_collector, time, turnCount);
        
    SER_COND(!dataAdded, "ERROR:Turn data not added\n\r");

    WHEEL_DBG_SER("Wheel turn (time:%d)\n\r", time);
  }
  //else
  //{
  //  WHEEL_DBG_SER("Wheel turn SKIPPED (time:%d)\n\r", time);
  //}
}

void TriggerOnWheelTurn()
{
  //SER("TriggerOnWheelTurn");
  
  OnWheelTurn(turnData.m_sensor, 1);

  int32_t next = rand() % 10;

  GJEventManager->DelayAdd(TriggerOnWheelTurn, next * 1000000);
}

const char *GetHostName()
{
  static char hostName[] = "peppe\0\0";

  const char suffix = (char)GJ_CONF_INT32_VALUE(modulesuffix);
  hostName[5] = suffix;

  return hostName;
}

void PrintVersion()
{
  const char *s_buildDate =  __DATE__ "," __TIME__;
  extern int __vectors_load_start__;
  extern int __FLASH_segment_used_end__;
  const char *hostName = GetHostName();
  const char *chipName = nullptr;
  const uint32_t partition = GetPartitionIndex();
  #if defined(NRF51)
    chipName = "NRF51";
  #elif defined(NRF52)
    chipName = "NRF52";
  #else
    chipName = "unknown Chip";
  #endif

  const uint32_t exeSize = (char*)&__FLASH_segment_used_end__ - (char*)&__vectors_load_start__;

  SER("%s Pepperoni Partition %d (0x%x, size:%d) Build:%s\r\n", chipName, partition, &__vectors_load_start__, exeSize, s_buildDate);
  SER("DeviceID:0x%x%x\n\r", NRF_FICR->DEVICEID[0], NRF_FICR->DEVICEID[1]);
  SER("Hostname:%s\n\r", hostName);
}

void Command_Version()
{
  PrintVersion();
}

void Command_TempDie()
{
  uint32_t temp = tempSensor.GetValue();
  SER("Die Temp:%d\r\n", temp);
}


//map file names to static flash locations
#if (NRF_FLASH_SECTOR_SIZE == 1024)
  #define FILE_SECTOR_BOOT      0x1bc00
  #define FILE_SECTOR_TURN_DATA 0x3d000
  #define FILE_SECTOR_LASTDATE  0x3f800
  #define FILE_SECTOR_CONFIG    0x3fc00
#elif (NRF_FLASH_SECTOR_SIZE == 4096)
  #define FILE_SECTOR_BOOT      0x7b000
  #define FILE_SECTOR_TURN_DATA 0x7c000
  #define FILE_SECTOR_LASTDATE  0x7e000
  #define FILE_SECTOR_CONFIG    0x7f000
#endif

DEFINE_FILE_SECTORS(boot,       "/boot",      FILE_SECTOR_BOOT,       1);
DEFINE_FILE_SECTORS(turndata,   "/turndata",  FILE_SECTOR_TURN_DATA,  8);
DEFINE_FILE_SECTORS(lastdate,   "/lastdate",  FILE_SECTOR_LASTDATE,   1);
DEFINE_FILE_SECTORS(config,     "/config",    FILE_SECTOR_CONFIG,     1);

#if defined(NRF51)
  BEGIN_BOOT_PARTITIONS()
  DEFINE_BOOT_PARTITION(0, 0x1c000, 0x10000)
  DEFINE_BOOT_PARTITION(1, 0x2d000, 0x10000)
  END_BOOT_PARTITIONS()
#elif defined(NRF52)
  BEGIN_BOOT_PARTITIONS()
  DEFINE_BOOT_PARTITION(0, 0x20000, 0x20000)
  DEFINE_BOOT_PARTITION(1, 0x40000, 0x20000)
  END_BOOT_PARTITIONS()
#endif

void Command_turndata(const char *command)
{
  CommandInfo info;

  GetCommandInfo(command, info);

  if (info.m_argCount < 1)
  {
    SER("Usage:turndata <disp|clear|debugtrigger|writedbg>\n\r");
    return;
  }

  StringView subCmd = info.m_args[0];

  if (subCmd == "disp")
  {
    uint32_t minTime = 0;

    if (info.m_argCount > 1)
    {
      StringView arg1 = info.m_args[1];
      minTime = strtol(arg1.data(), NULL, 10);
    }

    Display(*turnData.m_collector, minTime);
  }
  else if (subCmd == "clear")
  {
    ClearStorage(*turnData.m_collector);
  }
  else if (subCmd == "debugtrigger")
  {
    TriggerOnWheelTurn();
    SER("Data trigger\n\r");
  }
  else if (subCmd == "writedbg")
  {
    s_manufData.m_lastSessionUnixtime = GetUnixtime();
    RefreshManufData();
    WriteDebugData(*turnData.m_collector);
    SER("Debug Data written\n\r");
  }
}

DEFINE_COMMAND_ARGS(turndata, Command_turndata);
DEFINE_COMMAND_NO_ARGS(version, Command_Version);
DEFINE_COMMAND_NO_ARGS(tempdie, Command_TempDie);
DEFINE_COMMAND_NO_ARGS(batt, Command_ReadBattery);

int main(void)
{
  for (uint32_t i = 0 ; i < 32 ; ++i)
    nrf_gpio_cfg_default(i);

  REFERENCE_COMMAND(tempdie);
  REFERENCE_COMMAND(version);
  REFERENCE_COMMAND(turndata);
  REFERENCE_COMMAND(batt);

  Delay(100);

  InitMultiboot();

  InitializeDateTime();
  InitCommands(0);
  InitSerial();
  InitESPUtils();
  InitFileSystem("");

  InitConfig();
  PrintConfig();

  PrintVersion();

  GJOTA *otaInit = nullptr;
  ota.Init();
  otaInit = &ota;

  uint32_t maxEvents = 4;
  GJEventManager = new EventManager(maxEvents);

  uint32_t wheelPin = GJ_CONF_INT32_VALUE(wheelpin);
  int32_t wheelPinPull = GJ_CONF_INT32_VALUE(wheelpinpull);
  int32_t wheelPwrPin = GJ_CONF_INT32_VALUE(wheelpwrpin);
  turnData.m_sensor.SetPin(wheelPin, wheelPinPull);
  turnData.m_sensorCB.SetOnChange(OnWheelTurn);
  turnData.m_sensor.EnableInterrupts(true);

  SetupPin(wheelPwrPin, false, 0);
  WritePin(wheelPwrPin, 1);
  
  uint32_t turnDataId = GJ_CONF_INT32_VALUE(wheeldataid);
  turnData.m_collector = InitDataCollector("/turndata", turnDataId, period);

  const char *hostName = GetHostName();
  //note:must initialize after InitFStorage()
  bleServer.Init(hostName, otaInit);

  bleServer.SetAdvManufData(&s_manufData, sizeof(s_manufData));
  
  SER_COND(period != 60 * 15, "****PERIOD****\n\r");

  for (;;)
  {
      bleServer.Update();
      GJEventManager->WaitForEvents(0);

      bool bleIdle = bleServer.IsIdle();
      bool evIdle = GJEventManager->IsIdle();
      bool const isIdle = bleIdle && evIdle;
      if (isIdle)
      {
          power_manage();
      }
  }
}
