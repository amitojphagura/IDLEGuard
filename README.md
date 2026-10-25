# IDLEGuard

A smart engine idle monitor built in 6 hours at the E2C2 Hackathon. IDLEGuard tells drivers when their car is warmed up and ready to drive, then automatically shuts the engine off if the car keeps idling after it's warm. The goal: cut wasted fuel, money, and air pollution from unnecessary idling.

## Demo

[![Watch the IDLEGuard demo](https://img.youtube.com/vi/QY-_GydyjAU/hqdefault.jpg)](https://www.youtube.com/watch?v=QY-_GydyjAU)
[Watch the demo on YouTube](https://www.youtube.com/watch?v=QY-_GydyjAU)

## The Problem

Three extra minutes of idling a day wastes roughly $20 to $47 in fuel per car each year. If half of Edmonton idled for three extra minutes a day, that adds up to an estimated $20.5M lost annually ([source](https://carhub.ca/car-idling-how-long-is-too-long-and-what-are-the-effects)). Most drivers idle simply because they don't know when the engine is warm enough to drive.

## How It Works

An ESP32 reads engine temperature (TMP36) and vehicle motion (MPU6050 accelerometer), runs the decision logic, and pushes notifications to the driver's phone through [ntfy](https://ntfy.sh).

1. Engine start: Pressing the button simulates turning the car on. IDLEGuard records a baseline temperature and turns on the monitoring LED.
2. Ready for light driving: Once the temperature rises past the first threshold while the car is parked, the driver gets a notification that it's safe to start city driving (under 3000 RPM).
3. Optimal temperature: If the car is still parked after the second threshold, the driver gets an urgent alert and a 20 second shutdown countdown starts.
4. Automatic shutoff: If the car never moves, the engine is turned off and the driver is told how long it ran.
5. Driving detected: If the car starts moving after warming up, monitoring turns off for the rest of the trip. Movement during the countdown cancels the shutdown.

### Decision Logic

The behavior was designed from a truth table before writing any code.

| Temp reached (T) | Moving (M) | Idle timer exceeded (I) | Notify (N) | Shut off (S) |
|:---:|:---:|:---:|:---:|:---:|
| 0 | 0 | 0 | 0 | 0 |
| 0 | 0 | 1 | X | X |
| 0 | 1 | 0 | 0 | 0 |
| 0 | 1 | 1 | X | X |
| 1 | 0 | 0 | 1 | 0 |
| 1 | 0 | 1 | 1 | 1 |
| 1 | 1 | 0 | X | 0 |
| 1 | 1 | 1 | X | 0 |

### Firmware Highlights

- Motion detection: Movement is the change in acceleration between samples. It must exceed 1.0 m/s² for 3 consecutive readings (filters out bumps and sensor noise), and the car is only considered parked after 1.5 s of stillness.
- Temperature filtering: Each reading averages 20 ADC samples, then passes through an exponential moving average. Hardware capacitors were added to fix noisy readings.
- Non-blocking notifications: Phone alerts are queued and sent from a separate FreeRTOS task on the ESP32's second core, with up to 10 retries. The engine shutoff never waits on Wi-Fi, so it still works with no network.
- Throttling: Repeated alerts have a 10 second cooldown so the driver isn't spammed.

## Hardware

| Component | ESP32 Pin |
|---|---|
| TMP36 temperature sensor (output) | GPIO34 |
| MPU6050 SDA | GPIO21 |
| MPU6050 SCL | GPIO22 |
| Red LED (engine on) | GPIO25 |
| Green LED (monitoring active) | GPIO26 |
| Push button (other leg to 3.3V) | GPIO13 (internal pull-down) |

Power the TMP36 and MPU6050 from 3.3V and GND. Each LED needs a current limiting resistor (around 220 Ω). A small capacitor across the TMP36 supply helps reduce noise.

## Setup

1. Install the Arduino IDE with ESP32 board support. No extra libraries are needed.
2. Wire the circuit as shown above.
3. Open `IDLEGuard/IDLEGuard.ino` and fill in your Wi-Fi name, password, and an ntfy topic near the top of the file. Use a long, hard to guess topic name, since anyone who knows it can read your notifications.
4. Install the ntfy app on your phone and subscribe to the same topic.
5. Upload the sketch and open the Serial Monitor at 115200 baud to watch live status.
6. Press the button to start the "engine."

The temperature thresholds (+2.0 °C and +4.5 °C above baseline) are set low so the demo can be triggered by warming the sensor by hand. In a real vehicle these would be tuned to actual engine or coolant temperatures.

## Results

After debugging, the team ran 9 consecutive demo runs with 0 failures, all before the hackathon's build period ended.

## Team

- Amitoj Phagura
- Yojith Medury
- Barney Wong
- Clark Visser

The ESP32 decision logic was written together once each piece worked on its own.
