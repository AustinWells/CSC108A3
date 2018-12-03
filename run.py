#!/usr/bin/python3

import subprocess

if __name__ == "__main__":
    #./mserver -c 1111 -s 2222 -C ./srvcfg_local.txt
    mserver = subprocess.Popen(["./mserver",
                                "-c", "1111",
                                "-s", "2222",
                                "-C", "./srvcfg_local.txt"])

    #./client -h localhost -p 1111 -f ./ops_sample.txt
    client = subprocess.Popen(["./client",
                               "-h", "localhost",
                               "-p", "1111",
                               "-f", "./ops_sample.txt"])

    mserver.wait()
    client.wait()