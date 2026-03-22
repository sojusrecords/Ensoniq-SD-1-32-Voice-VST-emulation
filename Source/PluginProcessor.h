/*
  ==============================================================================

    Ensoniq SD-1 MAME VST Emulation
    Open Source GPLv2/v3
    https://www.sojusrecords.com

  ==============================================================================
*/

#pragma once

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
    
    // --- GLOBAL SETTINGS ---
        std::atomic<bool> requestGlobalSave{ false };
        void loadGlobalSettings();

    // --- MAME STATE MANAGEMENT ---
    // Used to safely orchestrate loading/saving states between the UI and the MAME thread
    std::atomic<bool> requestMameSave{ false };
    std::atomic<bool> requestMameLoad{ false };
    std::atomic<bool> mameStateIsReady{ false };
    juce::WaitableEvent mameStateEvent{ false };

    // --- MEDIA HANDLING (FLOPPY/CARTRIDGE) ---
    std::atomic<bool> requestFloppyLoad{ false };
    std::atomic<bool> requestCartLoad{ false };
    std::string pendingFloppyPath;
    std::string pendingCartPath;
    std::mutex mediaMutex;

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

    // --- SINGLE INSTANCE PROTECTION ---
    // MAME's architecture inherently limits it to a single running instance per process.
    // We must track this to disable additional VST instances in the DAW gracefully.
    std::atomic<bool> isBlockedByAnotherInstance{ false };
    bool isMasterInstance = false;

    uint64_t getTotalRead() const { return totalRead.load(std::memory_order_acquire); }
    uint64_t getTotalWritten() const { return totalWritten.load(std::memory_order_acquire); }

    //==============================================================================
    EnsoniqSD1AudioProcessor();
    ~EnsoniqSD1AudioProcessor() override;

    //==============================================================================
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    
    // --- VST AUTOMATIZATION ---
        struct SD1ButtonDef {
            juce::String paramID;
            juce::String paramName;
            const char* ioportTag;
            uint32_t ioportMask;
        };

        // FULL SD-1 LIST
        const std::vector<SD1ButtonDef> sd1Buttons = {
            // --- MASTER / MODE ---
            { "btn_cartbankset", "Cart/BankSet", ":buttons_32", 0x00100000 },
            { "btn_sounds",      "Sounds",       ":buttons_32", 0x00200000 },
            { "btn_presets",     "Presets",      ":buttons_32", 0x00400000 },
            { "btn_seq_mode",    "Seq (Mode)",   ":buttons_32", 0x00080000 },

            // --- SOFT BUTTONS ---
            { "btn_soft_tl", "Soft Top-Left",   ":buttons_32", 0x04000000 },
            { "btn_soft_tc", "Soft Top-Center", ":buttons_32", 0x00000400 },
            { "btn_soft_tr", "Soft Top-Right",  ":buttons_32", 0x00000800 },
            { "btn_soft_bl", "Soft Bot-Left",   ":buttons_32", 0x00040000 },
            { "btn_soft_bm", "Soft Bot-Middle", ":buttons_32", 0x00001000 },
            { "btn_soft_br", "Soft Bot-Right",  ":buttons_32", 0x00002000 },

            // --- BANK / NUMBER BUTTONS ---
            { "btn_bank0", "Bank 0", ":buttons_32", 0x00800000 },
            { "btn_bank1", "Bank 1", ":buttons_32", 0x01000000 },
            { "btn_bank2", "Bank 2", ":buttons_32", 0x02000000 },
            { "btn_bank3", "Bank 3", ":buttons_32", 0x00004000 },
            { "btn_bank4", "Bank 4", ":buttons_32", 0x00008000 },
            { "btn_bank5", "Bank 5", ":buttons_32", 0x00010000 },
            { "btn_bank6", "Bank 6", ":buttons_32", 0x00020000 },
            { "btn_bank7", "Bank 7", ":buttons_32", 0x00000008 },
            { "btn_bank8", "Bank 8", ":buttons_32", 0x00000004 },
            { "btn_bank9", "Bank 9", ":buttons_0",  0x02000000 },

            // --- DATA ENTRY ---
            { "btn_up",   "Up (Inc)",   ":buttons_32", 0x40000000 },
            { "btn_down", "Down (Dec)", ":buttons_32", 0x80000000 },

            // --- PROGRAMMING / SAVE ---
            { "btn_replace", "Replace Program", ":buttons_0", 0x20000000 },
            { "btn_select",  "Select Voice",    ":buttons_0", 0x00000020 },
            { "btn_copy",    "Copy",            ":buttons_0", 0x00000200 },
            { "btn_write",   "Write",           ":buttons_0", 0x00000008 },
            { "btn_compare", "Compare",         ":buttons_0", 0x00000100 },

            // --- PATCH / SYSTEM / MIDI / EFFECTS ---
            { "btn_patch_menu", "Patch Select", ":buttons_0", 0x04000000 },
            { "btn_midi",       "MIDI",         ":buttons_0", 0x08000000 },
            { "btn_effects1",   "Effects",      ":buttons_0", 0x10000000 },

            // --- KEY / ZONE ---
            { "btn_key_zone", "Key Zone", ":buttons_32", 0x00000080 },
            { "btn_transpose","Transpose",":buttons_32", 0x00000100 },
            { "btn_release",  "Release",  ":buttons_32", 0x00000200 },
            { "btn_volume",   "Volume",   ":buttons_32", 0x00000010 },
            { "btn_pan",      "Pan",      ":buttons_32", 0x00000020 },
            { "btn_timbre",   "Timbre",   ":buttons_32", 0x00000040 },

            // --- SYNTH PARAMETERS ---
            { "btn_wave",      "Wave",            ":buttons_0", 0x00000010 },
            { "btn_mod_mixer", "Mod Mixer",       ":buttons_0", 0x00000040 },
            { "btn_prog_ctrl", "Program Control", ":buttons_0", 0x00000004 },
            { "btn_effects2",  "Effects (Synth)", ":buttons_0", 0x00000080 },
            { "btn_pitch",     "Pitch",           ":buttons_0", 0x00000800 },
            { "btn_pitch_mod", "Pitch Mod",       ":buttons_0", 0x00002000 },
            { "btn_filters",   "Filters",         ":buttons_0", 0x00008000 },
            { "btn_output",    "Output",          ":buttons_0", 0x00020000 },
            { "btn_lfo",       "LFO",             ":buttons_0", 0x00000400 },
            { "btn_env1",      "Env 1",           ":buttons_0", 0x00001000 },
            { "btn_env2",      "Env 2",           ":buttons_0", 0x00004000 },
            { "btn_env3",      "Env 3",           ":buttons_0", 0x00010000 },

            // --- INTERNAL SEQUENCER TRANSPORT & TRACKS ---
            { "btn_track_1_6",  "Tracks 1-6",  ":buttons_0",  0x40000000 },
            { "btn_track_7_12", "Tracks 7-12", ":buttons_0",  0x80000000 },
            { "btn_record",     "Record",      ":buttons_0",  0x00080000 },
            { "btn_stop_cont",  "Stop/Cont",   ":buttons_0",  0x00400000 },
            { "btn_play",       "Play",        ":buttons_0",  0x00800000 },
            { "btn_click",      "Click",       ":buttons_32", 0x00000001 },
            { "btn_seq_ctrl",   "Seq Control", ":buttons_0",  0x00040000 },
            { "btn_locate",     "Locate",      ":buttons_32", 0x00000002 },

            // --- PERFORMANCE / SYSTEM ---
            { "btn_song",       "Song",         ":buttons_32", 0x10000000 },
            { "btn_seq_track",  "Seq (Track)",  ":buttons_32", 0x08000000 },
            { "btn_track",      "Track",        ":buttons_32", 0x20000000 },
            { "btn_master",     "Master",       ":buttons_0",  0x00100000 },
            { "btn_storage",    "Storage",      ":buttons_0",  0x00200000 },
            { "btn_midi_ctrl",  "MIDI Control", ":buttons_0",  0x01000000 },

            // --- PATCH SELECT ---
            { "btn_patch_sel_r", "Patch Select Right", ":patch_select", 0x2 },
            { "btn_patch_sel_l", "Patch Select Left",  ":patch_select", 0x1 }
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
    
    // Verifies the contents of the sd132.zip file against known SHA-256 hashes
    bool verifyRomChecksums(const juce::File& zipFile);

    // Callback to push generated audio from MAME into our ring buffers
    void pushAudioFromMame(const int16_t* pcmBuffer, int numSamples);

    // ========================================================
    // MIDI INPUT HANDLING (JUCE -> MAME)
    // ========================================================
    void pushMidiByte(uint8_t data, double targetMameTime);
    bool pollMidiData();
    int readMidiByte();

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

    void shutdownMame();
    
private:

    std::thread mameThread;
    std::atomic<bool> isMameRunning{ false };

    std::atomic<uint64_t> totalRead{ 0 };
    std::atomic<double> hostSampleRate{ 44100.0 };
    
    int getInternalHardwareLatencySamples() const {
        return static_cast<int>(0.0244 * hostSampleRate.load(std::memory_order_relaxed));
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
    std::atomic<double> anchorMameTime{ 0.0 };
    std::atomic<uint64_t> anchorDawSample{ 0 };

    // Double precision seconds
    struct TimestampedMidi {
        uint8_t data;
        double targetMameTime;
    };

    static constexpr int MIDI_BUFFER_SIZE = 4096;
    TimestampedMidi midiBuffer[MIDI_BUFFER_SIZE];

    std::atomic<int> midiWritePos{ 0 };
    std::atomic<int> midiReadPos{ 0 };

    std::atomic<bool> newFrameAvailable{ false };
    
    bool lastOfflineState = false;
    int64_t lastPlayheadPos = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EnsoniqSD1AudioProcessor)
};
