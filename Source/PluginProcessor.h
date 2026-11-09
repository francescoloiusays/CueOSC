#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_osc/juce_osc.h>
#include "CueMap.h"

#include <array>
#include <atomic>
#include <vector>

class CueOscProcessor;

//==============================================================================
// Thread che manda i messaggi OSC all'istante giusto.
// L'audio thread gli passa gli eventi (lock-free); lui li tiene in coda
// fino al momento esatto e poi li spedisce, cosi' l'audio non si blocca mai sulla rete.
class CueSender : public juce::Thread
{
public:
    struct Event
    {
        double timeMs;   // Time::getMillisecondCounterHiRes() in cui spedire
        int channel;
        int note;
    };

    explicit CueSender (CueOscProcessor& p);
    ~CueSender() override;

    bool pushFromAudio (const Event& e) noexcept;   // solo audio thread
    void pushImmediate (const CueRow& row);         // solo message thread (tasto Test)
    void clearPending() noexcept { clearRequested = true; }

    void run() override;

private:
    CueOscProcessor& owner;

    static constexpr int fifoSize = 2048;
    juce::AbstractFifo fifo { fifoSize };
    std::array<Event, fifoSize> buffer {};

    juce::CriticalSection immediateLock;
    std::vector<CueRow> immediate;

    std::vector<Event> pending;
    std::atomic<bool> clearRequested { false };
};

//==============================================================================
class CueOscProcessor : public juce::AudioProcessor,
                        private juce::OSCReceiver::Listener<juce::OSCReceiver::RealtimeCallback>,
                        private juce::AsyncUpdater
{
public:
    CueOscProcessor();
    ~CueOscProcessor() override;

    //== AudioProcessor ========================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "CueOSC"; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //== Parametri =============================================================
    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    bool isArmed() const noexcept     { return armParam->load() > 0.5f; }
    bool isRecording() const noexcept { return recParam->load() > 0.5f; }
    bool isDryRun() const noexcept    { return dryParam->load() > 0.5f; }

    std::atomic<bool> learn { false };

    //== Rete ==================================================================
    // Da chiamare dal message thread. Riconnette subito.
    void setNetwork (const juce::String& host, int sendPort, int listenPort);
    juce::String getHost() const;
    int getSendPort() const;
    int getListenPort() const;
    juce::String getNetStatus() const;

    //== Mappa =================================================================
    int getNumRows() const;
    CueRow getRow (int index) const;
    void setRow (int index, CueRow row);
    int addRow (CueRow row);             // ritorna l'indice
    void removeRow (int index);
    void sortRows();
    int getMapVersion() const noexcept { return mapVersion.load(); }
    CueRow makeNewRow (const juce::String& address, const juce::String& args) const;

    juce::StringArray getIgnored() const;
    void setIgnored (const juce::StringArray& addresses);
    void ignoreAddress (const juce::String& address);   // aggiunge e cancella le righe con quell'indirizzo
    int getIgnoredVersion() const noexcept { return ignoredVersion.load(); }

    void testRow (int index);

    //== Log / stato ===========================================================
    void log (const juce::String& line);
    juce::StringArray getLog() const;
    int getLogSerial() const noexcept { return logSerial.load(); }
    void clearLog();

    std::atomic<int> messagesIn { 0 }, messagesOut { 0 };
    std::atomic<float> jitterMs { 0.0f };
    std::atomic<bool> transportPlaying { false };

    //== Usati dal CueSender ===================================================
    void sendForNote (int channel, int note);
    void sendRow (const CueRow& row, bool isTest);

private:
    void oscMessageReceived (const juce::OSCMessage&) override;
    void oscBundleReceived (const juce::OSCBundle&) override;
    void handleIncoming (const juce::OSCMessage&);
    void handleAsyncUpdate() override;
    void connectNetwork();
    int findFreeSlotLocked() const;
    void updateJitter (double nowMs, double posMs, bool valid);

    std::atomic<float>* armParam = nullptr;
    std::atomic<float>* recParam = nullptr;
    std::atomic<float>* dryParam = nullptr;
    std::atomic<float>* delayParam = nullptr;

    // rete
    mutable juce::CriticalSection netLock;
    juce::OSCSender oscOut;
    juce::OSCReceiver oscIn;
    juce::String host { "127.0.0.1" };
    int sendPort = 9000, listenPort = 9001;
    juce::String netStatus { "non connesso" };

    // mappa
    mutable juce::CriticalSection mapLock;
    std::vector<CueRow> rows;
    juce::StringArray ignored;
    std::atomic<int> mapVersion { 0 }, ignoredVersion { 0 };

    // rete -> audio (note da registrare)
    static constexpr int inFifoSize = 512;
    juce::AbstractFifo inFifo { inFifoSize };
    std::array<std::pair<int, int>, inFifoSize> inBuffer {};

    // note-off in sospeso: campioni mancanti, indice = (canale-1)*128 + nota
    std::array<int, 16 * 128> offCountdown {};

    double sampleRate = 48000.0;

    // misura del jitter (per capire se REAPER sta processando in anticipo)
    bool jitterInit = false;
    double lastD = 0, winMin = 0, winMax = 0, winStart = 0;

    // log
    mutable juce::CriticalSection logLock;
    juce::StringArray logLines;
    std::atomic<int> logSerial { 0 };

    CueSender sender;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CueOscProcessor)
};
