# Synchronizing visuals to narration

Generate narration, preview it, then add or save the phase. Flow measures speech
against the finished audio and saves word and sentence cues with that phase.

1. In Manuscript, open **Speech cues** beside a saved phase. Choose Words or
   Sentences. Select a cue to seek to it and inspect its audio-relative times.
2. Create/select an animation keyframe in Timeline. Its **Speech cue** controls
   let you select a phase, find a word or sentence, choose Start/End, and enter
   an offset in seconds. Values, easing and key identity are preserved.
3. Regenerate narration normally. Identifiable cues retain their IDs, so bound
   animation keys follow the new measured timing automatically.

Uncertain recognition and changed/repeated words are flagged rather than given
estimated timing. Removed or ambiguous cues remain available for inspection.
Timeline's **Repair unresolved key** selector makes their keys accessible even
when they have no drawable time. Rebind to a current cue, or Detach: this keeps
its resolved time, or uses the playhead when its old cue cannot resolve.

For an active cue needing timing correction, select it in Manuscript, enter its
Start/End in seconds into that phase's audio, then **Set timing**. Corrections
are saved and undoable. Regeneration measures timestamps again.

Speech anchors share Flow's ordinary time resolver. Native keys, component
clock/event anchors, scene playback and export use the same timing. Clip trims
and placement are respected; replacing a clip's audio without reanalysis makes
its previous alignment unresolved.

## Local analysis

Flow bundles whisper.cpp and a multilingual Whisper Base model. Analysis runs on
a cancellable worker after audio generation, with a separate progress stage.
Audio is converted natively to 16 kHz mono for analysis. Recognition intervals
are matched conservatively to the supplied script; low-confidence or ambiguous
matches stay unresolved. These timestamps are measured recognition alignment,
not a guarantee that speech recognition identifies every word correctly.

CMake's `FLOW_BUILD_SPEECH_ALIGNMENT` defaults to ON, builds the pinned runner
and downloads a SHA-256-verified model beside Flow. Runtime analysis is local;
no audio or manuscript is sent to a service. Development overrides are
`FLOW_WHISPER` and `FLOW_WHISPER_MODEL` (a multilingual Whisper Base model for
DTW). Disabling packaging requires supplying both runner and model explicitly.

Upstream: [whisper.cpp](https://github.com/ggml-org/whisper.cpp), MIT license;
[converted model](https://huggingface.co/ggerganov/whisper.cpp).

## Removing narration

Each saved Manuscript phase has **Remove phase**. This removes its script and
its timeline narration together. The beat and audio file in Assets remain.
Undo restores the phase, clip and cue timing. Audio shared by another phase
stays on the timeline.

To remove only the timeline audio, use the trash button beside its audio row
in Timeline (or its context menu). The phase script stays available for editing
and regeneration. Cue-bound keys stay authored but unresolved until narration
is restored or those keys are rebound. Deleting a beat is a separate timing edit.
