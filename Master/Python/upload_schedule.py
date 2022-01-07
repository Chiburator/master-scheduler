import socket
import time

C_DELIMITER = b'\xff'
C_MESSSAGE_BEGIN = b'\x00'
C_MESSSAGE_CONTINUE = b'\x01'
C_MESSSAGE_END = b'\x02'
bytes_to_send = 20
bytes_to_write = bytes_to_send - 1

int_to_hex = {10: 'A', 11:'B', 12:'C', 13:'D', 14:'E', 15:'F'}

def byte_to_ascii(byte):
  higherNibble = (byte >> 4) & int.from_bytes(b'\x0F', byteorder="big")

  if(higherNibble > 9):
    higherNibble = int_to_hex[higherNibble]


  lowerNibble = byte & int.from_bytes(b'\x0F', byteorder="big")

  if(lowerNibble > 9):
    lowerNibble = int_to_hex[lowerNibble]

  char = '{higherNibble}{lowerNibble}'.format(higherNibble=higherNibble,
                                              lowerNibble=lowerNibble)
  return char

def upload_schedule(filepath):
  with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
    s.connect(('127.0.0.1', 60001))
    with open(filepath, "rb") as file:
      bytes = C_MESSSAGE_BEGIN + file.read(bytes_to_write)

      while(bytes):
        bytes_to_send = ""

        for byte in bytes:

          bytes_to_send += byte_to_ascii(byte)

        print(bytes)
        s.send(bytes_to_send.encode('utf-8'))

        bytes = file.read(bytes_to_write)

        if(not bytes):
          continue

        if(len(bytes) < bytes_to_write):
          bytes = C_MESSSAGE_END + bytes
        else:
          bytes = C_MESSSAGE_CONTINUE + bytes
        time.sleep(1/5)

if __name__ == '__main__':
  upload_schedule("MeinTest.bin")