import os
from re import search as re_search
from enum import Enum
from collections import defaultdict

class Data_table(Enum):
  etx = 0
  prr = 1
  rssi = 2

class Parser_EB(object):

  def __init__(self, valid_node_ids, folder_path, filename=None, max_etx_bound=None, min_rssi_bound=None):
    self.valid_node_ids = valid_node_ids
    if folder_path[-1] == '/':
      self.folder = folder_path
    else:
      self.folder = folder_path + '/'
    self.filename = filename
    self.max_etx_bound = max_etx_bound
    self.min_rssi_bound = min_rssi_bound
    self.etx_metric = {} #{1 : {node-id : etx_value , ...}, 2 : {...}, ...}#
    self.parse_graphs = []
    self.graph_etx = defaultdict(dict)
    self.schedule_finished = False

  def match_neighbor_data(self, line):
    match = re_search(r'ETX-Links - FROM (\d); ((\d:\d{1,3}.\d,? ?)+)', line)  # ETX-Links - FROM 1; 4:1.2, 3:1.2, 2:2.0, 4:2.2
    if match:
      receiver = int(match.group(1))  # == node_id

      sender_and_etx_values = match.group(2).strip().split(', ') # get the list os "sender:etx_value"

      for sender_and_etx_value in sender_and_etx_values:
        sender, etx_value = sender_and_etx_value.split(':')
        self.graph_etx[int(sender)][int(receiver)] = float(etx_value)

  def parse_file(self, filepath):
    print(filepath)

    with open(filepath, encoding="utf8", errors='ignore') as f:
      self.parse_graphs.append({})
      for line in f:
        match = re_search(r'ETX-Links finished!',line)  # ETX-Links finished
        if match:
          self.schedule_finished = True
        self.match_neighbor_data(line)

  def print_parsed_data_table(self, table_type):
    if table_type == Data_table.etx: #Expected transmission count
      output = 'ETX'
      graph = self.graph_etx
      entry_format_string = '{:8.3f}'
    elif table_type == Data_table.prr: #Packet Reception Rate
      output = 'PRR'
      graph = self.graph_prr
      entry_format_string = '{:8.3f}'
    elif table_type == Data_table.rssi: #Received Signal Strength Indicator
      output = 'RSSI'
      graph = self.graph_rssi
      entry_format_string = '{:8.1f}'

    output += ' table:\n^^^^^^^^^^\nr\\f'
    for sender in self.valid_node_ids:
      output += '{:8d}'.format(sender)
    output += '\n'
    for receiver in self.valid_node_ids:
      output += '{:3d}'.format(receiver)
      for sender in self.valid_node_ids:
        if sender in graph:
          receiver_ids = [*graph[sender]]
          if receiver in receiver_ids:
            output += entry_format_string.format(graph[sender][receiver])
          else:
            output += '        '
      output += '\n'
    print(output)

  def parse_neighbor_data(self, print_etx=False, print_prr=False, print_rssi=False):
    if self.filename:
      self.parse_file(self.folder + self.filename)
    else:
      for filename in os.listdir(self.folder):
        self.parse_file(self.folder + filename)
    #self.combine_parse_graphs()
    #if print_prr:
    #  self.print_parsed_data_table(Data_table.prr)
    if print_etx:
      self.print_parsed_data_table(Data_table.etx)
    #if print_rssi:
    #  self.print_parsed_data_table(Data_table.rssi)
    return self.schedule_finished
