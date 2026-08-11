# ESP32 AM Transmitter / Trasmettitore AM con ESP32

> Experimental direct-digital AM generator for ESP32-WROOM-32.  
> Generatore AM digitale sperimentale per ESP32-WROOM-32.

## Italiano

### Descrizione

Questo progetto usa un **ESP32-WROOM-32** e la periferica **I2S0 TX** per
generare direttamente su **GPIO27** una portante AM centrata a **1440 kHz**.
La configurazione verificata sperimentalmente e' volutamente mantenuta fissa:

- Arduino-ESP32 core **3.3.11**;
- I2S0 in trasmissione;
- `I2S_STD_CLK_DEFAULT_CONFIG(90000)`;
- parole a 16 bit, mono, formato MSB;
- uscita dati su GPIO27;
- il pattern continuo `0xAAAA` produce la portante a 1440 kHz.

L'audio analogico entra su **GPIO34**, polarizzato attorno a meta'
alimentazione. L'ADC1 viene letto in modalita' one-shot a 10 ksample/s. Una
coda cronologica separa il clock ADC dalle scritture DMA I2S: ogni campione
viene consumato nell'ordine corretto e l'inviluppo viene interpolato, evitando
campioni saltati o ripetuti quando il DMA accetta dati a raffiche.

### Caratteristiche attuali

- portante: **1440 kHz**;
- banda audio: circa **4,2 kHz** a -3 dB;
- banda RF AM: circa **8,4 kHz**;
- indice massimo di modulazione: **95%**;
- sample rate ADC: **10 kHz**;
- guadagno audio software: **8x**;
- buffer audio cronologico: 512 campioni;
- nessun RMT e nessun ADC continuous.

### Come funziona il modulatore

Il riferimento RF e' la sequenza `10 10 10 ...`, equivalente a `0xAAAA`.
Ogni coppia `10` rappresenta un ciclo di portante attivo e conserva sempre la
stessa fase. Per ridurre l'ampiezza il modulatore sopprime alcuni cicli usando
le coppie `00` e `11`, alternate per mantenere bilanciata la componente
continua. Un accumulatore Q16 distribuisce i cicli attivi in proporzione
all'inviluppo AM.

L'inviluppo normalizzato e' configurato come:

```text
0,50 +/- 0,475 * audio
```

Con audio normalizzato a +/-1, l'inviluppo varia fra 0,025 e 0,975: indice di
modulazione 0,475 / 0,50 = **95%**, senza inversione di fase.

### Collegamenti

![Schema hardware attuale](hardware/current-hardware-it.png)

Lo schema sopra documenta il cablaggio sperimentale attuale fornito durante lo
sviluppo del progetto. Due precisazioni sono essenziali:

- il nodo `AUDIO IN` deve ricevere realmente una polarizzazione di circa
  1,65 V dalla sorgente o da una rete di bias separata; il solo resistore R1
  verso massa non genera tale polarizzazione;
- L1 = 100 nH e C1 = 100 pF hanno una frequenza caratteristica di circa
  50 MHz e non costituiscono un filtro passa-basso efficace per eliminare le
  armoniche di una portante a 1,44 MHz. Prima di collegare un'antenna serve un
  filtro RF progettato e verificato per questa frequenza, oltre al rispetto
  delle norme locali.

Collegamenti minimi logici:

```text
LINE OUT --- accoppiamento/attenuazione/bias --- GPIO34 (ADC1 CH6)
GND sorgente ---------------------------------- GND ESP32
GPIO27 ---------------------------------------- uscita RF filtrata
```

Gli schemi analogici completi saranno inseriti nella cartella `hardware/`.
Non collegare un'uscita linea direttamente al GPIO34: il segnale deve essere
accoppiato in AC, attenuato se necessario e polarizzato attorno a 1,65 V,
restando sempre nell'intervallo ammesso dall'ESP32.

### Compilazione

1. Installare Arduino IDE e il pacchetto schede ESP32 versione 3.3.11.
2. Aprire `sketch_aug11a_linein.ino`.
3. Selezionare una scheda ESP32 generica compatibile con ESP32-WROOM-32.
4. Compilare e caricare lo sketch.
5. Aprire il monitor seriale a 115200 baud per la diagnostica.

Le righe `DIAG` mostrano campioni ADC/s, parole RF/s, estremi ADC, clipping,
campioni persi dalla coda (`DROP`) e inviluppo. In condizioni normali sono
attesi circa 10000 campioni/s, 90000 parole/s e `DROP=0/s`.

### Note sui tentativi precedenti

- RMT dinamico: discontinuita' e spurie durante gli aggiornamenti.
- ADC continuous insieme a I2S0: perdita della portante sull'ESP32 provato.
- Campionamento legato direttamente alle chiamate `i2s_channel_write`: media
  corretta, ma campioni ripetuti/saltati a causa delle raffiche DMA.
- Attesa attiva su un secondo core: watchdog reset.
- Coda cronologica ADC -> modulatore: soluzione stabile verificata.

### Sicurezza e conformita'

GPIO27 non e' un'uscita RF pronta per un'antenna. Usare carico fittizio,
accoppiamento molto debole, attenuazione e filtraggio passa-banda/passa-basso
adeguati. Non collegare direttamente un'antenna. Le emissioni radio sono
soggette alle norme locali: eseguire le prove in modo schermato e senza
irradiazione non autorizzata.

---

## English

### Overview

This project uses an **ESP32-WROOM-32** and the **I2S0 TX** peripheral to
generate an AM carrier centered at **1440 kHz** directly on **GPIO27**. The
experimentally verified configuration is intentionally kept unchanged:

- Arduino-ESP32 core **3.3.11**;
- I2S0 transmit mode;
- `I2S_STD_CLK_DEFAULT_CONFIG(90000)`;
- 16-bit mono MSB format;
- data output on GPIO27;
- a continuous `0xAAAA` pattern produces the 1440 kHz carrier.

Analog audio is applied to **GPIO34** and biased near half the supply voltage.
ADC1 is read in one-shot mode at 10 ksample/s. A chronological queue decouples
the ADC clock from bursty I2S DMA writes, preserving every sample in the
correct order. The RF envelope is interpolated between audio samples.

### Current specifications

- carrier: **1440 kHz**;
- audio bandwidth: approximately **4.2 kHz** at -3 dB;
- total AM RF bandwidth: approximately **8.4 kHz**;
- maximum modulation index: **95%**;
- ADC sample rate: **10 kHz**;
- software audio gain: **8x**;
- chronological audio queue: 512 samples;
- no RMT and no continuous-mode ADC.

### Modulator principle

The RF reference is the sequence `10 10 10 ...`, equivalent to `0xAAAA`.
Every `10` pair is an active carrier cycle with a fixed phase. Amplitude is
reduced by suppressing selected cycles with `00` and `11` pairs. The two quiet
states alternate to balance DC. A Q16 density accumulator distributes active
cycles in proportion to the requested AM envelope.

The normalized envelope is:

```text
0.50 +/- 0.475 * audio
```

For normalized audio in the range +/-1, the envelope remains between 0.025
and 0.975. This gives a **95%** maximum modulation index without phase
reversal.

### Connections

![Current hardware diagram](hardware/current-hardware-en.png)

The diagram above documents the current experimental wiring supplied during
project development. Two details are important:

- the `AUDIO IN` node must actually receive an approximately 1.65 V bias from
  the source or from a separate bias network; R1 to ground alone does not
  generate that bias;
- L1 = 100 nH and C1 = 100 pF have a characteristic frequency around 50 MHz
  and are not an effective low-pass filter for removing harmonics from a
  1.44 MHz carrier. A properly designed and verified RF filter is required
  before any antenna connection, together with compliance with local law.

Minimum logical connections:

```text
LINE OUT --- coupling/attenuation/bias network --- GPIO34 (ADC1 CH6)
Source GND --------------------------------------- ESP32 GND
GPIO27 ------------------------------------------- filtered RF output
```

Complete analog diagrams will be placed in `hardware/`. Do not connect a line
output directly to GPIO34. AC-couple, attenuate when required, and bias the
signal near 1.65 V while keeping it inside the ESP32 input limits.

### Build and upload

1. Install Arduino IDE and ESP32 board package 3.3.11.
2. Open `sketch_aug11a_linein.ino`.
3. Select a generic board compatible with ESP32-WROOM-32.
4. Compile and upload.
5. Open the serial monitor at 115200 baud for diagnostics.

`DIAG` lines report ADC samples/s, RF words/s, ADC range, clipping, queue drops
and the current envelope. Normal operation is approximately 10000 samples/s,
90000 words/s and `DROP=0/s`.

### Development findings

- Dynamic RMT updates introduced discontinuities and spurs.
- Continuous ADC operation conflicted with I2S0 on the tested ESP32 and made
  the carrier disappear.
- Sampling based directly on `i2s_channel_write` calls caused repeated and
  skipped samples because DMA accepts data in bursts.
- A busy-wait audio task triggered the watchdog.
- A chronological ADC-to-modulator queue produced stable, clean audio.

### Safety and regulatory notice

GPIO27 is not an antenna-ready RF output. Use a dummy load, very weak coupling,
proper attenuation, and suitable RF filtering. Do not connect an antenna
directly. Radio emissions are regulated: perform tests in a shielded setup and
do not radiate without the required authorization.

## Status

Working experimental prototype tested on ESP32-WROOM-32 with Arduino-ESP32
3.3.11. Contributions and hardware measurements are welcome.

