from machine import UART
import machine
from machine import RTC

import time
import os
import gc

rtc = machine.RTC()
rtc.init((2014, 5, 1, 4, 13, 0, 0, 0))
fp = open('/flash/config.json','r')
buf = fp.read()
rtc = machine.RTC()
rtc.now()
# Start the Pygate
machine.pygate_init(buf)

time.sleep(3)

while True:
    print("ola")
    time.sleep(2)
