# Detected Issues: 26-Sep-2026

## 1. Low memory
There is limit of internal RAM 334KB while static usage 259 kB (77%) while many task use heap.

## 2. PSRAM tack stack
There is task has place the stack on PSRAM while it access to SPI like NVS. Because PSRAM has use OCTO-SPI when user code access to SPI it's disable cache make system fault.

## 3. Architecture.
Bad architecture, code base is hard to manage and maintance. Many task for single process at time (That should same task with difference state-machine). There is task define but don't use.

## 4. Bluetooth.
The Bluetooth communication with app not stable (drop, error while transfer, etc.). The Bluetooth stack Bluedroid acquired a lot of memory while not use.

## 5. Wi-Fi.
The Wi-Fi connect not stable, always drop on heavy connection (Relate to LOW memory issue).
