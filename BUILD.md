# Build Instructions for Ensoniq SD-1 32-Voice VST

This project embeds a headless MAME emulation engine inside a JUCE VST3 plugin container to perfectly recreate the Ensoniq SD-1 synthesizer.

Because MAME is not natively designed to be run as a shared library inside a DAW, building this project requires patching the MAME source code, building it, and extracting the generated archive files to link them to the JUCE project.

## Prerequisites

**JUCE Framework** (v7 or v8, built on V8)

**MAME Source Code** (Built on version 0.287) ```git clone --depth 1 https://github.com/mamedev/mame.git```

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

   - esq16_dsk.cpp -> Overwrite in /src/lib/formats/esq16_dsk.cpp

   - required for macOS C++17 only: ioport.h -> Overwrite in /src/emu/ioport.h 

   - required for macOS C++17 only: language.h -> Overwrite in /src/lib/util/language.h

## Step 2: Compiling MAME and Extracting Archives

Instead of building a standalone static library, we build the standard MAME executable and extract the generated object archives (.a/.o/lib files) created during the build process.

1. Read the official MAME instructions. Build MAME from the source via terminal/UCRT64 (don't forget to cd to your MAME folder first) 
   - macOS:
   - ```export MACOSX_DEPLOYMENT_TARGET=11.00```
   - ```export CXXFLAGS="-D_LIBCPP_AVAILABILITY_HAS_INIT_PRIMARY_EXCEPTION=0"```
   - ```make SOURCES=src/mame/ensoniq/esq5505.cpp USE_BGFX=0 TARGETOS=macosx PTR64=1 PRECOMPILE=0 ARCHOPTS="-arch x86_64 -arch arm64" REGENIE=1 -j8```
   - Windows:
   - ```make vs2022 SUBTARGET=sd132 SOURCES=src/mame/ensoniq/esq5505.cpp -j8 USE_BGFX=0```

   - macOS: You have the final stuff typically located at [MAME_SOURCE_DIR]/build/osx_clang/bin/x64/Release/ (or arm64 if you are on Apple Silicon).

2. Windows only steps

   - Windows: We have to compile MAME in VS:

     - Go to the Visual Studio project folder typically located at [MAME_SOURCE_DIR]\build\projects\windows\mamesd132\vs2022
     
     - Open this in Visual Studio: sd132.vcxproj and follow the steps:

     - Set to Release the target
     
     - Retargeting: Upgrade to the current Visual Studio tools by selecting the Project menu or right-click the solution, and then selecting "Retarget solution" v143 to v145
     - NTDDI_VERSION conflict: In Property manager select all properties, right click - Properties, C/C++, Command line - Additional options a put this after the last "different options" ``` /U_WIN32_WINNT /UNTDDI_VERSION /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A000000``` There is a space before the /U!

     - Set "Treat warnings as errors" to "No" in Configuration Properties -> C/C++->General

     - After this you can check the output files here: [MAME_SOURCE_DIR]\build\vs2022\bin\x64\Release

3. You got the generated .a and .o (mac) .lib (win) files (such as liboptional.a/lib, libmame_mame.a/lib, libemu.a/lib, etc.) which contain the compiled engine.

## Step 3: Configuring Projucer

1. Open EnsoniqSD1-yourOS.jucer using the Projucer application. If you want to build for older OSes or computers: AVX1 optimized version for Windows (EnsoniqSD1-WIN-oldskool-AVX1) and also there is a Generic build for Linux in linux_build.yml

2. Go to the Exporters tab (e.g., Xcode macOS or Visual Studio 2026).

3. **Important:** Global Settings:

   - Update the Header Search Paths to point to your local MAME source directory.

   - Update the Library Search Paths to point to the correct Release directory from Step 2.

   - macOS only: put your mame/plugins folder into "Custom Xcode Resource Folders"

   - Ensure all the required MAME .a/.o/lib files are listed in your External Libraries to Link section.

## Windows-Specific Optimizations (Visual Studio 2026)

To ensure flawless real-time audio performance and zero dropouts on Windows, the Visual Studio 2026 exporter must be configured precisely:

- Select the Release configuration under the Visual Studio 2026 exporter.

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
- Linux /yourusername/Documents/
* Obtain the Ensoniq SD-1 32 variant AND Ensoniq 2x40 VFD ROM files and place files in that folder AND zip them to sd132.zip.<br/>
