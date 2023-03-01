#include "gj/base.h"

struct DataCollector;

void WriteDebugData(DataCollector &collector);

DataCollector* InitDataCollector(const char *path, uint32_t id, uint32_t period);
uint32_t GetBlockEndTime(const DataCollector &collector);

int32_t GetSessionTime(const DataCollector &collector);

bool AddData(DataCollector &collector, uint32_t time, uint16_t value);
void WriteDataSession(DataCollector &collector);
void InitDataSession(DataCollector &collector, uint32_t time);
bool IsExpired(const DataCollector &collector, uint32_t time);
void ClearStorage(DataCollector &collector);
void Display(const DataCollector &collector, uint32_t minTime);
