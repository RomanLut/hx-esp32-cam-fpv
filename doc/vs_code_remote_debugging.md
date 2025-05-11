
# Notes on Visual Studio Code Remote Development

 On **RubyFPV** based images, **VS Code** will fail installing **VS Code remote server** due to insufficiend space in /tmp folder, allocated in RAM.

It can be solved executing the follwing command once, before connecting VS Code:

- ```sudo mount -o remount,size=1G /tmp```

- Checking size of /tmp foder:
  
- ```df -h /tmp```


Lastest working **VS Code** version for kernel **5.10** is **1.89.1**. If you want to use remote development, install this **VS Code** version manually and disable automatic agrades.

In case if Git have problems, reassign all files in the folder to user pi ():
 
  ```sudo chown -R pi:pi esp32-cam-fpv```
