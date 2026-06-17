# Design Rationale: 137 Days, 120 Seconds, 60 Seconds

*The philosophy behind the three time constants of Elektron Net.*

---

## Introduction

Three numbers shape almost everything about how Elektron Net feels, behaves,
and stays secure:

- **60 seconds** — the heartbeat of the chain (block interval)
- **120 seconds** — the open hand for small miners (Stoic Awakening window)
- **137 days** — the limit of memory (mandatory prune depth, 197,280 blocks)

Each number on its own looks like a tuning parameter. Together they form a
coherent design that trades the archival, industrial Bitcoin of today for
something closer to Satoshi's original vision: a peer-to-peer cash system
secured by many small, individual participants, with privacy built in by the
simple act of forgetting.

This document explains the *why*. The *how* lives in
[`BITCOIN_CORE_DIFF.md`](BITCOIN_CORE_DIFF.md), the *philosophy* in
[`WHITEPAPER.md`](../WHITEPAPER.md), and the *audit* in
[`AUDIT_PRUNING_SNAPSHOT.md`](AUDIT_PRUNING_SNAPSHOT.md).

---

## 60 Seconds — The Heartbeat

Bitcoin targets a block every 600 seconds (10 minutes). Elektron Net targets
one every 60. Code reference: `nPowTargetSpacing = 60` in
`src/kernel/chainparams.cpp`.

### Why faster

A 10-minute block was a reasonable choice for a system that had to prove
itself in 2009. Today, when people pay for coffee with their phones, waiting
ten minutes for the first confirmation is a museum piece. At 60 seconds:

- First confirmation in a minute, six confirmations in six.
- 1,440 blocks per day (vs. Bitcoin's 144).
- The chain feels alive — block explorers update like a pulse.

### What stays the same

- **Total supply** is unchanged: `MAX_MONEY = 21,000,000 * COIN`.
- **Halving cadence** is rescaled to `nSubsidyHalvingInterval = 2,102,400`
  — same ~4 calendar years, just expressed in more blocks.
- **Smallest unit** is still 10⁸ per coin (called *lepton* here, instead of
  satoshi).

### The hidden consequence

Faster blocks create more frequent mining opportunities. That alone wouldn't
matter — Litecoin and Dogecoin already proved that. But combined with the
other two numbers, 60 seconds becomes the foundation of something bigger.

---

## 137 Days — The Memory

Constant: `MANDATORY_PRUNE_DEPTH = 197280` in `src/validation.h:79`.
At 60-second blocks, that's almost exactly 137 days.

### Why a hard limit, and not "optional pruning"

Bitcoin lets the user choose: keep the full archive, or prune to save space.
Elektron Net makes the choice for everyone: **all nodes prune to the last
137 days**. There is no full-archive mode. The user's `-prune=<GB>` setting
is silently ignored (`src/node/blockmanager_args.cpp`).

This is not a storage optimization. It is a **statement about what a public
ledger should be**.

#### The "Pocket" model

A wallet should be like a pocket — light enough to carry, holding what's
useful, forgetting what isn't. The right to be forgotten is not an
afterthought; it's the first principle. If the network cannot serve old
transactions, no third party can dig them up either. Forensic
de-anonymization tools depend on archival history. Without it, the chain
itself protects its users.

#### What is preserved, what is lost

| Preserved forever | Lost after 137 days |
|---|---|
| The current balance of every address | When a UTXO was received |
| Spendability of every coin | From whom it was received |
| The UTXO commitment of every checkpoint | The full transaction graph |
| Proof-of-work chain of headers | Per-transaction forensic trail |

Crucially: **balances are not lost when blocks are pruned.** The UTXO set
in `chainstate/` is a *cumulative* database. An unspent output created in
block 100 stays in the UTXO set until it is spent, even if block 100 itself
was deleted a year ago. The snapshot mechanism (see below) captures this
complete state at every checkpoint, so newcomers can join the network with
full and current knowledge of every balance.

#### Implicit finality

A 51% attacker on Bitcoin can, in theory, rewrite an arbitrarily deep
section of history given enough hashrate and time. On Elektron Net, after
137 days the blocks are simply gone. A reorg that deep is *physically
impossible* because no honest node has the data to participate. The chain
becomes finalized by erasure, not by checkpoint consensus.

---

## 120 Seconds — The Open Hand

If 60 seconds is the heartbeat and 137 days is the memory, 120 seconds is
the gesture of welcome.

The rule (Stoic Awakening, mainnet, active from height 1):
**if more than 120 seconds have passed since the last block, the next block
may be mined at the minimum difficulty.**

Code: `MinDifficultyActivationHeight = 1` in `src/kernel/chainparams.cpp`,
implementation in `src/pow.cpp`.

### What it actually does

It is *one* block of relief, then the timer resets. The next block after
that is back to normal difficulty unless 120 seconds pass again. This is
not a sustained low-difficulty period — it is a single open window, and it
only opens when the network slows down.

### Why it is not a security hole

A 51% attacker cannot use this to "chain-rush" — every min-difficulty block
needs another 120-second gap, which means waiting in real time. The
chainwork gained is marginal.

But for a Bitaxe in someone's kitchen, those 120 seconds are everything.

### The hand-up for small miners

Modern small mining devices — Bitaxe, NerdMiner, hobby ASICs in the
500 GH/s to 5 TH/s range — have essentially zero chance of finding a
Bitcoin block. The math is brutal: a single device against a global
exahash network is a lottery with no winners.

On Elektron Net, when the 120-second window opens, the difficulty drops to
`powLimit`. A solo Bitaxe can realistically find that block. Not often —
but realistically. The lottery has winners now.

### The barbell

This produces a deliberately asymmetric miner population:

- **Professional pools and data centres** mine the ~95% of blocks that
  appear within 120 seconds, at full difficulty. They carry the cost and
  carry the security.
- **Solo adventurers** with Bitaxes, NerdMiners, and basement rigs catch
  the blocks that fall through the 120-second window.

Nobody is displaced. Professionals are not undercut — they still win the
vast majority of blocks and the steady income. Hobbyists are not excluded
— they have a real, non-zero chance every time the network breathes.

### The energy ceiling

Bitcoin's energy use grows because difficulty grows because hashrate
grows. The arms race has no built-in brake. On Elektron Net, if difficulty
rises so high that even professional miners regularly take more than 120
seconds per block, the overflow falls to solo miners at minimum
difficulty. This acts as an **implicit safety valve** against unbounded
hashrate escalation. Over long horizons, the system self-regulates toward
a hashrate level where the arms race is no longer economically attractive.

This is not proof-of-work versus proof-of-stake. It is proof-of-work
**with self-limitation built in.**

---

## The Synthesis — Why the Three Numbers Need Each Other

Each constant in isolation is interesting. Together they form an
architecture.

- **60 seconds** makes mining frequent enough that solo participation is
  meaningful. At 600-second blocks, a Bitaxe owner would wait years
  between meaningful chances even with min-difficulty windows. At 60
  seconds, the chances arrive constantly.

- **120 seconds** opens the door for small miners *without* taking
  anything from large miners. The big operators still win when blocks
  come fast (most of the time). The small operators win when blocks come
  slow.

- **137 days** keeps the chain light enough that a Raspberry Pi, a phone,
  or a low-end laptop can be a full node. Light chains attract more
  nodes. More nodes mean more verification, more diversity, more
  resilience.

- **Per-block UTXO attestation** stitches it all together
  cryptographically. Every block commits, in its coinbase, to the hash
  of the UTXO set after that block. A new node bootstrapping from a
  snapshot does not have to trust the snapshot's author — it can verify
  the snapshot against the on-chain commitment, which is secured by all
  the proof-of-work backing the chain. (Code: `ValidateUTXOCheckpoint`,
  `src/validation.cpp:2920`; `WriteAutomaticSnapshot`,
  `src/validation.cpp:2439`.)

Pull on any one of these threads and the others come along.

---

## 51% Attacks — What This Architecture Changes

A 51% attack is still possible in principle — proof-of-work makes no other
promise. But the *shape* of the threat changes in ways that matter.

### What is harder

- **Renting hashrate.** Solo miners are not on NiceHash. They are in
  kitchens, garages, workshops. An attacker cannot simply purchase enough
  hashrate to overpower the chain — much of the honest hashrate is
  geographically and operationally distributed in places no marketplace
  reaches.

- **Geographic concentration.** Industrial mining tends toward a handful
  of regulatory regimes and a handful of cheap-electricity locations.
  Hobbyist mining lives everywhere — across power grids, ISPs, and
  jurisdictions. Confiscating or coercing that hashrate would mean
  knocking on a million doors.

- **Deep reorgs.** Past 137 days, no honest node holds the data needed to
  validate an alternative chain. Reorgs are bounded by erasure.

- **Snapshot poisoning.** A malicious peer cannot feed a new node a
  fabricated UTXO set, because the snapshot must hash-match the
  on-chain coinbase attestation. To forge a snapshot, the attacker would
  have to re-mine the checkpoint block and every block after it.

### What remains the same

- Short-range double-spends within the recent window are as feasible as
  on any proof-of-work chain.
- A young chain with low total hashrate is vulnerable, regardless of
  pruning model. This is a launch-phase risk, not a design flaw.

### What is structurally improved

A 51% adversary on Bitcoin must out-spend a few large pools. A 51%
adversary on a mature Elektron Net must out-spend a global swarm of
individuals who, by selection, are running their devices for reasons
other than pure profit. Ideologically motivated hashrate is sticky
hashrate. It is the hardest kind of hashrate to buy.

---

## The Gold-Panner

There is an older image that captures what this design is trying to do.

Before industrial mining came to the American West, the early gold rushes
were prospectors with pans, sluices, and stubborn optimism. The Colorado
rivers were full of strangers who built small towns, traded news, watched
each other's claims, and occasionally — not often, but occasionally —
found a nugget. The economy of those rivers was not concentrated. It
spread itself across the continent precisely because the work was small
enough for individuals.

Industrial mining made that economy obsolete. The same has happened to
Bitcoin: the romance of running a node and finding a block is now mostly
just romance.

Elektron Net puts the panners back at the river.

A Bitaxe under a desk is a pan in a stream. Most days it finds nothing.
But the stream is moving, the 120-second window keeps opening, and every
so often someone in Stuttgart or São Paulo or Sapporo will check their
device in the morning and find a block.

**Everyone, eventually, finds a nugget.**

That is not just a feature. It is the security model.

---

---

# Design-Begründung: 137 Tage, 120 Sekunden, 60 Sekunden

*Die Philosophie hinter den drei Zeitkonstanten von Elektron Net.*

---

## Einleitung

Drei Zahlen prägen fast alles an Elektron Net — wie es sich anfühlt, wie es
sich verhält, wie es sich sichert:

- **60 Sekunden** — der Herzschlag der Kette (Block-Intervall)
- **120 Sekunden** — die offene Hand für kleine Miner (Stoic Awakening)
- **137 Tage** — die Grenze des Gedächtnisses (197.280 Blöcke Prune-Tiefe)

Jede Zahl für sich sieht aus wie ein Tuning-Parameter. Zusammen ergeben sie
ein stimmiges Design, das das archivierende, industrielle Bitcoin von heute
gegen etwas tauscht, das näher an Satoshis ursprünglicher Vision liegt: ein
Peer-to-Peer-Bargeldsystem, gesichert von vielen kleinen, individuellen
Teilnehmern, mit Privatsphäre durch das schlichte Vergessen.

Dieses Dokument erklärt das *Warum*. Das *Wie* steht in
[`BITCOIN_CORE_DIFF.md`](BITCOIN_CORE_DIFF.md), die *Philosophie* im
[`WHITEPAPER.md`](../WHITEPAPER.md), das *Audit* in
[`AUDIT_PRUNING_SNAPSHOT.md`](AUDIT_PRUNING_SNAPSHOT.md).

---

## 60 Sekunden — der Herzschlag

Bitcoin zielt auf einen Block alle 600 Sekunden (10 Minuten). Elektron Net
auf einen alle 60. Code: `nPowTargetSpacing = 60` in
`src/kernel/chainparams.cpp`.

### Warum schneller

Zehn Minuten pro Block waren 2009 eine vernünftige Wahl für ein System, das
sich erst beweisen musste. Heute, wo Menschen mit dem Telefon den Kaffee
bezahlen, sind zehn Minuten Wartezeit auf die erste Bestätigung ein
Museumsstück. Bei 60 Sekunden:

- Erste Bestätigung in einer Minute, sechs Bestätigungen in sechs.
- 1.440 Blöcke pro Tag (statt 144 bei Bitcoin).
- Die Kette fühlt sich lebendig an — Block-Explorer aktualisieren wie ein Puls.

### Was gleich bleibt

- **Gesamt-Supply** unverändert: `MAX_MONEY = 21.000.000 * COIN`.
- **Halving-Takt** umskaliert auf `nSubsidyHalvingInterval = 2.102.400` —
  dieselben ~4 Kalenderjahre, nur in mehr Blöcken ausgedrückt.
- **Kleinste Einheit** weiterhin 10⁸ pro Coin (hier *Lepton* statt Satoshi).

### Die versteckte Konsequenz

Schnellere Blöcke schaffen häufigere Mining-Gelegenheiten. Das allein würde
nicht reichen — Litecoin und Dogecoin haben das längst bewiesen.
Kombiniert mit den anderen beiden Zahlen wird 60 Sekunden aber zur
Grundlage von etwas Größerem.

---

## 137 Tage — das Gedächtnis

Konstante: `MANDATORY_PRUNE_DEPTH = 197280` in `src/validation.h:79`.
Bei 60-Sekunden-Blöcken ergibt das fast genau 137 Tage.

### Warum eine harte Grenze und nicht "optionales Pruning"

Bitcoin überlässt dem Nutzer die Wahl: volle Archive behalten oder prunen.
Elektron Net entscheidet für alle: **jeder Node prunt auf die letzten 137
Tage**. Es gibt keinen Voll-Archiv-Modus. Die Nutzer-Einstellung
`-prune=<GB>` wird stillschweigend ignoriert
(`src/node/blockmanager_args.cpp`).

Das ist keine Speicher-Optimierung. Das ist eine **Aussage darüber, was ein
öffentliches Hauptbuch sein sollte**.

#### Das "Pocket"-Modell

Eine Wallet sollte wie eine Hosentasche sein — leicht genug zum Tragen,
hält das Nützliche, vergisst den Rest. Das Recht auf Vergessen ist kein
Nachtrag, sondern erstes Prinzip. Wenn das Netz alte Transaktionen nicht
mehr ausliefern kann, kann auch kein Dritter sie wieder ausgraben. Tools
zur forensischen De-Anonymisierung leben von der Archiv-Historie. Ohne
sie schützt die Kette ihre Nutzer von selbst.

#### Was bleibt, was geht

| Bleibt für immer erhalten | Verloren nach 137 Tagen |
|---|---|
| Aktueller Bestand jeder Adresse | Wann ein UTXO empfangen wurde |
| Ausgebbarkeit jeder Münze | Von wem empfangen |
| UTXO-Commitment jedes Checkpoints | Vollständiger Transaktionsgraph |
| PoW-Kette der Header | Forensische Tx-Spur |

Entscheidend: **Bestände gehen beim Pruning nicht verloren.** Das UTXO-Set
in `chainstate/` ist eine *kumulative* Datenbank. Ein unverbrauchter Output
aus Block 100 bleibt im UTXO-Set, bis er ausgegeben wird — auch wenn Block
100 selbst längst gelöscht ist. Der Snapshot-Mechanismus (siehe unten)
sichert diesen vollständigen Zustand an jedem Checkpoint. Neue Nodes
betreten das Netz mit vollständigem Wissen über jeden aktuellen Kontostand.

#### Implizite Finalität

Ein 51%-Angreifer auf Bitcoin kann theoretisch beliebig tief in die
Geschichte zurückschreiben, wenn er genug Hashrate und Zeit hat. Auf
Elektron Net sind die Blöcke nach 137 Tagen einfach weg. Eine Reorg in
diese Tiefe ist *physisch unmöglich*, weil kein ehrlicher Node die Daten
mehr hat, um mitzumachen. Die Kette wird durch Vergessen finalisiert, nicht
durch Checkpoint-Konsens.

---

## 120 Sekunden — die offene Hand

Wenn 60 Sekunden der Herzschlag und 137 Tage das Gedächtnis sind, dann sind
120 Sekunden die Geste des Willkommens.

Die Regel (Stoic Awakening, Mainnet, ab Höhe 1 aktiv):
**Wenn seit dem letzten Block mehr als 120 Sekunden vergangen sind, darf
der nächste Block mit Minimum-Schwierigkeit gemined werden.**

Code: `MinDifficultyActivationHeight = 1` in
`src/kernel/chainparams.cpp`, Implementierung in `src/pow.cpp`.

### Was tatsächlich passiert

Es ist *ein* Block Entlastung, dann startet der Timer neu. Der Block danach
hat wieder normale Schwierigkeit, außer es vergehen erneut 120 Sekunden.
Das ist kein dauerhaftes Low-Difficulty-Fenster — es ist ein einzelnes
offenes Türchen, und es öffnet sich nur, wenn das Netz langsamer wird.

### Warum das keine Sicherheitslücke ist

Ein 51%-Angreifer kann das nicht für eine "Chain-Rush" nutzen — jeder Min-
Difficulty-Block braucht wieder eine 120-Sekunden-Lücke, also echte
Wartezeit. Der gewonnene Chainwork-Vorsprung ist marginal.

Aber für einen Bitaxe in einer Küche sind diese 120 Sekunden alles.

### Die ausgestreckte Hand für kleine Miner

Moderne Solo-Mining-Geräte — Bitaxe, NerdMiner, Hobby-ASICs im Bereich
500 GH/s bis 5 TH/s — haben auf Bitcoin praktisch null Chance, einen Block
zu finden. Die Mathematik ist brutal: ein einzelnes Gerät gegen ein
globales Exahash-Netz ist eine Lotterie ohne Gewinner.

Auf Elektron Net fällt die Schwierigkeit beim Öffnen des 120-Sekunden-
Fensters auf `powLimit`. Ein Solo-Bitaxe kann diesen Block realistisch
finden. Nicht oft — aber realistisch. Die Lotterie hat Gewinner.

### Die Hantel

Daraus entsteht eine bewusst asymmetrische Miner-Population:

- **Professionelle Pools und Rechenzentren** minen die ~95% der Blöcke,
  die innerhalb von 120 Sekunden gefunden werden, mit voller Schwierigkeit.
  Sie tragen die Kosten und die Sicherheit.
- **Solo-Abenteurer** mit Bitaxes, NerdMinern und Bastelkeller-Rigs fangen
  die Blöcke, die durch das 120-Sekunden-Fenster fallen.

Niemand wird verdrängt. Profis werden nicht unterboten — sie gewinnen
weiterhin den Großteil der Blöcke und das stetige Einkommen. Hobbyisten
sind nicht ausgeschlossen — sie haben jedes Mal eine reale, nicht-triviale
Chance, wenn das Netz atmet.

### Die Energie-Deckelung

Bitcoins Energieverbrauch wächst, weil die Schwierigkeit wächst, weil die
Hashrate wächst. Das Wettrüsten hat keine eingebaute Bremse. Auf Elektron
Net gilt: Wird die Schwierigkeit so hoch, dass selbst professionelle Miner
regelmäßig länger als 120 Sekunden brauchen, fällt der Überlauf an Solo-
Miner mit Minimum-Schwierigkeit. Das wirkt wie ein **implizites
Sicherheitsventil** gegen unbegrenztes Hashrate-Wettrüsten. Über lange
Zeiträume reguliert sich das System auf ein Niveau, wo das Wettrüsten
ökonomisch unattraktiv wird.

Das ist nicht Proof-of-Work gegen Proof-of-Stake. Das ist Proof-of-Work
**mit eingebauter Selbstbegrenzung**.

---

## Die Synthese — warum die drei Zahlen einander brauchen

Jede Konstante für sich ist interessant. Zusammen ergeben sie eine
Architektur.

- **60 Sekunden** machen Mining häufig genug, dass Solo-Teilnahme sinnvoll
  ist. Bei 600-Sekunden-Blöcken würde ein Bitaxe-Besitzer Jahre auf
  bedeutsame Chancen warten, selbst mit Min-Difficulty-Fenstern. Bei 60
  Sekunden kommen die Chancen ständig.

- **120 Sekunden** öffnen die Tür für kleine Miner, *ohne* den großen
  etwas wegzunehmen. Die großen Operatoren gewinnen weiter, wenn Blöcke
  schnell kommen (meistens). Die kleinen gewinnen, wenn es langsam wird.

- **137 Tage** halten die Kette leicht genug, dass ein Raspberry Pi, ein
  Telefon oder ein günstiger Laptop ein voller Node sein kann. Leichte
  Ketten ziehen mehr Nodes an. Mehr Nodes bedeuten mehr Verifikation, mehr
  Vielfalt, mehr Widerstandsfähigkeit.

- **Per-Block UTXO-Attestation** näht alles kryptographisch zusammen.
  Jeder Block commited in seinem Coinbase den Hash des UTXO-Sets nach
  diesem Block. Ein neuer Node, der per Snapshot bootstrappt, muss dem
  Snapshot-Autor nicht vertrauen — er kann den Snapshot gegen das
  On-Chain-Commitment verifizieren, das durch die gesamte Proof-of-Work
  der Kette gesichert ist. (Code: `ValidateUTXOCheckpoint`,
  `src/validation.cpp:2920`; `WriteAutomaticSnapshot`,
  `src/validation.cpp:2439`.)

Zieh an einem dieser Fäden und die anderen kommen mit.

---

## 51%-Angriffe — was diese Architektur ändert

Ein 51%-Angriff bleibt prinzipiell möglich — Proof-of-Work verspricht
nichts anderes. Aber die *Gestalt* der Bedrohung verschiebt sich auf eine
Weise, die zählt.

### Was schwerer wird

- **Hashrate mieten.** Solo-Miner sind nicht auf NiceHash. Sie sind in
  Küchen, Garagen, Werkstätten. Ein Angreifer kann nicht einfach genug
  Hashrate kaufen, um die Kette zu überwältigen — ein großer Teil der
  ehrlichen Hashrate ist geografisch und betrieblich verteilt, wo kein
  Marktplatz hingreift.

- **Geografische Konzentration.** Industrielles Mining strebt zu einer
  Handvoll Regulierungsräume und einer Handvoll Strom-Schnäppchen.
  Hobby-Mining lebt überall — über Stromnetze, ISPs und Jurisdiktionen
  hinweg. Diese Hashrate zu beschlagnahmen oder zu erpressen hieße, an
  einer Million Türen zu klopfen.

- **Tiefe Reorgs.** Jenseits von 137 Tagen hat kein ehrlicher Node die
  Daten, um eine alternative Kette zu validieren. Reorgs sind durch
  Vergessen begrenzt.

- **Snapshot-Vergiftung.** Ein bösartiger Peer kann einem neuen Node kein
  gefälschtes UTXO-Set unterschieben, weil der Snapshot zum
  On-Chain-Commitment im Coinbase passen muss. Um einen Snapshot zu
  fälschen, müsste der Angreifer den Checkpoint-Block und alle Folgeblöcke
  neu minen.

### Was gleich bleibt

- Kurzreichweitige Double-Spends im jüngsten Fenster sind so machbar wie
  auf jeder PoW-Kette.
- Eine junge Kette mit niedriger Gesamt-Hashrate ist anfällig, unabhängig
  vom Pruning-Modell. Das ist ein Launch-Risiko, kein Design-Fehler.

### Was strukturell besser wird

Ein 51%-Angreifer auf Bitcoin muss ein paar große Pools überbieten. Ein
51%-Angreifer auf ein reifes Elektron Net muss einen weltweiten Schwarm
von Individuen überbieten, die ihre Geräte aus anderen Gründen als reinem
Profit laufen lassen. Ideologisch motivierte Hashrate ist klebrige
Hashrate. Sie ist am schwersten zu kaufen.

---

## Der Goldwäscher

Es gibt ein älteres Bild, das einfängt, worum es bei diesem Design geht.

Bevor das industrielle Mining in den amerikanischen Westen kam, waren die
frühen Goldräusche eine Sache von Goldwäschern mit Pfannen, Schleusen und
sturer Hoffnung. Die Flüsse Colorados waren voll von Fremden, die kleine
Städte bauten, Nachrichten austauschten, einander beim Claim
beobachteten — und gelegentlich, nicht oft, aber gelegentlich, ein Nugget
fanden. Die Wirtschaft dieser Flüsse war nicht konzentriert. Sie verteilte
sich über den Kontinent, gerade weil die Arbeit klein genug für Einzelne
war.

Das industrielle Mining hat diese Wirtschaft obsolet gemacht. Mit Bitcoin
ist dasselbe passiert: die Romantik, einen Node zu betreiben und einen
Block zu finden, ist heute meist nur noch Romantik.

Elektron Net setzt die Goldwäscher zurück an den Fluss.

Ein Bitaxe unter dem Schreibtisch ist eine Pfanne im Bach. Die meisten
Tage findet sie nichts. Aber der Bach fließt, das 120-Sekunden-Fenster
öffnet sich immer wieder, und ab und zu wird jemand in Stuttgart oder São
Paulo oder Sapporo morgens sein Gerät prüfen und einen Block finden.

**Jeder findet, irgendwann, einen Nugget.**

Das ist nicht nur ein Feature. Das ist das Sicherheitsmodell.
