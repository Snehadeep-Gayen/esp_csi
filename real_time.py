import threading
import queue
import time
import torch
import torch.nn as nn
import torch.optim as optim
import numpy as np
from socket import *
serverPort = 8001
# serverip = '127.0.0.1'
serverip = '192.168.4.2'

# get data pt from csv
data = np.loadtxt('test_datapt.csv', delimiter=',', dtype=np.float32)

# savefile name 
savefile = 'csi_data_none.csv'

# ML Model
class CSICNN(nn.Module):
        def __init__(self, num_classes):
            super(CSICNN, self).__init__()
            self.conv1 = nn.Conv2d(in_channels=1, out_channels=32, kernel_size=3)
            self.pool = nn.MaxPool2d(kernel_size=2, stride=2)
            self.conv2 = nn.Conv2d(in_channels=32, out_channels= 32, kernel_size=3)
            self.fc1 = nn.Linear(29*29*32, 4096)
            self.fc2 = nn.Linear(4096, 1024)
            self.fc3 = nn.Linear(1024, 256)
            self.fc4 = nn.Linear(256, 128)
            # self.fc4 = nn.Linear(128, 64)
            # self.fc5 = nn.Linear(500, 64)  # Adjust to match the number of classes
            self.fc6 = nn.Linear(128, num_classes)  # Output layer

        def forward(self, x):
            x = self.pool(torch.relu(self.conv1(x)))
            x = self.conv2(x)
            x = x.view(-1, 29*29*32)
            x = torch.relu(self.fc1(x))
            x = torch.relu(self.fc2(x))
            x = torch.relu(self.fc3(x))
            x = torch.relu(self.fc4(x))
            # x = torch.relu(self.fc5(x))
            x = self.fc6(x)  # Output layer
            return x

criterion = nn.CrossEntropyLoss()


# Function for the first thread
def producer(q):
    """
    This function opens a socket and gets the data from the socket
    """
    sock = socket(AF_INET, SOCK_STREAM)
    assert sock, "Failed to create socket"
    sock.bind((serverip, serverPort))
    assert sock, "Failed to bind socket to address"
    sock.listen(1)
    print('Server listening...')
    connectionSock, addr = sock.accept()
    assert connectionSock, "Failed to accept connection"
    print('The server is ready to receive')
    print(connectionSock)
    data = []
    for i in range(500000000):
        bytes = connectionSock.recv(64*128)
        if bytes == b'':
            continue
        for i in range(len(bytes)):
            data.append(bytes[i])
            if len(data) == 64*128+4:
                with open(savefile, 'a') as f:
                    np.savetxt(f, np.array(data).reshape((1, -1)), delimiter=',', fmt = '%d')
                amps = []
                for data_pt in range(64):
                    offset = data_pt * 128 + 4 # long has size 4
                    for j in range(64):
                        amps.append(np.sqrt(data[offset + 2*j ] ** 2 + \
                                            data[offset + 2*j + 1] ** 2))
                amps = np.array(amps, dtype=np.float32)
                q.put(np.reshape(amps, (64, 64)))
                data = []
                print(f"Produced: {i}")

# Function for the second thread
def consumer(q):
    """
    This function gets the data from queue, runs the ML model
    and prints the result
    """
    model = torch.load('check.pth', map_location=torch.device('cpu'))
    model.eval()
    with torch.no_grad():
        while True:
            item = q.get()
            if item is None:
                break
            else:
                item = torch.tensor([item])
                item = item.view(-1, 1, 64, 64)
                output = model(item)
                print('Predicted output: ', output.argmax().item())


# Create a shared queue
q = queue.Queue()

# Create and start the producer and consumer threads
producer_thread = threading.Thread(target=producer, args=(q,))
consumer_thread = threading.Thread(target=consumer, args=(q,))
producer_thread.start()
consumer_thread.start()

# Wait for the producer thread to finish
producer_thread.join()

# Signal the consumer thread to stop
q.put(None)

# Wait for the consumer thread to finish
consumer_thread.join()

print("All threads have finished")
