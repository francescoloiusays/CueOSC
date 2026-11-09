#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_osc/juce_osc.h>

// Una riga della tabella: nota MIDI (canale + numero) <-> messaggio OSC (indirizzo + argomenti).
struct CueRow
{
    bool enabled = true;
    int channel = 1;          // 1..16
    int note = 36;            // 0..127
    juce::String name;
    juce::String address;     // es. "/me1/pgm"
    juce::String args;        // es. "3"  oppure  "1.0 \"cam 1\""

    // cache degli argomenti gia' interpretati
    juce::Array<juce::OSCArgument> parsed;
    bool valid = false;
    juce::String error;

    void reparse();
};

namespace CueArgs
{
    // Testo -> argomenti OSC.
    //  3        -> int32
    //  3.0 / 1e3 -> float32
    //  "cam 1"  -> stringa (le virgolette permettono gli spazi)
    //  cam      -> stringa
    inline juce::Array<juce::OSCArgument> parse (const juce::String& text, juce::String* error = nullptr)
    {
        juce::Array<juce::OSCArgument> out;
        auto p = text.getCharPointer();

        auto isNumberToken = [] (const juce::String& t, bool& isInt) -> bool
        {
            if (t.isEmpty())
                return false;

            isInt = t.containsOnly ("+-0123456789") && t.containsAnyOf ("0123456789");
            if (isInt)
                return true;

            if (! t.containsOnly ("+-0123456789.eE") || ! t.containsAnyOf ("0123456789"))
                return false;

            // controllo grezzo ma sufficiente: deve rientrare in un double valido
            auto s = t.toStdString();
            char* end = nullptr;
            std::strtod (s.c_str(), &end);
            return end != nullptr && *end == 0;
        };

        while (! p.isEmpty())
        {
            if (p.isWhitespace()) { ++p; continue; }

            if (*p == '"')
            {
                ++p;
                juce::String s;
                while (! p.isEmpty() && *p != '"')
                    s += *p++;

                if (p.isEmpty())
                {
                    if (error != nullptr) *error = "virgolette non chiuse";
                    return {};
                }

                ++p; // chiude le virgolette
                out.add (juce::OSCArgument (s));
                continue;
            }

            juce::String tok;
            while (! p.isEmpty() && ! p.isWhitespace())
                tok += *p++;

            bool isInt = false;
            if (isNumberToken (tok, isInt))
            {
                if (isInt) out.add (juce::OSCArgument ((juce::int32) tok.getLargeIntValue()));
                else       out.add (juce::OSCArgument ((float) tok.getDoubleValue()));
            }
            else
            {
                out.add (juce::OSCArgument (tok));
            }
        }

        if (error != nullptr) *error = {};
        return out;
    }

    inline juce::String formatFloat (float f)
    {
        juce::String s ((double) f, 6);
        while (s.endsWithChar ('0') && ! s.endsWith (".0"))
            s = s.dropLastCharacters (1);
        return s;
    }

    inline juce::String format (const juce::OSCArgument& a)
    {
        if (a.isInt32())   return juce::String (a.getInt32());
        if (a.isFloat32()) return formatFloat (a.getFloat32());
        if (a.isString())  return "\"" + a.getString() + "\"";
        return "?";
    }

    template <typename Container>
    inline juce::String format (const Container& args)
    {
        juce::StringArray parts;
        for (const auto& a : args)
            parts.add (format (a));
        return parts.joinIntoString (" ");
    }

    inline bool argEquals (const juce::OSCArgument& expected, const juce::OSCArgument& got)
    {
        auto isNum = [] (const juce::OSCArgument& a) { return a.isInt32() || a.isFloat32(); };
        auto num   = [] (const juce::OSCArgument& a) { return a.isInt32() ? (double) a.getInt32() : (double) a.getFloat32(); };

        if (isNum (expected) && isNum (got))
            return std::abs (num (expected) - num (got)) < 1.0e-4;

        if (expected.isString() && got.isString())
            return expected.getString() == got.getString();

        return false;
    }

    inline bool matches (const juce::OSCMessage& m, const juce::Array<juce::OSCArgument>& expected)
    {
        if (m.size() != expected.size())
            return false;

        for (int i = 0; i < expected.size(); ++i)
            if (! argEquals (expected.getReference (i), m[i]))
                return false;

        return true;
    }
}

namespace CueNotes
{
    inline juce::String describe (int note)
    {
        return juce::String (note) + "  " + juce::MidiMessage::getMidiNoteName (note, true, true, 4);
    }

    // Accetta "60", "60  C4", "C4", "f#3", "Bb-1". Ritorna -1 se non valido.
    inline int parse (const juce::String& textIn)
    {
        auto text = textIn.trim();
        if (text.isEmpty())
            return -1;

        if (juce::CharacterFunctions::isDigit (text[0]))
        {
            auto n = text.getIntValue();
            return juce::isPositiveAndBelow (n, 128) ? n : -1;
        }

        static const int pcs[] = { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G
        auto letter = juce::CharacterFunctions::toUpperCase (text[0]);
        if (letter < 'A' || letter > 'G')
            return -1;

        int pc = pcs[letter - 'A'];
        int i = 1;
        if (text[i] == '#') { ++pc; ++i; }
        else if (text[i] == 'b') { --pc; ++i; }

        auto octText = text.substring (i).trim();
        if (octText.isEmpty() || ! octText.containsOnly ("-0123456789"))
            return -1;

        auto n = (octText.getIntValue() + 1) * 12 + pc;
        return juce::isPositiveAndBelow (n, 128) ? n : -1;
    }
}

inline void CueRow::reparse()
{
    error = {};
    parsed = CueArgs::parse (args, &error);

    valid = error.isEmpty();
    if (valid && ! address.startsWithChar ('/'))
    {
        valid = false;
        error = "l'indirizzo deve iniziare con /";
    }
    else if (valid)
    {
        try { juce::OSCAddressPattern test (address); }
        catch (...) { valid = false; error = "indirizzo OSC non valido"; }
    }
}
