# Ensoniq® SD-1 32-Voice VST/AU Emulation
MAME®-based VST3/AU cycle-accurate emulation of the Ensoniq® SD-1 32-Voice synthesizer built with JUCE®

![Screenshot of Ensoniq SD-1 VST](https://github.com/kukoricajoe/Ensoniq-SD-1-32-voices-VST-emulation/blob/main/sd-1.png)
Ensoniq SD-1 with Compact panel in FL Studio

# Ensoniq SD-1
The SD-1 (1990) comes from a long line of Ensoniq's evolving Transwave® wavetable digital synthesizers. It began with Ensoniq's earliest synthesizer, the ESQ-1. That led to the SQ-80, then the VFX and VFX-SD (the latter featuring an on-board sequencer) and then to the SD-1 (and it eventually led to the Fizmo). The SD-1 allows for additive synthesis using waveform modulation, a sort of wavetable synthesis. This puts it into a unique class of digital synthesizers along with the PPG Wave series and Waldorf Microwave series.

The SD-1 can create all sorts of acoustic, electric, digital, and analog-like sounds. Its piano sound has over 1 MB of 16-bit waveforms to give it a full and rich realistic tone not found in other digital synthesizers of the time.

A single patch can contain up to 6 of the 168 waves in its ROM memory that can be combined and layered. Advanced and analog-like synth parameters including its dual multi-mode digital filters, three 11-stage envelopes, LFO, and 15 modulation sources allow you to further shape and morph your sounds. There's even a built-in 24-bit VLSI dual effects processor with reverb, chorus, flanging and delay. The SD-1 also has a standard 61-note keyboard with velocity sensitivity, polyphonic aftertouch and full MIDI implementation with 12 channels for multitimbral functions as well as four 16-bit DAC outputs.
Like the VFX-SD, the SD-1 has a professional quality on-board sequencer making it a complete all-in-one music production workstation. This is a 24-track sequencer with 25,000 note capacity and it holds up to 60 sequences and 20 songs. There is quantization (96 ppqn), real-time or step entry, looped or linear mode, and auto-punch in/out. Tracks can be set to control the SD-1's internal voices or external MIDI equipment, or both at the same time! An on-board 3.5" disk drive allows you to store your programs, sequences, songs, and even MIDI SysEx data. The SD-1 is compatible with all VFX and VFX-SD program librarys too! [Source](https://www.vintagesynth.com/ensoniq/sd-1)

# About this project
We are Sojus Records, one of the longest-running netlabels still active. We are musicians, not programmers, but we love old synths and emulations. We decided to build a fully featured VST3/AU version of the MAME-emulated Ensoniq SD-1/32, which has never been emulated before. Thanks to the recent AI coding revolution, we have successfully built it. Finally a good use of vibe coding! This proof-of-concept is an important step for both musicians and coders. We are looking forward to bringing other MAME synths to life in the future!

# Download 
  - [Win x64 VST3 W10+](https://github.com/sojusrecords/Ensoniq-SD-1-32-Voice-VST-emulation/releases/download/v0.9.9/EnsoniqSD1-v.0.9.9-winVST3.7z)

  - [Win x64 VST3 oldskool AVX1 for pre-Haswell machines](https://github.com/sojusrecords/Ensoniq-SD-1-32-Voice-VST-emulation/releases/download/v0.9.9/EnsoniqSD1-v.0.9.9-winVST3-AVX1.7z)

  - [macOS Universal Binary VST3 macOS 11 or newer](https://github.com/sojusrecords/Ensoniq-SD-1-32-Voice-VST-emulation/releases/download/v0.9.9/EnsoniqSD1-v.0.9.9-macVST3.7z)

  - [macOS Universal Binary AU SELECTED DAWS ONLY macOS 11 or newer](https://github.com/sojusrecords/Ensoniq-SD-1-32-Voice-VST-emulation/releases/download/v0.9.9/EnsoniqSD1-v.0.9.9-macAU.7z)

  - [Linux VST3 AVX2](https://github.com/sojusrecords/Ensoniq-SD-1-32-Voice-VST-emulation/releases/download/v0.9.9/EnsoniqSD1-v.0.9.9-LINUX-AVX2.7z)

  - [Linux VST3 Generic for oldskool CPUs](https://github.com/sojusrecords/Ensoniq-SD-1-32-Voice-VST-emulation/releases/download/v0.9.9/EnsoniqSD1-v.0.9.9-LINUX-Generic.7z)
  
# What's working?
Everything. Check the original manual here: [SD-1 Manual at Polynominal](https://www.polynominal.com/ensoniq-sd1/ensoniq-sd1-manual.pdf)

# Features:
- **NEW: Removed instance blocking** You can load as many instances of a plugin as you want in your DAW! Thanks to [kbaccki](https://github.com/kbaccki) for investigating the singletons.

- **NEW: macOS AU first public build for SELECTED DAWS ONLY!** Please note that the AU plugin is ONLY for Logic, GarageBand, MainStage, Ableton Live, Fender Studio Pro (Studio One) and Reaper. Any other DAW must use the VST3 version! Tested on Logic 11, Fender Studio Pro 8, Reaper 7, Ableton Live 12.

- **NEW: Preset saving macro button for users without MIDI controllers** [Read the FAQ for more info!](https://github.com/sojusrecords/Ensoniq-SD-1-32-Voice-VST-emulation#faq)

- Windows 10+ 64 bit VST3 AVX1/AVX2, Mac Intel+ARM Universal Binary VST3 and AU, Linux VST3 Generic/AVX2

- VFX, VFX-SD, SD1 .SYX SYS-EX file import. [Read the FAQ for more info!](https://github.com/sojusrecords/Ensoniq-SD-1-32-Voice-VST-emulation#faq)

- FULL DAW automation (All keys, sliders and buttons.) [Read the FAQ for more info!](https://github.com/sojusrecords/Ensoniq-SD-1-32-Voice-VST-emulation#faq)

- Global settings saving 

- VST3/AU state saving

- 4 different panel layouts with resizable GUI and VFD display

- Buffer setting

- 4 outputs: stereo main out, optional stereo aux (dry signal with no effects)

- Can load and save all compatible VFX/VFX-SD/SD1-24/SD1-32 disk images (.img, .hfe, .dsk, .eda) and cartridges (.eeprom, .rom, .cart, .sc32) [Read the FAQ for more info!](https://github.com/sojusrecords/Ensoniq-SD-1-32-Voice-VST-emulation#faq)

# Known limitations
- DAW automation is not visible on GUI and you can NOT directly select buttons or sliders from the GUI to assign them to a controller.

- No floppy drive sound :D

- Mac binaries require one of the following methods to run due to Apple's security policies:

  - **all macOS versions BEFORE Tahoe**:
    - **Manual authorization** - The user must go to System Settings > Privacy & Security and, after the DAW has attempted to load the plugin, click the "Open Anyway" button.
    - **Better method to remove quarantine in terminal.** Open a terminal window and run the following command on the plugin bundle:
    - **for VST3**:
    ```sudo xattr -rd com.apple.quarantine /Library/Audio/Plug-Ins/VST3/EnsoniqSD1.vst3```
    - **for AU**:
    ```sudo xattr -rd com.apple.quarantine /Library/Audio/Plug-Ins/Components/EnsoniqSD1.component```
  - Get Sentinel if you stuck with authorization: https://github.com/alienator88/Sentinel
  - **macOS versions Tahoe and later**:
    - Remove quarantine and codesign the plugin. Open a terminal window and run the following command on the plugin bundle:
    - install Xcode Command Line Tools if needed:
      - ```xcode-select --install```
    - **for VST3**:
      - ```sudo xattr -rd com.apple.quarantine /Library/Audio/Plug-Ins/VST3/EnsoniqSD1.vst3```
      - ```sudo codesign --force --deep --sign - /Library/Audio/Plug-Ins/VST3/EnsoniqSD1.vst3```
    - **for AU**:
      - ```sudo xattr -rd com.apple.quarantine /Library/Audio/Plug-Ins/Components/EnsoniqSD1.component```
      - ```sudo codesign --force --deep --sign - /Library/Audio/Plug-Ins/Components/EnsoniqSD1.component```

# Requirements
- Please note that this is a cycle-accurate hardware-level emulation of the synthesizer, so it places **heavy demands on the CPU!** Set the buffer setting to higher if buffer underrun occurs. Examples for buffer settings: 2011 Sandy Bridge (AVX1) 2 core i5-2415m: 4096, 2013 Haswell (AVX2) 4 core i7-4770: 2048, 2018 Coffee Lake 6 core i7-8700: 256, 2020 Apple ARM M1 Pro 6P/2E: 128.

- Windows 10 or newer. Windows build is AVX1 or AVX2 optimized.

- Linux Ubuntu 22.04 or equivalent or later. Linux build is AVX2 or Generic optimized.

- macOS 11 Big Sur or newer. As MAME itself cannot be compiled lower than Big Sur (macOS 11) this is the minimum OS for mac. If your Mac is stuck on an older OS, my suggestion is to try OpenCore Legacy Patcher to update your Mac to a compatible OS.

- A VST3 compatible DAW. The AU build is for selected DAWs only! If it's not working, come back later :)
  - Tested and working:
    - macOS VST3: Ableton Live 12, Bitwig Studio 6, Cubase 15, Fender Studio Pro 8, FL Studio 2025, Reaper 7 etc.

    - macOS AU: **SELECTED DAWS ONLY! Please note that the AU plugin is ONLY for Logic, GarageBand, MainStage, Ableton Live, Fender Studio Pro (Studio One) and Reaper. Any other DAW must use the VST3 version!** Tested on Logic 11, Fender Studio Pro 8, Reaper 7, Ableton Live 12.

    - Windows: Ableton Live 12, Bitwig Studio 6, Cubase 15, FL Studio 2025, Reaper 7, Cantabile, Reason 12.5 etc.

    - Linux: Bitwig Studio 6, Reaper 7 etc.

  - If it's not working check [Troubleshooting](#Troubleshooting).

- IMPORTANT - ROM Files Required!<br/>
  Due to copyright reasons, the required Ensoniq ROM files are NOT included.

  * We've removed the strict ROM verification. Now it's up to MAME to accept your files; we only check for their presence, and it doesn't matter whether they're organized into a folder within the zip file or not. If your ROM has been good so far, it will continue to be good. At startup, it checks for the presence of sd132.zip; if it doesn't find it, you can set the exact path using a button. We also check to see if all 10 files are present, and you can rescan the zip file without reloading the plugin. The plugin performs a self-check at every startup, which checks the following: whether it has write permissions to the temp folder and the EnsoniqSD1 folder, checks the Lua plugins, and verifies if the MAME engine failed to start for any reason.

  To make the plugin work:
  * Create a folder named EnsoniqSD1 in your user's Documents folder:

    - Win C:\Users\yourusername\Documents

    - macOS /yourusername/Documents

    - Linux /Documents

  * Obtain the Ensoniq SD-1/32 ```sd132``` variant AND Ensoniq ```2x40 VFD ROM``` files and place these files in that folder AND zip them to sd132.zip.<br/>

    - ```esqvfd_font_vfx.bin```<br/>
    - ```sd1_32_402_hi.bin```<br/>
    - ```sd1_32_402_lo.bin```<br/>
    - ```sd1_410_hi.bin```<br/>
    - ```sd1_410_lo.bin```<br/>
    - ```u34.bin```<br/>
    - ```u35.bin```<br/>
    - ```u36.bin```<br/>
    - ```u37.bin```<br/>
    - ```u38.bin```<br/>

  * The final structure of sd132.zip in your Documents/EnsoniqSD1 folder looks like this:<br/>
    ![sd132.zip](https://github.com/sojusrecords/Ensoniq-SD-1-32-Voice-VST-emulation/blob/main/roms.png)<br/>
  
  - Optional: If you want to run the internal sequencer, you need the original disk image:
    - Ensoniq SD1 Sequencer OS v410 (SD-1 800K type) [Read the FAQ for more info!](https://github.com/sojusrecords/Ensoniq-SD-1-32-Voice-VST-emulation#faq)

# Troubleshooting
- The plugin performs a self-check at every startup, which checks the following: whether it has write permissions to the temp folder and the EnsoniqSD1 folder, checks the Lua plugins, and verifies if the MAME engine failed to start for any reason. It will notify you if it finds any errors.

- We've removed the strict ROM verification. Now it's up to MAME to accept your files; we only check for their presence, and it doesn't matter whether they're organized into a folder within the zip file or not. If your ROM has been good so far, it will continue to be good. At startup, it checks for the presence of sd132.zip; if it doesn't find it, you can set the exact path using a button. We also check to see if all 10 files are present, and you can rescan the zip file without reloading the plugin.

- Whitelist the plugin in your antivirus app. The plugin is writing some data to temp folder (e.g. nvram and osram files, lua plugins) and to your Documents/EnsoniqSD1 folder (e.g. settings.xml)

- Your sequencer is blacklisting the plugin: if the plugin scanner provides error message or a log file then send it to us.

- Reset global settings: go to Documents/EnsoniqSD1 and delete the file "settings.xml" and delete temp files

- Report problems at [GitHub issues](https://github.com/sojusrecords/Ensoniq-SD-1-32-Voice-VST-emulation/issues)

# FAQ
<details>
  <summary>How do I load my old SYS-EX (.syx) preset banks?</summary>

Loading SYS-EX files works exactly like the original 1990 hardware, simulating a physical MIDI cable connection at a 31250 baud rate.

Step-by-step:

  - **IMPORTANT** Enable SYS-EX on the Synth: On the SD-1 front panel, press System/MIDI CONTROL button TWICE, and set SYS-EX to ON.

  - Go to a safe screen: Press the Sounds or Presets button to return to the main playing screen.

  - Load the file: Click the Load Media button on the plugin interface and select your .syx file. A standard 64KB bank takes about 12 to 15 seconds to transfer. This is an authentic hardware limitation (the maximum speed of a physical MIDI cable).

  - You will see a "Transmitting SYS-EX Data..." overlay on the screen. Once the overlay disappears, the synth will instantly update its RAM, and your presets will be ready to play!

  - You can also save the presets to a disk image. Here you can find an [SD-1 formatted empty hfe disk image (1.44MB).](https://github.com/sojusrecords/Ensoniq-SD-1-32-Voice-VST-emulation/blob/main/SD-1-EMPTY-DISK.hfe) Thanks to Headroom for it.
</details>

<details>
  <summary>How can I save my presets?</summary>

  - Saving on the real hardware requires holding down PRESETS button and pressing a BANK button.

  - We added a macro button (SAVE PRESET) so users without a MIDI controller can also save presets.

  - You can also assign the buttons to a physical or virtual MIDI controller. With this workaround you can save the preset as you can hold down any buttons simultaneous.

</details>

<details>
  <summary>How can I automate the plugin's buttons and sliders?</summary>

  - You can NOT directly select buttons or sliders from the GUI to assign them to a controller. You should use your DAW's VST automation parameter listing for the plugin and choose from there.

  - Note: DAW automation is not visible on GUI!

</details>

<details>
  <summary>How do I load Floppy Disk Images (.img, .hfe, .dsk, .eda)?</summary>

   - Attach the disk image using the "Load Floppy/Cart/SYX" button. Press Storage, then select DISK. Press LOAD. The display will show the Disk Load page with the File Type selected. Move the data entry slider or push up/down buttons to select your file. You can convert SYS-EX data to [disk images with this tool](https://github.com/joemcmahon/sd1diskutil).

</details>

<details>
  <summary>How do I save my stuff to Floppy Disk Images (.img, .hfe, .dsk, .eda)?</summary>

   - Here you can find an [SD-1 formatted empty hfe disk image (1.44MB).](https://github.com/sojusrecords/Ensoniq-SD-1-32-Voice-VST-emulation/blob/main/SD-1-EMPTY-DISK.hfe)

   - Attach the disk image using the "Load Floppy/Cart/SYX" button. Press Storage, then select DISK. Press SAVE. The display will show the Disk SAVE page with the File Type selected. Move the data entry slider or push up/down buttons to select save type. Press *YES*. You can convert SYS-EX data to [disk images with this tool](https://github.com/joemcmahon/sd1diskutil).

</details>

<details>
  <summary>How can I load/save my Cartridges (.eeprom, .rom, .cart, .sc32)?</summary>

   - Attach the cartridge image using the "Load Floppy/Cart/SYX" button. Press Storage, then select CARTRIDGE. Choose PROGRAMS/PRESETS/BOTH. Choose Internal-to-Cartridge (SAVE) or Cartridge-to-Internal (LOAD).

</details>

<details>
  <summary>How can I load and use the internal Sequencer?</summary>

    - How to load: Attach the disk image using "Load Floppy/Cart/SYX". Press Storage, then select DISK. Press LOAD. The display will show the Disk Load page with the File Type selected. Move the data entry slider or push up/down buttons all the way up to select TYPE=SEQUENCER OS.

    - Now load the sequencer with pushing the SEQ button.

</details>

<details>
  <summary>Audio Settings: Why can't I set the Buffer to 0?</summary>

This plugin runs a cycle-accurate emulation of the original Motorola 68000 CPU, Ensoniq OTTO (ES-5506) and Ensoniq ESP (ES5510) and other chips. On the real physical SD-1, the time it takes from pressing a key to hearing a sound is approximately 24.4 milliseconds.

Because the MAME emulator runs asynchronously on its own background thread, we need a tiny "safety pool" (the plugin buffer) to ensure the audio stream never drops out.

  - A setting of 128 samples is incredibly fast and highly recommended for live playing.

  - The plugin reports its exact hardware latency to your DAW automatically (Plugin Delay Compensation), so during playback and rendering, your tracks will always be perfectly in sync and on the grid!

</details>

<details>
  <summary>Why there is no fancy preset manager like the ones in Usual Suspects emulations?</summary>

The Usual Suspects are also developing an SD-1 emulation, so it's guaranteed that their work will be far superior to ours. Just wait and see.

</details>

# In the news

- [Synth Anatomy](https://synthanatomy.com/2026/03/sojus-records-ensoniq-sd-1-an-open-source-emulation-of-the-1990-transewavetm-synth.html)

- [GearNews](https://www.gearnews.com/synth-emulations-synth/)

- [PrismNews](https://www.prismnews.com/hobbies/vintage-synthesizers/rom-based-synth-emulators-legal-and-practical-guide-for)

- [Bedroom Producers Blog](https://bedroomproducersblog.com/2026/04/02/sojus-ensoniq-sd/)

- [MatrixSynth](https://www.matrixsynth.com/2026/03/free-synth-ensoniq-sd-1-emulator-by.html)

- [project of napskint (J)](https://projectofnapskint.com/sd-1-emulation/)

- [BuenasIdeas (DE)](https://www.buenasideas.de/2026/ensoniq-sd1-legendaerer-synthesizer-kehrt-als-kostenlose-vst-emulation-zurueck/)

- [Rekkerd](https://rekkerd.org/sojus-records-releases-ensoniq-sd-1-32-free-synthesizer-vst3/)

- [AZU Soundworks (J)](https://azu-soundworks.net/%E3%80%90%E7%84%A1%E6%96%99%E3%80%91sojus%E3%81%8C90%E5%B9%B4%E4%BB%A3%E3%81%AE%E5%90%8D%E6%A9%9F%E3%80%8Censoniq-sd-1%E3%80%8D%E3%82%92%E5%AE%8C%E5%85%A8%E5%86%8D%E7%8F%BE%E3%81%97%E3%81%9F%E3%82%BD/)

- [GuitarGeek (F)](https://guitargeek.fr/emulations-de-synthetiseur-executer-des-synthetiseurs-sur-votre-ordinateur/)

- [SoHu (PRC)](https://www.sohu.com/a/1000002551_455142)

# License and credits

Built with love by MAMEDev and contributors and sojusrecords.com

MAME® Legal Information<br/>
Disclaimer<br/>
The source code to MAME® is provided under the GNU General Public License version 2 or later as of Git revision 35ccf865aa366845b574e1fdbc71c4866b3d6a0f and the release of MAME 0.172. Source files may also be licensed as specified in the file header. This license does not apply to prior versions of MAME. 

MAME® Copyright © 1997-2026 [MAMEDev and contributors](https://www.mamedev.org/)

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.<br/>
This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.<br/>
You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

es5510, vfxcart, esqpump, panels license:<br/>
BSD 3 Clause | Copyright © Christian Brunschen

m68000, wd_fdc Emulation license<br/>
BSD 3 Clause | Copyright © Olivier Galibert

esq5505 Emulation license<br/>
BSD 3 Clause | Copyright © R. Belmont, Parduz

esqpanel, vfd, midi Emulation license<br/>
BSD 3 Clause | Copyright © R. Belmont

es5506, emu, emupal, speaker Emulation license<br/>
BSD 3 Clause | Copyright © Aaron Giles

mc68681 Emulation license<br/>
BSD 3 Clause | Copyright © Mariusz Wojcieszek, R. Belmont, Joseph Zatarski

hd63450 Emulation license<br/>
BSD 3 Clause | Copyright © Barry Rodewald

esqlcd Emulation license<br/>
BSD 3 Clause | Copyright © Parduz

nvram Emulation license<br/>
BSD 3 Clause | Copyright © Nigel Barnes

floppy Emulation license<br/>
BSD 3 Clause | Copyright © Nathan Woods, Olivier Galibert, Miodrag Milanovic

softlist_dev Emulation license<br/>
BSD 3 Clause | Copyright © Wilbert Pol

esq16_dsk Emulation license<br/>
BSD 3 Clause | Copyright © R. Belmont, Olivier Galibert

hxchfe_dsk Emulation license<br/>
BSD 3 Clause | Copyright © Michael Zapf

logmacro Emulation license<br/>
BSD 3 Clause | Copyright © Vas Crabb 

and so many others. Thank you for your work!

Built with JUCE Framework [© Raw Material Software Limited](https://github.com/juce-framework/JUCE)

All trademarks are property of their respective owners.

Visit https://www.sojusrecords.com
