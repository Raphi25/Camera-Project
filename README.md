# Camera-Project
This master's project investigates the development of a low-cost wearable camera system intended to support caregivers by capturing images throughout the wearer's daily life. The system can take images in 30 second intervals in different settings while remaining lightweight, robust, serviceable, inexpensive and long battery life.

## Current firmware and shared App

The current firmware is in [Development Board Prototype/Code](Development%20Board%20Prototype/Code) and [Breadboard Prototype/Code](Breadboard%20Prototype/Code). Open either Code folder and build with `idf.py build`. Dependencies are restored by the ESP-IDF component manager; generated build output and Python environments are excluded.

Both projects use the single [App](App) folder. The packaged Windows application, [GUI.exe](App/GUI.exe), is included in the repository; use `Launch_GUI.cmd` to run it. To run from source, use `Setup_App.cmd` and `Launch_Source.cmd`, or rebuild the executable with `Build_GUI_App.cmd`. Each firmware folder also has launch and build shortcuts to the shared App. Local passwords and settings are not part of the repository.

## Architecture

Explore the system, firmware, and desktop application interactively:

[Open the interactive architecture explorer](https://raphi25.github.io/Camera-Project/project-architecture.html)
