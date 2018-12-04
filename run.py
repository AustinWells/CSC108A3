#!/usr/bin/python3

import os
import subprocess

if __name__ == "__main__":

    for file in os.listdir():
        if file.endswith(".log"):
            os.remove(file)

    #./mserver -c 1111 -s 2222 -C ./srvcfg_local.txt
    mserver = subprocess.Popen(["./mserver",
                                "-c", "1111",
                                "-s", "2222",
                                "-C", "./srvcfg_local.txt"])

    while True:
        #./client -h localhost -p 1111 -f ./ops_sample.txt
        client = subprocess.Popen(["./client",
                                "-h", "localhost",
                                "-p", "1111",
                                "-f", "./ops_sample.txt"])
        client.wait()
        break

    mserver.wait()