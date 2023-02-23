#include "datacollector.h"
#include "gj/eventmanager.h"
#include "gj/appendonlyfile.h"
#include "gj/datetime.h"

#define DELAY_ADD_EVENT(f, d) GJEventManager->DelayAdd(f, d)

struct DataSession
{
  static constexpr uint32_t MaxData = 16;
  uint32_t m_time = 0;
  uint32_t m_id = 0;
  uint32_t m_period = 0;  //seconds
  uint16_t m_values[MaxData] = {};
};

struct DataCollector
{
  char m_path[16];
  DataSession m_dataSession;
};

void DisplayDataSession(const DataSession &session)
{
  const uint16_t *v = session.m_values;

  char buffer[300];
  uint32_t len = sprintf(buffer, "id:%d t:%d p:%d",
      session.m_id,
      session.m_time,
      session.m_period);

  for (int i = 0 ; i < DataSession::MaxData ; ++i)
  {
    char tempBuffer[20];

    uint16_t value = v[i];
    sprintf(tempBuffer, " %d", (uint16_t)value);

    strcat(buffer, tempBuffer);
  }

  SER("%s\n", buffer); 
}

bool DisplayDataSessionAtIndex(const DataCollector *collector, uint32_t index, uint32_t minTime)
{
  uint32_t blockIndex = 0;
  bool read = false;

  auto onBlock = [&](uint32_t size, const void *data)
  {
    const DataSession *session = (DataSession*)data;
    const DataSession *sessionEnd = (DataSession*)((char*)data + size);

    while(session < sessionEnd)
    { 
      if (session->m_time < minTime)
      {
        session++;
        continue;
      }

      if (blockIndex == index)
      {
        read = true;

        DisplayDataSession(*session);
      }

      session++;
      blockIndex++;
    }
  };

  AppendOnlyFile file(collector->m_path);
  file.ForEach(onBlock);

  bool done = !read;
  return done;
}

void DisplayDataSessions(const DataCollector *collector, uint32_t index, uint32_t minTime)
{
  static uint32_t sessionCount = 0;

  if (!AreTerminalsReady())
  {
    EventManager::Function f;
    f = std::bind(DisplayDataSessions, collector, index, minTime);
    DELAY_ADD_EVENT(f, 100 * 1000);
    return;
  }

  if (index == 0)
  {
    SER("Data sessions:\n\r");
    sessionCount = 0;
  }

  bool done = DisplayDataSessionAtIndex(collector, index, minTime);    

  if (!done)
  {
    sessionCount ++;
    index++;
    EventManager::Function f;
    f = std::bind(DisplayDataSessions, collector, index, minTime);
    GJEventManager->Add(f);
  }
  else
  {
    SER("Total readings:%d\n\r", sessionCount);
  }
}

void Display(const DataCollector &collector, uint32_t minTime)
{
  DisplayDataSessions(&collector, 0, minTime);
}

void WriteDataSession(const char *path, const DataSession &session)
{
  AppendOnlyFile file(path);

  const uint32_t size = sizeof(DataSession);

  if (!file.BeginWrite(size))
  {
    file.Erase();
    SER("Session file erased\n\r");
    bool ret = file.BeginWrite(size);
    APP_ERROR_CHECK_BOOL(ret);
  }
  file.Write(&session, size);
  file.EndWrite();

  SER("Session file written\n\r");
}

void WriteDataSession(DataCollector &collector)
{
  WriteDataSession(collector.m_path, collector.m_dataSession);
}

bool AddSessionData(DataSession &session, uint32_t time, uint16_t value)
{
  bool success = false;

  for (int i = 0 ; i < DataSession::MaxData ; ++i)
  {
    uint32_t beginTime = session.m_time + i * session.m_period;
    uint32_t endTime = beginTime + session.m_period;
    if (time >= beginTime && time < endTime)
    {
      session.m_values[i] += value;
      success = true;
      break;
    }
  }

  return success;
}

void InitDataSession(DataSession &session, uint32_t time)
{
  session.m_time = time;
  memset(session.m_values, 0, sizeof(session.m_values));

  SER("Init data session time:%d\n\r", session.m_time);
}

void InitDataSession(DataCollector &collector, uint32_t time)
{
  InitDataSession(collector.m_dataSession, time);
}

bool IsExpired(const DataCollector &collector)
{
  const DataSession &session = collector.m_dataSession;

  const uint32_t currentTime = GetUnixtime();
  const uint32_t sessionEndTime = session.m_time + session.m_period * DataSession::MaxData;
  const bool expired = currentTime >= sessionEndTime;

  return expired;
}

bool AddData(DataCollector &collector, uint32_t time, uint16_t value)
{
  DataSession &session = collector.m_dataSession;

  if (session.m_time == 0)
    InitDataSession(session, time);

  const bool success = AddSessionData(session, time, value);
  return success;
}

void ClearStorage(DataCollector &collector)
{
  AppendOnlyFile file(collector.m_path);
  file.Erase();
  SER("cleared\n\r");
}

uint32_t GetBlockEndTime(const DataCollector &collector)
{
  const DataSession &dataSession = collector.m_dataSession;

  const uint32_t time = GetUnixtime();

  for (int i = 0 ; i < DataSession::MaxData ; ++i)
  {
    uint32_t beginTime = dataSession.m_time + i * dataSession.m_period;
    uint32_t endTime = beginTime + dataSession.m_period;
    if (time >= beginTime && time < endTime)
    {
      return endTime;
    }
  }

  return dataSession.m_time + DataSession::MaxData * dataSession.m_period;
}

void WriteDebugData(DataCollector &collector)
{
  DataSession session;
  InitDataSession(session, 1);
  session.m_id = 0xaabbccdd;
  session.m_period = 1;
  for (int i = 0 ; i < DataSession::MaxData ; ++i)
    session.m_values[i] = i;

    WriteDataSession(collector.m_path, session);
}

DataCollector* InitDataCollector(const char *path, uint32_t id, uint32_t period)
{
  DataCollector *collector = new DataCollector;
  strcpy(collector->m_path, path);
  
  collector->m_dataSession.m_id = id;
  collector->m_dataSession.m_period = period;

  InitDataSession(collector->m_dataSession, GetUnixtime());

  return collector;
}