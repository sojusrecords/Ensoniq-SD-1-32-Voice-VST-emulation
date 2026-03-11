/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin editor.
    
    Ensoniq SD-1 MAME VST Emulation
    Open Source GPLv2/v3
    https://www.sojusrecords.com

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
/**
*/
class EnsoniqSD1AudioProcessorEditor  : public juce::AudioProcessorEditor,
                                        private juce::Timer
{
public:
    EnsoniqSD1AudioProcessorEditor (EnsoniqSD1AudioProcessor&);
    ~EnsoniqSD1AudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    
    // Timer callback used for polling frame updates and layout changes
    void timerCallback() override;
    
    // --- JUCE MOUSE EVENTS ---
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseUp   (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;

private:
    EnsoniqSD1AudioProcessor& audioProcessor;
        
    juce::TextButton loadMediaButton { "Load Floppy/Cartridge" };
    std::unique_ptr<juce::FileChooser> fileChooser;
    void loadMediaButtonClicked();

    // --- SETTINGS PANEL GUI COMPONENTS ---
    juce::TextButton settingsButton { "Settings / About" };
    juce::GroupComponent settingsGroup { "settings_group", "Ensoniq(R) SD-1/32 Settings v0.9.5b" };
    
    juce::Label bufferLabel { "buffer_label", "MAME(R) Engine buffer:" };
    juce::ComboBox bufferCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> bufferAttachment;

    // --- DYNAMIC PANEL SELECTOR ---
    juce::Label viewLabel { "view_label", "Panel Layout:" };
    juce::ComboBox viewCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> viewAttachment;

    juce::Label aboutLabel { "about_label", "Built with love by MAMEDev and contributors and sojusrecords.com\n\nThis software includes MAME(R) emulator components\n and is licensed under the GPL v2/v3.\nAll trademarks are property of their respective owners." };
    juce::HyperlinkButton webLink { "visit sojusrecords.com", juce::URL("https://www.sojusrecords.com") };
    juce::TextButton closeSettingsButton { "Close" };

    bool isSettingsVisible = false;
    void toggleSettings();
    
    // Calculates and applies the optimal window size based on the active layout aspect ratio
    void updateWindowSize(); 
    
    // Visual drag handle for resizing the plugin window
    std::unique_ptr<juce::ResizableCornerComponent> resizer;
    
    // --- WINDOW SIZE TRACKING ---
    // Used by the Timer to detect when the MAME internal layout resolution changes
    int lastW = 0;
    int lastH = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EnsoniqSD1AudioProcessorEditor)
};
