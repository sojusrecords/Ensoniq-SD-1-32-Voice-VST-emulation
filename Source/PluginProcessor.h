/*
  ==============================================================================

    Ensoniq SD-1 MAME VST Emulation
    Open Source GPLv2/v3
    https://www.sojusrecords.com

  ==============================================================================
*/

#pragma once

// Uncomment to enable the debug rack panel (file manager + rack visible simultaneously)
//#define SD1_DEBUG_RACK_PANEL

// ==============================================================================
// MANDATORY MAME MACROS - MUST BE DEFINED BEFORE ANY MAME INCLUDES!
// These ensure compatibility with the MAME core data types and architectures.
// ==============================================================================
#ifndef PTR64
#define PTR64 1
#define LSB_FIRST 1
#define NDEBUG 1
#define __STDC_LIMIT_MACROS 1
#define __STDC_FORMAT_MACROS 1
#define __STDC_CONSTANT_MACROS 1
#endif

#include <JuceHeader.h>
#include <fstream> 

// MAME Core Includes
#include "emu.h"
#include "mame.h"

// Threading & Synchronization
#include <thread>
#include <atomic>
#include <mutex>

//==============================================================================

class EnsoniqSD1AudioProcessor : public juce::AudioProcessor,
    public juce::AudioProcessorValueTreeState::Listener
{
public:
    
    // --- VFD DISPLAY & LED HARDWARE STATES ---
        static constexpr int VFD_SIZE = 80; // 2 rows x 40 characters
        
        // Stores the raw 14-segment bitmask for each character
        std::atomic<uint16_t> vfdSegments[VFD_SIZE];
        
        // Stores the 32-bit integer where each bit represents a specific panel LED
        std::atomic<uint32_t> ledStateMask{ 0 };

        // MAME callback function triggered whenever a hardware output changes
        static void mameOutputNotifier(const char *outname, s32 value, void *param);
        
        // API for the Editor / File Manager to read the hardware state safely
        juce::String getHardwareVfdText();
        bool isHardwareLedOn(int ledBitIndex);

        // Dynamic dictionary to translate hardware bitmasks back to text
        std::unordered_map<uint16_t, char> segmentToAscii;
        void buildVfdDictionary();
    
    // New atomic flag to signal that the MAME engine is fully initialized and clocks are valid
        std::atomic<bool> mameIsFullyBooted{ false };
    
    // --- SELF CHECK ---
        std::atomic<bool> isSelfCheckFailed{ false };
        juce::String selfCheckErrorMsg { "" };
        bool runSelfCheck();
    
    // --- ROM MANAGEMENT ---
        juce::String customRomPath { "" };
        
        // --- FOLDER BOOKMARKS (max 10, persisted in settings.xml) ---
        juce::StringArray bookmarkFolders;
        
        // --- FILE MANAGER STATE (survives Editor destroy/recreate) ---
        struct FileManagerState {
            bool visible = false;
            juce::String category;          // "INT (RAM)", "ROM0", "BOOKMARK:/path", etc.
            juce::String openedFilePath;    // full path of opened file (if external)
            bool viewingDiskBank = false;
            juce::String openedDiskBankName;
            int selectedRow = -1;           // contentList row (fallback only)
            int bankSelectedRow = -1;       // bankContentList row (fallback only)
            juce::String selectedName;      // actual item name in contentList (primary restore key)
            juce::String bankSelectedName;  // actual item name in bankContentList (primary restore key)
            int scrollPosition = 0;         // contentList top row
            int bankScrollPosition = 0;     // bankContentList top row
            juce::String activeBookmark;    // bookmark path active at save time (for song state)
            int viewBeforeBrowser = 0;      // panel view index to restore when closing file manager
            int fmWindowWidth = 1200;       // Dedicated width for File Manager
            int fmWindowHeight = 925;       // Dedicated height for File Manager
        };
        FileManagerState fileManagerState;
        std::atomic<bool> stateJustLoaded{ false };  // prevents Editor destructor from overwriting song state
        std::atomic<bool> isWarmBoot{ false }; // NEW INSTANCE OR LOAD STATE
        std::atomic<bool> requestFileManagerUIRefresh{ false }; // Notifies GUI to update File Manager after state load
        std::atomic<bool> showWelcomeMessage{ false };  // Flag for first-launch UX message
        void checkRomAndBootMame();
    
    // --- GLOBAL SETTINGS ---
        std::atomic<bool> requestGlobalSave{ false };
        void loadGlobalSettings();
    
    // --- COMPARE STATE MANAGEMENT ---
    void forceCompareOff();
    std::atomic<double> scheduledCompareResetTime{ -1.0 };

    // --- MAME STATE MANAGEMENT ---
    // Used to safely orchestrate loading/saving states between the UI and the MAME thread
    std::atomic<bool> requestMameSave{ false };
    std::atomic<bool> requestMameLoad{ false };
    std::atomic<bool> mameStateIsReady{ false };
    juce::WaitableEvent mameStateEvent{ false };
    
    // Countdown timer (in samples) to trigger a delayed MIDI Panic after a state load
    std::atomic<int> panicDelaySamples{ 0 };

    // --- MEDIA HANDLING (FLOPPY/CARTRIDGE/SYSEX) ---
    std::atomic<bool> requestFloppyLoad{ false };
    std::atomic<bool> requestCartLoad{ false };
    std::string pendingFloppyPath;
    std::string pendingCartPath;
    std::mutex mediaMutex;
    
    // --- MEDIA STATE TRACKING ---
        std::atomic<bool> isFloppyLoaded{ false };
        std::atomic<bool> isCartLoaded{ false };
        juce::String loadedFloppyName{ "" };
        juce::String loadedCartName{ "" };

    // --- WINDOW SIZE PERSISTENCE ---
    // Stores the last window size set by the user to recall it upon project load
    int savedWindowWidth{ 0 };
    int savedWindowHeight{ 0 };

    // --- SYNCHRONIZATION ---
    // Throttle event used to prevent MAME from generating audio faster than the DAW consumes it
    juce::WaitableEvent mameThrottleEvent{ false };
    bool isMameRunningFlag() const { return isMameRunning.load(); }
    std::atomic<bool> isRomMissing{ false };

    
    // Flag to indicate if the zip file exists but contains invalid/missing ROMs
    std::atomic<bool> isRomInvalid{ false };

    std::atomic<bool> mameHasStarted{ false };
    std::atomic<double> initialSampleRate{ 0.0 };
    std::atomic<bool> sampleRateMismatch{ false };
    
    // Flag to indicate if the plugin is running as an AU in an unsupported host (e.g., FL Studio, Ableton)
    std::atomic<bool> isUnsupportedAUHost{ false };
    bool isMaschineHost = false;  // Set once in prepareToPlay, read-only in processBlock
    bool maschineInFastRender = false; // True once WAV RENDER FIX confirms fast render; resets on stop
    
    uint64_t getTotalRead() const { return totalRead.load(std::memory_order_acquire); }
    uint64_t getTotalWritten() const { return totalWritten.load(std::memory_order_acquire); }

    //==============================================================================
    EnsoniqSD1AudioProcessor();
    ~EnsoniqSD1AudioProcessor() override;

    //==============================================================================
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    
            // FULL SD-1 HARDWARE MATRIX DEFINITION
            struct SD1ButtonDef {
                juce::String paramID;
                juce::String paramName;
                const char* ioportTag;
                uint32_t ioportMask;
            };

            const std::vector<SD1ButtonDef> sd1Buttons = {
                // --- MASTER / MODE ---
                { "btn_cartbankset", "Cart/BankSet", ":panel:buttons_32", 0x00100000 },
                { "btn_sounds",      "Sounds",       ":panel:buttons_32", 0x00200000 },
                { "btn_presets",     "Presets",      ":panel:buttons_32", 0x00400000 },
                { "btn_seq_mode",    "Seq (Mode)",   ":panel:buttons_32", 0x00080000 },

                // --- SOFT BUTTONS (DISPLAY) ---
                { "btn_soft_tl", "Soft Top-Left",   ":panel:buttons_32", 0x04000000 },
                { "btn_soft_tc", "Soft Top-Center", ":panel:buttons_32", 0x00000400 },
                { "btn_soft_tr", "Soft Top-Right",  ":panel:buttons_32", 0x00000800 },
                { "btn_soft_bl", "Soft Bot-Left",   ":panel:buttons_32", 0x00040000 },
                { "btn_soft_bm", "Soft Bot-Middle", ":panel:buttons_32", 0x00001000 },
                { "btn_soft_br", "Soft Bot-Right",  ":panel:buttons_32", 0x00002000 },

                // --- BANK / NUMBER BUTTONS ---
                { "btn_bank0", "Bank 0", ":panel:buttons_32", 0x00800000 },
                { "btn_bank1", "Bank 1", ":panel:buttons_32", 0x01000000 },
                { "btn_bank2", "Bank 2", ":panel:buttons_32", 0x02000000 },
                { "btn_bank3", "Bank 3", ":panel:buttons_32", 0x00004000 },
                { "btn_bank4", "Bank 4", ":panel:buttons_32", 0x00008000 },
                { "btn_bank5", "Bank 5", ":panel:buttons_32", 0x00010000 },
                { "btn_bank6", "Bank 6", ":panel:buttons_32", 0x00020000 },
                { "btn_bank7", "Bank 7", ":panel:buttons_32", 0x00000008 },
                { "btn_bank8", "Bank 8", ":panel:buttons_32", 0x00000004 },
                { "btn_bank9", "Bank 9", ":panel:buttons_0",  0x02000000 },

                // --- DATA ENTRY ---
                { "btn_up",   "Up (Inc)",   ":panel:buttons_32", 0x40000000 },
                { "btn_down", "Down (Dec)", ":panel:buttons_32", 0x80000000 },

                // --- PROGRAMMING / SAVE ---
                { "btn_replace", "Replace Program", ":panel:buttons_0", 0x20000000 },
                { "btn_select",  "Select Voice",    ":panel:buttons_0", 0x00000020 },
                { "btn_copy",    "Copy",            ":panel:buttons_0", 0x00000200 },
                { "btn_write",   "Write",           ":panel:buttons_0", 0x00000008 },
                { "btn_compare", "Compare",         ":panel:buttons_0", 0x00000100 },

                // --- PATCH / SYSTEM / MIDI / EFFECTS ---
                { "btn_patch_menu", "Patch Select", ":panel:buttons_0", 0x04000000 },
                { "btn_midi",       "MIDI",         ":panel:buttons_0", 0x08000000 },
                { "btn_effects1",   "Effects",      ":panel:buttons_0", 0x10000000 },

                // --- KEY / ZONE ---
                { "btn_key_zone", "Key Zone", ":panel:buttons_32", 0x00000080 },
                { "btn_transpose","Transpose",":panel:buttons_32", 0x00000100 },
                { "btn_release",  "Release",  ":panel:buttons_32", 0x00000200 },
                { "btn_volume",   "Volume",   ":panel:buttons_32", 0x00000010 },
                { "btn_pan",      "Pan",      ":panel:buttons_32", 0x00000020 },
                { "btn_timbre",   "Timbre",   ":panel:buttons_32", 0x00000040 },

                // --- SYNTH PARAMETERS ---
                { "btn_wave",      "Wave",            ":panel:buttons_0", 0x00000010 },
                { "btn_mod_mixer", "Mod Mixer",       ":panel:buttons_0", 0x00000040 },
                { "btn_prog_ctrl", "Program Control", ":panel:buttons_0", 0x00000004 },
                { "btn_effects2",  "Effects (Synth)", ":panel:buttons_0", 0x00000080 },
                { "btn_pitch",     "Pitch",           ":panel:buttons_0", 0x00000800 },
                { "btn_pitch_mod", "Pitch Mod",       ":panel:buttons_0", 0x00002000 },
                { "btn_filters",   "Filters",         ":panel:buttons_0", 0x00008000 },
                { "btn_output",    "Output",          ":panel:buttons_0", 0x00020000 },
                { "btn_lfo",       "LFO",             ":panel:buttons_0", 0x00000400 },
                { "btn_env1",      "Env 1",           ":panel:buttons_0", 0x00001000 },
                { "btn_env2",      "Env 2",           ":panel:buttons_0", 0x00004000 },
                { "btn_env3",      "Env 3",           ":panel:buttons_0", 0x00010000 },

                // --- INTERNAL SEQUENCER TRANSPORT & TRACKS ---
                { "btn_track_1_6",  "Tracks 1-6",  ":panel:buttons_0", 0x40000000 },
                { "btn_track_7_12", "Tracks 7-12", ":panel:buttons_0", 0x80000000 },
                { "btn_record",     "Record",      ":panel:buttons_0", 0x00080000 },
                { "btn_stop_cont",  "Stop/Cont",   ":panel:buttons_0", 0x00400000 },
                { "btn_play",       "Play",        ":panel:buttons_0", 0x00800000 },
                { "btn_click",      "Click",       ":panel:buttons_32",0x00000001 },
                { "btn_seq_ctrl",   "Seq Control", ":panel:buttons_0", 0x00040000 },
                { "btn_locate",     "Locate",      ":panel:buttons_32",0x00000002 },

                // --- PERFORMANCE / SYSTEM ---
                { "btn_song",       "Song",         ":panel:buttons_32", 0x10000000 },
                { "btn_seq_track",  "Seq (Track)",  ":panel:buttons_32", 0x08000000 },
                { "btn_track",      "Track",        ":panel:buttons_32", 0x20000000 },
                { "btn_master",     "Master",       ":panel:buttons_0",  0x00100000 },
                { "btn_storage",    "Storage",      ":panel:buttons_0",  0x00200000 },
                { "btn_midi_ctrl",  "MIDI Control", ":panel:buttons_0",  0x01000000 },
                
                // --- PATCH SELECT (WHEELS) ---
                { "btn_patch_sel_l",  "Patch Select Left",  ":panel:patch_select", 0x1 },
                { "btn_patch_sel_r",  "Patch Select Right", ":panel:patch_select", 0x2 }
            };

        std::vector<std::atomic<float>*> buttonParams;

#ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
#endif

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Core function to boot the headless MAME environment
    void runMameEngine();
    
    // Verifies the unzipped ROM files in the sd132 directory
    bool verifyRomFiles();
    // Extracts only the required .bin files from a user-provided zip
    bool extractRomsFromZip(const juce::File& zipFile);
    // Copies the required .bin files from a user-provided directory
    bool copyRomsFromFolder(const juce::File& sourceDir);
    // Stores the list of missing ROM files to be displayed on the UI
    juce::String missingFilesList;

    // Callback to push generated audio from MAME into our ring buffers
    void pushAudioFromMame(const int16_t* pcmBuffer, int numSamples);

    // ========================================================
    // MIDI INPUT HANDLING (JUCE -> MAME)
    // ========================================================
    void pushMidiByte(uint8_t data, double targetMameTime);
    void clearMidiBuffer();
    bool pollMidiData();
    int readMidiByte();
    
    // --- MIDI OUTPUT (from SD-1 DUART TX → JUCE MIDI out) ---
    static constexpr int MIDI_OUT_BUFFER_SIZE = 16384;
    uint8_t midiOutBuffer[MIDI_OUT_BUFFER_SIZE];
    std::atomic<int> midiOutWritePos{ 0 };
    std::atomic<int> midiOutReadPos{ 0 };
    void pushMidiOutByte(uint8_t data);
    
    // MIDI output message assembler state
    std::vector<uint8_t> midiOutMsg;
    uint8_t midiOutRunningStatus = 0;
    bool midiOutInSysEx = false;

    // Pointer to the running MAME engine instance
    running_machine* mameMachine = nullptr;

    // --- MOUSE EVENT INJECTION (JUCE -> MAME) ---
    void injectMouseMove(int x, int y);
    void injectMouseDown(int x, int y);
    void injectMouseUp(int x, int y);

    // Thread-safe containers for mouse coordinates and button states
    std::atomic<int> mouseX{ 0 };
    std::atomic<int> mouseY{ 0 };
    std::atomic<uint32_t> mouseButtons{ 0 };

    juce::AudioProcessorValueTreeState apvts;
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // Triggered by the DAW when an automation parameter changes
    void parameterChanged(const juce::String& parameterID, float newValue) override;

    // Dynamically adjustable buffer threshold for MAME processing
    std::atomic<int> mameBufferThreshold{ 1024 };

    // Dynamic offline buffer for sync
    std::atomic<int> maxOfflineBuffer{ 1024 };
                
        // --- RAM INJECTION BUFFERS ---
        juce::MemoryBlock pendingOsram;
        juce::MemoryBlock pendingSeqRam;
        std::atomic<bool> pendingRamInjection{ false };
    
        // --- BANK INJECTION (60-program bank → osram, no CPU reset) ---
        juce::MemoryBlock pendingBankData;          // interleaved 31800 bytes
        std::atomic<bool> pendingBankInjection{ false };
        
        // --- STATE LOAD COMPARE RESET ---
        /*std::atomic<bool> needsCompareReset{ false };*/
        
        // --- MIDI INPUT SUPPRESS (during Write Single Preset) ---
        std::atomic<bool> suppressMidiInput{ false };
        
        // --- MIDI OPERATION CANCEL (set by onClose to abort pending timers) ---
        std::atomic<bool> midiOpCancelled{ false };
    
        // AU COLD BOOT HACK
        std::atomic<bool> needsBootPreRoll { false };
    
        // --- DYNAMIC PANEL LAYOUT SELECTION ---
        // 0 = Compact, 1 = Full, 2 = Panel, 3 = Tablet
        std::atomic<int> requestedViewIndex{ 0 };
        std::atomic<bool> requestViewChange{ false };

        // --- PIXEL PERFECT RENDERING ---
        // Stores the exact physical pixel dimensions of the current JUCE window.
        // MAME will strictly render at this 1:1 resolution to save CPU and maximize sharpness.
        std::atomic<int> windowWidth{ 1200 };
        std::atomic<int> windowHeight{ 539 };
        std::atomic<bool> requestRenderResize{ false };

        std::atomic<bool>& getFrameFlag() { return newFrameAvailable; }
        double getHostSampleRate() const { return hostSampleRate.load(); }

        // --- DOUBLE BUFFERED VIDEO RENDERING ---
        // Increased to 2560x2560 to safely fit the maximum allowed VST window size
        juce::Image cachedTexture{ juce::Image::ARGB, 2560, 2560, true, juce::SoftwareImageType() };

        juce::Image screenBuffers[2]{
            juce::Image(juce::Image::ARGB, 2560, 2560, true, juce::SoftwareImageType()),
            juce::Image(juce::Image::ARGB, 2560, 2560, true, juce::SoftwareImageType())
        };

    // Indicates which screen buffer (0 or 1) is fully rendered and ready to be drawn by the UI
    std::atomic<int> readyBufferIndex{ 0 };
    
    // PendingAUMidi
    std::vector<std::pair<juce::MidiMessage, int>> pendingAUMidi;
    
    // AnchorSet for AU
    std::atomic<bool> auAnchorSet{ false };
    
    // --- MACRO STATE ---
    std::atomic<bool> isSaveMacroActive{ false };
    
    void shutdownMame();
    
private:

        // Member variables to replace the problematic 'static' variables in processBlock.
        // This ensures each plugin instance has its own independent state.
        bool lastIsPlaying = false;
        bool localLastOffline = false;
        double lastAuMidiTime = 0.0;
        uint64_t captureReadPos = 0;
    
    bool extractLegacyMameState(const juce::String& base64State, juce::MemoryBlock& outOsram, juce::MemoryBlock& outSeqram);
    std::thread mameThread;
    std::atomic<bool> isMameRunning{ false };

    std::atomic<uint64_t> totalRead{ 0 };
    std::atomic<double> hostSampleRate{ 44100.0 };
                
    int getInternalHardwareLatencySamples() const {
        double sr = hostSampleRate.load(std::memory_order_relaxed);
                int base = static_cast<int>(0.0244 * sr);
                return base;
    }

    // Audio Ring Buffers (Generously sized to prevent underruns)
    static constexpr int RING_BUFFER_SIZE = 65536;

    // --- MAIN OUT BUFFERS ---
    float ringBufferL[RING_BUFFER_SIZE] = { 0.0f };
    float ringBufferR[RING_BUFFER_SIZE] = { 0.0f };

    // --- AUX OUT BUFFERS ---
    float ringBufferAuxL[RING_BUFFER_SIZE] = { 0.0f };
    float ringBufferAuxR[RING_BUFFER_SIZE] = { 0.0f };

    std::atomic<uint64_t> totalWritten{ 0 };

    // --- Timestamped MIDI ---
    std::atomic<bool> needAnchorSync{ true };
    std::atomic<bool> prepareWasCalled{ false }; // NEW: Prevents Logic AU double-reset
    std::atomic<double> anchorMameTime{ 0.0 };
    std::atomic<uint64_t> anchorDawSample{ 0 };

    // Double precision seconds
    struct TimestampedMidi {
        uint8_t data;
        double targetMameTime;
    };

    static constexpr int MIDI_BUFFER_SIZE = 524288;
    TimestampedMidi midiBuffer[MIDI_BUFFER_SIZE];

    std::atomic<int> midiWritePos{ 0 };
    std::atomic<int> midiReadPos{ 0 };

    std::atomic<bool> newFrameAvailable{ false };
    
    bool lastOfflineState = false;
    int64_t lastPlayheadPos = 0;
    
    juce::String instanceTempDir; // Unique sandbox directory for this plugin instance
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EnsoniqSD1AudioProcessor)
};
