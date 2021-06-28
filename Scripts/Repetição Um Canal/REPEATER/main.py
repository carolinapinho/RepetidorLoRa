from network import LoRa
import socket
import time
import ubinascii

# Initialise LoRa in LORAWAN mode.
# Please pick the region that matches where you are using the device:
# Asia = LoRa.AS923
# Australia = LoRa.AU915
# Europe = LoRa.EU868
# United States = LoRa.US915
def lora_raw_listen(data):
    lora = LoRa(mode=LoRa.LORAWAN, region=LoRa.EU868)
    lora.nvram_restore()
    #while not lora.has_joined():
    #    time.sleep(0.5)
    #    print('Not yet joined...')
    s.setblocking(True)
    s.send(data)
    s.setblocking(False)
    lora.nvram_save()
    print('2')

lora = LoRa(mode=LoRa.LORAWAN, region=LoRa.EU868)

# create an OTAA authentication parameters, change them to the provided credentials
app_eui = ubinascii.unhexlify('0000000000000000')
app_key = ubinascii.unhexlify('4af4d1992c97c869146d657f443acedf')
#uncomment to use LoRaWAN application provided dev_eui


lora.join(activation=LoRa.OTAA, auth=(app_eui, app_key), timeout=0)

# wait until the module has joined the network
while not lora.has_joined():
    time.sleep(2.5)
    print('Not yet joined...')

print('Joined')
lora.nvram_save()
# create a LoRa socket
s = socket.socket(socket.AF_LORA, socket.SOCK_RAW)
s_raw = socket.socket(socket.AF_LORA, socket.SOCK_RAW)

# set the LoRaWAN data rate
s.setsockopt(socket.SOL_LORA, socket.SO_DR, 5)
s_raw.setsockopt(socket.SOL_LORA, socket.SO_DR, 5)

# make the socket blocking
# (waits for the data to be sent and for the 2 receive windows to expire)
s.setblocking(False)
s_raw.setblocking(True)

while True:
    lora = LoRa(mode=LoRa.LORA, region=LoRa.EU868)
    data=s_raw.recv(64)
    print(data)
    if len(data) != 0:
        print('1')
        s_raw.send('1')
        s_raw.setblocking(False)
        time.sleep(1)
        lora_raw_listen(data)
    time.sleep(5)
