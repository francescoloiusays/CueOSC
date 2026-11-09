#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

class CueOscEditor : public juce::AudioProcessorEditor,
                     private juce::TableListBoxModel,
                     private juce::Timer
{
public:
    explicit CueOscEditor (CueOscProcessor&);
    ~CueOscEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    enum Column { colOn = 1, colChannel, colNote, colName, colAddress, colArgs, colTest };

    void cellEdited (int row, int column, const juce::String& text);
    void selectRowFromCell (int row, const juce::ModifierKeys& mods);
    CueOscProcessor& getProcessor() { return proc; }

private:
    // TableListBoxModel
    int getNumRows() override;
    void paintRowBackground (juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell (juce::Graphics&, int, int, int, int, bool) override {}
    juce::Component* refreshComponentForCell (int row, int column, bool selected, juce::Component* existing) override;
    void deleteKeyPressed (int lastRowSelected) override;

    void timerCallback() override;
    void applyNetwork();
    void deleteSelected();
    void refreshLog();

    CueOscProcessor& proc;

    juce::Label title, hostLbl, sendLbl, listenLbl, netStatus, delayLbl, statsLbl, warnLbl, ignoreLbl, helpLbl;
    juce::TextEditor hostEd, sendEd, listenEd;
    juce::TextButton connectBtn { "Connetti" };

    juce::TextButton armBtn { "INVIA" }, recBtn { "REGISTRA" }, dryBtn { "PROVA" }, learnBtn { "LEARN" };
    juce::Slider delaySlider;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> armAtt, recAtt, dryAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> delayAtt;

    juce::TableListBox table { "cue", this };
    juce::TextButton addBtn { "+ Aggiungi" }, delBtn { "Elimina" }, sortBtn { "Ordina" },
                     ignoreBtn { "Ignora indirizzo" }, clearLogBtn { "Pulisci log" };

    juce::TextEditor ignoreEd, logView;

    int lastMapVersion = -1, lastLogSerial = -1, lastIgnoredVersion = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CueOscEditor)
};
