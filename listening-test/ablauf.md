# Ablauf des Hörtests vor Ort

Dieses Blatt ist für die Person, die den Test leitet. Die technische Einrichtung steht im
[README](README.md). Alle Befehle gelten im Hauptordner des Repositorys.

## Einmal vorab

- [ ] **Kopfhörer über Kabel**, für alle dieselben und mit derselben Einstellung. Kein Bluetooth: Das
      komprimiert den Ton noch einmal und kann genau die Fehler überdecken, um die es hier geht. Ein
      Bluetooth-Kopfhörer mit Kabelanschluss geht, wenn er über das Kabel läuft.
- [ ] **Windows-Sound:** Einstellungen, System, Sound, das Ausgabegerät wählen. Format auf 48000 Hz
      stellen, Audioverbesserungen und räumlichen Klang ausschalten.
- [ ] **Chrome**, keine anderen Programme mit Ton, Benachrichtigungen aus.
- [ ] **Ruhiger Raum** und ein Termin ohne Zeitdruck.
- [x] **Probelauf:** am 9. November 2026, noch über Bluetooth. Speichern in Chrome klappt, der Ton kommt
      auf beiden Ohren an. Die Bewertungsseiten dauerten zusammen gut 7 Minuten, also knapp eine pro
      Ausschnitt. Wer die Versionen noch nicht kennt, braucht sicher länger; der Begrüßungstext nennt
      15 bis 20 Minuten.
- [ ] **Kurz mit Kabel gegenhören**, ob der Ton auch so auf beiden Ohren gleich laut ankommt.
- [ ] **Ergebnisordner leeren**, damit der Probelauf nicht mitzählt:

```bash
rm -rf listening-test/webmushra/results/audio_codec_lab
```

## Vor dem Testtag

Server starten und das Fenster offen lassen:

```bash
php -S localhost:8000 -t listening-test/webmushra
```

Die Adresse in Chrome ist `http://localhost:8000/?config=audio_codec_lab.yaml`.

## Für jede Person

1. **Seite neu laden**, damit die Reihenfolge neu gemischt wird.
2. **Das hier sagen:**

   > Du hörst kurze Ausschnitte in verschiedenen Qualitäten. „Reference“ ist immer das Original. Daneben
   > stehen neun Versionen in zufälliger Reihenfolge, und eine davon ist das Original selbst. Höre alle
   > an und bewerte sie: 100 heißt kein Unterschied zum Original, 0 heißt sehr schlecht. Alle Regler
   > stehen am Anfang auf 100, stelle also jeden einzelnen bewusst ein. Lass dir Zeit und höre jede
   > Version ganz an, gern mehrmals. Niemand erwartet, dass du alles hörst, und es gibt keine falschen
   > Antworten. Zuerst kommt eine Übung, dann acht Durchgänge. Zwischen den Seiten kannst du jederzeit
   > Pause machen. Stelle die Lautstärke auf der zweiten Seite ein und lasse sie danach so.
   >
   > Gespeichert werden nur deine Bewertungen, dein Kürzel, dein Kopfhörer und deine Hörerfahrung. Die
   > Bewertungen können ohne Kürzel im Projekt veröffentlicht werden. Ist das für dich in Ordnung?

3. **Neben der Person bleiben, aber nicht auf den Bildschirm schauen** und keine Hinweise geben. Auch
   über Eindrücke erst sprechen, wenn alle fertig sind, damit niemand die nächsten Hörer beeinflusst.
4. **Am Ende:** Kürzel eintragen lassen, das noch niemand benutzt hat, und abschicken.
5. **Prüfen, ob die Daten angekommen sind.** Die Seite bedankt sich auch, wenn das Speichern fehlschlägt:

```bash
wc -l listening-test/webmushra/results/audio_codec_lab/mushra.csv
```

   Die Zeilenzahl muss 1 + 81 × Anzahl der Personen sein, also 82 nach der ersten und 163 nach der
   zweiten: neun Seiten mit je neun Versionen.

## Am Ende

Mindestens 8 Personen, besser 12, denn das Post-Screening kann einige ausschließen. Die Testleitung
selbst zählt nicht mit, wenn sie die Versionen schon kennt.

```bash
python listening-test/analyze.py listening-test/webmushra/results/audio_codec_lab/mushra.csv
```

Das schreibt `listening-test/results/results.md` und `listening-test/results/scores.png`. Die Rohdaten
sicher aufbewahren, denn `listening-test/webmushra` ist nicht im Repository.
