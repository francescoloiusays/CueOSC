#include "PluginEditor.h"

namespace
{
    const juce::Colour bg       { 0xff1b1d21 };
    const juce::Colour panel    { 0xff25282e };
    const juce::Colour rowA     { 0xff22252a };
    const juce::Colour rowB     { 0xff272a30 };
    const juce::Colour rowSel   { 0xff31445e };
    const juce::Colour textCol  { 0xffe6e6e6 };
    const juce::Colour dimText  { 0xff9aa0a8 };
    const juce::Colour redOn    { 0xffd63a3a };
    const juce::Colour orangeOn { 0xffe0782b };
    const juce::Colour yellowOn { 0xffc9a227 };
    const juce::Colour blueOn   { 0xff2f7fd6 };

    //==========================================================================
    // Cella testuale: doppio clic per modificare.
    class TextCell : public juce::Label
    {
    public:
        explicit TextCell (CueOscEditor& e) : owner (e)
        {
            setEditable (false, true, false);
            setColour (juce::Label::textColourId, textCol);
            setColour (juce::Label::textWhenEditingColourId, textCol);
            setColour (juce::Label::backgroundWhenEditingColourId, juce::Colours::black);
            setColour (juce::Label::outlineWhenEditingColourId, blueOn);
            onTextChange = [this] { owner.cellEdited (row, column, getText()); };
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            owner.selectRowFromCell (row, e.mods);
            juce::Label::mouseDown (e);
        }

        int row = 0, column = 0;

    private:
        CueOscEditor& owner;
    };

    class ToggleCell : public juce::ToggleButton
    {
    public:
        explicit ToggleCell (CueOscEditor& e) : owner (e)
        {
            onClick = [this] { owner.cellEdited (row, CueOscEditor::colOn, getToggleState() ? "1" : "0"); };
        }
        int row = 0;
    private:
        CueOscEditor& owner;
    };

    class TestCell : public juce::TextButton
    {
    public:
        explicit TestCell (CueOscEditor& e) : juce::TextButton ("Test"), owner (e)
        {
            setTooltip ("Manda subito questo messaggio (rispetta PROVA)");
            onClick = [this] { owner.getProcessor().testRow (row); };
        }
        int row = 0;
    private:
        CueOscEditor& owner;
    };

    void styleToggle (juce::TextButton& b, juce::Colour on)
    {
        b.setClickingTogglesState (true);
        b.setColour (juce::TextButton::buttonColourId, panel.brighter (0.1f));
        b.setColour (juce::TextButton::buttonOnColourId, on);
        b.setColour (juce::TextButton::textColourOffId, dimText);
        b.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    }

    void styleEditor (juce::TextEditor& ed)
    {
        ed.setColour (juce::TextEditor::backgroundColourId, juce::Colours::black.withAlpha (0.35f));
        ed.setColour (juce::TextEditor::textColourId, textCol);
        ed.setColour (juce::TextEditor::outlineColourId, panel.brighter (0.25f));
    }

    void styleLabel (juce::Label& l, const juce::String& text, juce::Colour c = dimText)
    {
        l.setText (text, juce::dontSendNotification);
        l.setColour (juce::Label::textColourId, c);
    }
}

//==============================================================================
CueOscEditor::CueOscEditor (CueOscProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    setOpaque (true);

    styleLabel (title, "CueOSC", textCol);
    title.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    addAndMakeVisible (title);

    styleLabel (hostLbl, "IP BFE");
    styleLabel (sendLbl, "Porta invio");
    styleLabel (listenLbl, "Porta ascolto");
    for (auto* l : { &hostLbl, &sendLbl, &listenLbl })
    {
        l->setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (*l);
    }

    for (auto* ed : { &hostEd, &sendEd, &listenEd })
    {
        styleEditor (*ed);
        ed->onReturnKey = [this] { applyNetwork(); };
        addAndMakeVisible (*ed);
    }
    sendEd.setInputRestrictions (5, "0123456789");
    listenEd.setInputRestrictions (5, "0123456789");
    hostEd.setText (proc.getHost(), false);
    sendEd.setText (juce::String (proc.getSendPort()), false);
    listenEd.setText (juce::String (proc.getListenPort()), false);

    connectBtn.onClick = [this] { applyNetwork(); };
    addAndMakeVisible (connectBtn);

    styleLabel (netStatus, proc.getNetStatus());
    netStatus.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (netStatus);

    // --- interruttori ---
    styleToggle (armBtn, redOn);
    styleToggle (recBtn, orangeOn);
    styleToggle (dryBtn, yellowOn);
    styleToggle (learnBtn, blueOn);
    armBtn.setTooltip ("Le note della traccia diventano messaggi OSC verso il BFE. Spento = non parte niente.");
    recBtn.setTooltip ("I messaggi OSC che arrivano diventano note in uscita: arma la traccia in 'Record: output (MIDI)'.");
    dryBtn.setTooltip ("Modo prova: scrive nel log cosa manderebbe, ma non manda nulla.");
    learnBtn.setTooltip ("Ogni messaggio OSC nuovo che arriva crea da solo una riga con la prima nota libera.");

    armAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "arm", armBtn);
    recAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "rec", recBtn);
    dryAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "dry", dryBtn);

    learnBtn.setToggleState (proc.learn.load(), juce::dontSendNotification);
    learnBtn.onClick = [this] { proc.learn = learnBtn.getToggleState(); };

    for (auto* b : { &armBtn, &recBtn, &dryBtn, &learnBtn })
        addAndMakeVisible (*b);

    styleLabel (delayLbl, "Ritardo");
    delayLbl.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (delayLbl);
    delaySlider.setSliderStyle (juce::Slider::LinearHorizontal);
    delaySlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 22);
    delaySlider.setTextValueSuffix (" ms");
    delaySlider.setTooltip ("Ritarda tutti i comandi in uscita. Per anticiparli usa il 'playback offset' della traccia in REAPER.");
    delayAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "delay", delaySlider);
    addAndMakeVisible (delaySlider);

    styleLabel (statsLbl, {});
    statsLbl.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (statsLbl);
    styleLabel (warnLbl, {}, juce::Colour (0xffff6b6b));
    warnLbl.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    addAndMakeVisible (warnLbl);

    // --- tabella ---
    auto& h = table.getHeader();
    h.addColumn ("On",            colOn,      36, 36, 36, juce::TableHeaderComponent::notResizable);
    h.addColumn ("Can.",          colChannel, 48, 40, 60);
    h.addColumn ("Nota",          colNote,    80, 60, 120);
    h.addColumn ("Nome",          colName,   190, 80, 400);
    h.addColumn ("Indirizzo OSC", colAddress, 220, 80, 500);
    h.addColumn ("Argomenti",     colArgs,   150, 60, 400);
    h.addColumn ("",              colTest,    56, 56, 56, juce::TableHeaderComponent::notResizable);
    table.setRowHeight (24);
    table.setMultipleSelectionEnabled (true);
    table.setColour (juce::ListBox::backgroundColourId, rowA);
    table.setColour (juce::ListBox::outlineColourId, panel.brighter (0.2f));
    table.setOutlineThickness (1);
    addAndMakeVisible (table);

    addBtn.onClick = [this]
    {
        auto r = proc.makeNewRow ("/", "");
        if (r.channel == 0)
            return;
        r.name = "nuovo cue";
        const int idx = proc.addRow (r);
        table.updateContent();
        table.selectRow (idx);
    };
    delBtn.onClick = [this] { deleteSelected(); };
    sortBtn.onClick = [this] { proc.sortRows(); };
    ignoreBtn.setTooltip ("Ignora l'indirizzo delle righe selezionate (es. la leva T-bar) e cancella le sue righe.");
    ignoreBtn.onClick = [this]
    {
        juce::StringArray addrs;
        const auto sel = table.getSelectedRows();
        for (int i = 0; i < sel.size(); ++i)
            addrs.addIfNotAlreadyThere (proc.getRow (sel[i]).address);
        for (auto& a : addrs)
            if (a.isNotEmpty())
                proc.ignoreAddress (a);
        table.deselectAllRows();
    };
    clearLogBtn.onClick = [this] { proc.clearLog(); };
    for (auto* b : { &addBtn, &delBtn, &sortBtn, &ignoreBtn, &clearLogBtn })
        addAndMakeVisible (*b);

    styleLabel (ignoreLbl, "Indirizzi ignorati (uno per riga)");
    addAndMakeVisible (ignoreLbl);
    styleEditor (ignoreEd);
    ignoreEd.setMultiLine (true, false);
    ignoreEd.setReturnKeyStartsNewLine (true);
    ignoreEd.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    ignoreEd.onTextChange = [this] { proc.setIgnored (juce::StringArray::fromLines (ignoreEd.getText())); lastIgnoredVersion = proc.getIgnoredVersion(); };
    addAndMakeVisible (ignoreEd);

    styleLabel (helpLbl, "Doppio clic su una cella per modificarla.\n"
                         "Argomenti: 3 = intero, 3.0 = float, \"testo\" = stringa.\n"
                         "Piu' righe con la stessa nota = piu' messaggi insieme.");
    helpLbl.setFont (juce::FontOptions (12.0f));
    helpLbl.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (helpLbl);

    styleEditor (logView);
    logView.setMultiLine (true, false);
    logView.setReadOnly (true);
    logView.setCaretVisible (false);
    logView.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
    addAndMakeVisible (logView);

    setResizable (true, true);
    setResizeLimits (820, 560, 2400, 1600);
    setSize (1040, 700);

    timerCallback();
    startTimerHz (12);
}

CueOscEditor::~CueOscEditor()
{
    stopTimer();
}

//==============================================================================
void CueOscEditor::paint (juce::Graphics& g)
{
    g.fillAll (bg);
    g.setColour (panel);
    g.fillRect (getLocalBounds().removeFromTop (96));
}

void CueOscEditor::resized()
{
    auto area = getLocalBounds().reduced (10);

    // riga 1: titolo + rete
    {
        auto r = area.removeFromTop (30);
        title.setBounds (r.removeFromLeft (120));
        connectBtn.setBounds (r.removeFromRight (90));
        r.removeFromRight (8);
        listenEd.setBounds (r.removeFromRight (64));
        listenLbl.setBounds (r.removeFromRight (100));
        sendEd.setBounds (r.removeFromRight (64));
        sendLbl.setBounds (r.removeFromRight (90));
        hostEd.setBounds (r.removeFromRight (150));
        hostLbl.setBounds (r.removeFromRight (60));
    }
    netStatus.setBounds (area.removeFromTop (18).withTrimmedLeft (120));
    area.removeFromTop (8);

    // riga 2: interruttori
    {
        auto r = area.removeFromTop (34);
        for (auto* b : { &armBtn, &recBtn, &dryBtn, &learnBtn })
        {
            b->setBounds (r.removeFromLeft (110));
            r.removeFromLeft (8);
        }
        delayLbl.setBounds (r.removeFromLeft (60));
        delaySlider.setBounds (r.removeFromLeft (230));
        r.removeFromLeft (12);
        statsLbl.setBounds (r.removeFromTop (17));
        warnLbl.setBounds (r);
    }
    area.removeFromTop (14);

    // log in basso
    auto logArea = area.removeFromBottom (juce::jmax (120, area.getHeight() / 4));
    logView.setBounds (logArea);
    area.removeFromBottom (6);

    // pulsanti sotto la tabella
    auto buttons = area.removeFromBottom (28);
    for (auto* b : { &addBtn, &delBtn, &sortBtn, &ignoreBtn })
    {
        b->setBounds (buttons.removeFromLeft (b == &ignoreBtn ? 130 : 100));
        buttons.removeFromLeft (6);
    }
    clearLogBtn.setBounds (buttons.removeFromRight (100));
    area.removeFromBottom (6);

    // colonna destra
    auto side = area.removeFromRight (230);
    area.removeFromRight (8);
    ignoreLbl.setBounds (side.removeFromTop (20));
    helpLbl.setBounds (side.removeFromBottom (80));
    side.removeFromBottom (6);
    ignoreEd.setBounds (side);

    table.setBounds (area);
}

//==============================================================================
void CueOscEditor::timerCallback()
{
    if (proc.getMapVersion() != lastMapVersion)
    {
        lastMapVersion = proc.getMapVersion();
        table.updateContent();
        table.repaint();
    }

    if (proc.getIgnoredVersion() != lastIgnoredVersion && ! ignoreEd.hasKeyboardFocus (true))
    {
        lastIgnoredVersion = proc.getIgnoredVersion();
        ignoreEd.setText (proc.getIgnored().joinIntoString ("\n"), false);
    }

    if (proc.getLogSerial() != lastLogSerial)
        refreshLog();

    netStatus.setText (proc.getNetStatus(), juce::dontSendNotification);
    learnBtn.setToggleState (proc.learn.load(), juce::dontSendNotification);

    const float jit = proc.jitterMs.load();
    const bool playing = proc.transportPlaying.load();
    juce::String stats;
    stats << "IN " << proc.messagesIn.load() << "   OUT " << proc.messagesOut.load()
          << "   jitter " << (playing ? juce::String (juce::roundToInt (jit)) + " ms" : juce::String ("-"));
    statsLbl.setText (stats, juce::dontSendNotification);

    juce::String warn;
    if (playing && jit > 35.0f)
        warn = "Jitter alto: disattiva 'anticipative FX' su questa traccia";
    else if (proc.isArmed() && proc.isDryRun())
        warn = "PROVA attiva: non parte niente verso il BFE";
    warnLbl.setText (warn, juce::dontSendNotification);
}

void CueOscEditor::refreshLog()
{
    lastLogSerial = proc.getLogSerial();
    const auto lines = proc.getLog();
    logView.setText (lines.joinIntoString ("\n"), false);
    logView.moveCaretToEnd();
}

void CueOscEditor::applyNetwork()
{
    proc.setNetwork (hostEd.getText(), sendEd.getText().getIntValue(), listenEd.getText().getIntValue());
    hostEd.setText (proc.getHost(), false);
    sendEd.setText (juce::String (proc.getSendPort()), false);
    listenEd.setText (juce::String (proc.getListenPort()), false);
}

void CueOscEditor::deleteSelected()
{
    const auto sel = table.getSelectedRows();
    for (int i = sel.size(); --i >= 0;)
        proc.removeRow (sel[i]);
    table.deselectAllRows();
}

//==============================================================================
int CueOscEditor::getNumRows()
{
    return proc.getNumRows();
}

void CueOscEditor::paintRowBackground (juce::Graphics& g, int row, int, int, bool selected)
{
    g.fillAll (selected ? rowSel : (row % 2 == 0 ? rowA : rowB));
}

void CueOscEditor::deleteKeyPressed (int)
{
    deleteSelected();
}

void CueOscEditor::selectRowFromCell (int row, const juce::ModifierKeys& mods)
{
    table.selectRowsBasedOnModifierKeys (row, mods, false);
}

juce::Component* CueOscEditor::refreshComponentForCell (int row, int column, bool, juce::Component* existing)
{
    if (! juce::isPositiveAndBelow (row, proc.getNumRows()))
    {
        delete existing;
        return nullptr;
    }

    const auto r = proc.getRow (row);

    if (column == colOn)
    {
        auto* c = dynamic_cast<ToggleCell*> (existing);
        if (c == nullptr) { delete existing; c = new ToggleCell (*this); }
        c->row = row;
        c->setToggleState (r.enabled, juce::dontSendNotification);
        return c;
    }

    if (column == colTest)
    {
        auto* c = dynamic_cast<TestCell*> (existing);
        if (c == nullptr) { delete existing; c = new TestCell (*this); }
        c->row = row;
        return c;
    }

    auto* c = dynamic_cast<TextCell*> (existing);
    if (c == nullptr) { delete existing; c = new TextCell (*this); }
    c->row = row;
    c->column = column;

    juce::String text;
    switch (column)
    {
        case colChannel: text = juce::String (r.channel); break;
        case colNote:    text = CueNotes::describe (r.note); break;
        case colName:    text = r.name; break;
        case colAddress: text = r.address; break;
        case colArgs:    text = r.args; break;
        default: break;
    }

    if (! c->isBeingEdited())
        c->setText (text, juce::dontSendNotification);

    const bool bad = ! r.valid && (column == colAddress || column == colArgs);
    c->setColour (juce::Label::textColourId, bad ? juce::Colour (0xffff6b6b) : (r.enabled ? textCol : dimText));
    c->setTooltip (bad ? r.error : juce::String());
    return c;
}

void CueOscEditor::cellEdited (int row, int column, const juce::String& text)
{
    if (! juce::isPositiveAndBelow (row, proc.getNumRows()))
        return;

    auto r = proc.getRow (row);
    switch (column)
    {
        case colOn:      r.enabled = text == "1"; break;
        case colChannel: r.channel = juce::jlimit (1, 16, text.getIntValue()); break;
        case colNote:
        {
            const int n = CueNotes::parse (text);
            if (n >= 0) r.note = n;
            break;
        }
        case colName:    r.name = text.trim(); break;
        case colAddress: r.address = text.trim(); break;
        case colArgs:    r.args = text.trim(); break;
        default: return;
    }
    proc.setRow (row, r);
}
