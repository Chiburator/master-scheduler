import socket
import time

C_DELIMITER = b'\xff'
C_MESSSAGE_BEGIN = b'\x00'
C_MESSSAGE_CONTINUE = b'\x01'
C_MESSSAGE_END = b'\x02'
C_MESSAGE_LINE_END = b'\x0a'
bytes_to_send = 40
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

def upload_schedule_kiel(filepath, host, port):
  #time.sleep(1/100)
  time.sleep(1)
  with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
    s.connect((host, port))
    with open(filepath, "rb") as file:
      file.seek(0, 2)
      total_bytes = file.tell()
      print("len of file {}".format(total_bytes))
      file.seek(0, 0)
      bytes = C_MESSSAGE_BEGIN + file.read(bytes_to_write)

      while (bytes):
        bytes_to_send = ""

        for byte in bytes:
          bytes_to_send += byte_to_ascii(byte)

        print(bytes)
        print(bytes_to_send.encode('utf-8') + C_MESSAGE_LINE_END)
        s.send(bytes_to_send.encode('utf-8') + C_MESSAGE_LINE_END)


        bytes = file.read(bytes_to_write)

        if (not bytes):
          continue

        if (len(bytes) != bytes_to_write or file.tell() == total_bytes):
          bytes = C_MESSSAGE_END + bytes
        else:
          bytes = C_MESSSAGE_CONTINUE + bytes
        time.sleep(1 / 20)
        #time.sleep(1/5)
      time.sleep(1 / 20)
      #time.sleep(1 / 5)
    s.send(C_MESSAGE_LINE_END)

    #while(True):
    #  message = s.recv(1024)
    #  print(message)
    #s.close()

def upload_schedule_cooja(filepath, host, port):
  time.sleep(2)
  print("Lets go")
  with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
    s.connect((host, port))
    with open(filepath, "rb") as file:
      file.seek(0, 2)
      total_bytes = file.tell()
      print("len of file {}".format(file.tell()))
      file.seek(0, 0)
      bytes = C_MESSSAGE_BEGIN + file.read(bytes_to_write)

      while (bytes):
        bytes_to_send = ""

        for byte in bytes:
          bytes_to_send += byte_to_ascii(byte)

        print(bytes)

        print(bytes_to_send.encode('utf-8'))
        s.send(bytes_to_send.encode('utf-8'))

        bytes = file.read(bytes_to_write)

        if (not bytes):
          continue

        #TODO wenn die letzte zeile = bytes_to_write, wird immer continue gesendet und nicht END
        if (len(bytes) != bytes_to_write or file.tell() == total_bytes):
          bytes = C_MESSSAGE_END + bytes
        else:
          bytes = C_MESSSAGE_CONTINUE + bytes
        time.sleep(1 / 5)
      time.sleep(1)
    s.send(C_MESSAGE_LINE_END)

    #while(True):
    #  message = s.recv(1024)
    #  print(message)
    #s.close()

if __name__ == '__main__':
  upload_schedule_cooja("meinTest.bin", '127.0.0.1', 60007)
