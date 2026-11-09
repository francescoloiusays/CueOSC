# CueOSC

Un "CuePilot fatto in casa" dentro REAPER: un plugin VST3 da mettere su una traccia
che **registra** i messaggi OSC che arrivano dal BFE (lo stato del mixer Kayak) come
note MIDI, e in **play** rimanda al BFE gli stessi messaggi a tempo con la musica.

Niente TouchOSC, niente loopMIDI: il MIDI resta dentro REAPER, la rete la gestisce il plugin.

```
Traccia "REGIA" in REAPER
  item MIDI (i cue)  ->  [CueOSC]  <- OSC/UDP ->  BFE  <->  Kayak
```

## Installazione (Windows)

1. Scarica `CueOSC-Windows` dalla pagina **Actions** del repository (ultima build verde)
   oppure dalla pagina **Releases**.
2. Copia la cartella `CueOSC.vst3` in `C:\Program Files\Common Files\VST3\`.
3. In REAPER: *Options > Preferences > Plug-ins > VST > Re-scan*.
4. Al primo avvio Windows chiede il permesso del firewall per REAPER: consenti le **reti private**,
   altrimenti i messaggi in arrivo dal BFE vengono bloccati.

## Prima configurazione

1. Crea una traccia (es. "REGIA") e aggiungi **CueOSC** come FX.
2. In alto: **IP BFE**, **Porta invio** (dove il BFE ascolta OSC), **Porta ascolto**
   (dove il BFE manda OSC al PC). Premi **Connetti**. La riga sotto dice se e' tutto a posto.
3. Tasto destro sulla traccia > *Track performance options* > togli
   **"Allow anticipative FX processing"**. Se resta attivo, REAPER elabora la traccia in anticipo
   e gli stacchi arrivano fuori tempo; il plugin lo segnala con "Jitter alto".

## Imparare i messaggi (LEARN)

1. Accendi **LEARN**.
2. Fai sul mixer le azioni che vuoi automatizzare (stacchi CAM 1, CAM 2, AUTO, ecc.).
3. Ogni messaggio nuovo diventa una riga con la prima nota libera (da C2 = 36 in su, canale 1).
4. Spegni LEARN. Rinomina le righe ("CAM 1", "AUTO"...) con doppio clic.

Messaggi che arrivano in continuazione (es. la leva T-bar): seleziona la riga e premi
**Ignora indirizzo**, oppure scrivi l'indirizzo nel riquadro "Indirizzi ignorati".

Le righe si possono anche scrivere a mano: **+ Aggiungi**, poi doppio clic su indirizzo e argomenti.
Argomenti: `3` = intero, `3.0` = float, `"cam 1"` = stringa. Piu' righe con la stessa nota
mandano piu' messaggi insieme (es. stacco + richiamo E-MEM).

## Registrare una sequenza

1. Accendi **REGISTRA** nel plugin.
2. Sulla traccia: tasto destro sul pulsante di record > **Record: output > Record: output (MIDI)**,
   e arma la traccia.
3. Metti la canzone su un'altra traccia, premi Record in REAPER e fai la regia dal mixer.
4. Ogni stacco diventa una nota corta sulla traccia, allineata alla musica.
5. Spegni REGISTRA a fine registrazione.

Poi apri l'item nel MIDI editor: sposti le note per correggere gli stacchi, le agganci alla
griglia dei beat, copi i ritornelli. Il "Nome" delle righe non compare nel piano roll: per vedere
"CAM 1" al posto di "C2" puoi rinominare le note nel MIDI editor di REAPER.

## Mandare in onda

1. Disarma la traccia (niente record).
2. Accendi **INVIA**. Con **PROVA** acceso il plugin scrive nel log cosa manderebbe, senza mandare
   nulla: usalo per le prove a secco.
3. Play.

**INVIA spento = non parte niente**, anche se la traccia suona. E' anche un parametro
automatizzabile: puoi disattivare l'automazione in certe parti del brano con una curva.

### Tempi

- **Ritardo**: ritarda tutti i comandi (0-500 ms).
- Per **anticiparli** (compensare il ritardo del mixer) usa il playback offset della traccia:
  pulsante *Route* della traccia > *Track playback offset*, valore negativo in ms.
  25 fps = 40 ms per frame.

Attenzione: anche cliccare le note nel MIDI editor le "suona" e quindi, con INVIA acceso,
manda il comando al mixer. Comodo per provare, pericoloso in onda.

## Sviluppo

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/CueOSCTests            # test senza REAPER (OSC su loopback)
```

Ogni push su `main` compila Windows e macOS su GitHub Actions, esegue i test e controlla il
plugin con pluginval. Un tag `v*` (es. `v0.1.0`) crea una Release con gli zip.

Basato su [JUCE](https://juce.com) 8 (licenza AGPLv3 / JUCE Starter per uso personale).
