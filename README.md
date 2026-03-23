# Ensoniq® SD-1 32-Voice VST Emulation
MAME®-based VST3 emulation of the Ensoniq® SD-1 32-Voice synthesizer built with JUCE®

![Screenshot of Ensoniq SD-1 VST](https://github.com/kukoricajoe/Ensoniq-SD-1-32-voices-VST-emulation/blob/main/sd-1.png)
Ensoniq SD-1 with Compact panel in FL Studio

# Ensoniq SD-1
The SD-1 (1990) comes from a long line of Ensoniq's evolving Transwave® wavetable digital synthesizers. It began with Ensoniq's earliest synthesizer, the ESQ-1. That led to the SQ-80, then the VFX and VFX-SD (the latter featuring an on-board sequencer) and then to the SD-1 (and it eventually led to the Fizmo). The SD-1 allows for additive synthesis using waveform modulation, a sort of wavetable synthesis. This puts it into a unique class of digital synthesizers along with the PPG Wave series and Waldorf Microwave series.

The SD-1 can create all sorts of acoustic, electric, digital, and analog-like sounds. Its piano sound has over 1 MB of 16-bit waveforms to give it a full and rich realistic tone not found in other digital synthesizers of the time.

A single patch can contain up to 6 of the 168 waves in its ROM memory that can be combined and layered. Advanced and analog-like synth parameters including its dual multi-mode digital filters, three 11-stage envelopes, LFO, and 15 modulation sources allow you to further shape and morph your sounds. There's even a built-in 24-bit VLSI dual effects processor with reverb, chorus, flanging and delay. The SD-1 also has a standard 61-note keyboard with velocity sensitivity, polyphonic aftertouch and full MIDI implementation with 12 channels for multitimbral functions as well as four 16-bit DAC outputs.
Like the VFX-SD, the SD-1 has a professional quality on-board sequencer making it a complete all-in-one music production workstation. This is a 24-track sequencer with 25,000 note capacity and it holds up to 60 sequences and 20 songs. There is quantization (96 ppqn), real-time or step entry, looped or linear mode, and auto-punch in/out. Tracks can be set to control the SD-1's internal voices or external MIDI equipment, or both at the same time! An on-board 3.5" disk drive allows you to store your programs, sequences, songs, and even MIDI SysEx data. The SD-1 is compatible with all VFX and VFX-SD program librarys too! [Source](https://www.vintagesynth.com/ensoniq/sd-1)

# About this project
We are Sojus Records, one of the longest-running netlabels still active. We are musicians, not programmers, but we love old synths and emulations. We decided to build a fully featured VST3 version of the MAME-emulated Ensoniq SD-1/32, which has never been emulated before. Thanks to the recent AI coding revolution, we have successfully built it. This proof-of-concept is an important step for both musicians and coders. We are looking forward to bringing other MAME synths to life in the future!

# Download 
[Win x64 VST3](https://github.com/sojusrecords/Ensoniq-SD-1-32-Voice-VST-emulation/releases/download/v0.9.7b/EnsoniqSD1-v.0.9.7b-winVST3.7z)
[macOS UB VST3](https://github.com/sojusrecords/Ensoniq-SD-1-32-Voice-VST-emulation/releases/download/v0.9.7b/EnsoniqSD1-v.0.9.7b-macVST3.7z)

# What's working?
Everything. Check the original manual here: [SD-1 Manual at Polynominal](https://www.polynominal.com/ensoniq-sd1/ensoniq-sd1-manual.pdf)

# Features:
- Windows 10+ 64 bit VST3, Mac Intel+ARM Universal Binary VST3
- **NEW: VFX, VFX-SD, SD1 .SYX SYS-EX file import.** Read the FAQ for more info!
- **NEW: FULL automation** (All keys, sliders and buttons. Now you can save presets: Saving requires holding down PRESETS button and pressing a BANK button), full MIDI CC controlling, polyphonic aftertouch
- **NEW: Global settings saving** 
- VST3 state saving
- 4 different panel layouts with resizable GUI and VFD display
- Buffer setting
- 4 outputs: stereo main out, optional stereo aux (dry signal with no effects)
- Can load all compatible VFX/VFX-SD/SD1-24/SD1-32 disk images and cartridges (.img .bin .crt etc)
  - How to load: Attach the disk image using the "Load Floppy/Cartridge" button. Press Storage, then select DISK. Press LOAD. The display will show the Disk Load page with the File Type selected. Move the data entry slider to select your file.

# Known limitations
- Only 1 instance can run at a time per DAW due to engine limitations.
- Daw automation is not visible on GUI
- No floppy drive sound :D
- Mac binaries require one of the following methods to run due to Apple's security policies:
  - Manual authorization: The user must go to System Settings > Privacy & Security and, after the DAW has attempted to load the plugin, click the "Open Anyway" button.
  - or
  - Removing quarantine (via Terminal): Run the following command on the plugin bundle:
  ```
  sudo xattr -rd com.apple.quarantine /Library/Audio/Plug-Ins/VST3/EnsoniqSD1.vst3
  ```

# Requirements
- Please note that this is a hardware-level emulation of the synthesizer, so it places **heavy demands on the CPU!** Set the buffer setting to higher if buffer underrun occurs.
- Windows 10 or newer or macOS 10.14 (Mojave) or newer.
- Windows build is AVX2 optimized (Haswell or newer).
- A VST3 compatible DAW. If it's not working, come back later :)
  - Tested and working:
    - macOS: Ableton Live 12, Bitwig Studio 6, Cubase 15, Fender Studio Pro 8, FL Studio 2025, Reaper 7.
    - Windows: Ableton Live 12, Bitwig Studio 6, Cubase 15, FL Studio 2025, Reaper 7.
  - Working but wav render is wrong: Reason 12 (Windows)
  - If it's not working for you check [Troubleshooting](#Troubleshooting).
- IMPORTANT - ROM Files Required!<br/>
  Due to copyright reasons, the required Ensoniq ROM files are NOT included.
  To make the plugin work:
  * Create a folder named EnsoniqSD1 in your user's Documents folder:
    - Win C:\Users\yourusername\Documents
    - macOS /yourusername/Documents/
  * Obtain the Ensoniq SD-1 32 variant AND Ensoniq 2x40 VFD ROM files and place EXACTLY these files in that folder AND zip them to sd132.zip.<br/>

    Filename | SHA256
    - esqvfd_font_vfx.bin<br/>ab2f7ddc6ab7fafaf07985d01788197849cdaeb5a4a7d9f2f85098dfd65edf01
    - sd1_32_402_hi.bin<br/>90ae35de8661f5de0793b6ea59a4d6524e90c0828a29e6ea8906ff759116136d
    - sd1_32_402_lo.bin<br/>6b0c1235c4f813ce8698e89d66933e9c7c9168f4a095c9e2a50add7fe729481c
    - sd1_410_hi.bin<br/>1d6d6150373fb070da8b1a6da57762749bda9210e0ca5536441bb8194a3cafb7
    - sd1_410_lo.bin<br/>e3e42beca41989561c0d2a8266e48549561650a7606bb8a0d75b438847e8bd0c
    - u34.bin<br/>7a6e6e76da7eb8de5cbc3a0a2bfb27a461e312facdcc0b7ecc42b9d1eb261e12
    - u35.bin<br/>1df911a97e0e5a334d9345ba5e47eac7794d083282012f7ecf70901b88cf7e08
    - u36.bin<br/>2fdb401bea78eb323fa55408760a73319aeae68b465f193dc7a46d1b21277cdd
    - u37.bin<br/>e08931013c8aca2460b4f2c3512e1d3e9a610a7f921e22012bb13bd23a3e56d7
    - u38.bin<br/>2f185a185961a1c14472c2b706642c0d9e7a0792d57d946a349840905782e5ca<br/>

  * The final structure of sd132.zip in your Documents/EnsoniqSD1 folder looks like this:<br/>
    ![sd132.zip](https://github.com/sojusrecords/Ensoniq-SD-1-32-Voice-VST-emulation/blob/main/roms.png)<br/>
  
  - Optional: If you want to run the internal sequencer, you need the original disk image:
    - Ensoniq SD1 Sequencer OS v410 (SD-1 800K type)
    - How to load: Attach the disk image using "Load Floppy/Cartridge". Press Storage, then select DISK. Press LOAD. The display will show the Disk Load page with the File Type selected. Move the data entry slider all the way up to select TYPE=SEQUENCER OS.

# Troubleshooting
- The plugin is checking the contents of sd132.zip in your Documents/EnsoniqSD1 folder. If something is not right, then it will tell you.
- On macOS: if 'sudo xattr -rd...' fails, try codesign the plugin:
  - Open a terminal window, install Xcode Command Line Tools if needed: ```xcode-select --install```
  - ```sudo codesign -f -s /Library/Audio/Plug-Ins/VST3/EnsoniqSD1.vst3```
- Whitelist the plugin in your antivirus app. The plugin is writing some data to temp folder (e.g. nvram and osram files, lua plugins) and to your Documents/EnsoniqSD1 folder (e.g. settings.xml)
- Your sequencer is blacklisting the plugin: if the plugin scanner provides error message or a log file then send it to us.
- If the plugin is loaded but there's only "Load Floppy/Cartridge" and "Settings" buttons and blank window: the internal MAME engine is not loaded. Check if your OS/PC/MAC is capable to run it. 
- Reset global settings: go to Documents/EnsoniqSD1 and delete the file "settings.xml" and delete temp files

# FAQ
<details>
  <summary>How do I load my old SysEx (.syx) preset banks?</summary>

Loading SysEx files works exactly like the original 1990 hardware, simulating a physical MIDI cable connection at a 31250 baud rate.

Step-by-step:

  - **IMPORTANT** Enable Sys-Ex on the Synth: On the SD-1 front panel, press System/MIDI CONTROL button TWICE, and set SYS-EX to ON.

  - Go to a safe screen: Press the Sounds or Presets button to return to the main playing screen.

  - Load the file: Click the Load Media button on the plugin interface and select your .syx file. A standard 40KB bank takes about 12 to 15 seconds to transfer. This is an authentic hardware limitation (the maximum speed of a physical MIDI cable).

  - You will see a "Transmitting Sys-Ex Data..." overlay on the screen. Once the overlay disappears, the synth will instantly update its RAM, and your presets will be ready to play!

  - You can also save the presets to a disk image (here you can find an [SD-1 formatted empty disk image.](https://github.com/sojusrecords/Ensoniq-SD-1-32-Voice-VST-emulation/blob/main/SD-1-EMPTY-DISK.img)
</details>

<details>
  <summary>How do I load Floppy Disk Images (.img etc.) or Cartridges (.crt etc.)?</summary>

   - Click the Load Media button on the left side of the plugin and select your file.
    Once loaded, a small green retro LED indicator will appear in the bottom-left corner of the VFD display showing ```FLOPPY: yourfile.img``` or ```CART: yourfile.crt```.
   - From there, press the Storage button on the SD-1 panel, select Disk or Cartridge, and use the Load function just like on the real machine.
</details>

<details>
  <summary>Audio Settings: Why can't I set the Buffer to 0?</summary>
This plugin runs a cycle-accurate emulation of the original Motorola 68000 CPU and Ensoniq OTIS chips. On the real physical SD-1, the time it takes from pressing a key to hearing a sound is approximately 24.4 milliseconds.

Because the MAME emulator runs asynchronously on its own background thread, we need a tiny "safety pool" (the plugin buffer) to ensure the audio stream never drops out.

  - A setting of 256 samples is incredibly fast and highly recommended for live playing.

  - The plugin reports its exact hardware latency to your DAW automatically (Plugin Delay Compensation), so during playback and rendering, your tracks will always be perfectly in sync and on the grid!

</details>

<details>
  <summary>Why there is no fancy preset manager like the ones in Usual Suspects emulations?</summary>
The Usual Suspects are also developing an SD-1 emulation, so it's guaranteed that their work will be far superior to ours. Just wait and see.
</details>

<details>
  <summary>Why there is no AU version?</summary>
We are working on it. The AU version is really tricky; we haven't managed to create a build that works properly yet either (there are some AU specific codes already.)
</details>

<details>
  <summary>Why there is no Linux build?</summary>
Maybe in the future, or do it yourself if you can :)
</details>

# License and credits

Built with love by MAMEDev and contributors and sojusrecords.com

MAME® Legal Information<br/>
Disclaimer<br/>
The source code to MAME® is provided under the GNU General Public License version 2 or later as of Git revision 35ccf865aa366845b574e1fdbc71c4866b3d6a0f and the release of MAME 0.172. Source files may also be licensed as specified in the file header. This license does not apply to prior versions of MAME. 

MAME® Copyright (c) 1997-2026 MAMEDev and contributors

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.<br/>
This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.<br/>
You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

es5510, vfxcart, esqpump, panels license:<br/>
BSD 3 Clause | Copyright (c) Christian Brunschen

m68000, wd_fdc Emulation license<br/>
BSD 3 Clause | Copyright (c) Olivier Galibert

esq5505 Emulation license<br/>
BSD 3 Clause | Copyright (c) R. Belmont, Parduz

esqpanel, vfd, midi Emulation license<br/>
BSD 3 Clause | Copyright (c) R. Belmont

es5506, emu, emupal, speaker Emulation license<br/>
BSD 3 Clause | Copyright (c) Aaron Giles

mc68681 Emulation license<br/>
BSD 3 Clause | Copyright (c) Mariusz Wojcieszek, R. Belmont, Joseph Zatarski

hd63450 Emulation license<br/>
BSD 3 Clause | Copyright (c) Barry Rodewald

esqlcd Emulation license<br/>
BSD 3 Clause | Copyright (c) Parduz

nvram Emulation license<br/>
BSD 3 Clause | Copyright (c) Nigel Barnes

floppy Emulation license<br/>
BSD 3 Clause | Copyright (c) Nathan Woods, Olivier Galibert, Miodrag Milanovic

softlist_dev Emulation license<br/>
BSD 3 Clause | Copyright (c) Wilbert Pol

esq16_dsk Emulation license<br/>
BSD 3 Clause | Copyright (c) R. Belmont, Olivier Galibert

hxchfe_dsk Emulation license<br/>
BSD 3 Clause | Copyright (c) Michael Zapf

logmacro Emulation license<br/>
BSD 3 Clause | Copyright (c) Vas Crabb 

and so many others. Thank you for your work!

All trademarks are property of their respective owners.

Visit https://www.sojusrecords.com
