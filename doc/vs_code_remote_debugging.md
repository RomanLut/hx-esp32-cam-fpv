
Visual Studio Code Remote Debugging

 On Ruby based images, VS Code will fail installing VS Code remote server due to insufficiend space in /tmp folder, allocated in RAM.

- I can be solved executing command once before connecting:

   ```sudo mount -o remount,size=1G /tmp```

  Checking size of /tmp foder:
  
  ```df -h /tmp```

- Lastest working VSCode version for kernel 5.10  is 1.89.1. If you want to use remote development, install this version manually and disable automatic agrades.