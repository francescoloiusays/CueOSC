// Test senza REAPER: carica il plugin, gli manda OSC in UDP sul loopback,
// fa girare l'audio a mano e controlla note in uscita e messaggi OSC spediti.

#include "../Source/PluginProcessor.h"
#include <iostream>

namespace
{
    int failures = 0;

    void check (bool ok, const char* what)
    {
        std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
        if (! ok) ++failures;
    }

    struct Collector : juce::OSCReceiver::Listener<juce::OSCReceiver::RealtimeCallback>
    {
        void oscMessageReceived (const juce::OSCMessage& m) override
        {
            const juce::ScopedLock sl (lock);
            got.add (m.getAddressPattern().toString() + " " + CueArgs::format (m));
        }
        juce::StringArray take()
        {
            const juce::ScopedLock sl (lock);
            auto copy = got;
            got.clear();
            return copy;
        }
        juce::CriticalSection lock;
        juce::StringArray got;
    };

    void setParam (CueOscProcessor& p, const char* id, bool on)
    {
        p.apvts.getParameter (id)->setValueNotifyingHost (on ? 1.0f : 0.0f);
    }

    juce::MidiBuffer runBlock (CueOscProcessor& p, juce::MidiBuffer in = {})
    {
        juce::AudioBuffer<float> audio (2, 512);
        audio.clear();
        p.processBlock (audio, in);
        return in;
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;

    std::cout << "Argomenti OSC" << std::endl;
    {
        auto a = CueArgs::parse ("3 1.5 \"cam 1\" pgm -2");
        check (a.size() == 5, "5 argomenti");
        check (a.getReference (0).isInt32() && a.getReference (0).getInt32() == 3, "3 -> int");
        check (a.getReference (1).isFloat32() && std::abs (a.getReference (1).getFloat32() - 1.5f) < 1e-6f, "1.5 -> float");
        check (a.getReference (2).isString() && a.getReference (2).getString() == "cam 1", "\"cam 1\" -> stringa con spazio");
        check (a.getReference (3).isString() && a.getReference (3).getString() == "pgm", "pgm -> stringa");
        check (a.getReference (4).isInt32() && a.getReference (4).getInt32() == -2, "-2 -> int");
        check (CueArgs::format (a) == "3 1.5 \"cam 1\" \"pgm\" -2", "formato di ritorno");
        check (CueArgs::format (CueArgs::parse ("1.0")) == "1.0", "1.0 resta float");
        check (CueNotes::parse ("C4") == 60 && CueNotes::parse ("60  C4") == 60 && CueNotes::parse ("F#3") == 54, "nomi delle note");
    }

    CueOscProcessor p;
    p.prepareToPlay (48000.0, 512);
    p.setNetwork ("127.0.0.1", 39910, 39911);
    check (p.getNetStatus().contains ("ascolto porta 39911") && ! p.getNetStatus().contains ("ERRORE"), "connessione");

    juce::OSCSender bfe;           // finge di essere il BFE che manda lo stato
    bfe.connect ("127.0.0.1", 39911);
    juce::OSCReceiver bfeIn;       // finge di essere il BFE che riceve i comandi
    Collector collector;
    bfeIn.addListener (&collector);
    check (bfeIn.connect (39910), "ricevitore di test");

    std::cout << "LEARN + REGISTRA" << std::endl;
    {
        p.learn = true;
        setParam (p, "rec", true);
        bfe.send ("/me1/pgm", (juce::int32) 3);
        bfe.send ("/me1/pgm", (juce::int32) 5);
        bfe.send ("/me1/pgm", (juce::int32) 3);   // gia' imparato: stessa nota
        juce::Thread::sleep (300);

        check (p.getNumRows() == 2, "2 righe imparate");
        check (p.getRow (0).address == "/me1/pgm" && p.getRow (0).args == "3" && p.getRow (0).note == 36, "riga 1 = /me1/pgm 3 su nota 36");
        check (p.getRow (1).note == 37, "riga 2 su nota 37");

        auto out = runBlock (p);
        int ons = 0, n36 = 0;
        for (const auto meta : out)
            if (meta.getMessage().isNoteOn()) { ++ons; if (meta.getMessage().getNoteNumber() == 36) ++n36; }
        check (ons == 3 && n36 == 2, "3 note in uscita (36, 37, 36)");

        int offs = 0;
        for (int i = 0; i < 20; ++i)
            for (const auto meta : runBlock (p))
                if (meta.getMessage().isNoteOff()) ++offs;
        check (offs == 2, "note-off dopo ~100 ms");

        p.learn = false;
        setParam (p, "rec", false);
        bfe.send ("/altro", 1.0f);
        juce::Thread::sleep (200);
        check (p.getNumRows() == 2, "senza LEARN non aggiunge righe");
        bool silent = true;
        for (const auto meta : runBlock (p)) { juce::ignoreUnused (meta); silent = false; }
        check (silent, "senza REGISTRA non escono note");
    }

    std::cout << "PLAY" << std::endl;
    {
        juce::MidiBuffer in;
        in.addEvent (juce::MidiMessage::noteOn (1, 37, (juce::uint8) 100), 10);
        runBlock (p, in);
        juce::Thread::sleep (200);
        check (collector.take().isEmpty(), "disarmato: non parte niente");

        setParam (p, "arm", true);
        runBlock (p, in);
        juce::Thread::sleep (200);
        auto got = collector.take();
        check (got.size() == 1 && got[0] == "/me1/pgm 5", "armato: nota 37 -> /me1/pgm 5");

        // due messaggi sulla stessa nota
        CueRow extra = p.getRow (1);
        extra.address = "/me1/auto";
        extra.args = "";
        p.addRow (extra);
        runBlock (p, in);
        juce::Thread::sleep (200);
        got = collector.take();
        check (got.size() == 2 && got.contains ("/me1/auto "), "stessa nota -> due messaggi");

        setParam (p, "dry", true);
        runBlock (p, in);
        juce::Thread::sleep (200);
        check (collector.take().isEmpty(), "PROVA: non parte niente");
        setParam (p, "dry", false);

        // ritardo
        p.apvts.getParameter ("delay")->setValueNotifyingHost (p.apvts.getParameter ("delay")->convertTo0to1 (150.0f));
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        juce::MidiBuffer one;
        one.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 100), 0);
        runBlock (p, one);
        double arrived = 0;
        while (juce::Time::getMillisecondCounterHiRes() - t0 < 1000)
        {
            if (collector.take().size() > 0) { arrived = juce::Time::getMillisecondCounterHiRes() - t0; break; }
            juce::Thread::sleep (1);
        }
        std::cout << "        arrivato dopo " << arrived << " ms" << std::endl;
        // sulle macchine condivise di GitHub i tempi sono meno stabili
        const double maxMs = juce::SystemStats::getEnvironmentVariable ("CI", {}).isNotEmpty() ? 400.0 : 200.0;
        check (arrived > 145 && arrived < maxMs, "ritardo 150 ms rispettato");
    }

    std::cout << "Salvataggio nel progetto" << std::endl;
    {
        p.ignoreAddress ("/tbar");
        juce::MemoryBlock state;
        p.getStateInformation (state);

        CueOscProcessor q;
        q.setStateInformation (state.getData(), (int) state.getSize());
        check (q.getNumRows() == 3, "righe ripristinate");
        check (q.getRow (1).args == "5" && q.getRow (2).address == "/me1/auto", "contenuto righe");
        check (q.getSendPort() == 39910 && q.getListenPort() == 39911, "porte ripristinate");
        check (q.isArmed(), "ARM ripristinato");
        check (q.getIgnored().contains ("/tbar"), "ignorati ripristinati");
    }

    // immagine dell'interfaccia: CueOSCTests --snapshot percorso.png
    if (argc > 2 && juce::String (argv[1]) == "--snapshot")
    {
        p.learn = true;
        p.log ("IN    /me1/pgm 3   [LEARN -> ch 1 nota 36  C2]");
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        ed->setSize (1040, 700);
        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
        juce::File out (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
        out.deleteFile();
        juce::FileOutputStream os (out);
        juce::PNGImageFormat().writeImageToStream (img, os);
        std::cout << "immagine: " << out.getFullPathName() << std::endl;
    }

    bfeIn.disconnect();
    std::cout << (failures == 0 ? "\nTUTTO OK" : "\nCI SONO ERRORI") << std::endl;
    return failures == 0 ? 0 : 1;
}
