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

#define CENTRAL_LINK_COUNT              0                                           /**< Number of central links used by the application. When changing this number remember to adjust the RAM settings*/
#define PERIPHERAL_LINK_COUNT           1                                           /**< Number of peripheral links used by the application. When changing this number remember to adjust the RAM settings*/

#if defined(NRF51)
  #define GJ_NRF51_OR_NRF52(code51, code52) code51
#elif defined(NRF52)
  #define GJ_NRF51_OR_NRF52(code51, code52) code52
#endif

const char *version_app = 
#include "version.h"
;

const char *version_gj = 
#include "version_gj.h"
;


const char *tag_app = 
#include "tag.h"
;


const uint32_t oneHour = 60 * 60;
const uint32_t oneDay = oneHour * 24;

DEFINE_CONFIG_INT32(modulesuffix, modulesuffix, (uint32_t)'X');

DEFINE_CONFIG_INT32(wheelpwrpin, wheelpwrpin, GJ_NRF51_OR_NRF52(10, 17));
DEFINE_CONFIG_INT32(wheelpwrout, wheelpwrout, 0);

DEFINE_CONFIG_INT32(wheelpin, wheelpin, GJ_NRF51_OR_NRF52(4, 16));
DEFINE_CONFIG_INT32(wheelpinpull, wheelpinpull, 0);
DEFINE_CONFIG_INT32(wheeldbg, wheeldbg, 0);
DEFINE_CONFIG_INT32(wheeldataid, wheeldataid, 0);

DEFINE_CONFIG_INT32(bleenabletime, bleenabletime, oneHour * 7); //7h00

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
  if (bleServer.IsInit())
    bleServer.SetAdvManufData(&s_manufData, sizeof(s_manufData));
}

#define TD_ERR_NOT_ADDED 1001

struct TurnData
{
  TurnData();
  DataCollector *m_collector = nullptr;

  DigitalSensor m_sensor;
  DigitalSensorCB m_sensorCB;

  bool m_timerSet = false;
  uint32_t m_Time = 0;
  uint32_t m_NextTimer = 0;
  uint32_t m_LastError = 0;
} turnData;

TurnData::TurnData()
: m_sensor(10)
, m_sensorCB(&m_sensor)
{

}

void OnTurnDataTimer();
void SetTurnDataTimer();

#define WHEEL_DBG_SER(level, format, ...) { const bool wheeldbg = GJ_CONF_INT32_VALUE(wheeldbg) >= (level); if (wheeldbg) { SER(format, ##__VA_ARGS__); } } 

void WriteTurnData(uint32_t time)
{
  WHEEL_DBG_SER(1, "WriteTurnData\n\r");
  const int32_t sessionTime = GetSessionTime(*turnData.m_collector);
        
  WriteDataSession(*turnData.m_collector);
  InitDataSession(*turnData.m_collector, time);

  //update manuf unixtime after file write
  s_manufData.m_lastSessionUnixtime = sessionTime;
  RefreshManufData();

  turnData.m_timerSet = false;
}

void SetTurnDataTimer()
{
  const int32_t sessionTime = GetSessionTime(*turnData.m_collector);
  const int32_t endTime = sessionTime + period * 16;
  const int32_t delay = endTime - GetUnixtime();

  //make sure delay is positive and non zero
  const int32_t adjustedDelay = Max<int32_t>(delay, 1);
  
  WHEEL_DBG_SER(1, "Turn timer set to %d(%ds)\n\r", GetUnixtime() + adjustedDelay, adjustedDelay);

  turnData.m_Time = GetUnixtime();
  turnData.m_NextTimer = GetUnixtime() + adjustedDelay;

  const int64_t secsToMicros = 1000000;
  GJEventManager->DelayAdd(OnTurnDataTimer, adjustedDelay * secsToMicros);

  turnData.m_timerSet = true;
}

void OnTurnDataTimer()
{
  WHEEL_DBG_SER(1, "OnTurnDataTimer\n\r");

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

  if (turnData.m_timerSet == false)
  {
    SetTurnDataTimer();
  }
  
  if (val == 0)
  {
    const int16_t turnCount = 1;
    const bool dataAdded = AddData(*turnData.m_collector, time, turnCount);
        
    SER_COND(!dataAdded, "ERROR:Turn data not added\n\r");

    if (!dataAdded)
      turnData.m_LastError = TD_ERR_NOT_ADDED;

    WHEEL_DBG_SER(2, "Wheel turn (time:%d)\n\r", time);
  }
}

static volatile uint32_t nextTrigger = 15;
void TriggerOnWheelTurn()
{
  OnWheelTurn(turnData.m_sensor, 0);
  GJEventManager->DelayAdd(TriggerOnWheelTurn, nextTrigger * 1000);
}

const char *GetHostName()
{
  static char hostName[] = "peppe\0\0";

  const char suffix = (char)GJ_CONF_INT32_VALUE(modulesuffix);
  hostName[5] = suffix;

  return hostName;
}

void PrintVersion(uint32_t step)
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

  if (AreTerminalsReady())
  {
    SER_COND(step == 0, "%s Pepperoni (0x%x, size:%d) \r\n", chipName, &__vectors_load_start__, exeSize);
    SER_COND(step == 1, "DeviceID:0x%x%x\n\r", NRF_FICR->DEVICEID[0], NRF_FICR->DEVICEID[1]);
    SER_COND(step == 2, "Hostname:%s\n\r", hostName);
    SER_COND(step == 3, "Partition:%d\n\r", partition);
    SER_COND(step == 4, "App version %s (Built:%s)\n\r", tag_app, s_buildDate);
    SER_COND(step == 5, "App hash %s\n\r", version_app);
    SER_COND(step == 6, "GJ hash %s\n\r", version_gj);

    step++;
  }
  
  if (step < 7)
  {
    EventManager::Function func;
    func = std::bind(PrintVersion, step);
    GJEventManager->DelayAdd(func, 10);
  }
}

void Command_Version()
{
  PrintVersion(0);
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

void Command_TurnDataDisp(const CommandInfo &commandInfo)
{
  uint32_t minTime = 0;

  if (commandInfo.m_argCount >= 1)
  {
    StringView arg1 = commandInfo.m_args[0];
    minTime = strtol(arg1.data(), NULL, 0);
  }

  Display(*turnData.m_collector, minTime);
}
void Command_TurnDataActive(const CommandInfo &commandInfo)
{
  DisplayActiveDataSession(turnData.m_collector);
}
void Command_TurnDataClear(const CommandInfo &commandInfo)
{
  ClearStorage(*turnData.m_collector);
}
void Command_TurnDataDebugTrigger(const CommandInfo &commandInfo)
{
  TriggerOnWheelTurn();
  SER("Data trigger\n\r");
}
void Command_TurnDataWriteDbg(const CommandInfo &commandInfo)
{
  s_manufData.m_lastSessionUnixtime = GetUnixtime();
  RefreshManufData();
  WriteDebugData(*turnData.m_collector);
  SER("Debug Data written\n\r");
}

void TurnDataInfo(uint32_t step)
{
  const bool terminalsReady = AreTerminalsReady();

  if (terminalsReady)
  {
    const uint32_t time = GetUnixtime();
    const bool isSessionExpired = IsExpired(*turnData.m_collector, time);

    if (step == 0)
    {
      SER("Next timer is set:%d (Unixtime:%d)\n", turnData.m_timerSet, turnData.m_NextTimer);
    }
    else if (step == 1)
    {
      SER("Set at Unixtime:%d\n", turnData.m_Time);
    }
    else if (step == 2)
    {
      SER("Session expired:%d\n", isSessionExpired);
    }
    else if (step == 3)
    {
      SER("Last error:%d\n", turnData.m_LastError);
    }
    
    step++;
  }

  if (!terminalsReady || step < 4)
  {
    GJEventManager->Add(std::bind(TurnDataInfo, step));
  }
}

void Command_TurnDataInfo(const CommandInfo &commandInfo)
{
  TurnDataInfo(0);
}

void Command_turndata(const char *command)
{
  static constexpr const char * const s_argsName[] = {
    "disp",
    "active",
    "clear",
    "debugtrigger",
    "writedbg",
    "info"
  };

  static void (*const s_argsFuncs[])(const CommandInfo &commandInfo){
    Command_TurnDataDisp,
    Command_TurnDataActive,
    Command_TurnDataClear,
    Command_TurnDataDebugTrigger,
    Command_TurnDataWriteDbg,
    Command_TurnDataInfo,
    };

  const SubCommands subCommands = {6, s_argsName, s_argsFuncs};

  SubCommandForwarder(command, subCommands);
}

DEFINE_COMMAND_ARGS(turndata, Command_turndata);
DEFINE_COMMAND_NO_ARGS(version, Command_Version);
DEFINE_COMMAND_NO_ARGS(tempdie, Command_TempDie);
DEFINE_COMMAND_NO_ARGS(batt, Command_ReadBattery);


#define BLE_SCHED_SER(message, ...) 
//SER("BLE sched %d:" message, GetLocalUnixtime(), ##__VA_ARGS__)

static bool s_enableBLEScheduleOff = true;

void OnBLEScheduleTimer()
{
  const uint32_t bleenabletime = GJ_CONF_INT32_VALUE(bleenabletime);
  const uint32_t time = GetLocalUnixtime(); 
  const uint32_t sinceMidnight = time % oneDay;
  static bool hasClient = false;
  
  bool enable = true;
  uint32_t timer = 60;

  if (hasClient || bleServer.HasClient())
  {
    BLE_SCHED_SER("has clients\n\r");
  }
  else if (GetElapsedMillis() < (60 * 1000))
  {
    BLE_SCHED_SER("early timer\n\r");
  }
  else if (sinceMidnight >= bleenabletime)
  {
    BLE_SCHED_SER("later than bleenabletime(%d)\n\r", bleenabletime);
    timer = oneDay - sinceMidnight;  //seconds to midnight
  }
  else 
  {
    BLE_SCHED_SER("midnight\n\r");

    //BLE server off is disabled when the module resets from a hard fault
    enable = s_enableBLEScheduleOff ? false : true;
    timer = bleenabletime - sinceMidnight;   //seconds to bleenabletime
  }

  hasClient = bleServer.HasClient();

  if (enable && !bleServer.IsInit())
  {
    const char *hostName = GetHostName();
    bleServer.Init(hostName, &ota);
    RefreshManufData();
  }
  else if (!enable && bleServer.IsInit())
  {
    bleServer.Term();
  }

  timer = Min<uint32_t>(timer, oneHour);
  timer = Max<uint32_t>(timer, 5);

  BLE_SCHED_SER("timer:%d\n\r", timer);
  const int64_t toMicros = 1000000;
  GJEventManager->DelayAdd(OnBLEScheduleTimer, timer * toMicros);
}

int main(void)
{
  for (uint32_t i = 0 ; i < 32 ; ++i)
    nrf_gpio_cfg_default(i);

  REFERENCE_COMMAND(tempdie);
  REFERENCE_COMMAND(version);
  REFERENCE_COMMAND(turndata);
  REFERENCE_COMMAND(batt);
  
  InitCrashDataCommand();

  Delay(100);

  InitMultiboot();

  InitializeDateTime();
  InitCommands(0);
  InitSerial();
  InitESPUtils();
  InitFileSystem("");

  InitConfig();
  PrintConfig();

  InitSoftDevice(CENTRAL_LINK_COUNT, PERIPHERAL_LINK_COUNT);

  ota.Init();

  uint32_t maxEvents = 4;
  GJEventManager = new EventManager(maxEvents);

  PrintVersion(0);

  uint32_t wheelPin = GJ_CONF_INT32_VALUE(wheelpin);
  int32_t wheelPinPull = GJ_CONF_INT32_VALUE(wheelpinpull);
  int32_t wheelPwrPin = GJ_CONF_INT32_VALUE(wheelpwrpin);
  int32_t wheelPwrOut = GJ_CONF_INT32_VALUE(wheelpwrout);
  
  turnData.m_sensor.SetPin(wheelPin, wheelPinPull);
  turnData.m_sensor.SetPolarity(DigitalSensor::Fall);
  turnData.m_sensorCB.SetOnChange(OnWheelTurn);
  turnData.m_sensor.EnableInterrupts(true);

  SetupPin(wheelPwrPin, false, 0);
  WritePin(wheelPwrPin, wheelPwrOut);
  
  uint32_t turnDataId = GJ_CONF_INT32_VALUE(wheeldataid);
  turnData.m_collector = InitDataCollector("/turndata", turnDataId, period);

  //disable BLE server off when resetting
  //This is to prevent turning off BLE serv outside of expected window because of desynchronized unixtime 
  s_enableBLEScheduleOff = false;

  OnBLEScheduleTimer();

  SER_COND(period != 60 * 15, "****PERIOD****\n\r");

  for (;;)
  {
      bleServer.Update();
      GJEventManager->WaitForEvents(0);

      bool bleIdle = bleServer.IsIdle();
      s_enableBLEScheduleOff |= bleServer.HasClient();  //reenable BLE serv off once a client connects
      bool evIdle = GJEventManager->IsIdle();
      bool const isIdle = bleIdle && evIdle;
      if (isIdle)
      {
          power_manage();
      }
  }
}
