/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.
    
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
/**
*/
class EnsoniqSD1AudioProcessor  : public juce::AudioProcessor,
                                  public juce::AudioProcessorValueTreeState::Listener
{
public:

    // --- MAME STATE MANAGEMENT ---
    // Used to safely orchestrate loading/saving states between the UI and the MAME thread
    std::atomic<bool> requestMameSave { false };
    std::atomic<bool> requestMameLoad { false };
    std::atomic<bool> mameStateIsReady { false };
    juce::WaitableEvent mameStateEvent { false };

    // --- MEDIA HANDLING (FLOPPY/CARTRIDGE) ---
    std::atomic<bool> requestFloppyLoad { false };
    std::atomic<bool> requestCartLoad { false };
    std::string pendingFloppyPath;
    std::string pendingCartPath;
    std::mutex mediaMutex;

    // --- WINDOW SIZE PERSISTENCE ---
    // Stores the last window size set by the user to recall it upon project load
    int savedWindowWidth { 0 };
    int savedWindowHeight { 0 };

    // --- SYNCHRONIZATION ---
    // Throttle event used to prevent MAME from generating audio faster than the DAW consumes it
    juce::WaitableEvent mameThrottleEvent { false };
    bool isMameRunningFlag() const { return isMameRunning.load(); }
    std::atomic<bool> isRomMissing { false };

    std::atomic<bool> mameHasStarted { false };
    std::atomic<double> initialSampleRate { 0.0 };
    std::atomic<bool> sampleRateMismatch { false };

    // --- SINGLE INSTANCE PROTECTION ---
    // MAME's architecture inherently limits it to a single running instance per process.
    // We must track this to disable additional VST instances in the DAW gracefully.
    std::atomic<bool> isBlockedByAnotherInstance { false };
    bool isMasterInstance = false;

    uint64_t getTotalRead() const { return totalRead.load(std::memory_order_acquire); }
    uint64_t getTotalWritten() const { return totalWritten.load(std::memory_order_acquire); }

    //==============================================================================
    EnsoniqSD1AudioProcessor();
    ~EnsoniqSD1AudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif
    
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

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
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;
    
    // Core function to boot the headless MAME environment
    void runMameEngine();
    
    // Callback to push generated audio from MAME into our ring buffers
    void pushAudioFromMame(const int16_t* pcmBuffer, int numSamples);
          
    // ========================================================
    // MIDI INPUT HANDLING (JUCE -> MAME)
    // ========================================================
    void pushMidiByte(uint8_t data);
    bool hasMidiData();
    int readMidiByte();
            
    // Pointer to the running MAME engine instance
    running_machine* mameMachine = nullptr;
        
    // --- MOUSE EVENT INJECTION (JUCE -> MAME) ---
    void injectMouseMove(int x, int y);
    void injectMouseDown(int x, int y);
    void injectMouseUp(int x, int y);
            
    // Thread-safe containers for mouse coordinates and button states
    std::atomic<int> mouseX { 0 };
    std::atomic<int> mouseY { 0 };
    std::atomic<uint32_t> mouseButtons { 0 };

    juce::AudioProcessorValueTreeState apvts;
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    
    // Triggered by the DAW when an automation parameter changes
    void parameterChanged (const juce::String& parameterID, float newValue) override;

    // Dynamically adjustable buffer threshold for MAME processing
    std::atomic<int> mameBufferThreshold { 1024 };

    // --- DYNAMIC PANEL LAYOUT SELECTION ---
    std::mutex viewMutex;
    std::string requestedViewName = "Compact";
    std::atomic<bool> requestViewChange { false };
    
    // Stores the current internal rendering resolution of the selected MAME layout
    std::atomic<int> mameInternalWidth { 2048 };
    std::atomic<int> mameInternalHeight { 921 };
    
    std::atomic<bool>& getFrameFlag() { return newFrameAvailable; }
    double getHostSampleRate() const { return hostSampleRate.load(); }
    
    // --- DOUBLE BUFFERED VIDEO RENDERING ---
    juce::Image cachedTexture { juce::Image::ARGB, 2048, 2048, true, juce::SoftwareImageType() };
    
    // Pre-allocated maximum size (2048x2048) to accommodate all layout variations safely
    juce::Image screenBuffers[2] {
        juce::Image(juce::Image::ARGB, 2048, 2048, true, juce::SoftwareImageType()),
        juce::Image(juce::Image::ARGB, 2048, 2048, true, juce::SoftwareImageType())
    };

    // Indicates which screen buffer (0 or 1) is fully rendered and ready to be drawn by the UI
    std::atomic<int> readyBufferIndex { 0 };
    
private:

    std::thread mameThread;
    std::atomic<bool> isMameRunning { false };
        
    std::atomic<uint64_t> totalRead { 0 };
    std::atomic<double> hostSampleRate { 44100.0 };
    
    // Audio Ring Buffers (Generously sized to prevent underruns)
    static constexpr int RING_BUFFER_SIZE = 65536; 
    
    // --- MAIN OUT BUFFERS ---
    float ringBufferL[RING_BUFFER_SIZE] = { 0.0f };
    float ringBufferR[RING_BUFFER_SIZE] = { 0.0f };
    
    // --- AUX OUT BUFFERS ---
    float ringBufferAuxL[RING_BUFFER_SIZE] = { 0.0f };
    float ringBufferAuxR[RING_BUFFER_SIZE] = { 0.0f };
    
    std::atomic<uint64_t> totalWritten { 0 };

    // MIDI Ring Buffer
    static constexpr int MIDI_BUFFER_SIZE = 4096;
    uint8_t midiBuffer[MIDI_BUFFER_SIZE] = { 0 };
    std::atomic<int> midiWritePos { 0 };
    std::atomic<int> midiReadPos { 0 };
       
    std::atomic<bool> newFrameAvailable { false };
        
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EnsoniqSD1AudioProcessor)
};
