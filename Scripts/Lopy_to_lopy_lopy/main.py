from network import LoRa
import socket
import time
import ubinascii


# Please pick the region that matches where you are using the device

lora = LoRa(mode=LoRa.LORA, region=LoRa.EU868)
s = socket.socket(socket.AF_LORA, socket.SOCK_RAW)
s.setblocking(False)
app_eui = ubinascii.unhexlify('0000000000000000')
app_key = ubinascii.unhexlify('F87245C083E9125C7978352A58CA3E3A')
#uncomment to use LoRaWAN application provided dev_eui
dev_eui = ubinascii.unhexlify('a91e9b43532f9ae9')

i = 0
while True:
    s.send(app_key)
    print('Ping {}'.format(i))
    i= i+1
    time.sleep(5)
