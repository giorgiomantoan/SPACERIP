# SPACERIP

**Un viaggio interstellare che segue il ritmo della musica — visualizzazione gratuita per VLC.**

![SPACERIP](screenshot.png)

SPACERIP è un modulo (*plugin*) nativo per VLC 3.x. Mentre ascolti, ti incammini
alla velocità della luce attraverso un campo di stelle: i battiti fanno
accelerare il moto in avanti (warp) e fanno brillare un flash azzurro, i bassi
fanno respirare le nebule colorate, le melodie disegnano le scie delle stelle e
gli anelli dell'iperspazio si espandono a ritmo.

Analisi audio reale (FFT 2048 + rilevamento dei battiti sui bassi), rendering
software ottimizzato SSE2: **~30 fps anche a risoluzione 4K** (testato a
3840×1080 pieno schermo).

## Installazione

### VLC della Snap Store (consigliato)

1. Scarica l'archivio `SPACERIP-1.0.zip` dalla pagina [Releases](../../releases)
   e **svuotalo** in una cartella, ad esempio `~/Programmi/SPACERIP`.
2. Avvia la musica con SPACERIP già attivo:

   ```sh
   ~/Programmi/SPACERIP/spacerip-vlc.sh "la tua musica.mp3"
   ```

   Oppure avvia normalmente VLC e seleziona SPACERIP nelle impostazioni (vedi
   sotto): il launcher è solo un modo rapido.

### VLC installato dal pacchetto (deb/rpm)

```sh
./install.sh          # installa il plugin (chiede sudo)
```

Poi in VLC: **Audio → Effetti e filtri → Effetti audio → Visualizzazione**
scegli **SPACERIP**, oppure da riga di comando:

```sh
vlc --audio-visual=spacerip
```

## Opzioni

In **Preferenze → Audio → Effetti e filtri → Impostazioni Sottostanti**
(oppure da riga di comando con `--spacerip-…`):

| Opzione                   | Predefinito | Descrizione |
|---------------------------|-------------|-------------|
| `spacerip-width`/`height` | `0`/`0`     | 0 = risoluzione **nativa del monitor** (massima definizione) |
| `spacerip-fullscreen`     | off         | Apre il video a **tutto schermo** |
| `spacerip-fps`            | 30          | 10–60 frame al secondo |
| `spacerip-stars`          | 0           | Numero di stelle (0 = automatico secondo la risoluzione) |
| `spacerip-sensitivity`    | 1.0         | Quanto la musica guida il viaggio (0.1–5.0) |
| `spacerip-spread`         | 1.0         | Ampiezza del campo stellare (0.4–2.5) |
| `spacerip-delay`          | 80 ms       | Ritardo di presentazione: alza se i visuali sembrano in ritardo sul ritmo |

Esempio a tutto schermo in risoluzione nativa:

```sh
spacerip-vlc.sh --spacerip-fullscreen "album intero/"
```

## Prestazioni

* Background (nebule + bagliore + vignettatura) renderizzato in **SSE2** a metà
  risoluzione e upscaleizzato — ~20 ms a 3840×1080.
* Analisi FFT ritardata in modo da seguire il suono *mentre viene emesso*:
  il fotogramma arriva al vout esattamente al suo timestamp di presentazione,
  quindi la coda video resta vuota e il frame rate resta fisso a 30 fps
  indipendentemente dalla latenza della scheda audio.
* Supporta sia il nucleo VLC ufficiale (`vlc_entry__3_0_0f`) sia le build
  Debian/Ubuntu con ABI *t64* (`vlc_entry__3_0_0ft64`).

## Compilare da sorgente

```sh
# serve lo sorgente di VLC 3.x (include/ + config.h) e gcc
VLC_SRC=/percorso/vlc-3.0.24 ./build.sh
# produce dist/libspacerip_plugin.so
```

## Licenza

GPL-2.0-or-later — vedi [LICENSE](LICENSE). Gratuito: condividilo, modificalo,
ridistribuiscilo.

---

# SPACERIP (English)

**An interstellar journey that follows the rhythm of the music — a free
visualization for VLC.**

SPACERIP is a native module (*plugin*) for VLC 3.x. While you listen you travel
at light speed through a star field: beats push the warp speed forward and fire
a cool blue flash, bass makes the colored nebulae breathe, melodies draw the
star trails and hyperspace rings expand on the rhythm.

Real audio analysis (2048-point FFT plus bass-band beat detection) and an SSE2
optimized software renderer: **≈30 fps even at 4K** (tested fullscreen at
3840×1080).

## Install

### VLC from the Snap Store (recommended)

1. Download `SPACERIP-1.0.zip` from the [Releases](../../releases) page and
   **extract** it, e.g. into `~/Programs/SPACERIP`.
2. Start your music with SPACERIP already active:

   ```sh
   ~/Programs/SPACERIP/spacerip-vlc.sh "your music.mp3"
   ```

   (The launcher is just a shortcut: you can also start VLC normally and pick
   SPACERIP in the settings below.)

### VLC from your distribution packages (deb/rpm)

```sh
./install.sh          # installs the plugin system wide (asks for sudo)
```

Then in VLC: **Audio → Effects and Filters → Audio Effects → Visualization**
choose **SPACERIP**, or from the command line:

```sh
vlc --audio-visual=spacerip
```

## Options

In **Preferences → Audio → Effects and Filters → All settings**
(or on the command line as `--spacerip-…`):

| Option                  | Default | Description |
|-------------------------|---------|-------------|
| `spacerip-width`/`height` | `0`/`0` | 0 = **native monitor resolution** (maximum definition) |
| `spacerip-fullscreen`   | off     | Open the video **fullscreen** |
| `spacerip-fps`          | 30      | 10–60 frames per second |
| `spacerip-stars`        | 0       | Star count (0 = automatic for the resolution) |
| `spacerip-sensitivity`  | 1.0     | How strongly the music drives the journey (0.1–5.0) |
| `spacerip-spread`       | 1.0     | Star field spread (0.4–2.5) |
| `spacerip-delay`        | 80 ms   | Presentation delay: raise it if the visuals feel ahead of the beat |

## Performance

* Background (nebulae + glow + vignette) is rendered in **SSE2** at half
  resolution and upscaled — ~20 ms at 3840×1080.
* The FFT analysis is delayed by the audio buffering of the sound card, so
  every frame reaches the video output exactly at its presentation timestamp:
  the video queue stays empty and the frame rate is a rock solid 30 fps
  whatever the output latency is.
* Loads with both the official VLC core (`vlc_entry__3_0_0f`) and the
  Debian/Ubuntu *t64* ABI builds (`vlc_entry__3_0_0ft64`).

## Build from source

```sh
# needs the VLC 3.x source tree (include/ + config.h) and gcc
VLC_SRC=/path/vlc-3.0.24 ./build.sh
# produces dist/libspacerip_plugin.so
```

## License

GPL-2.0-or-later — see [LICENSE](LICENSE). Free software: share it, change it,
redistribute it.
