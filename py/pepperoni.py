import sys
import gc
import asyncio
from bleak import BleakScanner
from bleak import BleakClient
from bleak.backends.device import BLEDevice
from bleak.backends.scanner import AdvertisementData
import logging
import traceback
import urllib.request
import ssl
from datetime import datetime
import time
import os
from pathlib import Path

logging.basicConfig()

def AddLog(s:str):
    print(datetime.now(), ":", s)

def GetUnixtime():
  ct = datetime.now()
  unixtime = time.mktime(ct.timetuple())
  return unixtime

def GetSessionTime(t):
  def GetVarValue(t):
    words = t.split(":")
    return words[1]

  words = t.split(" ")

  time = GetVarValue(words[1])

  return int(time)

def GetSessionPeriod(t):
  def GetVarValue(t):
    words = t.split(":")
    return words[1]

  words = t.split(" ")

  period = GetVarValue(words[2])

  return int(period)

def GetSessionValueCount(t):
  def GetVarValue(t):
    words = t.split(":")
    return words[1]

  words = t.split(" ")
  return len(words) - 3
 
def SendServerRequest(server, url, urlParams):

  url = server + url

  headers = {
    'User-Agent': 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15; rv:78.0) Gecko/20100101 Firefox/78.0',
    'Accept': 'text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8',
    'Content-Type': 'application/x-www-form-urlencoded'

  }

  urlParams = urllib.parse.urlencode(urlParams)
  data = urlParams.encode('ascii') # data should be bytes
  req = urllib.request.Request(url, data, headers)

  AddLog(req.full_url +urlParams)
  
  context = ssl.create_default_context()

  try:
    opener = urllib.request.build_opener(urllib.request.HTTPHandler(debuglevel=0))
    urllib.request.install_opener(opener)
    with opener.open(req) as r:
      AddLog(r.read(100))
        
  except Exception as e:
    AddLog("http error")
    AddLog(req.header_items())
    if hasattr(e, 'code'):
      AddLog(e.code)
    if hasattr(e, 'reason'):
      AddLog(e.reason)
    if hasattr(e, 'headers'):
      AddLog(e.headers)


def SendTempSession(server, deviceName, t):
  def GetVarValue(t):
    words = t.split(":")
    return words[1]

  words = t.split(" ")

  cnt = GetVarValue(words[0])
  time = GetVarValue(words[1])
  period = GetVarValue(words[2])
  chunk = ",".join(words[3:len(words)])
  
  url = server + "/pepperoni/"

  urlParams = {
    'mod' : deviceName,
    'cnt' : cnt,
    'prd' : period,
    'time' : time,
    'readings' : chunk }

  SendServerRequest(server, "/pepperoni/", urlParams)

  return int(time)

async def SendCommand(instance, client, char, cmd):
  command = "gjcommand:" + cmd
  #AddLog("command:" + command)
  encoded_string = command.encode()
  byte_array = bytearray(encoded_string)
  await client.write_gatt_char(char, byte_array)

def UploadReadings(instance):
  
  if os.path.exists(instance.newReadingsFilepath) == False:
    return

  server = instance.server
  deviceName = instance.device.name

  uniqueSessions = set()

  if os.path.exists(instance.readingsFilepath):
    #create a set with unique readings
    readings = open(instance.readingsFilepath, "r")  
    for session in readings:
      uniqueSessions.add(session)
    readings.close()

  readings = open(instance.readingsFilepath, "a")
  newReadings = open(instance.newReadingsFilepath, "r")
  uploadCount = 0
  for session in newReadings:
    if session not in uniqueSessions:
      uniqueSessions.add(session)
      readings.write(session)
      session = session.replace("\n", "");
      SendTempSession(server, deviceName, session)
      uploadCount += 1

  readings.close()
  newReadings.close()
  AddLog("Uploaded {0} temperature sessions".format(uploadCount))

  os.remove(instance.newReadingsFilepath)

async def ReadValues(instance, client):
  svcs = await client.get_services()

  writeSrv = None
  readSrv = None

  for service in svcs:
      #AddLog("service {0}".format(service.uuid))
      #if service.uuid == "000000ff-0000-1000-8000-00805f9b34fb":
      #    writeSrv = service;
      if service.uuid == "000000ee-0000-1000-8000-00805f9b34fb":
          writeSrv = service;

  for char in writeSrv.characteristics:
    if char.uuid == "0000ee01-0000-1000-8000-00805f9b34fb":
      writeChar = char

  lastReceived = GetUnixtime()

  fullLine = ""
  done = False
  readingsCount = 0
  sessions = []

  def OnReceive(sender, data):
    nonlocal fullLine
    nonlocal done
    nonlocal readingsCount
    nonlocal sessions
    nonlocal lastReceived

    lastReceived = GetUnixtime()

    str = bytearray(data).decode("utf-8") 
    fullLine += str

    pos = fullLine.find("\n")
    if pos == -1:
      return

    fullLine = fullLine.replace("\n", "")
    AddLog(fullLine)

    if fullLine.find("Data readings") != -1:
      readingsCount = 0
    elif fullLine.find("Total readings:") != -1:
      readingsCount = int(fullLine[15:])
      done = True
    elif fullLine.find("id:") != -1 and fullLine.find("t:") != -1 and fullLine.find("p:") != -1:
      sessions.append(fullLine)

    fullLine = ""

  await client.start_notify(writeChar, OnReceive)

  #newestSessionTime is used to transfer new readings only
  #otherwise all readings are sent on each query until a clear is executed
  dispCommand = "turndata disp " + str(instance.newestSessionTime + 1)
  await SendCommand(instance, client, writeChar, dispCommand)
  AddLog("disp command:" + dispCommand)
  while done == False:
    elapsed = GetUnixtime() - lastReceived
    if elapsed >= 5:
      break
    await asyncio.sleep(1)

  await client.stop_notify(writeChar)

  if readingsCount != len(sessions):
    raise ValueError('Not all Readings were transfered')

  localFile = open(instance.newReadingsFilepath, "a")

  minTime = GetUnixtime()
  maxTime = 0
  for session in sessions:
    fileSession = session + "\n"
    localFile.write(fileSession)
    sessionTime = GetSessionTime(session)
    #period = GetSessionPeriod(session)
    #tempCount = GetSessionValueCount(session)
    minTime = min(minTime, sessionTime)
    maxTime = max(maxTime, sessionTime)

  localFile.close()

  if instance.oldestSessionTime == 0:
    instance.oldestSessionTime = minTime

  AddLog("transfered {0} data sessions".format(len(sessions)))

  elapsedSinceOldest = GetUnixtime() - instance.oldestSessionTime
  secondsIn3Days = 3 * 24 * 60 * 60

  #clear readings once in a while.
  #but not too often to avoid flash wear
  #this can duplicate readings in the webserver data file
  #and must be handled accordingly
  if elapsedSinceOldest >= secondsIn3Days and instance.oldestSessionTime != 0:
    await SendCommand(instance, client, writeChar, "turndata clear")
    instance.oldestSessionTime = 0
    AddLog("Data sessions cleared")

  #add 1 to skip the last recorded timestamp
  instance.newestSessionTime = max(maxTime, instance.newestSessionTime)

def ReadAdvertData(instance):
  AddLog("Reading advert data...")

  #AddLog(dir(instance.device))
  #AddLog(dir(instance.device.metadata))
  instance.device.metadata.update()

  b = instance.device.metadata['manufacturer_data']

  if len(b):
    b = b[65535]

    unixtime = 0

    #AddLog("Advert len {0} data {1}".format(len(b), b));

    if b[0] == 0x05 and b[1] == 0xff and len(b) == 6:
      unixtime = b[2] | (b[3] << 8) | (b[4] << 16) | (b[5] << 24) 

  return unixtime

async def ReadRecordedSessions(instance):
  exceptionCount = 0
  while True:
      if exceptionCount > 5:
          break
      try:
          
          #nr = datetime.fromtimestamp(instance.newestSessionTime).strftime('%Y-%m-%d %H:%M:%S')
          #AddLog("Newest session:" + nr + "(" + str(instance.newestSessionTime) + ")")

          #_4Hours = 60 * 60 * 4
          #_1Hours = 60 * 60 * 1

          #elapsed = GetUnixtime() - instance.newestSessionTime
          #readElapsed = time.time() - instance.lastSessionRead

          #if elapsed >= _4Hours and readElapsed >= _1Hours:
          AddLog("Connecting to device...")
          
          async with BleakClient(instance.device) as client:
            await client.is_connected()
            AddLog("Connected")
            #todo: set uc module time
            await ReadValues(instance, client)
            await client.disconnect()
            client = None
            #instance.lastSessionRead = time.time()


            
            #del(client)
            
          break

      except Exception as e:
          logger = logging.getLogger(__name__)
          tb = traceback.format_exc()
          logger.warning("exception in ReadRecordedSessions for {2} : {0} {1}".format(e, tb, instance.id))
          exceptionCount += 1
          #exit()

async def FindDevice(instance, name:str):
  scanner = BleakScanner()
  await scanner.start()
  target = None

  foundDeviceAddresses = set()
  foundDevices = set()
  beginTime = time.time()
  timeout = 60
  while target is None:
      devices = await scanner.get_discovered_devices()
      for device in devices:
          if device.address not in foundDeviceAddresses:
              #AddLog(device)
              foundDeviceAddresses.add(device.address)
              foundDevices.add(device)
          #AddLog(device)
          if device.name == name:
              target = device
              instance.address = target.address
              AddLog("found '{0}' {1}".format(name, target.address))
              break
      if (time.time() - beginTime) > timeout:
          break;
      await asyncio.sleep(0.5)

  await scanner.stop()

  if target is None:
    AddLog("Searching failed")
    AddLog("Scanned devices:")
    for device in foundDevices:
      AddLog(device)
  else:
    #AddLog("Searching done")
    instance.device = target
    instance.id = id

async def ReadDevice(instance, name:str):
  errors = 0
  while True:
      if errors > 5:
          errors = 0
          await asyncio.sleep(30)
          instance.device = None  #force rescan
      try:
          if instance.device is None:
            await FindDevice(instance, name)

          if instance.device is not None:
            advertSessionTime = ReadAdvertData(instance)
            AddLog("Advertized session time {0} Last Read Session time {1}".format(advertSessionTime, instance.newestSessionTime))

            if advertSessionTime != 0 and advertSessionTime != instance.newestSessionTime:
              await ReadRecordedSessions(instance)
              UploadReadings(instance)

            instance.device = None  #advert data not refreshed otherwise


          timeout = 60 * 15        #sleep 15 minutes

          nr = datetime.fromtimestamp(GetUnixtime() + timeout).strftime('%Y-%m-%d %H:%M:%S')
          AddLog("Next read will occur at {0}".format(nr))
        
          errors = 0
          
          await asyncio.sleep(timeout)    

      except Exception as e:
          logger = logging.getLogger(__name__)
          tb = traceback.format_exc()
          logger.warning("exception in ReadDevice for '{2}': {0} {1}".format(e, tb, instance.id))
          errors += 1


#run this script from a .plist file on macos
#/Users/michelvachon/Library/LaunchAgents/pepperoni_launcher.plist

#create login item:
#launchctl load /Users/michelvachon/Library/LaunchAgents/pepperoni_launcher.plist
#unload/stop
#launchctl unload /Users/michelvachon/Library/LaunchAgents/pepperoni_launcher.plist

async def run():

    mod = "peppe"

    argv = sys.argv

    script_dir = str(Path( __file__ ).parent.absolute() )
    
    instance = type('', (), {})()
    instance.newestSessionTime = 0
    instance.oldestSessionTime = 0
    instance.server = "https://devtest.michelvachon.com"
    instance.readingsFilepath = script_dir+"/readings.txt"
    instance.newReadingsFilepath = script_dir+"/newreadings.txt"
    instance.device = None
    instance.debug = False

    AddLog(argv)
    if len(argv) > 1:
      mod = argv[1]
      
      if mod == "peppeX":
        AddLog("Dev mode")
        instance.readingsFilepath = script_dir+"/readingsB.txt"
        instance.newReadingsFilepath = script_dir+"/newreadingsB.txt"
        instance.debug = True
      else:
        AddLog("Unknown module name")
        exit(0)
    
    AddLog("Server:" + instance.server)
    AddLog("Readings:" + instance.readingsFilepath)
    AddLog("New readings:" + instance.newReadingsFilepath)

    await asyncio.create_task(ReadDevice(instance, mod))


loop = asyncio.get_event_loop()
loop.run_until_complete(run())
