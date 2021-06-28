from network import LoRa
import socket
import time

# Please pick the region that matches where you are using the device

lora = LoRa(mode=LoRa.LORA, region=LoRa.EU868)
s = socket.socket(socket.AF_LORA, socket.SOCK_RAW)
s.setblocking(False)
i = 0
while True:
    (bytes, address)=s.recvfrom(64)  #permite obter o address do dispositivo tambem
    if len(bytes) != 0:
        print('Pong {}'.format(bytes))
        s.send(bytes)
        print('Pongadd {}'.format(address))
        i = i+1
    time.sleep(5)
