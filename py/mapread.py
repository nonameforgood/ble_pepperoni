import re
from inspect import getmembers
from pprint import pprint
import sys

class Object(object):
    pass

roDataName = ".rodata"
textName = ".text"
totalSize = 0
def ReadMapFile(filepath, symFilter):

  mapFile = open(mapFilePath, "r")  
  foundLinkerSection = False
  lastSymbol = 0
  
  libs = {roDataName:{}, textName:{}}

  posText = 0
  posRoData = 0

  for line in mapFile:
    #print(line)
    if foundLinkerSection == False:
      pos = line.find("Linker script and memory map")
      if pos != -1:
        #print("Found linker section")
        foundLinkerSection = True

    if foundLinkerSection:

      if lastSymbol == 0:
      
        posText = line.find(textName)
        posRoData = line.find(roDataName)

        if posText != -1 or posRoData != -1:
          lastSymbol = line

      pos = line.find(".o")
      if pos != -1 and lastSymbol != 0:

        symName = lastSymbol

        lastSymbol = 0
        m = re.search('\s*([\.a-zA-Z0-9_]*)\s*(0x[0-9xa-fA-F]+)\s*(0x[0-9xa-fA-F]+)\s*(.*)', line)

        if len(m.group(1)) != 0:
          symName = m.group(1)

        #print(symName)
        #print(line)
        #pprint(m.groups())
        #break
        #print(line)
        #print(m.group(2))
        address = int(m.group(2), 16)
        size = int(m.group(3), 16)
        if address >= 0x20000000:
          continue

        if symFilter != None and symName.find(symFilter) == -1:
          continue

        libName = m.group(4)

        lastSlash = libName.rfind("/")
        if lastSlash != -1:
          libName = libName[lastSlash:]

        memType = roDataName
        if posText != -1:
          memType = textName

        if libs[memType].get(libName) == None:
          newLib = Object()
          newLib.size = 0
          newLib.minAddress = 0xffffffff
          newLib.maxAddress = 0
          newLib.fill = 0
          newLib.symbols = []
          libs[memType][libName] = newLib
          
        lib = libs[memType][libName]
        lib.minAddress = min(lib.minAddress, address)
        lib.maxAddress = max(lib.maxAddress, address + size)
        lib.size += size

        symName = symName.strip() 
        memTypeLen = len(memType)
        symName = symName[memTypeLen:]
        if len(symName) > 0 and symName[0] == '.':
          symName = symName[1:]

        symbol = Object()
        symbol.name = symName
        symbol.size = size

        lib.symbols.append(symbol)
      
  return libs
  

def PrintSection(name, section):
  global totalSize

  print("*********************")
  print("{0}".format(name))
  print("*********************")
  
  sorted_keys = sorted(section.keys())

  for libName in sorted_keys:
    lib = section[libName]
    totalSize += lib.size

    text = "{0:8} {1}".format(lib.size, libName)
    print(text)

    sortedSymbols = sorted(lib.symbols, key=lambda sym: sym.name)
    for symbol in sortedSymbols:
      print("         {1:8} {0}".format(symbol.name, symbol.size))

  #text = "Section info: MinAdr = 0x{0:08x} MaxAdr = 0x{1:08x}".format(minAddress, maxAddress)
  #print(text)

argv = sys.argv

mapFilePath = argv[1]
symFilter = None

for i in range(2, len(argv)):
  v = argv[i]
  testString = "/symfilter:"
  if v.find(testString) != -1:
    symFilter = v[len(testString):]
    print("Symfilter:{0}".format(symFilter))


libs = ReadMapFile(mapFilePath, symFilter)

PrintSection(roDataName, libs[roDataName])
PrintSection(textName, libs[textName])

#pprint(libs)
print("Total size {0}".format(totalSize))
