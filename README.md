# Lora-remote-buzzer
Experimenting with Lora devices like Sensecap T1000-E Lora to make a buzzer between devices, one device clicks a button, the other buzzez.

Initial testing involves Sensecap T1000-E (Lora version) device, it might work on non-lora version, sold as "Meshtastic" version but I didn't test yet.
Also in future, I might expand this to more devices like SenseCap NRF kit, which would involve changing pin definitions in code.

Getting started

- put your T-1000-E in disk mode by holding it's button while connecting it's USB cable
- locate the drive of T-1000-E on your computer (e.g E:/), browse there and paste the .uf2 file
- T-1000-E should install the new detected .uf2 file automatically, will reboot and make a short beep
- Disconnect the USB cable and do the same for a scond device, put it in disk mode, copy file, etc
- Now when clicking button on one device, the other shoudl beep and show a light while receiving
