# Build Instructions for Ensoniq SD-1 32-Voice VST

This project embeds a headless MAME emulation engine inside a JUCE VST3 plugin container to perfectly recreate the Ensoniq SD-1 synthesizer.

Because MAME is not natively designed to be run as a shared library inside a DAW, building this project requires patching the MAME source code, building it, and extracting the generated archive files to link them to the JUCE project.

## Prerequisites

**JUCE Framework** (v7 or v8, built on V8)

**MAME Source Code** (Built on version 0.286) ```git clone --depth 1 https://github.com/mamedev/mame.git```

**macOS Build:** Xcode, SDL2 & SDL3 Frameworks (Installed in /Library/Frameworks) ```xcode-select --install``` then ```brew install pcre2 qt asio sqlite utf8proc flac pugixml portmidi portaudio pkg-config```

**Windows Build:** GCC (for GENIE), MSYS2, Python3 (for MAME compilation) and Visual Studio (built on 2026) (for VST3 plugin generation).

## Step 1: Patching the MAME Source

We had to make specific modifications to the MAME core to expose certain internal states and adapt the Ensoniq sound chip for VST audio streaming.

1. Download or clone the vanilla MAME source code.

2. Locate the MAME_Patches folder in this repository.

3. Copy and replace the following files in your MAME source tree:

   - ui.cpp -> Overwrite in /src/frontend/mame/ui/ui.cpp

   - esq5505.cpp -> Overwrite in src/mame/ensoniq/esq5505.cpp

   - sd132.lay -> Overwrite in /src/mame/layout/sd132.lay

## Step 2: Compiling MAME and Extracting Archives

Instead of building a standalone static library, we build the standard MAME executable and extract the generated object archives (.a/.o/lib files) created during the build process.

1. Read the official MAME instructions. Build MAME from the source via terminal/UCRT64 (don't forget to cd to your MAME folder first) 
   - ```make SUBTARGET=sd132 SOURCES=src/mame/ensoniq/esq5505.cpp USE_BGFX=0 TARGETOS=macosx PTR64=1 PRECOMPILE=0 ARCHOPTS="-arch arm64" -j8``` on mac,
   - ```make vs2022 SUBTARGET=sd132 SOURCES=src/mame/ensoniq/esq5505.cpp -j8 USE_BGFX=0``` on win). 

2. Windows only steps

   - macOS: You have the final stuff typically located at [MAME_SOURCE_DIR]/build/osx_clang/bin/x64/Release/ (or arm64 if you are on Apple Silicon).

   - Windows: We have to compile MAME in VS, follow the steps: go to the Visual Studio project folder typically located at [MAME_SOURCE_DIR]\build\projects\windows\mamesd132\vs2022
     
     - Open this in Visual Studio: sd132.vcxproj and follow the steps:
     
     - Retargeting: Upgrade to the current Visual Studio tools by selecting the Project menu or right-click the solution, and then selecting "Retarget solution" v143 to v145
     - NTDDI_VERSION confilct: In Property manager select all properties, right click - Properties, C/C++, Command line - Additional options a put this after the last "different options" ``` /U_WIN32_WINNT /UNTDDI_VERSION /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A000000``` There is a space before the /U!
     - After this you can check the output files here: [MAME_SOURCE_DIR]\build\vs2022\bin\x64\Release

3. You got the generated .a and .o (mac) .lib (win) files (such as liboptional.a/lib, libmame_mame.a/lib, libemu.a/lib, etc.) which contain the compiled engine.

## Step 3: Configuring Projucer

1. Open EnsoniqSD1-yourOS.jucer using the Projucer application. If you want to build for older OSes or computers: you can choose C++17 version for older macOS versions (EnsoniqSD1-macOS-oldskool) or AVX1 optimized version for Windows (EnsoniqSD1-WIN-oldskool-AVX1)

2. Go to the Exporters tab (e.g., Xcode macOS or Visual Studio 2026).

3. **Important:** Global Settings:

   - Update the Header Search Paths to point to your local MAME source directory.

   - Update the Library Search Paths to point to the correct Release directory from Step 2.

   - macOS only: put your mame/plugins folder into "Custom Xcode Resource Folders"

   - Ensure all the required MAME .a/.o/lib files are listed in your External Libraries to Link section.

## Windows-Specific Optimizations (Visual Studio 2026)

To ensure flawless real-time audio performance and zero dropouts on Windows, the Visual Studio 2026 exporter must be configured precisely:

- Select the Release configuration under the Visual Studio 2022 exporter.

- Set Optimisation to Maximise speed (/O2).

- Set Link Time Optimisation (LTO) to Enabled (Triggers /GL Whole Program Optimization).

- In the Extra Compiler Flags field, add: /Oi /Ot (Enables intrinsic functions and heavily favors fast code execution).

- Save the project and click "Open in IDE".

- Inline Function Expansion: Any Suitable (/Ob2) (in VS)

## Step 4: Building the VST3

**macOS (Xcode)**

- In Xcode, select the Ensoniq SD-1 - VST3 target.

- Build the project (Cmd + B).

### A Note on macOS Sandboxing & Post-Build Scripts
If you check the Projucer settings, you will see a custom Post-Build Script.
Because macOS Sequoia and strict DAW sandboxing block dynamic external library loading, this script automatically:

- Creates a Frameworks folder inside the generated .vst3 bundle.

- Copies the required SDL2.framework and SDL3.framework into the VST3.

- Applies an Ad-Hoc codesign to both the frameworks and the final VST3 plugin.

- The project uses double @rpath linker flags (-Wl,-rpath,/Library/Frameworks -Wl,-rpath,@loader_path/../Frameworks) so it works seamlessly both on the developer's machine and the end-user's machine.

**Windows (Visual Studio 2026)**

- In Visual Studio, ensure your build target is set to Release and x64.

- Build the Solution (Ctrl + Shift + B).

- The compiled .vst3 file will be located in your Builds\VisualStudio2022\x64\Release\VST3 directory.

## Step 5: ROM Installation

- IMPORTANT - ROM Files Required!
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
  - u38.bin<br/>2f185a185961a1c14472c2b706642c0d9e7a0792d57d946a349840905782e5ca
* The final structure of sd132.zip in your Documents/EnsoniqSD1 folder looks like this:<br/>
![sd132.zip](https://github.com/sojusrecords/Ensoniq-SD-1-32-Voice-VST-emulation/blob/main/roms.png)