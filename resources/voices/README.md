# Built-in narration voices

These short WAV references were generated on 2026-10-23 using the official
[Qwen/Qwen3-TTS](https://huggingface.co/spaces/Qwen/Qwen3-TTS) demo's
`generate_custom_voice` endpoint, with the Apache-2.0 licensed
[Qwen3-TTS-12Hz-1.7B-CustomVoice model](https://huggingface.co/Qwen/Qwen3-TTS-12Hz-1.7B-CustomVoice).

Parameters: language `English`, the named speaker, empty instruction, model size
`1.7B`. Text: "Hello. This is a preview of my narration voice."

Available references: Ryan, Aiden, Vivian, Serena and Eric. Flow plays these
files directly for instant voice previews and passes the same recording to its
local Qwen3-TTS Base runner to give generated narration the selected voice.
No network call is made for preview playback. CMake copies this directory next
to the app as `voices/`.

The current llama-tts runner uses speaker embeddings from the recording and
requires no recording transcript. The manuscript therefore does not request one.
