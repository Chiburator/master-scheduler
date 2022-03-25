from re import search as re_search
from collections import defaultdict
node_to_nbr_to_etx = defaultdict(dict)
source_to_dest_has_prr = defaultdict(dict)

input_file_path = "F:\WS_21_22_MasterThesis\master-scheduler\Master\Logs\EvalEBPrecision/5Nodes2.txt"
input_file_path2 = "F:\WS_21_22_MasterThesis\master-scheduler-original\Master\Logs\EvalEBPrecision/5Nodes2.txt"

dgrm_file_path = "F:\WS_21_22_MasterThesis\master-scheduler\Master\DGRM_configurations\DGRM5Test2.conf"

regex_etx = r'.*ID:(\d+)\tETX:(\d+) = (\d+), last (\d+), counted (\d+)'

regex_sent_old = r'.*sent;(\d+);(\d+).*'
regex_received_old = r'.*rcvd;(\d+);(\d+);\d+;(\d+).*'

dgrm_re = r'(\d+) (\d+) (\d+.\d+).*'


#Get probabilites first

with open(dgrm_file_path, 'r') as file:
  for line in file:
    match = re_search(dgrm_re, line)

    if(match):
      start_node = int(match.group(1))
      receiver_node = int(match.group(2))
      prr = round(float(match.group(3)) * 100, 2)
      source_to_dest_has_prr[start_node][receiver_node] = prr

  print(source_to_dest_has_prr)

#Get testresult from new MASTER

def get_new_master_accuracy():
  print("--------- MASTER ----------------")
  with open(input_file_path, 'r') as file:
    for line in file:
      match = re_search(regex_etx, line)

      if(match):
        node = int(match.group(1))
        neighbor = int(match.group(2))
        etx_link = int(match.group(3))
        last = int(match.group(4))
        received = int(match.group(5))

        node_to_nbr_to_etx[node][neighbor] = round((received / last) * 100, 2)

  sorted_dict = sorted(node_to_nbr_to_etx.items())

  for node, nbrs_to_etx in sorted_dict:
    metric = ""
    for nbr, measured_prr in nbrs_to_etx.items():
      metric += " {}={},".format(nbr, etx_link)
      real_prr = source_to_dest_has_prr[nbr][node]
      print("diff rec {} to send {} is dgrm {} vs test {}. Acc = {}".format(node, nbr, real_prr, measured_prr, 100 - (abs(measured_prr - real_prr) / real_prr) * 100))

    #print("{}: {}".format(node, metric[1:-1]))

#get testresult from old MASTER
def get_old_master_accuracy():
  sender_sent = defaultdict(list)
  receiver_to_sender_to_packets = defaultdict(lambda: defaultdict(list))
  print("--------- Default MASTER ----------------")
  with open(input_file_path2, 'r') as file:
    for line in file:
      match = re_search(regex_received_old, line)

      if match:
        receiver = int(match.group(1))  # == node_id
        sender = int(match.group(2))
        packet_number = int(match.group(3))

        receiver_to_sender_to_packets[receiver][sender].append(packet_number)

      else:
        match = re_search(regex_sent_old, line)  # sent;<node_id>;<number>

        if match:
          packet_number = int(match.group(2))
          sender = int(match.group(1))

          sender_sent[sender].append(packet_number)

    sorted_dict = sorted(receiver_to_sender_to_packets.items())

    for receiver, sender_to_received_packets in sorted_dict:
      for sender, received_packets in sender_to_received_packets.items():
        real_prr = source_to_dest_has_prr[sender][receiver]
        measured_prr = round((len(received_packets) / len(sender_sent[sender])) * 100, 2)
        #print("receiver {} got from {} packets {} vs sent {} ".format(receiver, sender, len(received_packets), len(sender_sent[sender])))
        print("diff for rec {} to send {} is dgrm {} vs test {}. Acc = {}".format(receiver, sender, real_prr, measured_prr, 100 - (abs(measured_prr - real_prr) / real_prr) * 100))


get_new_master_accuracy()
get_old_master_accuracy()

