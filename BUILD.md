# Build Instructions for Ensoniq SD-1 VST

This project embeds a headless MAME emulation engine inside a JUCE VST3 plugin container to perfectly recreate the Ensoniq SD-1 synthesizer. 

Because MAME is not natively designed to be run as a shared library inside a DAW, building this project requires patching the MAME source code, building it, and extracting the generated archive files to link them to the JUCE project.

## Prerequisites

1. **JUCE Framework** (v7 or v8, built on V8)
2. **MAME Source Code** (Built on version 0.286)
3. **SDL2 & SDL3 Frameworks** (Installed in `/Library/Frameworks` on macOS)
4. **Xcode** (for macOS build) or **MSYS2** (for Windows)

## Step 1: Patching the MAME Source

We had to make specific modifications to the MAME core to expose certain internal states and adapt the Ensoniq sound chip for VST audio streaming.

1. Download or clone the vanilla MAME source code.
2. Locate the `MAME_Patches` folder in this repository.
3. Copy and replace the following files in your MAME source tree:
   - `ui.cpp` -> Overwrite in `/src/frontend/mame/ui/ui.cpp`
   - `esq5505.cpp` -> Overwrite in `src/mame/ensoniq/esq5505.cpp` 

## Step 2: Compiling MAME and Extracting Archives

Instead of building a standalone static library, we build the standard MAME executable and extract the generated object archives (`.a` files) created during the build process.

1. Build MAME from the source via terminal (e.g., using `make SUBTARGET=sd132 USE_BGFX=0`. Don't forget to add universal binary switches if applicable (e.g. `TARGETOS=macosx PTR64=1 PRECOMPILE=0 ARCHOPTS="-arch arm64"`)
2. Once compiled, navigate to the internal build directory. On macOS, this is typically located at:
   `[MAME_SOURCE_DIR]/build/osx_clang/bin/x64/Release/` (or `arm64` if you are on Apple Silicon).
3. Here you will find the generated `.a` files (such as `liboptional.a`, `libmame_mame.a`, `libemu.a`, etc.) which contain the compiled engine.

## Step 3: Configuring Projucer

1. Open `EnsoniqSD1.jucer` using the **Projucer** application.
2. Go to the **Exporters** tab (e.g., Xcode macOS).
3. **Important:** - Update the **Header Search Paths** to point to your local MAME source directory.
   - Update the **Library Search Paths** to point to the `build/osx_clang/bin/.../Release/` directory from Step 2.
   - Ensure all the required MAME `.a` files are listed in your **External Libraries to Link** section.
4. Save the project and click **"Open in IDE"**.

## Step 4: Building the VST3 in Xcode (macOS)

1. In Xcode, select the `Ensoniq SD-1 - VST3` target.
2. Build the project (`Cmd + B`).

### A Note on macOS Sandboxing & Post-Build Scripts
If you check the Projucer settings, you will see a custom **Post-Build Script**. 
Because macOS Sequoia and strict DAW sandboxing block dynamic external library loading, this script automatically:
- Creates a `Frameworks` folder inside the generated `.vst3` bundle.
- Copies the required `SDL2.framework` and `SDL3.framework` into the VST3.
- Applies an Ad-Hoc `codesign` to both the frameworks and the final VST3 plugin.
- The project uses double `@rpath` linker flags (`-Wl,-rpath,/Library/Frameworks -Wl,-rpath,@loader_path/../Frameworks`) so it works seamlessly both on the developer's machine and the end-user's machine.

## Step 5: ROM Installation

Due to copyright reasons, the Ensoniq ROM files are **not** included in this repository. 
1. Once built, open the plugin in your DAW. 
2. A warning screen will appear showing you the correct path (e.g., `~/Documents/EnsoniqSD1/`).
3. Place your `sd132.zip` ROM file into that directory and reload the plugin.