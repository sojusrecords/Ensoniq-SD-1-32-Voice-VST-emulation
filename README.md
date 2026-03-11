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

# What's working?
Everything. Check the original manual here: [Polynominal](https://www.polynominal.com/ensoniq-sd1/ensoniq-sd1-manual.pdf)

# Features:
- Mac Intel+ARM universal binaries (Intel Windows build is in progress)
- VST3 state saving, MIDI input and automation, 4 different panel layouts, resizable GUI with VFD display, and an onboard keyboard
- Can load all compatible VFX/VFX-SD/SD1-24/SD1-32 disk images and cartridges (.img .bin .crt etc)
  - How to load: Attach the disk image using the "Load Floppy/Cartridge" button. Press Storage, then select DISK. Press LOAD. The display will show the Disk Load page with the File Type selected. Move the data entry slider to select your file.

# Known limitations
- Mac Universal Binary only (Windows build is currently in progress).
- Only 1 instance can run at a time per DAW due to engine limitations.
- No floppy drive sound :D
- Mac binaries require one of the following methods to run due to Apple's security policies:
  - Manual authorization: The user must go to System Settings > Privacy & Security and, after the DAW has attempted to load the plugin, click the "Open Anyway" button.
  - or
  - Removing quarantine (via Terminal): Run the following command on the plugin bundle:
  sudo xattr -rd com.apple.quarantine /Library/Audio/Plug-Ins/VST3/EnsoniqSD1.vst3

# Requirements
- macOS 14 (Ventura) or newer. (Windows 10 or newer when available).
- A VST3 compatible DAW.
- Original Ensoniq SD-1 32-voice variant AND Ensoniq 2x40 VFD ROM files placed at this exact location (can be zipped into (can be zipped into sd132.zip):

  Windows: 
  
  Mac: /Users/yourusername/Documents/EnsoniqSD1
  
  Filename | SHA256
  - esqvfd_font_vfx.bin ab2f7ddc6ab7fafaf07985d01788197849cdaeb5a4a7d9f2f85098dfd65edf01
  - sd1_32_402_hi.bin 90ae35de8661f5de0793b6ea59a4d6524e90c0828a29e6ea8906ff759116136d
  - sd1_32_402_lo.bin 6b0c1235c4f813ce8698e89d66933e9c7c9168f4a095c9e2a50add7fe729481c
  - sd1_410_hi.bin 1d6d6150373fb070da8b1a6da57762749bda9210e0ca5536441bb8194a3cafb7
  - sd1_410_lo.bin e3e42beca41989561c0d2a8266e48549561650a7606bb8a0d75b438847e8bd0c
  - u34.bin 7a6e6e76da7eb8de5cbc3a0a2bfb27a461e312facdcc0b7ecc42b9d1eb261e12
  - u35.bin 1df911a97e0e5a334d9345ba5e47eac7794d083282012f7ecf70901b88cf7e08
  - u36.bin 2fdb401bea78eb323fa55408760a73319aeae68b465f193dc7a46d1b21277cdd
  - u37.bin e08931013c8aca2460b4f2c3512e1d3e9a610a7f921e22012bb13bd23a3e56d7
  - u38.bin 2f185a185961a1c14472c2b706642c0d9e7a0792d57d946a349840905782e5ca
  
- Optional: If you want to run the internal sequencer, you need the original disk image:
  - Ensoniq SD1 Sequencer OS v410 (SD-1 800K type)
  - How to load: Attach the disk image using "Load Floppy/Cartridge". Press Storage, then select DISK. Press LOAD. The display will show the Disk Load page with the File Type selected. Move the data entry slider all the way up to select TYPE=SEQUENCER OS.

# License and credits

Built with love by Christian Brunschen and sojusrecords.com

MAME® Legal Information<br/>
Disclaimer<br/>
The source code to MAME® is provided under the GNU General Public License version 2 or later as of Git revision 35ccf865aa366845b574e1fdbc71c4866b3d6a0f and the release of MAME 0.172. Source files may also be licensed as specified in the file header. This license does not apply to prior versions of MAME. 

MAME® Copyright (c) 1997-2026  MAMEDev and contributors

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.<br/>
This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.<br/>
You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

SD-1 Emulation license:<br/>
BSD 3 Clause | Copyright (c) Christian Brunschen

All trademarks are property of their respective owners.

Visit https://www.sojusrecords.com
