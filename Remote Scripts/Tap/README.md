# Tap
Version: 2.0.1

An Ableton Live MIDI remote script for the iOS app 7III Tap by project7III.

More info: https://project7iii.com/tap
For troubleshooting and detailed instructions on how to use Tap, please refer to the [User Manual](https://project7iii.com/tap/manual/#2-2-connect-your-iphone-or-ipad)

## 1. Add the Tap MIDI Remote Script
1. Manually create a folder called `Remote Scripts` within your User Library if it does not already exist. The default User Library locations are:

   - **Windows:** `\Users\[username]\Documents\Ableton\User Library`
   - **Mac:** `Macintosh HD/Users/[username]/Music/Ableton/User Library`
   
2. Place the remote script folder called `Tap` (the folder you found this README.md in) into the `Remote Scripts` folder you just created.
3. Your directory should now look like this:

```
Ableton/
└── User Library/
    └── Remote Scripts/
        └── Tap/
            ├── __init__.py
            ├── Tap.py
            └── README.md (optional, can be removed)
```

>Important: Make sure the User Library is stored locally on your computer and matches the exact path described above. If it is cloud-based or even slightly misnamed, Ableton Live may fail to recognize the script.

## 2. Connect your Device
For the most reliable connection, use a wired setup whenever possible.  
If you are never using MIDI over WiFi, you can disable `MIDI over WiFi in Tap's settings.
Find the big connection guide 

### 2.1 macOS: Over USB-C

1. Connect your iPhone or iPad to your Mac using a USB-C cable.
2. Open the app **Audio MIDI Setup**.
3. Open the **Audio Devices** window.  
   → If it is not already visible, select **Window → Audio Devices**.
4. Find your iOS device in the sidebar and click **Enable**.


### 2.2 Windows: Wired MIDI interface setup

This is the recommended Windows setup. It is wired, bidirectional, and does not rely on WiFi. Feeling brave? Try the even more direct setup [below](#23-windows-experimental-direct-usb-midi-host-bridge) and let us know if it works for you.

The connection is:

```text
iPhone/iPad
◉
│ USB cable
◉
USB-C MIDI interface with two MIDI cables
◉          ◉
│ MIDI IN  │ MIDI OUT
◉          ◉
CME H2MIDI Pro or another MIDI interface
◉
│ USB cable
◉
Windows PC with Ableton Live
```

1. Connect a USB-C MIDI interface to your iPhone or iPad.
2. Connect the CME H2MIDI Pro or CME H4MIDI WC (or your existing audio interface with MIDI) to your Windows computer via USB.
3. Connect **MIDI OUT** from the iPhone/iPad interface to **MIDI IN** on the CME interface.
4. Connect **MIDI OUT** from the CME interface to **MIDI IN** on the iPhone/iPad interface.
5. Open Tap and select the connected MIDI interface as MIDI input/output if needed.
6. In Ableton Live on Windows, select the CME interface as the MIDI input and output for Tap.

This keeps the iPhone or iPad in its usual supported role: it is the USB host for a class-compliant USB MIDI interface. The CME interface handles the Windows side.

MIDI interface for iPhone/iPad:  
🇺🇸 [USB-C MIDI Interface on Amazon](https://amzn.to/3RXv3jN)  
🇪🇺 [USB-C MIDI Interface on Amazon](https://amzn.to/4ak5orI)

Windows-side USB MIDI host/interface options:  
🇺🇸 [CME H2MIDI Pro on Amazon](https://amzn.to/4ekFN3k)  
🇪🇺 [CME H2MIDI Pro on Amazon](https://amzn.to/4uUoIUF)

🇺🇸 [CME H4MIDI WC on Amazon](https://amzn.to/4fArM3A)  
🇪🇺 [CME H4MIDI WC on Amazon](https://amzn.to/4ogzJxh)

*Note: As an Amazon Associate we earn from qualifying purchases.*


### 2.3 Windows: Experimental direct USB MIDI host bridge

This setup may work, but it is not tested by us yet.

The connection would be:

```text
iPhone/iPad
◉
│ USB cable
◉
USB-A host port on CME H2MIDI Pro or CME H4MIDI WC
USB-C computer port on CME H2MIDI Pro or CME H4MIDI WC
◉
│ USB cable
◉
Windows PC with Ableton Live
```

1. Connect your iPhone or iPad to the **USB-A host port** of the CME H2MIDI Pro or CME H4MIDI WC.
2. Connect the **USB-C computer port** of the CME H2MIDI Pro or CME H4MIDI WC to your Windows computer.
3. Open Tap and select the connected USB MIDI interface as MIDI input/output if needed.
4. In Ableton Live on Windows, select the CME interface as the MIDI input and output for Tap.

This should allow communication through USB MIDI virtual ports if the iPhone or iPad is recognised correctly by the CME USB host port. We have not tested this yet. **Please let us know if it works for you in practice.**

If it does not work, use the wired MIDI interface setup above.


### 2.4 Windows: rtpMIDI over ad hoc WiFi

If a wired MIDI setup is not available, use a dedicated ad hoc WiFi network instead of a busy normal WiFi network.

1. Create an ad hoc WiFi network on your Windows computer.
2. Connect your iPhone or iPad to that WiFi network.
3. Download [rtpMIDI](https://www.tobias-erichsen.de/wp-content/uploads/2020/01/rtpMIDISetup_1_1_14_247.zip).
4. Open rtpMIDI on Windows and create a new session.
5. Open Tap on your iPhone or iPad.
6. Connect your iPhone or iPad in the rtpMIDI session.
7. In Ableton Live, select the rtpMIDI session as the MIDI input and output for Tap.


### 2.5 macOS: Over WiFi

If USB-C is not available, you can also use MIDI over WiFi on macOS. Use a clean, stable WiFi network and avoid busy public or shared networks.

1. Connect your iPhone or iPad to the same WiFi as your Mac.
2. Follow this [Apple guide](https://support.apple.com/en-ca/guide/audio-midi-setup/ams1012/mac). You do not need to do **Step 9**.

## 3. Set Up Live
1. Launch Live.
2. Open Live's Preferences and navigate to the **MIDI** tab.
3. Select the script `Tap` using the dropdown menu in the Control Surface column.
4. Assign your device or Network Session as input and output ports.
5. Activate `Track` and `Remote` for your active MIDI Ports.