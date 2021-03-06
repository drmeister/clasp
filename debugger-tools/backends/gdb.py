import gdb
import struct

ByteOrder = 'little'

def read_memory(address,len=8):
    i = gdb.inferiors()[0]
    #print "About to read_memory at 0x%x len: %d" % (address, len)
    mem = i.read_memory(address,len)
    #print "About to create fmt"
    fmt = ('<' if ByteOrder == 'little' else '>') + {1: 'B', 2: 'H', 4: 'L', 8: 'Q'}[len]
    #print "About to struct.unpack with fmt: |%s|" % fmt
    val = struct.unpack(fmt,mem)
    #print "Read and unpacked mem at 0x%x len: %d with fmt: %s and got: 0x%x" % (address,len,fmt,val[0])
    return val[0]

def test_debugger(arg):
    print("In udb test_debugger arg: %s" % arg)

def dump_memory(address):
    cmd0 = "x/8xg 0x%x" % (address-64)
    print("======dump before header")
    gdb.execute(cmd0)
    cmd = "x/16xg 0x%x" % address
    print("------Dump from header")
    gdb.execute(cmd)

    
def evaluate(string):
    return int(gdb.parse_and_eval(string))

def convenience_variable(name):
    return gdb.convenience_variable(name)

def set_convenience_variable(name,val):
    gdb.set_convenience_variable(name,val)
