# Study mode (Cert Study Hub)

Study mode runs Cert Study Hub's certification practice on the reader, offline: flashcards, quizzes, timed practice
tests and a matching game. It is built into touch builds (the Sticky) and is opened from **Home > Study**.

## Put a study pack on the SD card

The easy way is Sticky Hub on a Mac (see [Installation](installation.md)): open File Transfer on the reader, tick the
cert tracks, and press Send. It downloads the latest pack that Study-App publishes on its `eink-study-pack` release,
so no Study-App checkout is needed.

To build a pack yourself, in the Study-App repository:

```sh
python3 build.py
python3 tools/eink_pack.py ccna az104      # or no track names for every track
```

Copy `eink-out/study/` to the root of the SD card, so the card has `/study/manifest.json`. File Transfer works too.
Re-export and copy again whenever the app's content changes; the reader's own schedule and log are kept.

The format is defined in the Study-App's `EINK_STUDY_PACK.md`.

Over a USB cable, the [CrossBlink Flasher](https://jamesctippets-ui.github.io/CrossBlink/flash/) page in Chrome sends
the latest published pack: leave the reader on Home, tick the tracks and press **Send to Sticky**.

## Modes

| Mode | What it does |
|---|---|
| Flashcards | 20 cards per session, due and new cards first, using the app's SM-2 schedule. Tap to reveal, then **Got it** or **Don't got it**. |
| Quiz | 10 random questions with the answer and explanation after each one. Multi-select questions need **Check**. |
| Practice test | The track's exam length and time limit from the pack, no feedback until the end, then the score, the pass mark and a review of misses. |
| Match | Four terms and four definitions. Tap a term, then its definition. Not logged, like the app's Match. |

**Category** on the track menu narrows every mode to one category; tap it to cycle through them.

## Files the reader writes

| File | Purpose |
|---|---|
| `/study/progress.jsonl` | Every flashcard rating and answered question, one line each. Import it in the app under **Data & Progress > Import e-reader progress**. Importing again only adds new lines. |
| `/study/.srs/<track>.bin` | The reader's flashcard schedule. Delete it to start the track's schedule fresh. |

Times in the log come from the reader's clock. With no clock set they are left out and the app uses the import time.
