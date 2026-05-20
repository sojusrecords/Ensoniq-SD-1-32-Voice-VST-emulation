/*
  ==============================================================================

    Ensoniq SD-1 MAME VST Emulation
    Open Source GPLv2/v3
    https://www.sojusrecords.com

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

class SourceTreeItem : public juce::TreeViewItem
{
public:
    SourceTreeItem(const juce::String& name, bool isCategory = false)
        : itemName(name), category(isCategory) {}

    bool mightContainSubItems() override { return getNumSubItems() > 0; }

    void paintItem(juce::Graphics& g, int width, int height) override
        {
            lastPaintWidth = width;
            if (isSelected()) g.fillAll(juce::Colour(0xff3a5a7a));

            int textX = 15; // Align both categories and sub-items vertically at the same X position

            if (category) {
                // Draw a custom expand/collapse arrow since we disabled the default ones
                g.setColour(juce::Colours::grey);
                float arrowSize = 8.0f;
                float ax = 4.0f;
                float ay = (height - arrowSize) * 0.5f;
                
                juce::Path p;
                if (isOpen()) {
                    // Down arrow
                    p.addTriangle(ax, ay + 2.0f, ax + arrowSize, ay + 2.0f, ax + arrowSize * 0.5f, ay + arrowSize);
                } else {
                    // Right arrow
                    p.addTriangle(ax + 2.0f, ay, ax + arrowSize, ay + arrowSize * 0.5f, ax + 2.0f, ay + arrowSize);
                }
                g.fillPath(p);

                g.setColour(juce::Colours::orange);
                g.setFont(juce::Font(juce::FontOptions(15.0f)).boldened());
            } else {
                g.setColour(isSelected() ? juce::Colours::white : juce::Colours::lightgrey);
                g.setFont(juce::FontOptions(15.0f));
            }
            
            int textRight = onRemove ? width - 28 : width - textX;
            g.drawText(itemName, textX, 0, textRight, height, juce::Justification::centredLeft, true);
            
            if (onRemove) {
                g.setColour(juce::Colours::red);
                g.setFont(juce::Font(juce::FontOptions(16.0f)).boldened());
                g.drawText("X", width - 25, 0, 20, height, juce::Justification::centred);
            }
        }

        juce::String getUniqueName() const override { return itemName; }

        void itemSelectionChanged(bool isNowSelected) override {
            if (pendingRemove) { pendingRemove = false; return; }
            if (isNowSelected && onSelection) onSelection(itemName);
        }
        
        void itemClicked(const juce::MouseEvent& e) override {
            // Single click anywhere on the category row toggles its open/close state
            if (category) {
                setOpen(!isOpen());
            }
            
            if (onRemove && e.x > lastPaintWidth - 30) {
                pendingRemove = true;
                juce::MessageManager::callAsync([cb = onRemove]() { if (cb) cb(); });
            }
        }

    juce::String itemName;
    bool category;
    std::function<void(juce::String)> onSelection;
    std::function<void()> onRemove;
    
private:
    int lastPaintWidth = 200;
    bool pendingRemove = false;
};

class PresetBrowserComponent : public juce::Component,
                               public juce::ListBoxModel,
                               private juce::Timer
{
public:
    PresetBrowserComponent(EnsoniqSD1AudioProcessor& p);
    ~PresetBrowserComponent() override;

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;
    void timerCallback() override;
    
    // SysEx Virtual Button compare toggle — goes through MIDI, properly ordered
    // Replaces the old APVTS button parameter hack
    void setCompareState(bool shouldBeOn);

    // Clean return from external SYX mode to internal banks
    // Sends: F7 (terminate SysEx) → Compare OFF (SysEx button) → Bank Select + Program Change
    void returnToInternalMode(int bank, int program);

    std::function<void()> onClose;
    
    // --- FILE MANAGER OPERATION STATES ---
        bool isSYXProcessing = false;
        bool wasWriteSinglePreset = false;
        bool wasSYXPreview = false;
        bool wasSYXImport = false;
        bool wasSEQImport = false;
        bool isErrorState = false;
        bool wasPresetPreviewed = false;
    
    int getNumRows() override { return currentListItems.size(); }
    void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;

    void updateContentList(const juce::String& categoryName);

    void injectAndPreview(double startTimeOffset = -1.0);
    
    // Save/restore file manager state to/from processor (survives editor destroy)
    void saveStateToProcessor();
    void restoreStateFromProcessor();
    
    // FILE MANAGER EXIT SEQUENCE
    void showClosingProgress(const juce::String& message, double durationMs);
    
private:
    EnsoniqSD1AudioProcessor& audioProcessor;

    bool isRestoringUI = false;
    std::unique_ptr<SourceTreeItem> rootItem;
    juce::File currentlyLoadedArchive;
    juce::StringArray currentListItems;
    
    juce::Label headerLabel{"header", "SD-1 Preset & File Manager"};
    juce::TextButton closeButton{"Back to Synth"};
    juce::TextEditor bankContentText;

    // --- PROGRESS BAR ---
    double progressValue = -1.0;            // -1 = hidden, 0..1 = transfer progress
    juce::ProgressBar progressBar{ progressValue };
    juce::Label progressLabel{"progress", ""};
    double transferStartTimeMs = 0.0;
    double transferDurationMs = 0.0;

    juce::TextButton backToFilesButton{"<< BACK TO ROOT"};
    
    juce::TreeView sourcesTree;
    juce::ListBox contentList;

    class BankListModel : public juce::ListBoxModel {
    public:
        juce::StringArray items;
        std::function<void(int)> onSelect;
        PresetBrowserComponent* parentComponent = nullptr;

        int getNumRows() override { return items.size(); }
        
        void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override {
            if (selected) g.fillAll(juce::Colour(0xff3a5a7a));
            g.setColour(selected ? juce::Colours::white : juce::Colours::lightgrey);
            g.setFont(juce::FontOptions(14.0f));
            g.drawText(items[row], 5, 0, w - 10, h, juce::Justification::centredLeft, true);
        }
        void listBoxItemClicked(int row, const juce::MouseEvent&) override {
            if (onSelect) onSelect(row);
        }
    };

    juce::GroupComponent inspectorGroup{"inspector", "Inspector"};
    juce::Label selectionNameLabel{"name", "No selection"};
    juce::Label playHintLabel{"hint", "Play MIDI keyboard to preview!"};
    
    juce::TextButton writeSingleButton{"Write Single Program"};
    juce::TextButton importBankButton{"Import Full Bank"};
    juce::TextButton exportSysExButton{"Export .SYX"};

    BankListModel bankModel;
    juce::ListBox bankContentList; 
    
    // We store the linearized data here and interleave it when writing to RAM
    juce::MemoryBlock currentlyLoadedBankData;
    juce::MemoryBlock combinedDumpProgramSyx; // non-empty if loaded file is a combined dump (programs + seq)
    juce::MemoryBlock currentlySelectedSingleRawData;
    
    std::unique_ptr<juce::FileChooser> folderChooser;
    juce::File customBrowseFolder;
    
    bool isViewingDiskBank = false;
    juce::String currentDiskBankName;
    
    // Parallel arrays: raw FAT names and file types indexed by bankContentList row
    // These avoid fragile string-parsing of the display string to recover names/types.
    juce::StringArray bankRawNames;
    std::vector<uint8_t> bankEntryTypes;
    
    // 11 byte Ensoniq filename
    std::vector<std::array<char, 11>> bankExactNames;
    
    // Clears all bank list state in one place
    void clearBankList();
        
    juce::TextButton refreshButton{"Refresh / Rescan"};
    juce::String currentCategory;
    juce::String lastBankContext = "INT (RAM)"; // tracks actual SD-1 bank, independent of file manager navigation
    double lastPreviewEndTime = 0.0;            // MAME time when last injectAndPreview SysEx finished + margin
    bool wasBootingOverlay = false;              // tracks boot indicator state across timer ticks (NOT static — must reset on editor recreate)

    // Write Single Program — VFD/LED-driven state machine (runs in timerCallback)
    int wpState = 0;           // 0=idle 1=prepare 2=inject 3=write 4=complete
    int wpTargetSlot = -1;
    double wpStateStartMs = 0;
    bool wpCmdSent = false;
    void runWriteProgramSM();

    // Helper: Reads the actual names from the merged INT RAM
    juce::StringArray getRealIntRamNames();
    juce::String decodeEnsoniqName(const char* nameBuf);
    
    // Helper: pushes an SD-1 Virtual Button SysEx command through the MIDI buffer
    // buttonNum: 0-95 = button down, add 96 for button up
    // See SD-1 SysEx Spec Section 3.1.1
    void pushSysExButtonPress(int buttonNum, double downTime, double upTime);

    void rebuildTree();
    
    // --- SEQUENCE TRANSFER OVERLAY ---
    // Blocks the entire GUI during slow MIDI SysEx transfers (sequences, etc.)
    bool isSequenceTransferActive = false;
    double seqTransferStartTimeMs = 0.0;
    double seqTransferDurationMs = 0.0;
    juce::String seqTransferMessage;
    void startSequenceTransfer(const juce::MemoryBlock& rawSyxData, const juce::String& message);
    void paintOverlay(juce::Graphics& g);

    std::unique_ptr<juce::Component> clickBlocker;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetBrowserComponent)
};
