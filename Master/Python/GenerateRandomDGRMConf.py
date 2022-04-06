import random
import shutil
import os

MAX_NBRS = 16
nodes = 20
node_range = nodes + 1


def get_connection(node1, node2):
  prr = random.randrange(100, 1000)
  if(prr < 500 and random.randrange(1, 3) > 1):
    prr += prr
  return (node1, node2, prr / 1000, 0, 0, 0, random.randrange(60, 99)*(-1), 0, 0)

def update_nbrs(main_node, node_to_nbrs):
  # remember connections for other nbr to this node
  for nbr in node_to_nbrs[main_node]:
    if(nbr in node_to_nbrs.keys()):
      node_to_nbrs[nbr].add(main_node)
    else:
      node_to_nbrs[nbr] = set([main_node])


def create_dgrm_graph(beacons, version):
  graph = {}
  node_to_nbrs = {}  # {2 : [3, 2, 1], ... }

  for node in range(1, node_range):

    if(node not in node_to_nbrs.keys()):
      node_to_nbrs[node] = set(random.sample(range(1, node_range), k=random.randrange(2, MAX_NBRS)))
      update_nbrs(node, node_to_nbrs)
    else:
      total_nbrs = random.randrange(2, MAX_NBRS)
      current_nbr = len(node_to_nbrs[node])
      if(current_nbr >= total_nbrs):
        nbrs = node_to_nbrs[node]
      else:
        for entry in set(random.sample(range(1, node_range), k=total_nbrs - current_nbr)):
          node_to_nbrs[node].add(entry)
        update_nbrs(node, node_to_nbrs)

    if(node in node_to_nbrs and node in node_to_nbrs[node]):
      node_to_nbrs[node].remove(node)

  for node in range(1, node_range):
    #remove connections if there are too many
    while(len(node_to_nbrs[node]) > MAX_NBRS):
      to_remove = node_to_nbrs[node].pop()
      node_to_nbrs[to_remove].remove(node)
      print("remove {} and {}".format(node, to_remove))

    #build connections
    connections = []
    #print("Building for {} with nbrs {}".format(node, node_to_nbrs[node]))
    for nbr in node_to_nbrs[node]:
      connections.append(get_connection(node, nbr))
      #if our nbr does not have us in his list and his id is smaller, we already set all conections for nbr
      # since this is now a new connection we need to add another one
      if(node > nbr and (node not in node_to_nbrs[nbr])):
        #print("node {} in {} for nbr {}? {}".format(node,node_to_nbrs[nbr],nbr, node in node_to_nbrs[nbr] ))
        #print("from: {} to {} with {}".format(node, nbr, node_to_nbrs[nbr]))
        connections.append(get_connection(nbr, node))

    graph[node] = connections

  success = True
  for node in graph:
    if len(node_to_nbrs[node]) > MAX_NBRS:
      print("TOo many nbrs for node {}".format(node))
      success = False

  for node in graph:
    if len(graph[node]) > MAX_NBRS:
      print("TOo many nbrs for node {}".format(node))
      succes = False

  for node in graph:
    count = 0;
    for second_node in graph:
      if(node == second_node):
        continue

      for entry in graph[second_node]:
        if(node == entry[1]):
          count += 1
    if(count > MAX_NBRS):
      print("too many nbrs sending to" + str(node))
      success = False

  filename = "DGRM{}_{}EBS_Test{}.conf".format(nodes, beacons, version)
  #for key in node_to_nbrs.keys():
  #  print(str(key) + ' ' + str(node_to_nbrs[key]) + '\n')
  textfile = open("../DGRM_configurations/{}".format(filename), "w")
  for node in graph:
    for connection in graph[node]:
      textfile.write("{} {} {} {} {} {} {} {} {}\n".format(connection[0], connection[1], connection[2], connection[3], connection[4], connection[5],connection[6],connection[7],connection[8]))

  textfile.close()
  try:
    shutil.copyfile(os.path.join("F:/WS_21_22_MasterThesis/master-scheduler/MASTER/DGRM_configurations/", filename),
                    os.path.join("F:/WS_21_22_MasterThesis/master-scheduler-original/Master/DGRM_configurations", filename))
    print("File copied successfully.")

  # If source and destination are same
  except shutil.SameFileError:
    print("Source and destination represents the same file.")

  # If destination is a directory.
  except IsADirectoryError:
    print("Destination is a directory.")

  # If there is any permission issue
  except PermissionError:
    print("Permission denied.")

  # For other errors
  except:
    print("Error occurred while copying file.")

  print("finished version {} with {}".format(version, success))
  return success

count = 1

while(count < 11):
  count += create_dgrm_graph(400, count)
