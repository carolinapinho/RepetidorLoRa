from network import LoRa
import socket
import time
import ubinascii

# Initialise LoRa in LORAWAN mode.
# Please pick the region that matches where you are using the device
# Asia = LoRa.AS923
# Australia = LoRa.AU915
# Europe = LoRa.EU868
# United States = LoRa.US915
lora = LoRa(mode=LoRa.LORAWAN, region=LoRa.EU868)

# create an OTAA authentication parameters, change them to the provided credentials
app_eui = ubinascii.unhexlify('14a97ce23e7bd732')
app_key = ubinascii.unhexlify('4af4d1992c97c869146d657f443acedf')
#uncomment to use LoRaWAN application provided dev_eui
#dev_eui = ubinascii.unhexlify('a91e9b43532f9ae9')

# Uncomment for US915  AU915 & Pygate
# for i in range(0,8)
#     lora.remove_channel(i)
# for i in range(16,65)
#     lora.remove_channel(i)
# for i in range(66,72)-
#     lora.remove_channel(i)

# join a network using OTAA (Over the Air Activation)
#uncomment below to use LoRaWAN application provided dev_eui
lora.join(activation=LoRa.OTAA, auth=(app_eui, app_key), timeout=0)
#lora.join(activation=LoRa.OTAA, auth=(dev_eui, app_eui, app_key), timeout=0)

# wait until the module has joined the network
while not lora.has_joined():
    time.sleep(2.5)
    print('Not yet joined...')

print('Joined')
# create a LoRa socket
s = socket.socket(socket.AF_LORA, socket.SOCK_RAW)

# set the LoRaWAN data rate
s.setsockopt(socket.SOL_LORA, socket.SO_DR, 5)

# make the socket blocking
# (waits for the data to be sent and for the 2 receive windows to expire)
s.setblocking(True)

# send some data

while True:
    data = "testes";
    s.send(data.encode())
    s.setblocking(False)
    data = s.recv(64)
    time.sleep(10)


# make the socket non-blocking
# (because if there's no data received it will block forever...)
s.setblocking(False)

# get any data received (if any...)

print(data)
