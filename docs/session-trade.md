# Sitzungstausch

Stand: erste abgegrenzte Fassung. Das ist **kein** Tausch über die
DS-Funkschicht und **kein** fertiger Union-Room-Tausch. Zwei Eidos-Spieler
in einer **privaten Link-Sitzung** bieten je ein Pokémon an, sehen das
Angebot der anderen Seite und bestätigen beide **dieselben Bytes**. Erst
danach schreibt jede Seite ihr eigenes Spiel.

Der Funkweg in `docs/link-play.md` bleibt unverändert. Ob zwei Spieler im
Union Room über die emulierte WM-Schicht tauschen können, ist nicht geprüft.
Dieser Tausch spricht das nicht nach.

## Was die beiden bestätigen

Jede Seite schickt den Spielrekord, den das Spiel selbst speichert: 0xDC
Bytes, Persönlichkeitswert, Prüfsumme, gemischte Blöcke, maskierte
Parteifläche. Die Prüfsumme ist die Halbwortsumme aus `poke_tool.c`. Die
Blockreihenfolge ist die 32-zeilige Tabelle aus den Bits 13 bis 17 des
Persönlichkeitswerts.

Bestätigt wird der Hash dieser Bytes und die Angebotsnummer. Ein älteres
Angebot, ein wiederholtes altes Tausch-Kennzeichen oder ein Angebot, das
sich danach ändert, löst keinen Tausch aus. Die Partylänge bleibt gleich:
der Platz wird ersetzt, es wird nichts gelöscht und danach neu eingefügt.
Ein leeres Fach, ein Rekord mit kaputter Prüfsumme, eine Art, ein Item oder
eine Attacke über der Grenze dieses Laufs, oder ein Pokémon mit Anhang
(Mail, Design nicht 0xFF) wird abgelehnt.

Die Grenzen kommen von `bw_limit_get`. Die Artgrenze dieses Ports ist noch
die DS-Tabelle, 649. Ein Datenmod, das eine höhere Artnummer braucht, kann
dieses Pokémon hier nicht tauschen, bis der Port die Arttabellen vergrößert.
Items und Attacken folgen der angehobenen Grenze, wenn ein Mod sie anhebt.

## Entwicklung

Es gibt **keine** eingebauten Entwicklungslisten. Zeilen kommen aus dem
NARC `a/0/1/3` (Archivart 0x13) im Datenverzeichnis, also aus der Datei, die
ein Datenmod im Ganzen ersetzt. Fehlt die Datei, entwickelt nichts: die Art
bleibt die angebotene, und der Tausch kann trotzdem durchgehen.

Liegt eine Tauschentwicklung an und das Spiel kann `POKE_TOOL_ChangeSpecies`
nicht rufen, wird **nicht** getauscht. Eine halbe Artänderung ohne Fähigkeit,
Spitzname und Werte ist hier kein Erfolg. Methode 6 (Tausch mit Item) nimmt
das Item weg, wenn die Spielfunktion da ist. Methode 7 bleibt das Paar aus
dem Spielcode, Karrablast (588) und Shelmet (616); das Ziel steht in der
NARC-Zeile. Everstone ist die Haltewirkung 0x40, sobald ein Aufruf sie
liefert. Diese Fassung setzt den Aufruf im laufenden Spiel nicht, weil die
Itemdaten ein Mod ändern kann und eine feste Itemnummer die falsche wäre.
Kadabra (Art 0x40) ist die Ausnahme aus `shinka_check.c`, sobald die Wirkung
bekannt ist.

Deutsche Texte in `a/0/0/2` und `a/0/0/3` ändern keine Pokémon-Bytes und
werden nicht gelesen. Blaze Black v3.2 Full ist ein Datenmod: Entwicklungen
gelten, wenn sein `a/0/1/3` das Datenverzeichnis ist. Ob jede seiner
Änderungen in die DS-Grenzen passt, sagt die Artgrenze oben, nicht eine
Annahme.

Die bestätigten Bytes sind das Angebot **vor** der Entwicklung. Die
Entwicklung ist eine lokale Folge der eigenen NARC-Datei. Zwei Spieler mit
verschiedenen Daten können deshalb verschiedene Arten erhalten. Die Zeile
der anderen Seite zeigt, was **diese** Seite daraus machen würde.

## Ansprechen im Mehrspieler

Die Figur des anderen Spielers ist ein Avatar ohne Skript
(`sdk/mod_avatar.h`). Der normale Ansprech-Knopf des Spiels startet damit
kein Gespräch: die Prüfung in `field_event_check.c` handelt nur, wenn das
Modell davor eine Zonenskript-Nummer hat. Der Avatar hat keine.

Dieselbe Taste A zählt danach noch einmal, und nur dann:

- Koop läuft (nicht die Lobby, nicht die Pause, nicht die öffentliche Luft).
- Das Feld ist ruhig: kein Ereignis, also kein NPC-Gespräch, kein Schild,
  kein Menü, keine Story, kein Kampf.
- Der Spieler schaut auf das Nachbarfeld. Die Richtung ist die des Spiels:
  0 ein Feld in -z, 1 in +z, 2 in -x, 3 in +x (`FLDMMDL_MOVE_AddDirVector`).
- Auf diesem Feld steht der gezeichnete Avatar, nicht nur eine gemerkte
  Position.

Dann geht eine Einladung nur an diesen Peer (`ti1`). Die andere Seite sieht
„Tausch annehmen?“ mit Ja und Nein (Klick oder A und B). Ablehnen, Abbruch,
Weggehen und 15 Sekunden ohne Antwort beenden die Einladung. Zwei
gleichzeitige Einladungen: die kleinere Peer-Nummer bleibt, die andere wird
abgelehnt. Eine alte Nummer wird ignoriert.

Ja öffnet den bestehenden Tausch. Angebote und die doppelte Bestätigung
bleiben. Es wird nichts getauscht, nur weil jemand Ja gesagt hat.

Auf dem Feld liegt danach die Tauschansicht, nicht nur die Einstellungszeile:
links die eigene Party, rechts das Angebot der anderen Seite, darunter Art,
Level und Item. A bietet das gewählte Fach an und bestätigt danach genau
diese Bytes. B bricht ab, solange diese Seite noch nicht speichert. Während
des Speicherns und solange der Ausgang unklar ist, bricht B nicht ab und
rollt nichts zurück. Die Karten rücken erst zusammen, wenn das Journal
geschrieben ist; der Balken dazu heißt nicht, dass gespeichert wurde.
„Beide Seiten haben gespeichert“ erscheint erst, wenn jede Seite ihr
eigenes Save gemeldet hat, und auch dann ist das kein gemeinsamer
Schreibvorgang.

Ein Bild kommt nur aus den lokalen Party-Icons (`a/0/0/7` über
`ui_mon_icon`), wenn diese Daten im laufenden Spiel liegen. Fehlt das
Icon, steht die Nummer. Diese Fassung hat die Icons nicht gegen eine ROM
geprüft und lädt keine Nintendo-Bilder herunter.

Die Zeile Einstellungen → Netzwerk → Session trade bleibt, auf Englisch,
für die private Link-Sitzung und als Weg im Koop, wenn das Feld keine
Einladung zeigt.

Koop und private Link sind zwei Sitzungsarten auf demselben Relay. Der
Avatar gibt es im Koop. Die Einladung läuft über die Koop-Nachrichten
(`eidos:port`), der Tausch danach über `eidos:trade` in derselben Sitzung.
Das ist der Koop-Weg. Er ändert nicht, wem der Spielstand gehört: der Host
hält die Dateien unter `multiplayer/games/<Name>`, der Beitretende spielt
auf `multiplayer/session.sav` und lädt sie hoch. Jede Seite schreibt ihr
eigenes Save. Das Journal und die Bestätigung der Bytes gelten weiter.
Ein gemeinsamer atomarer Schreibvorgang entsteht dadurch nicht. Ein
Koop-Tausch ist deshalb kein zweiter, sicherer Speicher.

Solange niemand eine Einladung annimmt und niemand die Tauschzeile öffnet,
liest der Koop-Rahmen die Evolutionsdatei nicht und setzt den Tauschkanal
nicht. Ein Avatar auf dem Feld allein startet das nicht. Nach einem Laden
zählt ein Avatar erst, wenn die kurze Sperre nach dem Aufräumen vorbei ist.

## Ablauf für zwei Spieler

1. Metaxy lokal starten. Kein öffentlicher Relay, kein Upload von ROMs
   oder Spielständen.
2. Zwei Eidos mit getrennten Speicherordnern (`BW_SAVE_DIR`) und getrennten
   Einstellungen. Nicht die bereits laufende, bekannte EXE überschreiben:
   diese Fassung in einem eigenen Build-Verzeichnis bauen.
3. Entweder eine **private** Link-Sitzung (`EIDOS_LINK_TEST=host:<Datei>`
   und `join:<Datei>`, nicht die öffentliche Luft) oder eine normale
   Koop-Sitzung, in der beide Figuren auf demselben Feld sichtbar sind.
4. Im Koop: vor den Avatar stellen und A. Ja auf der anderen Seite öffnet
   die Feldansicht. Sonst: Einstellungen, Netzwerk, **Session trade**.
   Die Zeile gibt es in der privaten Link-Sitzung und im laufenden Koop,
   nicht in der Lobby und nicht in der öffentlichen Luft.
5. Jede Seite ein Parteifach, dann beide **Confirm** / A, solange die
   Zeile noch dasselbe Pokémon nennt. Eine Bestätigung allein speichert
   nicht. Eine leere Party lässt sich nicht anbieten; das Fach wird dann
   nicht gelesen.
6. Gespeichert wird erst, wenn beide bestätigt haben, das Journal
   geschrieben ist und das Feld ruhig ist (`GAME_DATA_SaveStart` /
   `SaveMain`). Vorher liegt das Pokémon noch auf dem alten Platz. Die
   Ansicht darf bis dahin nicht „fertig“ sagen.
7. **Cancel** / B vor dem Speichern bewegt nichts. B während des Speicherns
   rollt nicht zurück.

Im Koop darf der Tausch-Save nicht ein zweites Mal `SaveStart` aufrufen,
weil der Host nach einem Schreibvorgang „savenow“ an den Beitretenden
schickt. Dieses zweite Starten ist genau die Zeile „saving this game (the
host saved)“. Liegt der Tausch-Save schon, wird das zweite Starten
verworfen und eine Zeile geloggt. Das ist kein Nachweis, dass ein
bestimmter Windows-Lauf damit zu Ende gekommen ist.

## Wenn die Verbindung abreißt

| Zeitpunkt | Folge |
|---|---|
| Vor dem beidseitigen Lock | Nichts bewegt sich. |
| Journal geschrieben, Party noch nicht gespeichert | Die Party bleibt. Das Journal wartet. Kommt derselbe Partner mit demselben Tausch zurück, läuft er weiter. Sonst **Acknowledge**: der Tausch wird fallengelassen, die Party bleibt. |
| Save dieser Seite läuft oder ist fertig, die andere nicht | Diese Seite rollt ein fertiges Save nicht zurück. Das Journal bleibt ungelöst. **Acknowledge** schreibt die Party nicht um; es gibt den nächsten Tausch nur frei, nachdem die Spieler verglichen haben. Die alte Journaldatei liegt als `journal.bin.prev`. |

Ein gemeinsamer atomarer Schreibvorgang über zwei Rechner gibt es nicht.
Metaxy leitet Nachrichten weiter und hält keine Pokémon. Fällt eine Seite
nach ihrem Save und vor dem Save der anderen aus, kann ein Pokémon auf
einer Seite schon weg und auf der anderen noch da sein. Das Journal der
Seite, die geschrieben hat, behält den vorherigen Rekord in `journal.bin`
bzw. `.prev`. Es setzt ihn nicht von selbst wieder ein: das wäre die
Kopie, wenn die andere Seite ebenfalls behalten hat. Unklar heißt hier
stehenbleiben.

Ein beschädigtes Journal ändert die Party nicht und blockiert neue Tausche
bis Acknowledge. Diese Fassung bestätigt ein offenes Journal nicht von
selbst. Eine Zeile `journal on disk ... Not acknowledged` heißt: liegen
lassen und die Parties im Spiel vergleichen, bevor jemand Acknowledge
drückt oder denselben Tausch noch einmal anbietet.

### Testkopien vom 8. November 2026, etwa 19:33 Berlin

Auf der alten Fassung `f58ab369` (die laufenden Windows-Prozesse, nicht
dieser Quelltext) lagen danach zwei Journale à 908 Bytes und zwei
Save-Dateien (`host.sav` und `multiplayer/session.sav`). Der Join-Prozess
hing bei „saving this game (the host saved)“ und verbrauchte CPU; der Host
blieb bedienbar und meldete zweimal „1 to come“. Ob die Parties getauscht
wurden, ist daraus nicht zu sehen. Die Journalbytes wurden nicht gelesen.

Diese Kopien nicht über einen echten Spielstand legen. Die Prozesse nicht
von hier aus beenden. Wenn sie noch laufen, im Task-Manager selbst beenden,
bevor neu gebaut wird. Die Journale nicht löschen und nicht Acknowledge
geben, solange die beiden Parties nicht im Spiel verglichen sind. Ein
vorhandenes Journal plus ein geschriebenes Save ist kein abgeschlossener
Tausch. Die nächste Fassung loggt die Phase ohne Pokémon-Bytes und räumt
das Journal nicht weg.

Die Ereigniszahl „kind 13“ vor dieser Zeile ist `trainer.spotted` in
Bytes, nicht in Ereignissen, und lag zeitlich vor dem Speicherlog. Sie ist
keine festgestellte Ursache. Ein zweites `SaveStart` ist im Quelltext der
Weg, der zu genau dieser letzten Zeile führt; der Windows-Lauf wurde hier
nicht wiederholt.

## Was diese Fassung nicht ist

- Kein DS-Union-Room, kein C-Gear-Tausch, kein GTS, kein Wunder-Tausch.
- Kein Kampf und kein Koop-Ausbau.
- Kein automatischer Tausch. Anbieten und Bestätigen sind eigene Schritte.
- Keine Garantie, dass beide Spielstände in jedem Absturzfenster zusammenpassen.
- Everstone wird im laufenden Spiel erst blockiert, wenn eine Haltewirkung
  angebunden ist. Die Tests prüfen die Regel mit einem gesetzten Aufruf.
- Fateful-Encounter- und Reglement-Sperren des Union Room stehen in
  Spieldaten, die diese Fassung nicht rät. Geprüft werden Prüfsumme,
  Grenzen, Mail, Level 1 bis 100 und Ei (ein Ei darf angeboten werden,
  es entwickelt sich nicht).

Nicht als „fertig für beliebige Spieler im Netz“ bezeichnen.

## Bauen und prüfen, ohne ROM

Nur das Protokoll, auf dem Rechner, der das Repository hat:

```
gcc -std=c99 -Wall -Wextra -Werror -I sdk -I tests -o test_session_trade ^
    tests\test_session_trade.c sdk\session_trade.c
test_session_trade
```

Unter einer normalen Eidos-Konfiguration (mit Aletheia, ohne dass die
Tests ein ROM brauchen):

```
cmake --build build --target test_session_trade
build\test_session_trade
```

Die Fälle: gültiger Tausch, Entwicklung aus einer NARC-Zeile und ohne
erfundene Liste, Abbruch, Abbruch der Verbindung vor dem Save und während
des Saves, geändertes Angebot, kaputte Bytes, Journal nach Neustart,
Bestätigung ohne freies Save, Save noch nicht fertig, vorauseilendes
Lock, abgelaufene einseitige Bestätigung, leere Party ohne Lesezugriff,
und die Sperre gegen ein zweites Koop-Save. `test_trade_invite` prüft
zusätzlich viele ruhige Frames mit sichtbarem Partner ohne A und ein
unruhiges Feld.

## Windows, neben einer laufenden Fassung

Bekannte laufende Fassung hier: Snapshot `4ac965b44b8d`, 32-bit MinGW,
US/EU IRBO rev0, Blaze Black v3.2 nur als Daten, deutsche Originaltexte
über `a/0/0/2` und `a/0/0/3`. Metaxy `2fe527c9b24c20a807b106c95106caa0781ee1cd`
unverändert. Diese Anleitung ersetzt keine EXE, die schon läuft.

Eigenes Build-Verzeichnis, eigene `BW_SAVE_DIR` pro Prozess, lokales
Metaxy (`cargo run` in einem Checkout von metaxy auf genau diesem Commit).
`EIDOS_LINK_TEST` wie in `demo/eidos_boot.c`. Keine ROMs, keine extrahierten
Spieldaten und keine persönlichen Spielstände ins Repository oder in den
PR. Aletheia ist der Pin in `cmake/aletheia_pin.cmake`
(`7f30ac00d6fcdd7cf777bd582ac6f126d54906c5`); `tools/pinned_checkout.py`
holt eine saubere Kopie, wenn das Checkout öffentlich erreichbar ist.
