# ESP32 To MPU6050: Telemetry Data Logger 

An ESP32 reads sensor data from an MPU6050, shows its status on an OLED, 
logs samples to internal flash, then signs the session with HMAC-SHA256 and 
uploads it to a FastAPI backend on Railway that re-computes the signature to verify the data wasn't altered. 
The code for the Railway backend is available in my other repository, flight-data-backend. 
If WiFi is unavailable or the upload fails, the signed data stays on the device and is retried automatically.

# The Stages: 

Calibration: On boot the device averages 50 MPU6050 readings to measure its zero-offset error. 
             Keep it flat (Z axis up) and still. Later readings have these offsets subtracted.
             
Idle:        Waits for a button press. Any saved-but-unsent bundles are retried in the background every 10 s.

Recording: 	 Every 0.5 s one calibrated sample is appended to /log.txt in SPIFFS. 
             The OLED shows the entry count.
             
Sign Off:    Pressing the button stops recording. The device computes an HMAC-SHA256 over the log using the shared secret, 
             saves a bundle file (/bundle_0001.json, ...) to flash, then tries to upload it. 
             The bundle is deleted only after the server accepts it.

# How verification works

The ESP32 signs the bytes of the log file with the key from config.h. Note that each line ends with \n. This is important because 
the backend has to backtrack it later. I'm sure there's a more efficient way to do this, but oh well. 
The server rebuilds those bytes with "\n".join(telemetry_log) + "\n", computes its own HMAC with SHARED_SECRET, 
and compares using a constant-time check. If the signatures match, the log is stored in PostgreSQL. If not, the server returns 401.
This proves the log wasn't changed in transit and was actually produced by a device holding SHARED_SECRET. 

# Assembling the hardware: 

You can follow the instructions given in this tutorial: https://randomnerdtutorials.com/esp32-mpu-6050-accelerometer-gyroscope-arduino/. 
This tutorial is great because it also walks you through the basics of coding the MPU6050 and the OLED. 
Note that my current code requires a linear switch that toggles between the stages. You can re-create the same effect as the linear switch 
by shorting GPIO PIN 4 to GND on the ESP32 with a Dupont wire (or wire up your own linear switch to those pins on the breadboard). 

# Setting up the backend: 

Follow the steps in the README.md provided on my other repository, flight-data-backend. 
Note that you'll need a config.h file with the Railway Server URL and your own secret key. 
Mine's not included... because it holds my secret key. 

# Using the Logger: 

After you've set everything up, press the Build button (the checkmark). 
Once it succeeds, press the Upload (-> button) to flash the code to the ESP32. 
Upon first boot, you won't have any wifi saved. Connect your phone to the "ESP32-Telemetry-Setup" 
network and choose your WiFi in the page that opens. Credentials are remembered afterwards. 







