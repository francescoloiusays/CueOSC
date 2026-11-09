#include "PluginProcessor.h"
#include "PluginEditor.h"

#if JUCE_WINDOWS
 // Su Windows i timer di sistema hanno risoluzione 15,6 ms: lo portiamo a 1 ms
 // finche' il thread di invio e' attivo, altrimenti gli stacchi "ballano".
 extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod (unsigned int);
 extern "C" __declspec(dllimport) unsigned int __stdcall timeEndPeriod (unsigned int);
#endif

//==============================================================================
CueSender::CueSender (CueOscProcessor& p) : juce::Thread ("CueOSC sender"), owner (p)
{
    pending.reserve (fifoSize);
}

CueSender::~CueSender()
{
    stopThread (2000);
}

bool CueSender::pushFromAudio (const Event& e) noexcept
{
    int s1, n1, s2, n2;
    fifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 + n2 < 1)
        return false;

    buffer[(size_t) (n1 > 0 ? s1 : s2)] = e;
    fifo.finishedWrite (1);
    return true;
}

void CueSender::pushImmediate (const CueRow& row)
{
    const juce::ScopedLock sl (immediateLock);
    immediate.push_back (row);
}

void CueSender::run()
{
   #if JUCE_WINDOWS
    timeBeginPeriod (1);
   #endif

    while (! threadShouldExit())
    {
        // 1) prendi gli eventi arrivati dall'audio thread
        {
            int s1, n1, s2, n2;
            const int ready = fifo.getNumReady();
            fifo.prepareToRead (ready, s1, n1, s2, n2);
            for (int i = 0; i < n1; ++i) pending.push_back (buffer[(size_t) (s1 + i)]);
            for (int i = 0; i < n2; ++i) pending.push_back (buffer[(size_t) (s2 + i)]);
            fifo.finishedRead (n1 + n2);
        }

        if (clearRequested.exchange (false) || ! owner.isArmed())
            pending.clear();

        // 2) i Test manuali partono subito
        std::vector<CueRow> now;
        {
            const juce::ScopedLock sl (immediateLock);
            now.swap (immediate);
        }
        for (auto& r : now)
            owner.sendRow (r, true);

        // 3) spedisci quelli arrivati a scadenza
        if (pending.empty())
        {
            wait (2);
            continue;
        }

        std::stable_sort (pending.begin(), pending.end(),
                          [] (const Event& a, const Event& b) { return a.timeMs < b.timeMs; });

        const double t = juce::Time::getMillisecondCounterHiRes();
        size_t sent = 0;
        while (sent < pending.size() && pending[sent].timeMs <= t + 0.25)
        {
            owner.sendForNote (pending[sent].channel, pending[sent].note);
            ++sent;
        }
        pending.erase (pending.begin(), pending.begin() + (std::ptrdiff_t) sent);

        if (pending.empty())
            continue;

        const double remaining = pending.front().timeMs - juce::Time::getMillisecondCounterHiRes();
        if (remaining > 3.0)
            wait (juce::jmin (50, (int) (remaining - 2.0)));
        else
            juce::Thread::yield();   // ultimi ms: attesa attiva per essere precisi
    }

   #if JUCE_WINDOWS
    timeEndPeriod (1);
   #endif
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout CueOscProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "arm", 1 }, "Invia (ARM)", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "rec", 1 }, "Registra", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "dry", 1 }, "Prova (non invia)", false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "delay", 1 }, "Ritardo (ms)",
                                                       NormalisableRange<float> (0.0f, 500.0f, 1.0f), 0.0f));
    return layout;
}

CueOscProcessor::CueOscProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "CueOSC", createLayout()),
      sender (*this)
{
    armParam   = apvts.getRawParameterValue ("arm");
    recParam   = apvts.getRawParameterValue ("rec");
    dryParam   = apvts.getRawParameterValue ("dry");
    delayParam = apvts.getRawParameterValue ("delay");

    oscIn.addListener (this);
    sender.startThread (juce::Thread::Priority::highest);
    triggerAsyncUpdate();   // connessione con le impostazioni di default
}

CueOscProcessor::~CueOscProcessor()
{
    cancelPendingUpdate();
    sender.stopThread (2000);

    const juce::ScopedLock sl (netLock);
    oscIn.removeListener (this);
    oscIn.disconnect();
    oscOut.disconnect();
}

void CueOscProcessor::prepareToPlay (double sr, int)
{
    sampleRate = sr > 0 ? sr : 48000.0;
    offCountdown.fill (0);
    jitterInit = false;
}

bool CueOscProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void CueOscProcessor::updateJitter (double nowMs, double posMs, bool valid)
{
    if (! valid)
    {
        jitterInit = false;
        return;
    }

    const double d = nowMs - posMs;
    if (! jitterInit || std::abs (d - lastD) > 1000.0)   // partenza o salto di posizione
    {
        jitterInit = true;
        winMin = winMax = d;
        winStart = nowMs;
    }
    else
    {
        winMin = juce::jmin (winMin, d);
        winMax = juce::jmax (winMax, d);
        if (nowMs - winStart > 1000.0)
        {
            jitterMs = (float) (winMax - winMin);
            winStart = nowMs;
            winMin = winMax = d;
        }
    }
    lastD = d;
}

void CueOscProcessor::processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = audio.getNumSamples();

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        audio.clear (ch, 0, numSamples);

    const double nowMs = juce::Time::getMillisecondCounterHiRes();

    // posizione del trasporto, solo per diagnostica
    bool playing = false;
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            playing = pos->getIsPlaying() || pos->getIsRecording();
            if (auto samples = pos->getTimeInSamples())
                updateJitter (nowMs, (double) *samples * 1000.0 / sampleRate, playing);
            else
                updateJitter (nowMs, 0, false);
        }
    }
    transportPlaying = playing;

    // --- PLAY: note in ingresso -> OSC -------------------------------------------
    if (isArmed())
    {
        const double delay = (double) delayParam->load();
        for (const auto meta : midi)
        {
            const auto m = meta.getMessage();
            if (m.isNoteOn())
                sender.pushFromAudio ({ nowMs + meta.samplePosition * 1000.0 / sampleRate + delay,
                                        m.getChannel(), m.getNoteNumber() });
        }
    }

    midi.clear();   // le note dei cue non proseguono sulla traccia

    // --- REC: OSC in arrivo -> note in uscita (REAPER le registra) ---------------
    const int noteLen = juce::jmax (1, (int) (sampleRate * 0.1));   // note da 100 ms
    {
        int s1, n1, s2, n2;
        inFifo.prepareToRead (inFifo.getNumReady(), s1, n1, s2, n2);

        auto emit = [&] (const std::pair<int, int>& ev)
        {
            const int ch = ev.first, note = ev.second;
            const int idx = (ch - 1) * 128 + note;
            if (offCountdown[(size_t) idx] > 0)
                midi.addEvent (juce::MidiMessage::noteOff (ch, note), 0);
            midi.addEvent (juce::MidiMessage::noteOn (ch, note, (juce::uint8) 100), 0);
            offCountdown[(size_t) idx] = noteLen + numSamples;   // il ciclo sotto sottrae questo blocco
        };

        const bool rec = isRecording();
        for (int i = 0; i < n1; ++i) if (rec) emit (inBuffer[(size_t) (s1 + i)]);
        for (int i = 0; i < n2; ++i) if (rec) emit (inBuffer[(size_t) (s2 + i)]);
        inFifo.finishedRead (n1 + n2);
    }

    for (size_t i = 0; i < offCountdown.size(); ++i)
    {
        auto& c = offCountdown[i];
        if (c <= 0)
            continue;

        if (c <= numSamples)
        {
            midi.addEvent (juce::MidiMessage::noteOff ((int) (i / 128) + 1, (int) (i % 128)), juce::jmax (0, c - 1));
            c = 0;
        }
        else
        {
            c -= numSamples;
        }
    }
}

//==============================================================================
void CueOscProcessor::setNetwork (const juce::String& h, int sp, int lp)
{
    {
        const juce::ScopedLock sl (netLock);
        host = h.trim().isEmpty() ? juce::String ("127.0.0.1") : h.trim();
        sendPort = juce::jlimit (1, 65535, sp);
        listenPort = juce::jlimit (1, 65535, lp);
    }
    connectNetwork();
}

juce::String CueOscProcessor::getHost() const       { const juce::ScopedLock sl (netLock); return host; }
int CueOscProcessor::getSendPort() const            { const juce::ScopedLock sl (netLock); return sendPort; }
int CueOscProcessor::getListenPort() const          { const juce::ScopedLock sl (netLock); return listenPort; }
juce::String CueOscProcessor::getNetStatus() const  { const juce::ScopedLock sl (netLock); return netStatus; }

void CueOscProcessor::handleAsyncUpdate()
{
    connectNetwork();
}

void CueOscProcessor::connectNetwork()
{
    juce::String status;
    {
        const juce::ScopedLock sl (netLock);
        oscOut.disconnect();
        oscIn.disconnect();

        const bool outOk = oscOut.connect (host, sendPort);
        const bool inOk  = oscIn.connect (listenPort);

        status = juce::String (outOk ? "invio -> " + host + ":" + juce::String (sendPort)
                                     : "ERRORE invio verso " + host + ":" + juce::String (sendPort))
               + "   |   "
               + juce::String (inOk ? "ascolto porta " + juce::String (listenPort)
                                    : "ERRORE porta " + juce::String (listenPort) + " (gia' in uso?)");
        netStatus = status;
    }
    log ("RETE  " + status);
}

//==============================================================================
void CueOscProcessor::oscBundleReceived (const juce::OSCBundle& bundle)
{
    for (const auto& element : bundle)
    {
        if (element.isMessage())     handleIncoming (element.getMessage());
        else if (element.isBundle()) oscBundleReceived (element.getBundle());
    }
}

void CueOscProcessor::oscMessageReceived (const juce::OSCMessage& m)
{
    handleIncoming (m);
}

void CueOscProcessor::handleIncoming (const juce::OSCMessage& m)
{
    const auto address = m.getAddressPattern().toString();

    int channel = 0, note = 0;
    juce::String learned;
    {
        const juce::ScopedLock sl (mapLock);
        if (ignored.contains (address))
            return;

        for (const auto& r : rows)
        {
            if (r.enabled && r.valid && r.address == address && CueArgs::matches (m, r.parsed))
            {
                channel = r.channel;
                note = r.note;
                break;
            }
        }

        if (channel == 0 && learn.load())
        {
            auto r = makeNewRow (address, CueArgs::format (m));
            if (r.channel > 0)
            {
                rows.push_back (r);
                channel = r.channel;
                note = r.note;
                learned = r.name;
                ++mapVersion;
            }
        }
    }

    ++messagesIn;
    auto line = "IN    " + address + " " + CueArgs::format (m);
    if (learned.isNotEmpty())
        line << "   [LEARN -> ch " << channel << " nota " << CueNotes::describe (note) << "]";
    else if (channel == 0)
        line << "   (non mappato)";
    log (line);

    if (channel > 0 && isRecording())
    {
        int s1, n1, s2, n2;
        inFifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 + n2 > 0)
        {
            inBuffer[(size_t) (n1 > 0 ? s1 : s2)] = { channel, note };
            inFifo.finishedWrite (1);
        }
    }
}

//==============================================================================
void CueOscProcessor::sendForNote (int channel, int note)
{
    std::vector<CueRow> matching;
    {
        const juce::ScopedLock sl (mapLock);
        for (const auto& r : rows)
            if (r.enabled && r.channel == channel && r.note == note)
                matching.push_back (r);
    }

    if (matching.empty())
    {
        log ("OUT   ch " + juce::String (channel) + " nota " + CueNotes::describe (note) + "   (nessun messaggio associato)");
        return;
    }

    for (const auto& r : matching)
        sendRow (r, false);
}

void CueOscProcessor::sendRow (const CueRow& r, bool isTest)
{
    const auto text = r.address + " " + CueArgs::format (r.parsed);
    const juce::String tag = isTest ? "TEST  " : "OUT   ";

    if (! r.valid)
    {
        log ("ERR   riga \"" + r.name + "\": " + r.error);
        return;
    }

    if (isDryRun())
    {
        log (tag + text + "   (PROVA: non inviato)");
        return;
    }

    bool ok = false;
    try
    {
        juce::OSCMessage msg { juce::OSCAddressPattern (r.address) };
        for (const auto& a : r.parsed)
            msg.addArgument (a);

        const juce::ScopedLock sl (netLock);
        ok = oscOut.send (msg);
    }
    catch (...)
    {
        ok = false;
    }

    if (ok) ++messagesOut;
    log (tag + text + (ok ? juce::String() : juce::String ("   ERRORE di invio")));
}

//==============================================================================
int CueOscProcessor::getNumRows() const
{
    const juce::ScopedLock sl (mapLock);
    return (int) rows.size();
}

CueRow CueOscProcessor::getRow (int index) const
{
    const juce::ScopedLock sl (mapLock);
    return juce::isPositiveAndBelow (index, (int) rows.size()) ? rows[(size_t) index] : CueRow();
}

void CueOscProcessor::setRow (int index, CueRow row)
{
    row.channel = juce::jlimit (1, 16, row.channel);
    row.note = juce::jlimit (0, 127, row.note);
    row.reparse();

    const juce::ScopedLock sl (mapLock);
    if (juce::isPositiveAndBelow (index, (int) rows.size()))
    {
        rows[(size_t) index] = row;
        ++mapVersion;
    }
}

int CueOscProcessor::addRow (CueRow row)
{
    row.channel = juce::jlimit (1, 16, row.channel);
    row.note = juce::jlimit (0, 127, row.note);
    row.reparse();

    const juce::ScopedLock sl (mapLock);
    rows.push_back (row);
    ++mapVersion;
    return (int) rows.size() - 1;
}

void CueOscProcessor::removeRow (int index)
{
    const juce::ScopedLock sl (mapLock);
    if (juce::isPositiveAndBelow (index, (int) rows.size()))
    {
        rows.erase (rows.begin() + index);
        ++mapVersion;
    }
}

void CueOscProcessor::sortRows()
{
    const juce::ScopedLock sl (mapLock);
    std::stable_sort (rows.begin(), rows.end(), [] (const CueRow& a, const CueRow& b)
    {
        return a.channel != b.channel ? a.channel < b.channel : a.note < b.note;
    });
    ++mapVersion;
}

int CueOscProcessor::findFreeSlotLocked() const
{
    std::array<bool, 16 * 128> used {};
    for (const auto& r : rows)
        used[(size_t) ((r.channel - 1) * 128 + r.note)] = true;

    // si parte da C1 (36) sul canale 1, comodo da vedere nel piano roll
    for (int ch = 0; ch < 16; ++ch)
        for (int k = 0; k < 128; ++k)
        {
            const int note = (36 + k) % 128;
            if (! used[(size_t) (ch * 128 + note)])
                return ch * 128 + note;
        }
    return -1;
}

CueRow CueOscProcessor::makeNewRow (const juce::String& address, const juce::String& args) const
{
    const juce::ScopedLock sl (mapLock);
    CueRow r;
    const int slot = findFreeSlotLocked();
    if (slot < 0)
    {
        r.channel = 0;   // tabella piena
        return r;
    }
    r.channel = slot / 128 + 1;
    r.note = slot % 128;
    r.address = address;
    r.args = args;
    r.name = (address + " " + args).trim();
    r.reparse();
    return r;
}

juce::StringArray CueOscProcessor::getIgnored() const
{
    const juce::ScopedLock sl (mapLock);
    return ignored;
}

void CueOscProcessor::setIgnored (const juce::StringArray& addresses)
{
    juce::StringArray clean;
    for (auto a : addresses)
        if ((a = a.trim()).isNotEmpty())
            clean.addIfNotAlreadyThere (a);

    const juce::ScopedLock sl (mapLock);
    if (clean != ignored)
    {
        ignored = clean;
        ++ignoredVersion;
    }
}

void CueOscProcessor::ignoreAddress (const juce::String& address)
{
    const juce::ScopedLock sl (mapLock);
    ignored.addIfNotAlreadyThere (address);
    rows.erase (std::remove_if (rows.begin(), rows.end(), [&] (const CueRow& r) { return r.address == address; }),
                rows.end());
    ++ignoredVersion;
    ++mapVersion;
}

void CueOscProcessor::testRow (int index)
{
    if (juce::isPositiveAndBelow (index, getNumRows()))
        sender.pushImmediate (getRow (index));
}

//==============================================================================
void CueOscProcessor::log (const juce::String& line)
{
    const auto stamp = juce::Time::getCurrentTime().formatted ("%H:%M:%S.")
                     + juce::String (juce::Time::getCurrentTime().getMilliseconds()).paddedLeft ('0', 3);

    const juce::ScopedLock sl (logLock);
    logLines.add (stamp + "  " + line);
    if (logLines.size() > 400)
        logLines.removeRange (0, logLines.size() - 400);
    ++logSerial;
}

juce::StringArray CueOscProcessor::getLog() const
{
    const juce::ScopedLock sl (logLock);
    return logLines;
}

void CueOscProcessor::clearLog()
{
    const juce::ScopedLock sl (logLock);
    logLines.clear();
    ++logSerial;
}

//==============================================================================
void CueOscProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    juce::ValueTree net ("Net");
    net.setProperty ("host", getHost(), nullptr);
    net.setProperty ("sendPort", getSendPort(), nullptr);
    net.setProperty ("listenPort", getListenPort(), nullptr);
    net.setProperty ("learn", learn.load(), nullptr);

    juce::ValueTree rowsTree ("Rows");
    juce::ValueTree ign ("Ignored");
    {
        const juce::ScopedLock sl (mapLock);
        for (const auto& r : rows)
        {
            juce::ValueTree t ("Row");
            t.setProperty ("enabled", r.enabled, nullptr);
            t.setProperty ("channel", r.channel, nullptr);
            t.setProperty ("note", r.note, nullptr);
            t.setProperty ("name", r.name, nullptr);
            t.setProperty ("address", r.address, nullptr);
            t.setProperty ("args", r.args, nullptr);
            rowsTree.appendChild (t, nullptr);
        }
        ign.setProperty ("list", ignored.joinIntoString ("\n"), nullptr);
    }

    for (auto* name : { "Net", "Rows", "Ignored" })
        state.removeChild (state.getChildWithName (name), nullptr);

    state.appendChild (net, nullptr);
    state.appendChild (rowsTree, nullptr);
    state.appendChild (ign, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void CueOscProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);

    auto net = tree.getChildWithName ("Net");
    auto rowsTree = tree.getChildWithName ("Rows");
    auto ign = tree.getChildWithName ("Ignored");

    std::vector<CueRow> newRows;
    for (const auto& t : rowsTree)
    {
        CueRow r;
        r.enabled = (bool) t.getProperty ("enabled", true);
        r.channel = juce::jlimit (1, 16, (int) t.getProperty ("channel", 1));
        r.note    = juce::jlimit (0, 127, (int) t.getProperty ("note", 36));
        r.name    = t.getProperty ("name").toString();
        r.address = t.getProperty ("address").toString();
        r.args    = t.getProperty ("args").toString();
        r.reparse();
        newRows.push_back (r);
    }

    {
        const juce::ScopedLock sl (mapLock);
        rows = std::move (newRows);
        ignored = juce::StringArray::fromLines (ign.getProperty ("list").toString());
        ignored.removeEmptyStrings();
        ++mapVersion;
        ++ignoredVersion;
    }

    if (net.isValid())
    {
        const juce::ScopedLock sl (netLock);
        host       = net.getProperty ("host", "127.0.0.1").toString();
        sendPort   = (int) net.getProperty ("sendPort", 9000);
        listenPort = (int) net.getProperty ("listenPort", 9001);
        learn      = (bool) net.getProperty ("learn", false);
    }

    for (auto* name : { "Net", "Rows", "Ignored" })
        tree.removeChild (tree.getChildWithName (name), nullptr);

    apvts.replaceState (tree);
    triggerAsyncUpdate();   // riconnette con le porte salvate nel progetto
}

//==============================================================================
juce::AudioProcessorEditor* CueOscProcessor::createEditor()
{
    return new CueOscEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CueOscProcessor();
}
