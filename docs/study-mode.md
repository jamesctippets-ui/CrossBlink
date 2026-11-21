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
| Flashcards | 20 cards per session, due and new cards first, using the app's SM-2 schedule. Turn the page (or choose **Show answer**) to see the answer, then **Got it** or **Don't got it**. |
| Quiz | 10 random questions with the answer and explanation after each one. Multi-select questions need **Check**. |
| Practice test | The track's exam length and time limit from the pack, no feedback until the end, then the score, the pass mark and a review of misses. |
| Match | Four terms and four definitions. Tap a term, then its definition. Not logged, like the app's Match. |

**Category** on the track menu narrows every mode to one category; tap it to cycle through them.

## Reading and controls

Flashcards, Quiz and Practice test read like a book. They use the reader's font family and size, line spacing,
margins and status bars (the book title is the track, the chapter is the mode and the card or question number). A
question, answer or explanation that does not fit on one page continues on the next, so nothing is cut off; the status
bar shows the page within the current card or question.

| Action | Buttons | Touch |
|---|---|---|
| Next page, or the next choice on the page | Down (or Right) | Tap the right of the page, or swipe left |
| Previous page or choice | Up (or Left) | Tap the left of the page, or swipe right |
| Pick the highlighted choice | OK | Tap the choice |
| Leave | Hold Up or Down for a second, or Back | Swipe up |

The first button press shows a frame around the choice that OK would pick; a tap hides it again. Turning past the
last page of a flashcard's prompt shows its answer, and paging back from the answer shows the prompt again. After
quiz feedback or a reviewed miss, turning past the last page goes to the next question. Page-turn taps and swipes
follow the reader's touch settings. The reader's built-in font is used even when an SD card font is chosen for books.

## Files the reader writes

| File | Purpose |
|---|---|
| `/study/progress.jsonl` | Every flashcard rating and answered question, one line each. Import it in the app under **Data & Progress > Import e-reader progress**. Importing again only adds new lines. |
| `/study/.srs/<track>.bin` | The reader's flashcard schedule. Delete it to start the track's schedule fresh. |

Times in the log come from the reader's clock. With no clock set they are left out and the app uses the import time.

## Study page on the Dashboard

With the **Dashboard** Home screen, swipe left on Home to open the Study page. Swipe right (or tap the back arrow) to
return to Home. It shows, from the reader's own log and flashcard schedules:

- **Cards due**: flashcards whose next review is now or overdue, across all tracks.
- **Day streak**: days in a row with at least one flashcard or question, counting today or yesterday.
- **Quiz score**: the share of quiz and practice-test questions answered right, all time.
- Today's and all-time counts of flashcards rated and questions answered.
- Each track, most studied first, with the cards seen so far, the cards due and its quiz score. Tap a track to study it.

**Open Study** goes to the full track list. Cards due, the streak and today's counts need the reader's clock to be set
(it is set over Wi-Fi); without it they show a dash. Days follow the time zone in the clock settings.
